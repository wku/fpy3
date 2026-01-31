# FPY: High-Performance HTTP/3 Server

`fpy3` - высокопроизводительный асинхронный HTTP/3 (QUIC) сервер для Python 3.12+, построенный на `MsQuic`, `nghttp3` и `uvloop`.

## Возможности
- **HTTP/3 и QUIC** - нативная поддержка через `MsQuic` и `nghttp3`
- **HTTP/1.1 Fallback** - автоматическая поддержка старых браузеров
- **Alt-Svc Discovery** - автоматический апгрейд на HTTP/3
- **ASGI 3.0** - полная совместимость через `ASGIServer`
- **Высокая производительность** - критические пути реализованы на C

## Установка

### Требования
- Python 3.12+
- GCC/Clang, CMake, Ninja, Autotools, pkg-config
- OpenSSL dev headers

### Локальная сборка

```bash
# 1. Сборка зависимостей (MsQuic, nghttp3)
./scripts/build_deps.sh

# 2. Установка пакета
pip install .
```

Библиотеки `libmsquic.so.2` и `libnghttp3.so.9` бандлятся в пакет автоматически.

### Docker

```bash
cd examples/docker_https
# Сгенерируйте сертификаты (см. раздел "Сертификаты")
docker-compose up --build
```

## Сертификаты

Для HTTPS/QUIC требуются TLS-сертификаты.

### Локальная разработка (mkcert)

```bash
# Установка mkcert (Ubuntu/Debian)
sudo apt install libnss3-tools
curl -JLO "https://dl.filippo.io/mkcert/latest?for=linux/amd64"
chmod +x mkcert-v*-linux-amd64
sudo mv mkcert-v*-linux-amd64 /usr/local/bin/mkcert

# Создание локального CA
mkcert -install

# Генерация сертификатов
mkcert -key-file key.pem -cert-file cert.pem localhost 127.0.0.1 ::1
```

### Продакшн (Let's Encrypt)

Используйте certbot или ACME-клиент для получения реальных сертификатов.

## Использование

### Быстрый старт (ASGI)

Файл `hello_world.py`:

```python
import asyncio
import uvloop
from fpy3.asgi import ASGIServer

async def app(scope, receive, send):
    if scope['type'] == 'http':
        body = f"Hello World from FPY over HTTP/{scope['http_version']}!".encode()
        await send({
            'type': 'http.response.start',
            'status': 200,
            'headers': [(b'content-type', b'text/plain'), (b'server', b'fpy3-asgi')]
        })
        await send({
            'type': 'http.response.body',
            'body': body
        })

async def main():
    server = ASGIServer(app, debug=True)
    server.start("0.0.0.0", 8080)
    await asyncio.Event().wait()

if __name__ == "__main__":
    uvloop.install()
    asyncio.run(main())
```

### Запуск

```bash
export LD_LIBRARY_PATH=$(pwd)/vendor/dist/lib
python3.12 hello_world.py --debug --host 0.0.0.0 --port 8080
```

## Тестирование

### HTTP/1.1 (curl)

```bash
# Проверка контента
curl -k https://127.0.0.1:8080/

# Проверка заголовков (включая Alt-Svc)
curl -k -I https://127.0.0.1:8080/
```

Ожидаемый вывод:
```
HTTP/1.1 200 OK
Alt-Svc: h3=":8080"; ma=3600
Content-Length: 40
content-type: text/plain
```

### HTTP/3 (Python клиент)

```bash
export LD_LIBRARY_PATH=$(pwd)/vendor/dist/lib
python3.12 test_http3_client.py
```

### HTTP/3 (Chrome с принудительным QUIC)

```bash
# Закройте все окна Chrome
pkill -9 chrome

# Запустите с флагом
google-chrome \
  --user-data-dir=/tmp/chrome_quic_test \
  --origin-to-force-quic-on=127.0.0.1:8080 \
  --ignore-certificate-errors \
  https://127.0.0.1:8080
```

В DevTools (F12) -> Network -> колонка "Protocol" должна показывать `h3`.

### HTTP/3 (Chrome с Alt-Svc discovery)

Для работы без флагов браузер должен доверять сертификату:

1. Найдите путь к Root CA:
   ```bash
   mkcert -CAROOT
   ```

2. Импортируйте в Chrome:
   - `chrome://settings/certificates` -> Authorities -> Import
   - Выберите `rootCA.pem` из пути выше
   - Включите "Trust for websites"

3. Перезапустите Chrome и откройте `https://127.0.0.1:8080/`

## Docker примеры

### Простой HTTPS (docker_https)

```bash
cd examples/docker_https

# Сгенерируйте сертификаты
mkcert -key-file key.pem -cert-file cert.pem localhost 127.0.0.1

docker-compose up --build
```

### С Traefik (docker_traefik_nip)

```bash
cd examples/docker_traefik_nip

# Сгенерируйте wildcard сертификат
mkcert -key-file key.pem -cert-file cert.pem "*.127.0.0.1.nip.io" localhost 127.0.0.1

docker-compose up --build
```

Откройте `https://app.127.0.0.1.nip.io/`

## Архитектура

```
                    +------------------+
Client (HTTP/1.1) ->|  TCP Listener    |-> ASGI App -> HTTP/1.1 Response + Alt-Svc
                    |  (asyncio ssl)   |
                    +------------------+
                           |
                           v (Alt-Svc upgrade)
                    +------------------+
Client (HTTP/3)   ->|  QUIC Listener   |-> ASGI App -> HTTP/3 Response
                    |  (MsQuic)        |
                    +------------------+
```

## Решение проблем

### ImportError: libmsquic.so.2

```bash
# Убедитесь что зависимости собраны
./scripts/build_deps.sh

# Переустановите пакет
pip install --force-reinstall .
```

### ERR_QUIC_PROTOCOL_ERROR в браузере

Браузер не доверяет сертификату. Импортируйте Root CA в браузер (см. раздел "HTTP/3 Chrome с Alt-Svc discovery").

### Порт уже занят

```bash
pkill -f hello_world.py
```

## API Reference

### ASGIServer

```python
from fpy3.asgi import ASGIServer

server = ASGIServer(app, loop=None, debug=False)
server.start(host, port)
```

- `app` - ASGI 3.0 callable
- `loop` - asyncio event loop (опционально)
- `debug` - включить отладочные логи

### ASGI Scope

```python
{
    'type': 'http',
    'asgi': {'version': '3.0', 'spec_version': '2.3'},
    'http_version': '3' | '1.1',
    'server': (host, port),
    'client': (host, port),
    'scheme': 'https',
    'method': 'GET' | 'POST' | ...,
    'path': '/...',
    'query_string': b'...',
    'headers': [(name, value), ...]
}
```