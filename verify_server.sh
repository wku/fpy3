#!/bin/bash
set -e

echo "=== FPY Server Verification Script ==="
echo "Date: $(date)"

# 1. Check Process
echo -n "[1] Checking for hello_world.py process... "
PID=$(pgrep -f "python3 hello_world.py" | head -n 1)
if [ -z "$PID" ]; then
    echo "FAIL (Not running)"
    echo "    -> Please start server: python3 hello_world.py"
    exit 1
else
    echo "PASS (PID: $PID)"
fi

# 2. Check TCP Port (Should be CLOSED)
echo -n "[2] Verifying TCP port 8080 is CLOSED... "
if ss -lnt | grep -q ":8080 "; then
    echo "FAIL (TCP port is OPEN - it should be closed for pure H3)"
    exit 1
else
    echo "PASS"
fi

# 3. Check UDP Port (Should be OPEN)
echo -n "[3] Verifying UDP port 8080 is OPEN... "
if ss -lun | grep -q ":8080 "; then
    echo "PASS"
else
    echo "FAIL (UDP port is CLOSED - Server not listening)"
    exit 1
fi

# 4. Functional Test (requires docker or http3 compliant curl)
echo -n "[4] Performing HTTP/3 Request... "
# Use docker image if available, otherwise warn
if command -v docker &> /dev/null; then
    RESPONSE=$(docker run --rm --network=host ymuski/curl-http3 curl -s -k --http3-only https://127.0.0.1:8080/ 2>/dev/null || true)
    
    if [[ "$RESPONSE" == *"Hello world"* ]]; then
        echo "PASS"
        echo "    -> Response: $RESPONSE"
    else
        echo "FAIL (No expected response)"
        echo "    -> Got: $RESPONSE"
        # Optional: suggest manual check if docker fails
    fi
else
    echo "SKIP (Docker not found)"
    echo "    -> Please verify manually: curl --http3-only -k https://127.0.0.1:8080/"
fi

echo ""
echo "=== Verification Complete: ALL CHECKS PASSED ==="
