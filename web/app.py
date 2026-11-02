"""Web UI backend: converts images, runs playlists and passes requests on to the board."""
import asyncio
import io
import json
import logging
import os
import sqlite3
import time
import uuid
from contextlib import asynccontextmanager
from pathlib import Path
from typing import Literal

import httpx
from fastapi import Depends, FastAPI, File, Form, HTTPException, UploadFile
from fastapi.responses import FileResponse, Response
from PIL import Image, ImageOps, UnidentifiedImageError
from pydantic import BaseModel, Field
from starlette.concurrency import run_in_threadpool

from prepare_image import MAX_BYTES, SIZE, convert

BOARD_URL = f"http://{os.environ.get('PIXEL_DISPLAY_HOST', 'pixel-display.local')}"
DATA_DIR = Path(os.environ.get("DATA_DIR", "/data"))
IMAGE_DIR = DATA_DIR / "images"
DB_PATH = DATA_DIR / "pixel-display.db"
MAX_UPLOAD_BYTES = 20 * 1024 * 1024
MAX_ZOOM = 8
MAX_ENHANCE = 2
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
STATUS_INTERVAL = 10
# one failed check can just be a slow moment on a weak Wi-Fi signal
OFFLINE_AFTER_FAILURES = 2
# sends can take a second or more on a weak Wi-Fi signal
MIN_DURATION = 5
MAX_DURATION = 24 * 60 * 60
DEFAULT_DURATION = 15
RETRY_SECONDS = 5
CONNECT_TIMEOUT = 5
# worth retrying the same image: the board or the display is away
RETRY_STATUS_CODES = {502, 503, 504}
STATIC_DIR = Path(__file__).parent / "static"

log = logging.getLogger("uvicorn.error")

# The board's web server handles one client at a time, so requests take turns
board_lock = asyncio.Lock()
client: httpx.AsyncClient = None
status = {"data": None, "checked_at": None, "failures": OFFLINE_AFTER_FAILURES}

player = {
    "active": False,
    "generation": 0,
    "playlist_id": None,
    "index": 0,
    "state": "stopped",
    "message": "",
    "image_id": None,
    "next_change_at": None,
    "needs_slideshow_stop": False,
}
player_wake = asyncio.Event()


class PlayerStopped(Exception):
    pass


def db():
    connection = sqlite3.connect(DB_PATH)
    connection.row_factory = sqlite3.Row
    connection.execute("PRAGMA foreign_keys = ON")
    return connection


def init_storage():
    IMAGE_DIR.mkdir(parents=True, exist_ok=True)
    with db() as connection:
        connection.executescript(
            """
            CREATE TABLE IF NOT EXISTS images (
                id TEXT PRIMARY KEY,
                name TEXT NOT NULL,
                bytes INTEGER NOT NULL,
                created_at REAL NOT NULL
            );
            CREATE TABLE IF NOT EXISTS playlists (
                id INTEGER PRIMARY KEY,
                name TEXT NOT NULL UNIQUE
            );
            CREATE TABLE IF NOT EXISTS playlist_items (
                playlist_id INTEGER NOT NULL REFERENCES playlists(id) ON DELETE CASCADE,
                position INTEGER NOT NULL,
                image_id TEXT NOT NULL REFERENCES images(id) ON DELETE CASCADE,
                duration INTEGER NOT NULL,
                PRIMARY KEY (playlist_id, position)
            );
            CREATE TABLE IF NOT EXISTS player_state (
                id INTEGER PRIMARY KEY CHECK (id = 1),
                playlist_id INTEGER,
                position INTEGER NOT NULL
            );
            """
        )
        # images added before crop and fit settings existed have none
        columns = [row["name"] for row in connection.execute("PRAGMA table_info(images)")]
        if "settings" not in columns:
            connection.execute("ALTER TABLE images ADD COLUMN settings TEXT")


async def board_request(method, path, timeout=30, still_wanted=None, **kwargs):
    async with board_lock:
        if still_wanted is not None and not still_wanted():
            raise PlayerStopped()
        try:
            # give up quickly on an unreachable board, other requests are waiting
            response = await client.request(
                method, path, timeout=httpx.Timeout(timeout, connect=CONNECT_TIMEOUT), **kwargs
            )
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


def save_player_state():
    playlist_id = player["playlist_id"] if player["active"] else None
    with db() as connection:
        connection.execute(
            "INSERT INTO player_state (id, playlist_id, position) VALUES (1, ?, ?) "
            "ON CONFLICT (id) DO UPDATE SET playlist_id = excluded.playlist_id, position = excluded.position",
            (playlist_id, player["index"]),
        )


def start_player(playlist_id, index=0):
    player.update(
        active=True,
        generation=player["generation"] + 1,
        playlist_id=playlist_id,
        index=index,
        state="starting",
        message="",
        image_id=None,
        next_change_at=None,
        needs_slideshow_stop=True,
    )
    save_player_state()
    player_wake.set()


def stop_player(message=""):
    player.update(
        active=False,
        generation=player["generation"] + 1,
        state="stopped",
        message=message,
        image_id=None,
        next_change_at=None,
    )
    save_player_state()
    player_wake.set()


async def wait_for_wake(seconds):
    """Waits for the given time, or until the player is started or stopped."""
    try:
        await asyncio.wait_for(player_wake.wait(), seconds)
    except asyncio.TimeoutError:
        pass
    player_wake.clear()


def playlist_items(playlist_id):
    with db() as connection:
        return connection.execute(
            "SELECT items.image_id, items.duration, images.name FROM playlist_items items "
            "JOIN images ON images.id = items.image_id "
            "WHERE items.playlist_id = ? ORDER BY items.position",
            (playlist_id,),
        ).fetchall()


async def play_next():
    # a wake from before this image, like a skip during a send, must not end its turn
    player_wake.clear()
    generation = player["generation"]

    def still_wanted():
        return player["generation"] == generation

    # read every time, so edits to a playing playlist apply from the next image
    items = playlist_items(player["playlist_id"])
    if not items:
        stop_player("The playlist has no images")
        return

    index = player["index"] % len(items)
    item = items[index]
    player.update(index=index, image_id=item["image_id"])

    # an image turns the screen back on, so wait while it's off and keep the place
    board = status["data"]
    if board and board.get("screen_on") is False:
        player.update(state="paused", message="The screen is off", next_change_at=None)
        await wait_for_wake(RETRY_SECONDS)
        return

    try:
        if player["needs_slideshow_stop"]:
            await board_request("POST", "/slideshow/stop", still_wanted=still_wanted)
            player["needs_slideshow_stop"] = False
        data = (IMAGE_DIR / f"{item['image_id']}.png").read_bytes()
        await board_request(
            "POST",
            "/show-png",
            still_wanted=still_wanted,
            files={"file": ("image.png", data, "image/png")},
        )
    except PlayerStopped:
        return
    except HTTPException as error:
        if not still_wanted():
            return
        player.update(state="waiting", message=str(error.detail), next_change_at=None)
        await wait_for_wake(RETRY_SECONDS)
        # the same image is retried, unless the board rejected this image itself
        if still_wanted() and error.status_code not in RETRY_STATUS_CODES:
            player["index"] = index + 1
        return
    except OSError as error:
        log.error("Could not read image %s: %s", item["image_id"], error)
        player.update(state="waiting", message=f"Could not read {item['name']}", next_change_at=None)
        await wait_for_wake(RETRY_SECONDS)
        if still_wanted():
            player["index"] = index + 1
        return

    # it can have been stopped or restarted during the send
    if not still_wanted():
        return

    # each image's time starts once it has been sent
    player.update(state="playing", message="", next_change_at=time.time() + item["duration"])
    save_player_state()
    await wait_for_wake(item["duration"])
    if still_wanted():
        player["index"] = index + 1


async def run_player():
    while True:
        if not player["active"]:
            await player_wake.wait()
            player_wake.clear()
            continue
        try:
            await play_next()
        except Exception:
            log.exception("Playlist player failed")
            player.update(state="waiting", message="Something went wrong, retrying", next_change_at=None)
            await wait_for_wake(RETRY_SECONDS)


def resume_player():
    with db() as connection:
        row = connection.execute(
            "SELECT state.playlist_id, state.position FROM player_state state "
            "JOIN playlists ON playlists.id = state.playlist_id"
        ).fetchone()
    if row:
        start_player(row["playlist_id"], row["position"])


@asynccontextmanager
async def lifespan(app):
    global client
    init_storage()
    client = httpx.AsyncClient(base_url=BOARD_URL)
    resume_player()
    tasks = [asyncio.create_task(poll_status()), asyncio.create_task(run_player())]
    yield
    for task in tasks:
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


def adjustments(
    mode: Literal["crop", "fit"] = Form("crop"),
    zoom: float = Form(1, ge=1, le=MAX_ZOOM),
    x: float = Form(0.5, ge=0, le=1),
    y: float = Form(0.5, ge=0, le=1),
    background: str = Form("#000000", pattern=r"^#[0-9a-fA-F]{6}$"),
    contrast: float = Form(1, ge=0, le=MAX_ENHANCE),
    saturation: float = Form(1, ge=0, le=MAX_ENHANCE),
    resize: Literal["smooth", "pixel"] = Form("smooth"),
):
    return {
        "mode": mode,
        "zoom": zoom,
        "x": x,
        "y": y,
        "background": background,
        "contrast": contrast,
        "saturation": saturation,
        "resize": resize,
    }


def inspect_and_convert(data, settings):
    with Image.open(io.BytesIO(data)) as image:
        original_size = ImageOps.exif_transpose(image).size
    return (*convert(io.BytesIO(data), **settings), original_size)


async def convert_upload(data, settings):
    try:
        return await run_in_threadpool(inspect_and_convert, data, settings)
    except (UnidentifiedImageError, OSError, Image.DecompressionBombError) as error:
        raise HTTPException(400, "This file can't be read as an image") from error
    except ValueError as error:
        raise HTTPException(422, str(error)) from error


@app.post("/api/convert")
async def convert_image(file: UploadFile = File(...), settings: dict = Depends(adjustments)):
    data = await read_upload(file, MAX_UPLOAD_BYTES)
    png, colors, original_size = await convert_upload(data, settings)
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
    stop_player()
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
    stop_player()
    return await command("/slideshow/start")


@app.post("/api/slideshow/stop")
async def slideshow_stop():
    return await command("/slideshow/stop")


@app.post("/api/clock")
async def clock():
    stop_player()
    return await command("/start-clock")


class Brightness(BaseModel):
    value: int = Field(ge=5, le=100)


class Screen(BaseModel):
    on: bool


@app.post("/api/brightness")
async def brightness(body: Brightness):
    response = await board_request("POST", "/brightness", data={"value": body.value})
    await refresh_status()
    return {"message": response.text}


@app.post("/api/screen")
async def screen(body: Screen):
    response = await board_request("POST", "/screen", data={"on": "1" if body.on else "0"})
    await refresh_status()
    return {"message": response.text}


@app.post("/api/images/clear")
async def clear_images():
    return await command("/images/clear")


def image_row_to_dict(row):
    return {
        "id": row["id"],
        "name": row["name"],
        "bytes": row["bytes"],
        "used_in": row["used_in"],
        "settings": json.loads(row["settings"]) if row["settings"] else None,
        "url": f"/api/library/{row['id']}.png",
    }


IMAGE_QUERY = (
    "SELECT images.*, (SELECT COUNT(DISTINCT playlist_id) FROM playlist_items "
    "WHERE image_id = images.id) AS used_in FROM images"
)


@app.get("/api/library")
async def library():
    with db() as connection:
        rows = connection.execute(f"{IMAGE_QUERY} ORDER BY images.created_at DESC").fetchall()
    return [image_row_to_dict(row) for row in rows]


@app.post("/api/library", status_code=201)
async def add_to_library(file: UploadFile = File(...), settings: dict = Depends(adjustments)):
    data = await read_upload(file, MAX_UPLOAD_BYTES)
    png, _, _ = await convert_upload(data, settings)
    image_id = uuid.uuid4().hex
    name = Path(file.filename or "image").stem[:100] or "image"

    # the original is kept so an image can be converted differently later
    (IMAGE_DIR / f"{image_id}.original").write_bytes(data)
    (IMAGE_DIR / f"{image_id}.png").write_bytes(png)
    with db() as connection:
        connection.execute(
            "INSERT INTO images (id, name, bytes, created_at, settings) VALUES (?, ?, ?, ?, ?)",
            (image_id, name, len(png), time.time(), json.dumps(settings)),
        )
        row = connection.execute(f"{IMAGE_QUERY} WHERE images.id = ?", (image_id,)).fetchone()
    return image_row_to_dict(row)


def require_image(image_id):
    with db() as connection:
        row = connection.execute("SELECT id FROM images WHERE id = ?", (image_id,)).fetchone()
    if row is None:
        raise HTTPException(404, "Image not found")


@app.get("/api/library/{image_id}.png")
async def library_image(image_id: str):
    require_image(image_id)
    # images can change when they're adjusted, so browsers have to check for a new one
    return FileResponse(
        IMAGE_DIR / f"{image_id}.png", media_type="image/png", headers={"Cache-Control": "no-cache"}
    )


@app.get("/api/library/{image_id}/original")
async def library_original(image_id: str):
    require_image(image_id)
    path = IMAGE_DIR / f"{image_id}.original"
    if not path.exists():
        raise HTTPException(404, "The original of this image is missing")
    return FileResponse(
        path,
        media_type="application/octet-stream",
        headers={"Cache-Control": "no-cache", "X-Content-Type-Options": "nosniff"},
    )


@app.put("/api/library/{image_id}/adjust")
async def adjust_library_image(image_id: str, settings: dict = Depends(adjustments)):
    """Converts the kept original again, playlists use the new version from the next image."""
    require_image(image_id)
    path = IMAGE_DIR / f"{image_id}.original"
    if not path.exists():
        raise HTTPException(404, "The original of this image is missing")
    png, _, _ = await convert_upload(path.read_bytes(), settings)

    temporary = IMAGE_DIR / f"{image_id}.png.new"
    temporary.write_bytes(png)
    temporary.replace(IMAGE_DIR / f"{image_id}.png")
    with db() as connection:
        connection.execute(
            "UPDATE images SET bytes = ?, settings = ? WHERE id = ?",
            (len(png), json.dumps(settings), image_id),
        )
        row = connection.execute(f"{IMAGE_QUERY} WHERE images.id = ?", (image_id,)).fetchone()
    return image_row_to_dict(row)


@app.post("/api/library/{image_id}/show")
async def show_library_image(image_id: str):
    require_image(image_id)
    stop_player()
    data = (IMAGE_DIR / f"{image_id}.png").read_bytes()
    response = await board_request("POST", "/show-png", files={"file": ("image.png", data, "image/png")})
    await refresh_status()
    return {"message": response.text}


@app.delete("/api/library/{image_id}", status_code=204)
async def delete_library_image(image_id: str):
    require_image(image_id)
    with db() as connection:
        connection.execute("DELETE FROM images WHERE id = ?", (image_id,))
    for suffix in ("png", "original"):
        (IMAGE_DIR / f"{image_id}.{suffix}").unlink(missing_ok=True)


class PlaylistName(BaseModel):
    name: str = Field(min_length=1, max_length=60)


class PlaylistItem(BaseModel):
    image_id: str
    duration: int = Field(ge=MIN_DURATION, le=MAX_DURATION)


class PlaylistUpdate(PlaylistName):
    items: list[PlaylistItem]


def playlist_to_dict(connection, row):
    items = connection.execute(
        "SELECT image_id, duration FROM playlist_items WHERE playlist_id = ? ORDER BY position",
        (row["id"],),
    ).fetchall()
    return {
        "id": row["id"],
        "name": row["name"],
        "items": [{"image_id": item["image_id"], "duration": item["duration"]} for item in items],
    }


def clean_name(name):
    name = name.strip()
    if not name:
        raise HTTPException(400, "The playlist needs a name")
    return name


@app.get("/api/playlists")
async def playlists():
    with db() as connection:
        rows = connection.execute("SELECT * FROM playlists ORDER BY name COLLATE NOCASE").fetchall()
        return [playlist_to_dict(connection, row) for row in rows]


@app.post("/api/playlists", status_code=201)
async def create_playlist(body: PlaylistName):
    try:
        with db() as connection:
            cursor = connection.execute("INSERT INTO playlists (name) VALUES (?)", (clean_name(body.name),))
            row = connection.execute("SELECT * FROM playlists WHERE id = ?", (cursor.lastrowid,)).fetchone()
            return playlist_to_dict(connection, row)
    except sqlite3.IntegrityError as error:
        raise HTTPException(409, "A playlist with this name already exists") from error


@app.put("/api/playlists/{playlist_id}")
async def update_playlist(playlist_id: int, body: PlaylistUpdate):
    try:
        with db() as connection:
            updated = connection.execute(
                "UPDATE playlists SET name = ? WHERE id = ?", (clean_name(body.name), playlist_id)
            )
            if updated.rowcount == 0:
                raise HTTPException(404, "Playlist not found")
            connection.execute("DELETE FROM playlist_items WHERE playlist_id = ?", (playlist_id,))
            connection.executemany(
                "INSERT INTO playlist_items (playlist_id, position, image_id, duration) VALUES (?, ?, ?, ?)",
                [
                    (playlist_id, position, item.image_id, item.duration)
                    for position, item in enumerate(body.items)
                ],
            )
            row = connection.execute("SELECT * FROM playlists WHERE id = ?", (playlist_id,)).fetchone()
            return playlist_to_dict(connection, row)
    except sqlite3.IntegrityError as error:
        raise HTTPException(409, "The name is already used, or an image no longer exists") from error


@app.delete("/api/playlists/{playlist_id}", status_code=204)
async def delete_playlist(playlist_id: int):
    if player["active"] and player["playlist_id"] == playlist_id:
        stop_player()
    with db() as connection:
        deleted = connection.execute("DELETE FROM playlists WHERE id = ?", (playlist_id,))
    if deleted.rowcount == 0:
        raise HTTPException(404, "Playlist not found")


@app.post("/api/playlists/{playlist_id}/play")
async def play_playlist(playlist_id: int):
    with db() as connection:
        row = connection.execute("SELECT id FROM playlists WHERE id = ?", (playlist_id,)).fetchone()
    if row is None:
        raise HTTPException(404, "Playlist not found")
    if not playlist_items(playlist_id):
        raise HTTPException(400, "Add images to the playlist first")
    start_player(playlist_id)
    return {"message": "Playlist started"}


class PlayerSkip(BaseModel):
    playlist_id: int
    position: int = Field(ge=0)


def skip_to(position, items):
    player.update(
        generation=player["generation"] + 1,
        index=position,
        state="starting",
        message="",
        next_change_at=None,
    )
    save_player_state()
    player_wake.set()
    return {"message": f"Skipped to {items[position]['name']}"}


@app.post("/api/player/skip")
async def player_skip(body: PlayerSkip):
    if not player["active"] or player["playlist_id"] != body.playlist_id:
        raise HTTPException(409, "This playlist isn't playing")
    items = playlist_items(body.playlist_id)
    if body.position >= len(items):
        raise HTTPException(400, "This image is no longer in the playlist")
    return skip_to(body.position, items)


class PlayerStep(BaseModel):
    direction: Literal[-1, 1]


@app.post("/api/player/step")
async def player_step(body: PlayerStep):
    """Goes to the previous or next image of the playing playlist, wrapping around."""
    if not player["active"]:
        raise HTTPException(409, "No playlist is playing")
    items = playlist_items(player["playlist_id"])
    if not items:
        raise HTTPException(400, "The playlist has no images")
    return skip_to((player["index"] + body.direction) % len(items), items)


@app.post("/api/player/stop")
async def player_stop():
    stop_player()
    return {"message": "Playlist stopped, the current image stays on screen"}


def player_status():
    result = {
        "active": player["active"],
        "state": player["state"],
        "message": player["message"],
        "playlist_id": player["playlist_id"] if player["active"] else None,
        "playlist_name": None,
        "position": None,
        "count": None,
        "image_name": None,
        "seconds_left": None,
    }
    if not player["active"]:
        return result

    with db() as connection:
        row = connection.execute("SELECT name FROM playlists WHERE id = ?", (player["playlist_id"],)).fetchone()
    items = playlist_items(player["playlist_id"])
    result["playlist_name"] = row["name"] if row else None
    result["count"] = len(items)
    if items:
        index = player["index"] % len(items)
        result["position"] = index + 1
        result["image_name"] = items[index]["name"]
    if player["next_change_at"]:
        result["seconds_left"] = max(0, round(player["next_change_at"] - time.time()))
    return result


@app.get("/api/status")
async def get_status():
    checked_at = status["checked_at"]
    return {
        "online": status["failures"] < OFFLINE_AFTER_FAILURES and status["data"] is not None,
        "seconds_since_check": None if checked_at is None else round(time.time() - checked_at),
        "board": status["data"],
        "player": player_status(),
    }


@app.get("/api/limits")
async def limits():
    return {
        "size": SIZE[0],
        "max_png_bytes": MAX_BYTES,
        "max_upload_bytes": MAX_UPLOAD_BYTES,
        "min_duration": MIN_DURATION,
        "max_duration": MAX_DURATION,
        "default_duration": DEFAULT_DURATION,
    }


@app.get("/")
async def index():
    # browsers have to check for a new version after an update
    return FileResponse(STATIC_DIR / "index.html", headers={"Cache-Control": "no-cache"})
