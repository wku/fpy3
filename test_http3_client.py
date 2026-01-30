#!/usr/bin/env python3
import asyncio
import ssl
from aioquic.asyncio import connect
from aioquic.asyncio.protocol import QuicConnectionProtocol
from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.events import StreamDataReceived, HandshakeCompleted
from aioquic.h3.connection import H3_ALPN, H3Connection
from aioquic.h3.events import HeadersReceived, DataReceived

class Http3Client(QuicConnectionProtocol):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.h3 = None
        self.response_received = asyncio.Event()
        self.headers = []
        self.data = b""
        
    def quic_event_received(self, event):
        print(f"QUIC Event: {event}")
        
        if isinstance(event, HandshakeCompleted):
            if self.h3 is None:
                self.h3 = H3Connection(self._quic)
        
        if self.h3 is None:
            return
            
        for h3_event in self.h3.handle_event(event):
            print(f"H3 Event: {h3_event}")
            if isinstance(h3_event, HeadersReceived):
                self.headers = h3_event.headers
                print(f"Headers: {self.headers}")
            elif isinstance(h3_event, DataReceived):
                self.data += h3_event.data
                print(f"Data: {h3_event.data}")
                if h3_event.stream_ended:
                    self.response_received.set()

async def main():
    configuration = QuicConfiguration(
        is_client=True,
        alpn_protocols=H3_ALPN,
    )
    configuration.verify_mode = ssl.CERT_NONE
    
    print("Connecting to 127.0.0.1:8080...")
    try:
        async with connect(
            "127.0.0.1",
            8080,
            configuration=configuration,
            create_protocol=Http3Client,
        ) as protocol:
            print("Connected!")
            
            await asyncio.sleep(0.5)
            
            if protocol.h3 is None:
                print("H3 not initialized, doing it manually...")
                protocol.h3 = H3Connection(protocol._quic)
            
            stream_id = protocol._quic.get_next_available_stream_id()
            print(f"Using stream_id: {stream_id}")
            
            protocol.h3.send_headers(
                stream_id=stream_id,
                headers=[
                    (b":method", b"POST"),
                    (b":scheme", b"https"),
                    (b":authority", b"127.0.0.1:8080"),
                    (b":path", b"/"),
                ],
                end_stream=False
            )
            
            # Send Body in chunks
            protocol.h3.send_data(stream_id, b"Part 1: Hello ", False)
            await asyncio.sleep(0.1)
            protocol.h3.send_data(stream_id, b"Part 2: Stream!", True)
            
            protocol.transmit()
            
            print("Request sent, waiting for response...")
            try:
                await asyncio.wait_for(protocol.response_received.wait(), timeout=10)
                print(f"Response Headers: {protocol.headers}")
                print(f"Response Body: {protocol.data.decode('utf-8', errors='replace')}")
            except asyncio.TimeoutError:
                print("Timeout waiting for response")
                
    except Exception as e:
        print(f"Error: {e}")
        import traceback
        traceback.print_exc()

if __name__ == "__main__":
    asyncio.run(main())
