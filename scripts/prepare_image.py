"""Converts an image to a 32x32 PNG the display can show."""
import io
import os
import sys

from PIL import Image, ImageOps

SIZE = (32, 32)
# Keep in sync with max_png_size in the firmware
MAX_BYTES = 2923


def encode(image):
    buffer = io.BytesIO()
    image.convert("RGBA").save(buffer, "PNG", optimize=True)
    return buffer.getvalue()


def main(source, target):
    with Image.open(source) as original:
        image = ImageOps.exif_transpose(original).convert("RGBA")

    # the display has no transparency, so flatten onto black
    background = Image.new("RGBA", image.size, (0, 0, 0, 255))
    image = Image.alpha_composite(background, image).convert("RGB")
    image = ImageOps.fit(image, SIZE, Image.Resampling.LANCZOS)

    data = encode(image)
    colors = None
    for count in (256, 192, 128, 96, 64, 48, 32, 24, 16):
        if len(data) <= MAX_BYTES:
            break
        colors = count
        data = encode(image.quantize(colors=count, dither=Image.Dither.NONE))

    if len(data) > MAX_BYTES:
        sys.exit(f"Could not get {source} under {MAX_BYTES} bytes")

    with open(target, "wb") as file:
        file.write(data)

    detail = f", reduced to {colors} colors" if colors else ""
    print(f"{os.path.basename(target)}: {SIZE[0]}x{SIZE[1]}, {len(data)} bytes{detail}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
