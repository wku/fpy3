# Вариант 1 — заставляем использовать только HTTP/3
curl --http3-only -v --connect-timeout 3 http://127.0.0.1:8080

# Вариант 2 — более современный флаг (curl ≥ 7.88–8.0+)
curl --http3 -v --http3-only http://127.0.0.1:8080

Вариант А — Docker (самый быстрый и чистый)


docker run --rm -it ymuski/curl-http3 \
  curl --http3-only -v http://host.docker.internal:8080
