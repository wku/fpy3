
import asyncio
import sys
from fpy3.asgi import ASGIServer

async def echo_app(scope, receive, send):
    if scope['type'] != 'http':
        return
        
    print(f"ASGI Request: {scope['method']} {scope['path']}")
    
    await send({
        'type': 'http.response.start',
        'status': 200,
        'headers': [
            (b'content-type', b'text/plain'),
        ],
    })
    
    # Echo streaming body
    await send({
        'type': 'http.response.body',
        'body': b'Echo Response for /. Body: ',
        'more_body': True
    })

    more_body = True
    while more_body:
        message = await receive()
        body_chunk = message.get('body', b'')
        more_body = message.get('more_body', False)
        print(f"ASGI Received Chunk: {body_chunk}")
        
        await send({
            'type': 'http.response.body',
            'body': body_chunk,
            'more_body': more_body
        })

async def main():
    loop = asyncio.get_running_loop()
    # Create ASGIServer with echo_app
    server = ASGIServer(echo_app, loop)
    server.start("127.0.0.1", 8080)
    print("ASGI Server running on 8080...")
    await asyncio.Event().wait()

if __name__ == "__main__":
    asyncio.run(main())
