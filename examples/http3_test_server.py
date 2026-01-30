# Simple HTTP/3 server using aioquic (official example adapted for quick testing)
import argparse
import asyncio
import logging
import os
import ssl
from typing import cast

from aioquic.asyncio import serve
from aioquic.h0.connection import H0_ALPN
from aioquic.h3.connection import H3_ALPN, H3Connection
from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.events import QuicEvent, StreamDataReceived
from aioquic.tls import SessionTicketHandler

class SessionTicketStore(SessionTicketHandler):
    def __init__(self) -> None:
        self.tickets: list[bytes] = []

    def create(self, ticket: bytes) -> None:
        self.tickets.append(ticket)

    def get(self, label: bytes) -> bytes | None:
        return self.tickets[0] if self.tickets else None

class HttpServerProtocol:
    def __init__(self, *, configuration: QuicConfiguration) -> None:
        self._http: H3Connection | None = None
        self._quic: QuicConfiguration = configuration

    def quic_event_received(self, event: QuicEvent) -> None:
        if isinstance(event, StreamDataReceived):
            # Simple echo-like response for testing
            data = event.data
            if event.stream_id % 4 == 0:  # bidirectional stream
                response = (
                    b"HTTP/3 Test Server\n"
                    b"Protocol: HTTP/3 (QUIC)\n"
                    b"Received: " + data[:200] + b"\n"
                )
                self._http.send_data(event.stream_id, response, end_stream=True)
                self._http.send_headers(
                    event.stream_id,
                    [(b":status", b"200"), (b"server", b"aioquic-test")],
                    end_stream=True,
                )

async def main(
    host: str,
    port: int,
    certificate: str,
    private_key: str,
) -> None:
    configuration = QuicConfiguration(
        is_client=False,
        alpn_protocols=H3_ALPN + H0_ALPN,
        certificate=certificate,
        private_key=private_key,
    )

    ticket_store = SessionTicketStore()
    configuration.session_ticket_handler = ticket_store

    print(f"Starting HTTP/3 server on https://{host}:{port}")
    print("Test command:")
    print(f"  docker run --rm -it --network=host ymuski/curl-http3 curl -vk --http3-only https://{host}:{port}/")

    await serve(
        host,
        port,
        configuration=configuration,
        create_protocol=HttpServerProtocol,
        stream_handler=lambda: None,  # we handle in quic_event_received
    )

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="HTTP/3 server")
    parser.add_argument("--host", type=str, default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8081)
    parser.add_argument("--certificate", type=str, default="cert.pem")
    parser.add_argument("--private-key", type=str, default="key.pem")
    args = parser.parse_args()

    # Generate self-signed cert if missing
    if not os.path.exists(args.certificate):
        print("Generating self-signed certificate for localhost...")
        os.system(
            f"openssl req -x509 -newkey rsa:2048 -nodes -sha256 "
            f"-keyout {args.private_key} -out {args.certificate} -days 3650 "
            f"-subj '/CN=localhost'"
        )

    asyncio.run(
        main(
            host=args.host,
            port=args.port,
            certificate=args.certificate,
            private_key=args.private_key,
        )
    )



"""
pip install --upgrade aioquic

python3 http3_test_server.py

docker run --rm -it --network=host ymuski/curl-http3 \
  curl -vk --http3-only https://127.0.0.1:8081

"""