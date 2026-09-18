#!/usr/bin/env bash

set -euo pipefail

SOURCE="/opt/mammotion-rc/web-server"
APP="/data/mammotion-rc"

echo "=========================================="
echo " Mammotion Remote - Home Assistant"
echo "=========================================="

mkdir -p "$APP"

echo "[1/3] Updating mammotion-rc files..."
cp -a "$SOURCE/." "$APP/"

cd "$APP"

echo "[2/3] Checking HTTPS certificate..."

if [ ! -f cert.pem ] || [ ! -f key.pem ]; then
    echo "Generating HTTPS certificate..."
    python gen_cert.py \
        --cert cert.pem \
        --key key.pem \
        --host localhost
fi

echo "[3/3] Starting server..."
echo "Web interface: https://<home-assistant-host>:8443/"

exec python -m uvicorn app:app \
    --host 0.0.0.0 \
    --port 8443 \
    --ssl-keyfile key.pem \
    --ssl-certfile cert.pem \
    --timeout-graceful-shutdown 2
