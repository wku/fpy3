# Traefik + nip.io + mkcert (Local, Valid HTTPS)

This setup mirrors a production-like architecture where **Traefik** terminates SSL/HTTP/3 and proxies traffic to the backend. It uses `nip.io` to provide a real domain name for localhost.

## Architecture
1.  **Traefik**: Listens on 443 (TCP/UDP). Handles SSL termination and HTTP/3.
2.  **mkcert**: Provides trusted certificates for `*.127.0.0.1.nip.io`.
3.  **nip.io**: DNS service that resolves `anything.127.0.0.1.nip.io` to `127.0.0.1`.

## Setup

### 1. Generate Wildcard Certificate
Generate a certificate for the nip.io wildcard domain (and localhost just in case):

```bash
cd examples/docker_traefik_nip
../../mkcert -key-file key.pem -cert-file cert.pem "*.127.0.0.1.nip.io" localhost 127.0.0.1
```

### 2. Trust CA (If not already done)
Ensure your browser trusts the `mkcert` Root CA (see previous instructions).

### 3. Run
```bash
docker-compose up --build
```

### 4. Access
- **https://app.127.0.0.1.nip.io**
- Browser will see "Valid Certificate" (issued by mkcert).
- Traefik will handle HTTP/3 negotiation.
