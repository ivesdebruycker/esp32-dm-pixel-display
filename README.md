# ESP32 iDotMatrix Pixel Display

Firmware for an ESP32-C3 mounted on the back of an iDotMatrix pixel display. It keeps the display connected over BLE and lets you control it over HTTP on your Wi-Fi network.

```text
HTTP client → Wi-Fi → ESP32-C3 → BLE → iDotMatrix display
```

## Features

- Reconnects to the display on its own after it restarts or drops the link
- HTTP stays available while the display is disconnected
- Stores up to 10 PNGs in flash, replacing the oldest first
- Rejects PNGs over 3,663 bytes, the largest size confirmed working (a 4,196 byte PNG sent as two chunks wasn't shown)
- Remembers the display mode across restarts
- Firmware updates over Wi-Fi

## Setup

Copy `include/secrets.example.h` to `include/secrets.h` and fill it in. `secrets.h` is gitignored.

The display's BLE address is `displayAddress` in `src/esp32-dm-pixel-display-poc.cpp`.

## Build

Needs Docker and nothing else. The toolchain stays in the `pio-cache` and `pio-venv` Docker volumes.

```bash
scripts/build.sh
```

Binaries end up in `dist/`.

## Flash

### First time, over USB

Run this on the Windows machine the board is plugged into. It fetches `dist/` from the build host over SSH and needs the [esptool](https://github.com/espressif/esptool/releases) executable.

```powershell
scripts\flash.ps1 -Esptool C:\path\to\esptool.exe -Port COM5
```

### Afterwards, over Wi-Fi

```bash
scripts/ota.sh pixel-display.local
```

Pass the board's IP address if `.local` names don't resolve, which is common in WSL.

## Serial output

```powershell
scripts\monitor.ps1 -Port COM5 -Seconds 30
```

Opening the port can restart the board.

## HTTP API

Replace `pixel-display.local` with the board's IP address if needed.

| Method and path | Purpose |
|---|---|
| `GET /status` | Connection state, mode, image count, memory and storage |
| `POST /send-png` | Store a PNG and show it, multipart field with a `.png` filename. Doesn't change the mode, in clock mode it's only stored |
| `POST /show-png` | Show a PNG without storing it, same upload and limits. A running slideshow continues after 15 seconds, and the clock stops |
| `POST /brightness` | Set brightness with `value` from 5 to 100, kept across restarts |
| `POST /screen` | Turn the screen on or off with `on` as 1 or 0, kept across restarts |
| `POST /start-clock` | Show the clock, this stops the slideshow |
| `POST /slideshow/start` | Cycle through stored images every 15 seconds |
| `POST /slideshow/stop` | Stop cycling and leave the current content |
| `POST /images/clear` | Remove all stored images |
| `POST /send-raw` | Send a raw command as hex in the `plain` form field |
| `POST /update` | Upload firmware, needs the `admin` user and `OTA_PASSWORD` |

```bash
curl http://pixel-display.local/status
curl -F "file=@mushroom.png" http://pixel-display.local/send-png
curl -X POST http://pixel-display.local/start-clock
curl -X POST http://pixel-display.local/send-raw -d "plain=0500040101"
```

Only `/update` needs a password, so keep the board off untrusted networks.

## Web UI

A page to upload images, with automatic resizing and a preview, to keep an image library and playlists, and to control the display. It runs in Docker and passes requests on to the board.

Playlists have a duration per image (at least 5 seconds) and aren't limited to the board's 10 slots, because the server sends images one at a time and keeps the library in a Docker volume. They resume when the container restarts. When the server is off, the last image stays on screen until the board is told otherwise.

```bash
PIXEL_DISPLAY_HOST=192.168.1.60 docker compose up -d --build
```

Open port 8032 on that machine, set `WEB_PORT` to use another one. It has no login, so only run it on a network you trust. It restarts along with Docker. `docker compose down` removes the container and keeps the library, `docker compose down -v` also deletes the library and playlists.

## Preparing images

Converts any image to a PNG the display can show: crops to a centred square, resizes to 32x32 and reduces colors if needed to fit the size limit. It needs Docker, and the original is left alone.

```bash
scripts/prepare-image.sh photo.jpg
curl -F "file=@photo-32.png" http://pixel-display.local/send-png
```

To show an image right away without storing it, converting it only if needed:

```bash
scripts/show-image.sh photo.jpg 192.168.1.60
```

The host defaults to `$PIXEL_DISPLAY_HOST`, then `pixel-display.local`.

## Based on

- https://github.com/derkalle4/python3-idotmatrix-library
- https://github.com/nj-designs/go-idot
- https://github.com/Kezii/esp32cam_rs
- https://github.com/ivesdebruycker/esp32-r2d2

## References

- https://micropython.org/download/ESP32_GENERIC/
- https://www.flamingo-tech.nl/2021/03/21/installing-and-using-esptools/
- https://docs.micropython.org/en/latest/esp8266/tutorial/intro.html
- https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers?tab=downloads
- https://tttapa.github.io/ESP8266/Chap12%20-%20Uploading%20to%20Server.html
