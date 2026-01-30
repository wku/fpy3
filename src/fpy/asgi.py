import asyncio
import ssl
import os
from fpy.protocol import cquic

class ASGIServer(cquic.QuicServer):
    def __init__(self, app, loop=None, debug=False):
        if loop is None:
            loop = asyncio.get_running_loop()
        super().__init__(app, loop, debug=debug)
        self._loop = loop
        self.asgi_app = app
        self.debug = debug
        self.streams = {} # handle -> { queue: asyncio.Queue, task: asyncio.Task, headers: list }

    def start(self, host, port):
        # Start QUIC Listener (UDP)
        super().start(host, port)
        
        # Start TCP/TLS Listener (for Alt-Svc discovery)
        # Assuming cert.pem and key.pem exist (as QuicServer C-impl assumes hardcoded names too)
        if os.path.exists("cert.pem") and os.path.exists("key.pem"):
            print(f"Starting TCP/TLS Listener on {host}:{port} for Alt-Svc discovery...")
            self._loop.create_task(self._start_tcp(host, port))
        else:
            print("Warning: cert.pem/key.pem not found. TCP Listener for Alt-Svc skipped.")

    async def _start_tcp(self, host, port):
        ssl_ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ssl_ctx.load_cert_chain("cert.pem", "key.pem")
        # Optimization: Use weak ciphers or fast setup if possible? No, browsers need valid TLS.
        
        server = await asyncio.start_server(
            self._handle_tcp_client, host, port, ssl=ssl_ctx
        )
        await server.serve_forever()

    async def _handle_tcp_client(self, reader, writer):
        try:
            addr = writer.get_extra_info('peername')
            if self.debug:
                print(f"[DEBUG] TCP connection from {addr}")

            request_data = await self._read_http11_request(reader)
            if not request_data:
                return
            
            if self.debug:
                print(f"[DEBUG] TCP Request data len={len(request_data)}")

            scope, body = self._parse_http11_request(request_data, writer)
            if scope is None:
                return

            port = writer.get_extra_info('sockname')[1]
            response_started = False
            response_status = 200
            response_headers = []
            response_body_parts = []

            body_queue = asyncio.Queue()
            body_queue.put_nowait({'type': 'http.request', 'body': body, 'more_body': False})

            async def receive():
                return await body_queue.get()

            async def send(message):
                nonlocal response_started, response_status, response_headers
                if message['type'] == 'http.response.start':
                    response_started = True
                    response_status = message['status']
                    response_headers = message.get('headers', [])
                elif message['type'] == 'http.response.body':
                    response_body_parts.append(message.get('body', b''))

            try:
                await self.asgi_app(scope, receive, send)
            except Exception as e:
                if self.debug:
                    print(f"[DEBUG] ASGI App Error (HTTP/1.1): {e}")
                response_status = 500
                response_body_parts = [b"Internal Server Error"]

            http_response = self._build_http11_response(
                response_status, response_headers, response_body_parts, port
            )
            writer.write(http_response)
            if self.debug:
                print(f"[DEBUG] TCP Sent HTTP/1.1 Response (status={response_status})")
            await writer.drain()

        except Exception as e:
            if self.debug:
                print(f"[DEBUG] TCP Handler Error: {e}")
        finally:
            writer.close()
            try:
                await writer.wait_closed()
            except:
                pass

    async def _read_http11_request(self, reader) -> bytes:
        data = b""
        while True:
            chunk = await reader.read(4096)
            if not chunk:
                break
            data += chunk
            if b"\r\n\r\n" in data:
                header_end = data.index(b"\r\n\r\n") + 4
                headers_part = data[:header_end]
                content_length = 0
                for line in headers_part.split(b"\r\n"):
                    if line.lower().startswith(b"content-length:"):
                        content_length = int(line.split(b":")[1].strip())
                        break
                body_received = len(data) - header_end
                while body_received < content_length:
                    chunk = await reader.read(min(4096, content_length - body_received))
                    if not chunk:
                        break
                    data += chunk
                    body_received += len(chunk)
                break
        return data

    def _parse_http11_request(self, data: bytes, writer) -> tuple:
        try:
            header_end = data.index(b"\r\n\r\n")
            headers_part = data[:header_end]
            body = data[header_end + 4:]

            lines = headers_part.split(b"\r\n")
            request_line = lines[0].decode('latin-1')
            parts = request_line.split(" ")
            method = parts[0]
            full_path = parts[1] if len(parts) > 1 else "/"
            
            if "?" in full_path:
                path, query_string = full_path.split("?", 1)
            else:
                path = full_path
                query_string = ""

            headers = []
            host = ""
            for line in lines[1:]:
                if b":" in line:
                    key, value = line.split(b":", 1)
                    key = key.strip().lower()
                    value = value.strip()
                    headers.append((key, value))
                    if key == b"host":
                        host = value.decode('latin-1')

            sockname = writer.get_extra_info('sockname')
            peername = writer.get_extra_info('peername')

            scope = {
                'type': 'http',
                'asgi': {'version': '3.0', 'spec_version': '2.3'},
                'http_version': '1.1',
                'server': (sockname[0], sockname[1]) if sockname else ('127.0.0.1', 8080),
                'client': (peername[0], peername[1]) if peername else ('127.0.0.1', 0),
                'scheme': 'https',
                'method': method,
                'path': path,
                'raw_path': path.encode('latin-1'),
                'query_string': query_string.encode('latin-1'),
                'headers': headers,
            }
            return scope, body
        except Exception as e:
            if self.debug:
                print(f"[DEBUG] HTTP/1.1 Parse Error: {e}")
            return None, b""

    def _build_http11_response(self, status: int, headers: list, body_parts: list, port: int) -> bytes:
        body = b"".join(body_parts)
        
        status_text = {
            200: "OK", 201: "Created", 204: "No Content",
            301: "Moved Permanently", 302: "Found", 304: "Not Modified",
            400: "Bad Request", 401: "Unauthorized", 403: "Forbidden",
            404: "Not Found", 405: "Method Not Allowed",
            500: "Internal Server Error", 502: "Bad Gateway", 503: "Service Unavailable"
        }.get(status, "OK")

        response = f"HTTP/1.1 {status} {status_text}\r\n"
        response += f"Alt-Svc: h3=\":{port}\"; ma=3600\r\n"
        response += f"Content-Length: {len(body)}\r\n"
        response += "Connection: close\r\n"
        
        has_content_type = False
        for k, v in headers:
            key = k.decode('latin-1') if isinstance(k, bytes) else k
            val = v.decode('latin-1') if isinstance(v, bytes) else v
            response += f"{key}: {val}\r\n"
            if key.lower() == "content-type":
                has_content_type = True
        
        if not has_content_type:
            response += "Content-Type: text/plain\r\n"
        
        response += "\r\n"
        return response.encode('latin-1') + body



    def on_headers(self, stream_handle, headers):
        # headers is list of (key, value) bytes
        stream_id = self.get_stream_id(stream_handle)
        scope = self._build_scope(headers)
        
        queue = asyncio.Queue()
        
        async def receive():
            item = await queue.get()
            return item
            
        async def send(message):
            await self._handle_asgi_send(stream_handle, message)
            
        task = self._loop.create_task(self._run_app(scope, receive, send, stream_handle))
        self.streams[stream_id] = {'queue': queue, 'task': task}


    def on_data(self, stream_handle, data):
        stream_id = self.get_stream_id(stream_handle)
        if stream_id in self.streams:
            self.streams[stream_id]['queue'].put_nowait({
                'type': 'http.request',
                'body': data,
                'more_body': True # We don't know if it's last until on_fin
            })

    def on_fin(self, stream_handle):
        stream_id = self.get_stream_id(stream_handle)
        if stream_id in self.streams:
            # Send final empty chunk with more_body=False
            self.streams[stream_id]['queue'].put_nowait({
                'type': 'http.request',
                'body': b'',
                'more_body': False
            })

    async def _run_app(self, scope, receive, send, stream_handle):
        stream_id = self.get_stream_id(stream_handle)
        try:
            await self.asgi_app(scope, receive, send)
        except Exception as e:
            print(f"ASGI App Error: {e}")
            # Ensure stream is closed?
            # self.send_data(stream_handle, b"", True) 
        finally:
            self.streams.pop(stream_id, None)

    async def _handle_asgi_send(self, stream_handle, message):
        if message['type'] == 'http.response.start':
            status = message['status']
            headers = message.get('headers', [])
            
            # Convert headers to list of (key, value) bytes
            # ASGI headers are list of [bytes, bytes]
            
            resp_headers = [(b":status", str(status).encode())]
            for k, v in headers:
                resp_headers.append((k, v))
                
            resp_headers.append((b"server", b"fpy"))
            
            self.send_headers(stream_handle, resp_headers, False)
            
        elif message['type'] == 'http.response.body':
            body = message.get('body', b'')
            more_body = message.get('more_body', False)
            self.send_data(stream_handle, body, not more_body)

    def _build_scope(self, headers):
        # Build ASGI http scope
        # headers is list of (name, value) bytes. value is bytes.
        
        # Need to extract :method, :path, :authority, :scheme
        method = b"GET"
        path = b"/"
        scheme = b"https"
        authority = b""
        
        clean_headers = []
        for n, v in headers:
            if n == b":method": method = v
            elif n == b":path": path = v
            elif n == b":scheme": scheme = v
            elif n == b":authority": authority = v
            elif n.startswith(b":"): continue
            else: clean_headers.append((n, v))
            
        return {
            'type': 'http',
            'asgi': {'version': '3.0', 'spec_version': '2.3'},
            'http_version': '3',
            'server': ('127.0.0.1', 443), # TODO: Get actual binding
            'client': ('127.0.0.1', 0), # TODO: Get peer address
            'scheme': scheme.decode(),
            'method': method.decode(),
            'path': path.decode(),
            'raw_path': path, # TODO: Handle query string separation
            'query_string': b'', # TODO: extract from path
            'headers': clean_headers,
        }
