#!/bin/sh
# Converts an image to a PNG the display can show, in a throwaway container.
# Usage: scripts/prepare-image.sh input.jpg [output.png]
set -e

if [ -z "$1" ]; then
  echo "Usage: $0 input [output.png]" >&2
  exit 1
fi

input="$(realpath "$1")"
output="$(realpath -m "${2:-${1%.*}-32.png}")"
if [ "$input" = "$output" ]; then
  echo "Output would overwrite the input, pass another output path" >&2
  exit 1
fi
scripts="$(cd "$(dirname "$0")" && pwd)"

docker run --rm \
  -v image-tools:/opt/tools \
  -v "$scripts":/scripts:ro \
  -v "$(dirname "$input")":/in:ro \
  -v "$(dirname "$output")":/out \
  -u "$(id -u):$(id -g)" \
  -e HOME=/tmp \
  python:3.12-slim \
  sh -c '
    [ -x /opt/tools/venv/bin/python ] || { python -m venv /opt/tools/venv && /opt/tools/venv/bin/pip install -q pillow; }
    /opt/tools/venv/bin/python /scripts/prepare_image.py "$@"
  ' -- "/in/$(basename "$input")" "/out/$(basename "$output")"
