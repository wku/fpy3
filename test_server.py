
import asyncio
from fpy3.protocol import cquic

class AppServer(cquic.QuicServer):
    def __init__(self, app, loop):
        super().__init__(app, loop)
        self.streams = {}

    def on_headers(self, stream_handle, headers):
        print(f"Stream {stream_handle}: Headers received: {headers}")
        # Store stream state if needed
        self.streams[stream_handle] = headers
        
        # Send Response Headers immediately (Echo)
        resp_headers = [
            (b":status", b"200"),
            (b"content-type", b"text/plain"),
            (b"server", b"fpy-cquic"),
        ]
        self.send_headers(stream_handle, resp_headers, False)
        
        # Send initial body part
        self.send_data(stream_handle, b"Echo Response for /. Body: ", False)

    def on_data(self, stream_handle, data):
        print(f"Stream {stream_handle}: Data received: {data}")
        # Echo back data
        self.send_data(stream_handle, data, False)

    def on_fin(self, stream_handle):
        print(f"Stream {stream_handle}: Fin received")
        # End response
        self.send_data(stream_handle, b"", True)
        self.streams.pop(stream_handle, None)

async def main():
    loop = asyncio.get_running_loop()
    server = AppServer(None, loop)
    server.start("127.0.0.1", 8080)
    print("Streaming Server implementation running on 8080...")
    await asyncio.Event().wait()

if __name__ == "__main__":
    asyncio.run(main())
