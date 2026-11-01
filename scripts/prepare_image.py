"""Converts an image to a 32x32 PNG the display can show."""
import io
import os
import sys

from PIL import Image, ImageEnhance, ImageOps

SIZE = (32, 32)
# Keep in sync with max_png_size in the firmware
MAX_BYTES = 3663


def encode(image):
    buffer = io.BytesIO()
    image.convert("RGBA").save(buffer, "PNG", optimize=True)
    return buffer.getvalue()


def crop_square(image, zoom, x, y, resampling):
    """Crops a square around the point x, y (0 to 1), keeping it inside the image."""
    width, height = image.size
    side = min(width, height) / zoom
    left = min(max(x * width - side / 2, 0), width - side)
    top = min(max(y * height - side / 2, 0), height - side)
    return image.resize(SIZE, resampling, box=(left, top, left + side, top + side))


def fit_with_bars(image, background, resampling):
    fitted = ImageOps.contain(image, SIZE, resampling)
    canvas = Image.new("RGB", SIZE, background)
    canvas.paste(fitted, ((SIZE[0] - fitted.width) // 2, (SIZE[1] - fitted.height) // 2))
    return canvas


def convert(
    source, mode="crop", zoom=1, x=0.5, y=0.5, background="#000000",
    contrast=1, saturation=1, resize="smooth",
):
    """Returns the PNG bytes and the color count if colors had to be reduced.

    source is a path or a binary file object. mode is "crop", which uses zoom,
    x and y, or "fit", which adds bars in the background color. contrast and
    saturation are factors where 1 is unchanged. resize is "smooth", or "pixel"
    which keeps hard edges and the original colors. Raises ValueError if the
    image can't be made small enough.
    """
    resampling = Image.Resampling.NEAREST if resize == "pixel" else Image.Resampling.LANCZOS

    with Image.open(source) as original:
        image = ImageOps.exif_transpose(original).convert("RGBA")

    # the display has no transparency, so flatten it
    flatten = background if mode == "fit" else "#000000"
    image = Image.alpha_composite(Image.new("RGBA", image.size, flatten), image).convert("RGB")
    if mode == "fit":
        image = fit_with_bars(image, background, resampling)
    else:
        image = crop_square(image, zoom, x, y, resampling)

    # applied to the final pixels, so they match what the display shows
    if contrast != 1:
        image = ImageEnhance.Contrast(image).enhance(contrast)
    if saturation != 1:
        image = ImageEnhance.Color(image).enhance(saturation)

    data = encode(image)
    colors = None
    for count in (256, 192, 128, 96, 64, 48, 32, 24, 16):
        if len(data) <= MAX_BYTES:
            break
        colors = count
        data = encode(image.quantize(colors=count, dither=Image.Dither.NONE))

    if len(data) > MAX_BYTES:
        raise ValueError(f"Could not get the image under {MAX_BYTES} bytes")
    return data, colors


def main(source, target):
    try:
        data, colors = convert(source)
    except ValueError as error:
        sys.exit(f"{source}: {error}")

    with open(target, "wb") as file:
        file.write(data)

    detail = f", reduced to {colors} colors" if colors else ""
    print(f"{os.path.basename(target)}: {SIZE[0]}x{SIZE[1]}, {len(data)} bytes{detail}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
