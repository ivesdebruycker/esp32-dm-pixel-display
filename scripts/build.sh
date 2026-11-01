#!/bin/sh
# Builds the firmware in a throwaway container, the toolchain stays in Docker volumes.
# Binaries end up in dist/. Extra arguments are passed to `pio run`.
set -e

cd "$(dirname "$0")/.."

docker run --rm \
  -v pio-cache:/root/.platformio \
  -v pio-venv:/opt/pio \
  -v "$(pwd)":/src \
  -w /src \
  -e HOST_UID="$(id -u)" \
  -e HOST_GID="$(id -g)" \
  python:3.12-slim \
  sh -c '
    [ -x /opt/pio/venv/bin/pio ] || { python -m venv /opt/pio/venv && /opt/pio/venv/bin/pip install -q platformio; }
    status=0
    /opt/pio/venv/bin/pio run "$@" || status=$?
    if [ $status -eq 0 ]; then
      mkdir -p dist
      cp .pio/build/esp32-c3-devkitm-1/bootloader.bin \
         .pio/build/esp32-c3-devkitm-1/partitions.bin \
         .pio/build/esp32-c3-devkitm-1/firmware.bin \
         /root/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin \
         dist/
    fi
    chown -R "$HOST_UID:$HOST_GID" .pio dist 2>/dev/null || true
    exit $status
  ' -- "$@"
