# Docker + Let's Encrypt (Real Domain, Localhost)

This setup allows you to obtain a **real, globally trusted** Let's Encrypt certificate for your local development environment. This eliminates the need to import CA certificates into browsers.

## How it works
1.  You own a domain (e.g., `dev.example.com`).
2.  You set its DNS `A` record to `127.0.0.1`.
3.  **Caddy** uses the **DNS-01 Challenge** to prove ownership of the domain to Let's Encrypt (using your DNS provider's API).
4.  Let's Encrypt issues a valid certificate.
5.  Caddy proxies traffic to your app.

## Prerequisites
1.  **A Domain Name** (e.g., from Cloudflare, GoDaddy, Namecheap).
2.  **DNS Managed by a Supported Provider**. (This example uses **Cloudflare**).
3.  **API Token** from your DNS provider with permission to edit DNS records (Zone:Edit).

## Setup

1.  **DNS Configuration**:
    - Log in to your DNS provider (e.g., Cloudflare).
    - Create an `A` record for your subdomain (e.g., `local`) pointing to `127.0.0.1`.

2.  **Environment Variables**:
    - Create a `.env` file in this directory:
    ```bash
    CLOUDFLARE_API_TOKEN=your_token_here
    ```

3.  **Caddyfile**:
    - Open `Caddyfile` and replace `dev.example.com` with your actual domain (e.g., `local.yourdomain.com`).

4.  **Run**:
    ```bash
    # You might need to generate dummy certs in ../docker_https first for the app to start
    cd ../docker_https && ../../mkcert ... && cd ../docker_letsencrypt
    
    docker-compose up --build
    ```

5.  **Access**:
    - https://local.yourdomain.com
    - It should have a valid green lock immediately.
