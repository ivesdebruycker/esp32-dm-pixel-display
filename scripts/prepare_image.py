"""Converts an image to a 32x32 PNG, or an animation to a 32x32 GIF, the display can show."""
import io
import math
import os
import sys
from typing import NamedTuple

from PIL import Image, ImageEnhance, ImageOps, ImageSequence

SIZE = (32, 32)
# Keep in sync with max_png_size in the firmware
MAX_BYTES = 3663
# Keep in sync with max_gif_size in the firmware
MAX_BOARD_GIF_BYTES = 11318
# The display keeps every GIF it receives and plays its own copy of one it has seen,
# even a broken one, so each send gets a random comment that makes it new
NONCE_BYTES = 8
MAX_GIF_BYTES = MAX_BOARD_GIF_BYTES - (NONCE_BYTES + 4)
GIF_COLORS = (64, 48, 32, 24, 16, 12, 8)
# GIF players treat frames without a delay as 100 ms
DEFAULT_FRAME_MS = 100
MIN_FRAME_MS = 20
# more frames than this never fit, so they're dropped before the first try
MAX_START_FRAMES = 48
# read at most this many, longer animations are cut off
MAX_SOURCE_FRAMES = 1000


class Converted(NamedTuple):
    data: bytes
    # the color count if colors had to be reduced, a GIF always has a reduced count
    colors: int | None
    # frames in the result, None for a still image
    frames: int | None
    source_frames: int

    @property
    def is_gif(self):
        return self.frames is not None


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


def prepare(image, mode, zoom, x, y, background, contrast, saturation, resampling):
    """Turns one image or frame into the 32x32 RGB pixels the display shows."""
    image = image.convert("RGBA")
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
    return image


def encode_png(image, source_frames):
    data = encode(image)
    colors = None
    for count in (256, 192, 128, 96, 64, 48, 32, 24, 16):
        if len(data) <= MAX_BYTES:
            break
        colors = count
        data = encode(image.quantize(colors=count, dither=Image.Dither.NONE))

    if len(data) > MAX_BYTES:
        raise ValueError(f"Could not get the image under {MAX_BYTES} bytes")
    return Converted(data, colors, None, source_frames)


def encode_gif(frames, durations, colors):
    # one palette for every frame keeps colors from flickering and the file small
    strip = Image.new("RGB", (SIZE[0], SIZE[1] * len(frames)))
    for index, frame in enumerate(frames):
        strip.paste(frame, (0, SIZE[1] * index))
    palette = strip.quantize(colors=colors, dither=Image.Dither.NONE)
    quantized = [frame.quantize(palette=palette, dither=Image.Dither.NONE) for frame in frames]

    buffer = io.BytesIO()
    quantized[0].save(
        buffer, "GIF", save_all=True, append_images=quantized[1:], duration=durations, loop=0
    )
    return buffer.getvalue()


def drop_frames(frames, durations, step):
    """Keeps every step-th frame, each one lasting as long as the frames it replaces."""
    kept = frames[::step]
    times = [sum(durations[start:start + step]) for start in range(0, len(frames), step)]
    return kept, times


def encode_animation(frames, durations):
    """Fits the frames under MAX_GIF_BYTES, using fewer colors first and then fewer frames."""
    step = max(1, math.ceil(len(frames) / MAX_START_FRAMES))
    while True:
        kept, times = drop_frames(frames, durations, step)
        if len(kept) < 2:
            raise ValueError(f"Could not get the animation under {MAX_GIF_BYTES} bytes")
        for count in GIF_COLORS:
            data = encode_gif(kept, times, count)
            if len(data) <= MAX_GIF_BYTES:
                return Converted(data, count, len(kept), len(frames))
        step += 1


def unique_gif(data):
    """Adds a random comment before the trailer, see NONCE_BYTES."""
    nonce = os.urandom(NONCE_BYTES)
    return data[:-1] + b"\x21\xfe" + bytes([NONCE_BYTES]) + nonce + b"\x00\x3b"


def convert(
    source, mode="crop", zoom=1, x=0.5, y=0.5, background="#000000",
    contrast=1, saturation=1, resize="smooth", animate=True,
):
    """Returns a Converted with the PNG or GIF bytes.

    source is a path or a binary file object. mode is "crop", which uses zoom,
    x and y, or "fit", which adds bars in the background color. contrast and
    saturation are factors where 1 is unchanged. resize is "smooth", or "pixel"
    which keeps hard edges and the original colors. An animation becomes a GIF
    with its own timing, unless animate is false, then its first frame is used.
    Raises ValueError if the image can't be made small enough.
    """
    resampling = Image.Resampling.NEAREST if resize == "pixel" else Image.Resampling.LANCZOS
    settings = (mode, zoom, x, y, background, contrast, saturation, resampling)

    with Image.open(source) as original:
        source_frames = getattr(original, "n_frames", 1)
        if source_frames < 2 or not animate:
            image = prepare(ImageOps.exif_transpose(original), *settings)
            return encode_png(image, source_frames)

        frames, durations = [], []
        for frame in ImageSequence.Iterator(original):
            if len(frames) == MAX_SOURCE_FRAMES:
                break
            duration = frame.info.get("duration") or DEFAULT_FRAME_MS
            durations.append(max(round(duration), MIN_FRAME_MS))
            # made small right away, so a long animation doesn't fill the memory
            frames.append(prepare(frame, *settings))

    return encode_animation(frames, durations)


def main(source, target):
    # the scripts using this send PNGs, so an animation becomes its first frame
    try:
        result = convert(source, animate=False)
    except ValueError as error:
        sys.exit(f"{source}: {error}")

    with open(target, "wb") as file:
        file.write(result.data)

    detail = f", reduced to {result.colors} colors" if result.colors else ""
    print(f"{os.path.basename(target)}: {SIZE[0]}x{SIZE[1]}, {len(result.data)} bytes{detail}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
