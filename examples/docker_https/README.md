# Docker HTTPS/QUIC Local Development

This example shows how to run the FPY server in Docker with valid HTTPS/HTTP/3 certificates.

## Prerequisites

1.  **Docker** & **Docker Compose**.
2.  **mkcert** (installed on your host machine to generate valid certs).

## Setup

### 1. Generate Certificates
Run the following command IN THIS DIRECTORY (`examples/docker_https`) to generate valid certificates for localhost:

```bash
# Verify you are in examples/docker_https
../../mkcert -key-file key.pem -cert-file cert.pem localhost 127.0.0.1 ::1
```

> **Note**: `../../mkcert` refers to the binary in the project root. If you have `mkcert` installed globally, you can just use `mkcert`.

### 2. Import Root CA (One-Time Setup)
For the browser to trust these certificates (and enable HTTP/3), you MUST import the `mkcert` Root CA into your browser's "Authorities" store.

1.  Find the CA location: `../../mkcert -CAROOT`
2.  **Chrome**: `chrome://settings/certificates` -> **Authorities** -> Import -> Check all trust boxes.
3.  **Firefox**: Settings -> CA Certificates -> Import.

### 3. Run
```bash
docker-compose up --build
```

The server will be available at:
- **HTTPS/TCP**: `https://127.0.0.1:8080` (Check devtools for Alt-Svc)
- **HTTP/3 (QUIC)**: `UDP 8080` (Browser will upgrade automatically if cert is trusted)
