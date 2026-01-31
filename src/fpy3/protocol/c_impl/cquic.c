#define _GNU_SOURCE
#include <Python.h>
#include <msquic.h>
#include <nghttp3/nghttp3.h>
#include <arpa/inet.h> 
#include <pthread.h>

const QUIC_API_TABLE* MsQuic;
HQUIC Registration;
HQUIC Configuration;

const QUIC_REGISTRATION_CONFIG RegConfig = { "fpy3", QUIC_EXECUTION_PROFILE_LOW_LATENCY };
const QUIC_BUFFER AlpnBuffers[] = { 
    { sizeof("h3") - 1, (uint8_t*)"h3" },
    { sizeof("h3-29") - 1, (uint8_t*)"h3-29" }
};

typedef struct StreamContext_s StreamContext;

// --- Event System ---
typedef enum { EVT_HEADERS, EVT_DATA, EVT_FIN } EventType;

typedef struct Header_s {
    char* name;
    size_t name_len;
    char* value;
    size_t value_len;
    struct Header_s* next;
} Header;

typedef struct PendingEvent_s {
    EventType type;
    StreamContext* sctx;
    
    // For HEADERS
    Header* headers;
    
    // For DATA
    char* data; 
    size_t len;
    
    struct PendingEvent_s* next;
} PendingEvent;

typedef struct ResponseChunk_s {
    char* data;
    size_t len;
    size_t sent;
    struct ResponseChunk_s* next;
} ResponseChunk;

typedef struct {
    PyObject_HEAD
    HQUIC Listener;
    PyObject* app;
    PyObject* loop;
    
    pthread_mutex_t pending_lock;
    PendingEvent* pending_head;
    PendingEvent* pending_tail; 
    int debug_mode;
} QuicServer;

typedef struct {
    QuicServer* server;
    nghttp3_conn* http3;
    HQUIC Connection;
    HQUIC CtrlStream;
    HQUIC QEncStream;
    HQUIC QDecStream;
    int streams_started_count;
    int is_ready;
    StreamContext* streams[64];
    int stream_count;
    pthread_mutex_t lock;
} ConnectionContext;

struct StreamContext_s {
    int64_t stream_id;
    ConnectionContext* conn_ctx;
    HQUIC Stream;
    int is_uni;
    int has_error; // Flag to indicate stream processing failed
    
    // Request Headers Accumulation (Temporary before pushing EVT_HEADERS)
    Header* temp_headers_head;
    Header* temp_headers_tail;
    
    // Response Streaming
    ResponseChunk* resp_head;
    ResponseChunk* resp_tail;
    ResponseChunk* finished_head; // To hold chunks until FlushStream is done
    int resp_fin; // If true, send EOF after chunks
    
    int is_ctrl;
};

typedef struct {
    QUIC_BUFFER* Buffers;
    uint32_t BufferCount;
} SendContext;

// --- Helpers ---

void AddStreamContext(ConnectionContext* ctx, StreamContext* sctx) {
    if (ctx->stream_count < 64) {
        ctx->streams[ctx->stream_count++] = sctx;
    }
}

StreamContext* FindStreamContext(ConnectionContext* ctx, int64_t stream_id) {
    for (int i = 0; i < ctx->stream_count; ++i) {
        if (ctx->streams[i] && ctx->streams[i]->stream_id == stream_id) {
            return ctx->streams[i];
        }
    }
    return NULL;
}

void FreeHeaders(Header* head) {
    while (head) {
        Header* next = head->next;
        free(head->name);
        free(head->value);
        free(head);
        head = next;
    }
}

void AddTempHeader(StreamContext* sctx, uint8_t* name, size_t namelen, uint8_t* value, size_t valuelen) {
    Header* h = malloc(sizeof(Header));
    h->name = malloc(namelen + 1); memcpy(h->name, name, namelen); h->name[namelen] = 0;
    h->name_len = namelen;
    h->value = malloc(valuelen + 1); memcpy(h->value, value, valuelen); h->value[valuelen] = 0;
    h->value_len = valuelen;
    h->next = NULL;
    
    if (sctx->temp_headers_tail) {
        sctx->temp_headers_tail->next = h;
        sctx->temp_headers_tail = h;
    } else {
        sctx->temp_headers_head = sctx->temp_headers_tail = h;
    }
}

// Push Event to Queue
void PushEvent(QuicServer* server, PendingEvent* evt) {
    pthread_mutex_lock(&server->pending_lock);
    if (server->pending_tail) {
        server->pending_tail->next = evt;
        server->pending_tail = evt;
    } else {
        server->pending_head = server->pending_tail = evt;
    }
    pthread_mutex_unlock(&server->pending_lock);
    
    // Wake up Python
    PyGILState_STATE gstate = PyGILState_Ensure();
    PyObject* method = PyObject_GetAttrString((PyObject*)server, "process_pending");
    if (method) {
        PyObject* res = PyObject_CallMethod(server->loop, "call_soon_threadsafe", "O", method);
        if (res) {
             Py_DECREF(res);
        } else {
             PyErr_Print();
        }
        Py_DECREF(method);
    } else {
        PyErr_Print();
    }
    PyGILState_Release(gstate);
}

// --- IO Logic ---

int64_t GetStreamID(HQUIC Stream) {
    uint64_t id = 0;
    uint32_t len = sizeof(id);
    MsQuic->GetParam(Stream, QUIC_PARAM_STREAM_ID, &len, &id);
    return (int64_t)id;
}

void FlushConn(ConnectionContext* ctx) {
    if (!ctx || !ctx->http3) return;
    
    HQUIC UniStreams[] = { ctx->CtrlStream, ctx->QEncStream, ctx->QDecStream };
    
    for (int i=0; i<3; ++i) {
        if (!UniStreams[i]) continue;
        int64_t sid = GetStreamID(UniStreams[i]);
        
        int loop_guard = 0;
        for (;;) {
            if (++loop_guard > 100) break;
            nghttp3_vec vec[16];
            int64_t id_out;
            int fin_out;
            nghttp3_ssize s = nghttp3_conn_writev_stream(ctx->http3, &id_out, &fin_out, vec, 16);
            
            if (s <= 0) break;
            
            HQUIC TargetStream = NULL;
            if (id_out == GetStreamID(ctx->CtrlStream)) TargetStream = ctx->CtrlStream;
            else if (id_out == GetStreamID(ctx->QEncStream)) TargetStream = ctx->QEncStream;
            else if (id_out == GetStreamID(ctx->QDecStream)) TargetStream = ctx->QDecStream;
            
            if (TargetStream) {
                QUIC_BUFFER* Buffers = malloc(sizeof(QUIC_BUFFER) * s);
                size_t total_len = 0;
                for(int j=0; j<s; ++j) {
                     void* data = malloc(vec[j].len);
                     memcpy(data, vec[j].base, vec[j].len);
                     Buffers[j].Buffer = data;
                     Buffers[j].Length = (uint32_t)vec[j].len;
                     total_len += vec[j].len;
                }
                
                if (total_len > 0) {
                    nghttp3_conn_add_write_offset(ctx->http3, id_out, total_len);
                    SendContext* sc = malloc(sizeof(SendContext));
                    sc->Buffers = Buffers; sc->BufferCount = (uint32_t)s;
                    MsQuic->StreamSend(TargetStream, Buffers, (uint32_t)s, QUIC_SEND_FLAG_NONE, sc);
                } else {
                    for(int j=0; j<s; ++j) free(Buffers[j].Buffer);
                    free(Buffers);
                }
            }
        }
    }
}

void FreeFinishedChunks(StreamContext* sctx) {
    while (sctx->finished_head) {
        ResponseChunk* next = sctx->finished_head->next;
        free(sctx->finished_head->data);
        free(sctx->finished_head);
        sctx->finished_head = next;
    }
}

void FlushStream(StreamContext* sctx) {
     if (!sctx || !sctx->conn_ctx) return;
     ConnectionContext* ctx = sctx->conn_ctx;
     if (!ctx->http3) return;
     
     int loop_guard = 0;
     for (;;) {
        if (++loop_guard > 100) break;
        nghttp3_vec vec[16];
        int64_t id_out;
        int fin_out;
        nghttp3_ssize s = nghttp3_conn_writev_stream(ctx->http3, &id_out, &fin_out, vec, 16);
        if (s < 0) break;
        if (s == 0 && !fin_out) break;
        
        HQUIC TargetStream = NULL;
        if (id_out == sctx->stream_id) TargetStream = sctx->Stream;
        else if (ctx->CtrlStream && id_out == GetStreamID(ctx->CtrlStream)) TargetStream = ctx->CtrlStream;
        else if (ctx->QEncStream && id_out == GetStreamID(ctx->QEncStream)) TargetStream = ctx->QEncStream;
        else if (ctx->QDecStream && id_out == GetStreamID(ctx->QDecStream)) TargetStream = ctx->QDecStream;
        
        if (TargetStream) {
             QUIC_BUFFER* Buffers = malloc(sizeof(QUIC_BUFFER) * s);
             size_t total_len = 0;
             for(int j=0; j<s; ++j) {
                 void* data = malloc(vec[j].len);
                 memcpy(data, vec[j].base, vec[j].len);
                 Buffers[j].Buffer = data;
                 Buffers[j].Length = (uint32_t)vec[j].len;
                 total_len += vec[j].len;
             }
             if (total_len > 0 || fin_out) {
                 if (total_len > 0) nghttp3_conn_add_write_offset(ctx->http3, id_out, total_len);
                 
                 SendContext* sc = malloc(sizeof(SendContext));
                 sc->Buffers = Buffers; sc->BufferCount = (uint32_t)s;
                 MsQuic->StreamSend(TargetStream, Buffers, (uint32_t)s, (fin_out && TargetStream!=ctx->CtrlStream && TargetStream!=ctx->QEncStream && TargetStream!=ctx->QDecStream)?QUIC_SEND_FLAG_FIN:QUIC_SEND_FLAG_NONE, sc);
             } else { 
                 // s=0, fin_out=0 (already handled above), or total_len=0 but s>0?
                 for(int j=0; j<s; ++j) free(Buffers[j].Buffer);
                 free(Buffers); 
             }
        }
        
        // Clean up chunks that were consumed in this writev cycle
        FreeFinishedChunks(sctx);
     }
     FreeFinishedChunks(sctx); // Ensure cleanup
}

// --- nghttp3 Callbacks ---

nghttp3_ssize server_read_data(nghttp3_conn *conn, int64_t stream_id, nghttp3_vec *vec, size_t veccnt, uint32_t *pflags, void *user_data, void *stream_user_data) {
    ConnectionContext* ctx = (ConnectionContext*)user_data;
    StreamContext* sctx = stream_user_data ? (StreamContext*)stream_user_data : FindStreamContext(ctx, stream_id);
    if (!sctx) return NGHTTP3_ERR_CALLBACK_FAILURE;
    
    if (veccnt < 1) return 0;
    
    if (sctx->resp_head) {
        // We have data
        ResponseChunk* chunk = sctx->resp_head;
        size_t remaining = chunk->len - chunk->sent;
        
        vec[0].base = (uint8_t*)(chunk->data + chunk->sent);
        vec[0].len = remaining;
        
        chunk->sent += remaining;
        
        if (chunk->sent >= chunk->len) {
            // Move to finished list instead of freeing immediately
            sctx->resp_head = chunk->next;
            if (!sctx->resp_head) sctx->resp_tail = NULL;
            
            chunk->next = sctx->finished_head;
            sctx->finished_head = chunk;
        }
        
        // If no more data AND fin is set
        if (!sctx->resp_head && sctx->resp_fin) {
             *pflags |= NGHTTP3_DATA_FLAG_EOF;
        }
        
        return 1;
    } else {
        if (sctx->resp_fin) {
            *pflags |= NGHTTP3_DATA_FLAG_EOF;
            return 0;
        } else {
            return NGHTTP3_ERR_WOULDBLOCK;
        }
    }
}

int server_recv_header(nghttp3_conn *conn, int64_t stream_id, int32_t token, nghttp3_rcbuf *name, nghttp3_rcbuf *value, uint8_t flags, void *user_data, void *stream_user_data) {
    ConnectionContext* ctx = (ConnectionContext*)user_data;
    StreamContext* sctx = stream_user_data ? (StreamContext*)stream_user_data : FindStreamContext(ctx, stream_id);
    if (!sctx) return 0;
    
    nghttp3_vec n = nghttp3_rcbuf_get_buf(name);
    nghttp3_vec v = nghttp3_rcbuf_get_buf(value);
    AddTempHeader(sctx, n.base, n.len, v.base, v.len);
    
    return 0;
}

int server_recv_data(nghttp3_conn *conn, int64_t stream_id, const uint8_t *data, size_t datalen, void *user_data, void *stream_user_data) {
    ConnectionContext* ctx = (ConnectionContext*)user_data;
    StreamContext* sctx = stream_user_data ? (StreamContext*)stream_user_data : FindStreamContext(ctx, stream_id);
    if (!sctx) return 0;
    
    // Check if we have pending headers first? 
    // Usually headers come before data. 
    // Assuming end_headers callback handles headers push. 
    
    if (datalen > 0) {
        PendingEvent* evt = calloc(1, sizeof(PendingEvent));
        evt->type = EVT_DATA;
        evt->sctx = sctx;
        evt->data = malloc(datalen);
        memcpy(evt->data, data, datalen);
        evt->len = datalen;
        PushEvent(ctx->server, evt);
    }
    
    return 0;
}

int server_end_headers(nghttp3_conn *conn, int64_t stream_id, int fin, void *user_data, void *stream_user_data) {
    ConnectionContext* ctx = (ConnectionContext*)user_data;
    StreamContext* sctx = stream_user_data ? (StreamContext*)stream_user_data : FindStreamContext(ctx, stream_id);
    if (!sctx) return 0;

    // Push HEADERS Event
    PendingEvent* evt = calloc(1, sizeof(PendingEvent));
    evt->type = EVT_HEADERS;
    evt->sctx = sctx;
    evt->headers = sctx->temp_headers_head;
    
    // Clear temp pointers from sctx but Keep the list for the event
    sctx->temp_headers_head = sctx->temp_headers_tail = NULL;
    
    PushEvent(ctx->server, evt);
    return 0;
}

int server_end_stream(nghttp3_conn *conn, int64_t stream_id, void *user_data, void *stream_user_data) {
    ConnectionContext* ctx = (ConnectionContext*)user_data;
    StreamContext* sctx = stream_user_data ? (StreamContext*)stream_user_data : FindStreamContext(ctx, stream_id);
    if (!sctx) return 0;
    
    PendingEvent* evt = calloc(1, sizeof(PendingEvent));
    evt->type = EVT_FIN;
    evt->sctx = sctx;
    PushEvent(ctx->server, evt);
    
    return 0;
}

const nghttp3_callbacks callbacks = { 
    .recv_header = server_recv_header, 
    .end_headers = server_end_headers,
    .recv_data = server_recv_data,
    .end_stream = server_end_stream 
};

// --- QUIC Callbacks (Same as before, simplified) ---

void FinalizeHttp3Setup(ConnectionContext* ctx) {
    nghttp3_conn_bind_control_stream(ctx->http3, GetStreamID(ctx->CtrlStream));
    nghttp3_conn_bind_qpack_streams(ctx->http3, GetStreamID(ctx->QEncStream), GetStreamID(ctx->QDecStream));
    FlushConn(ctx);
    ctx->is_ready = 1;
    
    // Re-enable receive on any streams that were deferred
    for (int i = 0; i < ctx->stream_count; i++) {
        if (ctx->streams[i]) {
            MsQuic->StreamReceiveSetEnabled(ctx->streams[i]->Stream, TRUE);
        }
    }
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(QUIC_STREAM_CALLBACK)
QUIC_STATUS QUIC_API ServerStreamCallback(_In_ HQUIC Stream, _In_opt_ void* Context, _Inout_ QUIC_STREAM_EVENT* Event) {
    StreamContext* sctx = (StreamContext*)Context;
    ConnectionContext* ctx = sctx->conn_ctx;

    if (ctx->server->debug_mode) {
        fprintf(stderr, "[DEBUG] Stream Event: Type=%d StreamID=%ld\n", Event->Type, (long)sctx->stream_id);
        fflush(stderr);
    }
    
    switch (Event->Type) {
    case QUIC_STREAM_EVENT_START_COMPLETE:
        if (sctx->is_ctrl && QUIC_SUCCEEDED(Event->START_COMPLETE.Status)) {
            pthread_mutex_lock(&ctx->lock);
            ctx->streams_started_count++;
            if (ctx->streams_started_count == 3) FinalizeHttp3Setup(ctx);
            pthread_mutex_unlock(&ctx->lock);
        }
        break;
    case QUIC_STREAM_EVENT_RECEIVE:
        pthread_mutex_lock(&ctx->lock);
        if (ctx->server->debug_mode) {
            fprintf(stderr, "[DEBUG] Stream Receive: Len=%llu Flags=%08x Ready=%d StreamID=%ld\n", 
                (unsigned long long)Event->RECEIVE.TotalBufferLength, 
                Event->RECEIVE.Flags, ctx->is_ready, (long)sctx->stream_id);
             fflush(stderr);
        }
        
        // If stream already failed, ignore data
        if (sctx->has_error) {
            pthread_mutex_unlock(&ctx->lock);
            break;
        }

        if (ctx->is_ready && ctx->http3) {
            int is_fin = (Event->RECEIVE.Flags & QUIC_RECEIVE_FLAG_FIN) ? 1 : 0;
            int is_bidi = (sctx->stream_id % 4 == 0);
            int first_read = 1;
            
            for(unsigned i=0; i<Event->RECEIVE.BufferCount; ++i) {
                int last = (i == Event->RECEIVE.BufferCount - 1);
                uint32_t buflen = Event->RECEIVE.Buffers[i].Length;
                
                // Skip zero-length buffers
                if (buflen == 0) {
                     // Check fin on zero-length last buffer
                     if (last && is_fin) {
                        nghttp3_conn_read_stream(ctx->http3, sctx->stream_id, NULL, 0, 1);
                     }
                     continue;
                }
                
                int fin_flag = (last && is_fin) ? 1 : 0;
                
                nghttp3_ssize rv = nghttp3_conn_read_stream(ctx->http3, sctx->stream_id, 
                    Event->RECEIVE.Buffers[i].Buffer, buflen, fin_flag);
                
                if (first_read && is_bidi && rv > 0) {
                    nghttp3_conn_set_stream_user_data(ctx->http3, sctx->stream_id, sctx);
                    first_read = 0;
                }
                
                if (rv < 0) {
                    if (ctx->server->debug_mode) {
                        fprintf(stderr, "[DEBUG] nghttp3_conn_read_stream error: %ld (stream=%ld). Stopping stream processing.\n", 
                            (long)rv, (long)sctx->stream_id);
                        fflush(stderr);
                    }
                    // Mark stream as failed
                    sctx->has_error = 1;
                    
                    // If it's the control stream, maybe we should close connection?
                    // For now, just stop processing this stream to prevent crash.
                    // nghttp3 internal state for this stream is now undefined.
                    break;
                }
            }
            if (!sctx->has_error) {
                FlushStream(sctx);
            }
        } else {
            // Not ready yet - tell MsQuic to hold the data and retry later
            if (ctx->server->debug_mode) {
                fprintf(stderr, "[DEBUG] Deferring stream data (not ready yet)\n");
                fflush(stderr);
            }
            MsQuic->StreamReceiveSetEnabled(sctx->Stream, FALSE);
        }
        pthread_mutex_unlock(&ctx->lock);
        break;
    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        {
            SendContext* sc = (SendContext*)Event->SEND_COMPLETE.ClientContext;
            if (sc) { 
                for(uint32_t i=0; i<sc->BufferCount; ++i) free(sc->Buffers[i].Buffer);
                free(sc->Buffers);
                free(sc);
            }
        }
        break;
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        FreeHeaders(sctx->temp_headers_head);
        // Free Response Chunks
        while(sctx->resp_head) {
            ResponseChunk* next = sctx->resp_head->next;
            free(sctx->resp_head->data);
            free(sctx->resp_head);
            sctx->resp_head = next;
        }
        // Free Finished Chunks
        while(sctx->finished_head) {
            ResponseChunk* next = sctx->finished_head->next;
            free(sctx->finished_head->data);
            free(sctx->finished_head);
            sctx->finished_head = next;
        }
        free(sctx);
        MsQuic->StreamClose(Stream);
        break;
    default: break;
    }
    return QUIC_STATUS_SUCCESS;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(QUIC_CONNECTION_CALLBACK)
QUIC_STATUS QUIC_API ServerConnectionCallback(_In_ HQUIC Connection, _In_opt_ void* Context, _Inout_ QUIC_CONNECTION_EVENT* Event) {
    ConnectionContext* ctx = (ConnectionContext*)Context; 
    if (ctx->server->debug_mode) {
        const char* type_str = "UNKNOWN";
        switch(Event->Type) {
            case QUIC_CONNECTION_EVENT_CONNECTED: type_str = "CONNECTED"; break;
            case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT: type_str = "SHUTDOWN_BY_TRANSPORT"; break;
            case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER: type_str = "SHUTDOWN_BY_PEER"; break;
            case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE: type_str = "SHUTDOWN_COMPLETE"; break;
            case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: type_str = "PEER_STREAM_STARTED"; break;
            case QUIC_CONNECTION_EVENT_RESUMPTION_TICKET_RECEIVED: type_str = "RESUMPTION_TICKET"; break;
            case QUIC_CONNECTION_EVENT_PEER_CERTIFICATE_RECEIVED: type_str = "PEER_CERTIFICATE"; break;
            default: type_str = "OTHER"; break;
        }
        fprintf(stderr, "[DEBUG] Connection Event: %s (%d)\n", type_str, Event->Type);
        
        if (Event->Type == QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT) {
             fprintf(stderr, "[DEBUG]   Status=0x%x ErrorCode=%llu\n", 
                Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status, 
                (unsigned long long)Event->SHUTDOWN_INITIATED_BY_TRANSPORT.ErrorCode);
        }
        fflush(stderr);
    }
    switch (Event->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED:
        pthread_mutex_lock(&ctx->lock);
        nghttp3_settings settings; nghttp3_settings_default(&settings);
        nghttp3_conn_server_new(&ctx->http3, &callbacks, &settings, nghttp3_mem_default(), ctx);
        
        StreamContext* ctrl = calloc(1, sizeof(StreamContext)); ctrl->conn_ctx = ctx; ctrl->is_ctrl = 1; ctrl->is_uni=1;
        MsQuic->StreamOpen(Connection, QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL, ServerStreamCallback, ctrl, &ctx->CtrlStream);
        ctrl->Stream = ctx->CtrlStream;
        MsQuic->StreamStart(ctx->CtrlStream, QUIC_STREAM_START_FLAG_IMMEDIATE);
        
        StreamContext* enc = calloc(1, sizeof(StreamContext)); enc->conn_ctx = ctx; enc->is_ctrl = 1; enc->is_uni=1;
        MsQuic->StreamOpen(Connection, QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL, ServerStreamCallback, enc, &ctx->QEncStream);
        enc->Stream = ctx->QEncStream;
        MsQuic->StreamStart(ctx->QEncStream, QUIC_STREAM_START_FLAG_IMMEDIATE);
        
        StreamContext* dec = calloc(1, sizeof(StreamContext)); dec->conn_ctx = ctx; dec->is_ctrl = 1; dec->is_uni=1;
        MsQuic->StreamOpen(Connection, QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL, ServerStreamCallback, dec, &ctx->QDecStream);
        dec->Stream = ctx->QDecStream;
        MsQuic->StreamStart(ctx->QDecStream, QUIC_STREAM_START_FLAG_IMMEDIATE);
        pthread_mutex_unlock(&ctx->lock);
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        if (ctx) { 
            if (ctx->http3) nghttp3_conn_del(ctx->http3); 
            pthread_mutex_destroy(&ctx->lock);
            free(ctx); 
        }
        MsQuic->ConnectionClose(Connection);
        break;
    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
        {
            int64_t id = 0;
            uint32_t len = sizeof(id);
            MsQuic->GetParam(Event->PEER_STREAM_STARTED.Stream, QUIC_PARAM_STREAM_ID, &len, &id);
            
            StreamContext* sctx = calloc(1, sizeof(StreamContext));
            sctx->Stream = Event->PEER_STREAM_STARTED.Stream;
            sctx->conn_ctx = ctx;
            sctx->stream_id = id;
            
            pthread_mutex_lock(&ctx->lock);
            AddStreamContext(ctx, sctx);
            pthread_mutex_unlock(&ctx->lock);
            
            MsQuic->SetCallbackHandler(Event->PEER_STREAM_STARTED.Stream, (void*)ServerStreamCallback, sctx);
            MsQuic->StreamReceiveSetEnabled(Event->PEER_STREAM_STARTED.Stream, TRUE);
        }
        break;
    default: break;
    }
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API ServerListenerCallback(_In_ HQUIC Listener, _In_opt_ void* Context, _Inout_ QUIC_LISTENER_EVENT* Event) {
    QuicServer* server = (QuicServer*)Context;
    if (server->debug_mode) {
        fprintf(stderr, "[DEBUG] Listener Event: Type=%d\n", Event->Type);
        fflush(stderr);
    }
    if (Event->Type == QUIC_LISTENER_EVENT_NEW_CONNECTION) {
        ConnectionContext* ctx = calloc(1, sizeof(ConnectionContext));
        ctx->Connection = Event->NEW_CONNECTION.Connection;
        ctx->server = server;
        pthread_mutex_init(&ctx->lock, NULL);
        MsQuic->SetCallbackHandler(Event->NEW_CONNECTION.Connection, (void*)ServerConnectionCallback, ctx); 
        MsQuic->ConnectionSetConfiguration(Event->NEW_CONNECTION.Connection, Configuration);
        return QUIC_STATUS_SUCCESS;
    }
    return QUIC_STATUS_SUCCESS;
}

// --- Python Methods ---

static PyObject* QuicServer_process_pending(QuicServer* self, PyObject* args) {
    pthread_mutex_lock(&self->pending_lock);
    PendingEvent* head = self->pending_head;
    self->pending_head = self->pending_tail = NULL;
    pthread_mutex_unlock(&self->pending_lock);
    
    if (!head) return Py_None;
    
    while (head) {
        PendingEvent* evt = head;
        head = head->next;
        
        PyObject* stream_handle = PyCapsule_New(evt->sctx, "StreamContext", NULL);
        
        switch (evt->type) {
        case EVT_HEADERS:
            {
                PyObject* headers_list = PyList_New(0);
                Header* h = evt->headers;
                while (h) {
                    PyObject* tuple = PyTuple_New(2);
                    PyTuple_SetItem(tuple, 0, PyBytes_FromStringAndSize(h->name, h->name_len));
                    PyTuple_SetItem(tuple, 1, PyBytes_FromStringAndSize(h->value, h->value_len));
                    PyList_Append(headers_list, tuple);
                    Py_DECREF(tuple);
                    h = h->next;
                }
                PyObject* res = PyObject_CallMethod((PyObject*)self, "on_headers", "OO", stream_handle, headers_list);
                if (!res) PyErr_Print();
                Py_XDECREF(res);
                Py_DECREF(headers_list);
                FreeHeaders(evt->headers);
            }
            break;
        case EVT_DATA:
            {
                PyObject* data_obj = PyBytes_FromStringAndSize(evt->data, evt->len);
                PyObject* res = PyObject_CallMethod((PyObject*)self, "on_data", "OO", stream_handle, data_obj);
                if (!res) PyErr_Print();
                Py_XDECREF(res);
                Py_DECREF(data_obj);
                free(evt->data);
            }
            break;
        case EVT_FIN:
            {
                PyObject* res = PyObject_CallMethod((PyObject*)self, "on_fin", "O", stream_handle);
                if (!res) PyErr_Print();
                Py_XDECREF(res);
            }
            break;
        }
        Py_DECREF(stream_handle);
        free(evt);
    }
    Py_RETURN_NONE;
}

static PyObject* QuicServer_send_headers(QuicServer* self, PyObject* args) {
    PyObject* capsule;
    PyObject* headers_list;
    int fin;
    if (!PyArg_ParseTuple(args, "OOp", &capsule, &headers_list, &fin)) return NULL;
    
    StreamContext* sctx = (StreamContext*)PyCapsule_GetPointer(capsule, "StreamContext");
    if (!sctx) return NULL;
    ConnectionContext* ctx = sctx->conn_ctx;

    Py_BEGIN_ALLOW_THREADS
    pthread_mutex_lock(&ctx->lock);
    
    Py_ssize_t len = PyList_Size(headers_list);
    nghttp3_nv* nva = malloc(sizeof(nghttp3_nv) * len);
    for (int i=0; i<len; ++i) {
        PyObject* tuple = PyList_GetItem(headers_list, i);
        nva[i].name = (uint8_t*)PyBytes_AsString(PyTuple_GetItem(tuple, 0));
        nva[i].namelen = PyBytes_Size(PyTuple_GetItem(tuple, 0));
        nva[i].value = (uint8_t*)PyBytes_AsString(PyTuple_GetItem(tuple, 1));
        nva[i].valuelen = PyBytes_Size(PyTuple_GetItem(tuple, 1));
        nva[i].flags = NGHTTP3_NV_FLAG_NONE;
    }
    
    // Set response FIN if requested
    if (fin) sctx->resp_fin = 1;
    
    nghttp3_data_reader dr; 
    dr.read_data = server_read_data;
    
    nghttp3_conn_submit_response(ctx->http3, sctx->stream_id, nva, len, &dr);
    free(nva);
    FlushStream(sctx);
    
    pthread_mutex_unlock(&ctx->lock);
    Py_END_ALLOW_THREADS
    Py_RETURN_NONE;
}

static PyObject* QuicServer_send_data(QuicServer* self, PyObject* args) {
    PyObject* capsule;
    PyObject* data_obj;
    int fin;
    if (!PyArg_ParseTuple(args, "OOp", &capsule, &data_obj, &fin)) return NULL;
    
    StreamContext* sctx = (StreamContext*)PyCapsule_GetPointer(capsule, "StreamContext");
    if (!sctx) return NULL;
    ConnectionContext* ctx = sctx->conn_ctx;

    Py_BEGIN_ALLOW_THREADS
    pthread_mutex_lock(&ctx->lock);

    if (PyBytes_Check(data_obj)) {
        char* buf; Py_ssize_t len;
        PyBytes_AsStringAndSize(data_obj, &buf, &len);
        if (len > 0) {
            ResponseChunk* chunk = malloc(sizeof(ResponseChunk));
            chunk->data = malloc(len);
            memcpy(chunk->data, buf, len);
            chunk->len = len;
            chunk->sent = 0;
            chunk->next = NULL;
            
            if (sctx->resp_tail) {
                sctx->resp_tail->next = chunk;
                sctx->resp_tail = chunk;
            } else {
                sctx->resp_head = sctx->resp_tail = chunk;
            }
        }
    }
    
    if (fin) sctx->resp_fin = 1;
    
    nghttp3_conn_resume_stream(ctx->http3, sctx->stream_id);
    FlushStream(sctx);
    
    pthread_mutex_unlock(&ctx->lock);
    Py_END_ALLOW_THREADS
    Py_RETURN_NONE;
}

static int QuicServer_init(QuicServer* self, PyObject* args, PyObject* kwds) {
    PyObject *app, *loop;
    int debug = 0;
    static char *kwlist[] = {"app", "loop", "debug", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OO|p", kwlist, &app, &loop, &debug)) return -1;
    fprintf(stderr, "DEBUG: QuicServer_init debug=%d\n", debug); fflush(stderr);
    Py_INCREF(app); self->app = app; 
    Py_INCREF(loop); self->loop = loop;
    self->debug_mode = debug;
    self->Listener = NULL;
    pthread_mutex_init(&self->pending_lock, NULL);
    self->pending_head = self->pending_tail = NULL;
    return 0;
}

static void QuicServer_dealloc(QuicServer* self) {
    if (self->Listener) { MsQuic->ListenerStop(self->Listener); MsQuic->ListenerClose(self->Listener); }
    Py_XDECREF(self->app); Py_XDECREF(self->loop);
    pthread_mutex_destroy(&self->pending_lock);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyObject* QuicServer_start(QuicServer* self, PyObject* args) {
    char* host; int port;
    if (!PyArg_ParseTuple(args, "si", &host, &port)) return NULL;
    if (!MsQuic) {
        MsQuicOpen2(&MsQuic);
        MsQuic->RegistrationOpen(&RegConfig, &Registration);
        QUIC_SETTINGS Settings = {0};
        Settings.IdleTimeoutMs = 5000; Settings.IsSet.IdleTimeoutMs = TRUE;
        Settings.IsSet.PeerBidiStreamCount = TRUE; Settings.PeerBidiStreamCount = 100;
        Settings.IsSet.PeerUnidiStreamCount = TRUE; Settings.PeerUnidiStreamCount = 3;
        MsQuic->ConfigurationOpen(Registration, AlpnBuffers, 2, &Settings, sizeof(Settings), NULL, &Configuration);
        QUIC_CREDENTIAL_CONFIG CredConfig; memset(&CredConfig, 0, sizeof(CredConfig));
        QUIC_CERTIFICATE_FILE CertFileParams = { .CertificateFile = "cert.pem", .PrivateKeyFile = "key.pem" };
        CredConfig.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE; CredConfig.CertificateFile = &CertFileParams;
        QUIC_STATUS Status = MsQuic->ConfigurationLoadCredential(Configuration, &CredConfig);
        if (QUIC_FAILED(Status)) {
            fprintf(stderr, "ERROR: ConfigurationLoadCredential failed: 0x%x\n", Status);
            fflush(stderr);
        }
    }
    MsQuic->ListenerOpen(Registration, ServerListenerCallback, self, &self->Listener);
    QUIC_ADDR Address = {0};
    Address.Ipv4.sin_family = QUIC_ADDRESS_FAMILY_INET; 
    Address.Ipv4.sin_port = htons(port);
    Address.Ipv4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    MsQuic->ListenerStart(self->Listener, AlpnBuffers, 2, &Address);
    Py_RETURN_NONE;
}

static PyObject* QuicServer_get_stream_id(QuicServer* self, PyObject* args) {
    PyObject* capsule;
    if (!PyArg_ParseTuple(args, "O", &capsule)) return NULL;
    StreamContext* sctx = (StreamContext*)PyCapsule_GetPointer(capsule, "StreamContext");
    if (!sctx) return NULL;
    return PyLong_FromLongLong(sctx->stream_id);
}

static PyMethodDef QuicServer_methods[] = { 
    {"start", (PyCFunction)QuicServer_start, METH_VARARGS, ""}, 
    {"get_stream_id", (PyCFunction)QuicServer_get_stream_id, METH_VARARGS, ""},
    {"send_headers", (PyCFunction)QuicServer_send_headers, METH_VARARGS, ""},
    {"send_data", (PyCFunction)QuicServer_send_data, METH_VARARGS, ""},
    {"process_pending", (PyCFunction)QuicServer_process_pending, METH_VARARGS, ""},
    {NULL} 
};

static PyTypeObject QuicServerType = {
    PyVarObject_HEAD_INIT(NULL, 0) "cquic.QuicServer", sizeof(QuicServer), 0, (destructor)QuicServer_dealloc,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,"QuicServer",0,0,0,0,0,0,QuicServer_methods, 0,0,0,0,0,0,0, (initproc)QuicServer_init, 0, PyType_GenericNew,
};

static PyModuleDef cquic = { PyModuleDef_HEAD_INIT, "cquic", "", -1, NULL, NULL, NULL, NULL, NULL };

PyMODINIT_FUNC PyInit_cquic(void) {
    PyObject* m;
    if (PyType_Ready(&QuicServerType) < 0) return NULL;
    m = PyModule_Create(&cquic);
    Py_INCREF(&QuicServerType);
    PyModule_AddObject(m, "QuicServer", (PyObject*)&QuicServerType);
    return m;
}
