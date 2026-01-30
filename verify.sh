#!/bin/bash
rm -f server.log curl.log
python3.12 hello_world.py > server.log 2>&1 &
PID=$!
sleep 3
curl -v http://127.0.0.1:8080/ > curl.log 2>&1
kill $PID
echo "=== SERVER LOG ==="
cat server.log
echo "=== CURL LOG ==="
cat curl.log
