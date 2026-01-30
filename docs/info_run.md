
Остановите все контейнеры:

docker-compose down


Запустите сервер напрямую (без Docker/Traefik):

cd ~/WKU/new_src/korysnyk/fpy
export LD_LIBRARY_PATH=$(pwd)/vendor/dist/lib
python3.12 hello_world.py --debug --host 0.0.0.0 --port 8080


Закройте ВСЕ окна Chrome полностью (включая фоновые процессы):

pkill -9 chrome

Запустите Chrome с флагом принудительного QUIC:

google-chrome --origin-to-force-quic-on=127.0.0.1:8080 https://127.0.0.1:8080

Проверьте протокол: В DevTools (F12) -> Network -> выберите запрос -> колонка "Protocol" должна показывать h3.



curl -I -k https://app.127.0.0.1.nip.io

curl -I -k https://127.0.0.1:8080

sudo tcpdump -i lo udp port 8080 -v


fuser -k 8080/tcp


google-chrome --origin-to-force-quic-on=127.0.0.1:8080 https://127.0.0.1:8080




+
pip install --force-reinstall .
export LD_LIBRARY_PATH=$(pwd)/vendor/dist/lib
python3.12 hello_world.py --debug --host 0.0.0.0 --port 8080





