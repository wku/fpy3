#!/usr/bin/env bash
# Проверка поддержки HTTP/3 (QUIC) на localhost:8080
# Запуск: bash check_http3.sh

PORT=8080
HOST="127.0.0.1"
CURL_HTTP3="docker run --rm -it --network=host ymuski/curl-http3 curl"

echo "=== Проверка HTTP/3 на ${HOST}:${PORT} ==="
echo "Дата и время проверки: $(date '+%Y-%m-%d %H:%M:%S')"
echo

echo "1. Сокеты (TCP + UDP)"
echo "----------------------------------------"
sudo ss -tulnp | grep ":${PORT}" || echo "  → ничего не найдено на порту ${PORT}"
echo

echo "2. Кто слушает порт (процесс + PID)"
echo "----------------------------------------"
sudo lsof -iTCP:${PORT} -sTCP:LISTEN -P -n || \
 sudo netstat -tulnp 2>/dev/null | grep ":${PORT}" || \
 echo "  → не удалось определить процесс"
echo

echo "3. Проверка обычного HTTP/1.1 (ожидаем ответ)"
echo "----------------------------------------"
curl -v --http1.1 --connect-timeout 3 "http://${HOST}:${PORT}/" 2>&1 | grep -E "< HTTP/1|curl:|\* "
echo

echo "4. Проверка HTTPS + HTTP/1.1 или HTTP/2 (ожидаем TLS handshake)"
echo "----------------------------------------"
curl -v --http2 --insecure --connect-timeout 4 "https://${HOST}:${PORT}/" 2>&1 | \
  grep -E "HTTP/|ALPN|h2|curl:|error:|refused|WRONG_VERSION"
echo

echo "5. Проверка QUIC / HTTP/3 — только соединение (без --only)"
echo "   (ожидаем QUIC handshake + draining если нет ответа)"
echo "----------------------------------------"
${CURL_HTTP3} -v --http3 --insecure "https://${HOST}:${PORT}/" 2>&1 | \
  grep -E "HTTP/3|using HTTP/3|ALPN|h3|draining|refused|error:"
echo

echo "6. Проверка QUIC / HTTP/3 — строго (ожидаем h3 или ошибку)"
echo "----------------------------------------"
${CURL_HTTP3} -v --http3-only --insecure "https://${HOST}:${PORT}/" 2>&1 | \
  grep -E "HTTP/3|using HTTP/3|ALPN|h3|draining|refused|error:|Closing connection"
echo

echo "7. Краткие выводы"
echo "----------------------------------------"
echo

if sudo ss -uln | grep -q ":${PORT}"; then
  echo "✓ UDP сокет на ${PORT} существует → QUIC возможен"
else
  echo "✗ Нет UDP-сокета → HTTP/3 невозможен"
fi

if sudo ss -tln | grep -q ":${PORT}"; then
  echo "✓ TCP LISTEN на ${PORT} существует"
else
  echo "✗ Нет TCP LISTEN → обычный HTTPS/HTTP/2 тоже невозможен"
fi

if ${CURL_HTTP3} -s --http3-only --insecure --connect-timeout 4 "https://${HOST}:${PORT}/" >/dev/null 2>&1; then
  echo "✓ Удалось установить QUIC-соединение (handshake прошёл)"
else
  echo "✗ QUIC-соединение не устанавливается"
fi

echo
echo "Наиболее вероятная ситуация на основе логов:"
echo "  • QUIC/TLS handshake проходит (UDP + ALPN h3 работает)"
echo "  • Сервер принимает соединение, но НЕ отправляет HTTP/3-ответ"
echo "  • curl получает draining → ошибка 95 (но это не значит, что HTTP/3 не поддерживается)"
echo
echo "Вывод: HTTP/3 частично поддерживается (QUIC-уровень OK),"
echo "       но приложение не реализует полноценную обработку HTTP/3-запросов."
echo
echo "Следующий шаг: найти и исправить код/конфиг сервера, чтобы он"
echo "отправлял хотя бы минимальный ответ (:status: 200 + end_stream)."