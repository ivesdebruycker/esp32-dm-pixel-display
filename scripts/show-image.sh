#!/bin/sh
# Shows an image on the display without storing it, converting it first if needed.
# Usage: scripts/show-image.sh image [host]
# The host defaults to $PIXEL_DISPLAY_HOST, then pixel-display.local.
set -e

if [ -z "$1" ]; then
  echo "Usage: $0 image [host]" >&2
  exit 1
fi
if [ ! -f "$1" ]; then
  echo "File not found: $1" >&2
  exit 1
fi

image="$1"
host="${2:-${PIXEL_DISPLAY_HOST:-pixel-display.local}}"
scripts="$(cd "$(dirname "$0")" && pwd)"
# Keep in sync with max_png_size in the firmware
max_bytes=3663

# A 32x32 PNG within the limit can be sent as it is
is_ready() {
  case "$1" in
    *.png) ;;
    *) return 1 ;;
  esac
  [ "$(wc -c < "$1")" -le "$max_bytes" ] || return 1
  header="$(od -An -tx1 -N24 "$1" | tr -d ' \n')"
  case "$header" in
    89504e470d0a1a0a????????????????0000002000000020) return 0 ;;
    *) return 1 ;;
  esac
}

if ! is_ready "$image"; then
  temp="$(mktemp -d)"
  trap 'rm -rf "$temp"' EXIT
  "$scripts/prepare-image.sh" "$image" "$temp/image.png"
  image="$temp/image.png"
fi

curl --silent --show-error --fail-with-body -m 30 -F "file=@$image;filename=image.png" "http://$host/show-png"
echo
