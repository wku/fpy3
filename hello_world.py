import asyncio
import logging
from fpy3.asgi import ASGIServer
import argparse

logging.basicConfig(level=logging.INFO)

async def hello_app(scope, receive, send):
    """
    A simple ASGI application that says Hello World.
    """
    if scope['type'] != 'http':
        return

    print(f"Received request: {scope['method']} {scope['path']}")

    # Send Response Immediately (don't wait for body for "Hello World")
    # Note: In HTTP/3 you can stream response while receiving request.
    
    await send({
        'type': 'http.response.start',
        'status': 200,
        'headers': [
            (b'content-type', b'text/plain'),
            (b'server', b'fpy-asgi')
        ],
    })
    
    await send({
        'type': 'http.response.body',
        'body': b'Hello World from FPY over HTTP/3 (QUIC)!',
    })

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--debug', action='store_true', help='Enable debug logging')
    parser.add_argument('--host', default='127.0.0.1', help='Bind host (default: 127.0.0.1)')
    parser.add_argument('--port', type=int, default=8080, help='Bind port (default: 8080)')
    args = parser.parse_args()

    print(f"Starting FPY ASGI Server on {args.host}:{args.port}...")
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    
    server = ASGIServer(hello_app, loop=loop, debug=args.debug)
    server.start(args.host, args.port)
    
    try:
        loop.run_forever()
    except KeyboardInterrupt:
        print("\nStopping...")

