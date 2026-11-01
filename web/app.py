"""Web UI backend: converts images and passes requests on to the board."""
import asyncio
import io
import os
import time
from contextlib import asynccontextmanager
from pathlib import Path

import httpx
from fastapi import FastAPI, File, HTTPException, UploadFile
from fastapi.responses import FileResponse, Response
from PIL import Image, UnidentifiedImageError
from starlette.concurrency import run_in_threadpool

from prepare_image import MAX_BYTES, SIZE, convert

BOARD_URL = f"http://{os.environ.get('PIXEL_DISPLAY_HOST', 'pixel-display.local')}"
MAX_UPLOAD_BYTES = 20 * 1024 * 1024
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
STATUS_INTERVAL = 10
# one failed check can just be a slow moment on a weak Wi-Fi signal
OFFLINE_AFTER_FAILURES = 2
STATIC_DIR = Path(__file__).parent / "static"

# The board's web server handles one client at a time, so requests take turns
board_lock = asyncio.Lock()
client: httpx.AsyncClient = None
status = {"data": None, "checked_at": None, "failures": OFFLINE_AFTER_FAILURES}


async def board_request(method, path, timeout=30, **kwargs):
    async with board_lock:
        try:
            response = await client.request(method, path, timeout=timeout, **kwargs)
        except httpx.TimeoutException as error:
            raise HTTPException(504, "The board took too long to respond") from error
        except httpx.HTTPError as error:
            raise HTTPException(502, "Could not reach the board") from error

    if response.status_code >= 400:
        raise HTTPException(response.status_code, response.text or "The board returned an error")
    return response


async def refresh_status():
    try:
        response = await board_request("GET", "/status", timeout=8)
        status["data"] = response.json()
        status["checked_at"] = time.time()
        status["failures"] = 0
    except (HTTPException, ValueError):
        status["failures"] += 1


async def poll_status():
    while True:
        # skip a check while the board is busy, it can only handle one request
        if not board_lock.locked():
            await refresh_status()
        await asyncio.sleep(STATUS_INTERVAL)


@asynccontextmanager
async def lifespan(app):
    global client
    client = httpx.AsyncClient(base_url=BOARD_URL)
    task = asyncio.create_task(poll_status())
    yield
    task.cancel()
    await client.aclose()


app = FastAPI(title="Pixel display", lifespan=lifespan, docs_url=None, redoc_url=None)


async def read_upload(file, limit):
    data = await file.read(limit + 1)
    if len(data) > limit:
        raise HTTPException(413, "File too large")
    if not data:
        raise HTTPException(400, "The file is empty")
    return data


def inspect_and_convert(data):
    with Image.open(io.BytesIO(data)) as image:
        original_size = image.size
    return (*convert(io.BytesIO(data)), original_size)


@app.post("/api/convert")
async def convert_image(file: UploadFile = File(...)):
    data = await read_upload(file, MAX_UPLOAD_BYTES)
    try:
        png, colors, original_size = await run_in_threadpool(inspect_and_convert, data)
    except (UnidentifiedImageError, OSError, Image.DecompressionBombError) as error:
        raise HTTPException(400, "This file can't be read as an image") from error
    except ValueError as error:
        raise HTTPException(422, str(error)) from error

    return Response(
        png,
        media_type="image/png",
        headers={
            "Cache-Control": "no-store",
            "X-Image-Bytes": str(len(png)),
            "X-Image-Colors": str(colors or ""),
            "X-Original-Size": f"{original_size[0]}x{original_size[1]}",
        },
    )


async def send_png(path, file):
    data = await read_upload(file, MAX_BYTES)
    if not data.startswith(PNG_SIGNATURE):
        raise HTTPException(400, "File must be a PNG")
    response = await board_request("POST", path, files={"file": ("image.png", data, "image/png")})
    await refresh_status()
    return {"message": response.text}


@app.post("/api/show")
async def show(file: UploadFile = File(...)):
    return await send_png("/show-png", file)


@app.post("/api/save")
async def save(file: UploadFile = File(...)):
    return await send_png("/send-png", file)


async def command(path):
    response = await board_request("POST", path)
    await refresh_status()
    return {"message": response.text}


@app.post("/api/slideshow/start")
async def slideshow_start():
    return await command("/slideshow/start")


@app.post("/api/slideshow/stop")
async def slideshow_stop():
    return await command("/slideshow/stop")


@app.post("/api/clock")
async def clock():
    return await command("/start-clock")


@app.post("/api/images/clear")
async def clear_images():
    return await command("/images/clear")


@app.get("/api/status")
async def get_status():
    checked_at = status["checked_at"]
    return {
        "online": status["failures"] < OFFLINE_AFTER_FAILURES and status["data"] is not None,
        "seconds_since_check": None if checked_at is None else round(time.time() - checked_at),
        "board": status["data"],
    }


@app.get("/api/limits")
async def limits():
    return {"size": SIZE[0], "max_png_bytes": MAX_BYTES, "max_upload_bytes": MAX_UPLOAD_BYTES}


@app.get("/")
async def index():
    return FileResponse(STATIC_DIR / "index.html")
