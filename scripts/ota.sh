#!/bin/sh
# Uploads dist/firmware.bin over Wi-Fi. Usage: scripts/ota.sh [host]
set -e

cd "$(dirname "$0")/.."

host="${1:-pixel-display.local}"
password="$(sed -n 's/^#define OTA_PASSWORD "\(.*\)"$/\1/p' include/secrets.h)"

curl --fail-with-body -u "admin:$password" -F "firmware=@dist/firmware.bin" "http://$host/update"
echo
