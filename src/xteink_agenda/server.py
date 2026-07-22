"""FastAPI server — the Xteink X4 device API, calendar-only."""

from __future__ import annotations

import asyncio
import logging
import os
import stat
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timedelta
from pathlib import Path

from dotenv import load_dotenv
from fastapi import FastAPI, Request
from fastapi.middleware.gzip import GZipMiddleware
from fastapi.responses import FileResponse, HTMLResponse, JSONResponse

from .config import ConfigError, all_calendar_ids, load_config, parse_views, read_env_raw, write_env
from .gcal import delete_event, get_agenda

# Setup: load .env relative to project root (works regardless of cwd)
_project_root = Path(__file__).parent.parent.parent
load_dotenv(_project_root / ".env")

# Check sensitive file permissions
_sa_file = _project_root / "service-account.json"
if _sa_file.exists() and _sa_file.stat().st_mode & stat.S_IROTH:
    logging.warning("service-account.json is world-readable! Run: chmod 600 service-account.json")
_env_file = _project_root / ".env"
if _env_file.exists() and _env_file.stat().st_mode & stat.S_IROTH:
    logging.warning(".env is world-readable! Run: chmod 600 .env")

# Logging
logging.basicConfig(level=logging.INFO, stream=sys.stderr,
                    format="%(asctime)s [%(name)s] %(message)s", datefmt="%H:%M:%S")
logger = logging.getLogger("xteink_agenda")

# Fail fast on invalid VIEW_* env vars
try:
    parse_views()
except ConfigError as e:
    logger.critical("Configuration error: %s", e)
    raise SystemExit(2) from e

_executor = ThreadPoolExecutor(max_workers=2)

# X4 agenda bounds — keep in sync with firmware clamps in main.cpp.
DAY_OFFSET_MIN = -7
DAY_OFFSET_MAX = 30

# Agenda cache: avoids re-hitting GCal on every button press. 60s TTL + explicit
# invalidation whenever an event is deleted (via _invalidate_agenda_cache).
AGENDA_CACHE_TTL_SEC = 60
_agenda_cache: dict[tuple, tuple[float, list[dict]]] = {}
_agenda_cache_lock = threading.Lock()


def _invalidate_agenda_cache() -> None:
    """Drop all cached agenda entries. Called after any state change."""
    with _agenda_cache_lock:
        _agenda_cache.clear()


app = FastAPI(title="xteink-agenda")

# Gzip responses above ~1KB when the client sends `Accept-Encoding: gzip`.
# Agenda JSON is typically 2-5KB — typical savings 40-60% for any client that
# supports it (web UI, curl). The ESP32 firmware doesn't send the header so
# it's unaffected.
app.add_middleware(GZipMiddleware, minimum_size=1000)


def _resolve_view(view: int) -> tuple[str, list[str]]:
    views = parse_views()
    view = max(0, min(view, len(views) - 1)) if views else 0
    if views:
        return views[view]
    return "All", all_calendar_ids()


def _get_agenda_items(cal_ids: list[str], day_offset: int = 0) -> list[dict]:
    # Cache key is stable across calls: sorted cal_ids + day_offset.
    # Each entry is (stored_at, items). Stale entries are recomputed.
    cache_key = (tuple(sorted(cal_ids)), day_offset)
    with _agenda_cache_lock:
        cached = _agenda_cache.get(cache_key)
    if cached is not None:
        stored_at, items = cached
        if time.time() - stored_at < AGENDA_CACHE_TTL_SEC:
            return items

    items = _get_agenda_items_fresh(cal_ids, day_offset=day_offset)
    with _agenda_cache_lock:
        _agenda_cache[cache_key] = (time.time(), items)
    return items


def _get_agenda_items_fresh(cal_ids: list[str], day_offset: int = 0) -> list[dict]:
    cfg = load_config()
    now = datetime.now().astimezone()
    target = now + timedelta(days=day_offset)
    target_date = target.strftime("%Y-%m-%d")
    items: list[dict] = []

    if not cfg["sa_path"].exists():
        logger.warning("Service account file %s not found — agenda is empty", cfg["sa_path"])
        return items

    try:
        # Query GCal for the target day
        day_end = target.replace(hour=23, minute=59, second=59, microsecond=0)
        day_start = target.replace(hour=0, minute=0, second=0, microsecond=0)
        agenda = get_agenda(
            service_account_path=cfg["sa_path"],
            calendar_ids=cal_ids,
            target_start=day_start,
            target_end=day_end,
        )
        all_events = ([agenda.now] if agenda.now else []) + agenda.next
        for ev in all_events:
            if not ev:
                continue
            # Include events that start on the target date,
            # OR overnight events still ongoing (started yesterday, ending today)
            if ev.start.strftime("%Y-%m-%d") != target_date:
                if day_offset != 0 or ev.end <= now:
                    continue
            items.append({
                "id": f"gcal-{ev.calendar_id}-{ev.start.isoformat()}",
                "title": ev.title,
                "start": ev.start.strftime("%H:%M"),
                "end": ev.end.strftime("%H:%M"),
                "start_dt": ev.start.isoformat(),
                "end_dt": ev.end.isoformat(),
                "duration_min": ev.duration_min,
                "type": "event",
                "completed": False,
                "calendar_id": ev.calendar_id,
                "gcal_event_id": ev.gcal_event_id,
            })
    except Exception as e:
        logger.warning(f"GCal fetch failed: {e}")

    # Deduplicate (overnight events can appear twice: once from today, once from yesterday)
    seen = set()
    deduped = []
    for item in items:
        key = (item["id"], item["start"], item["end"])
        if key not in seen:
            seen.add(key)
            deduped.append(item)

    deduped.sort(key=lambda x: x.get("start", ""))
    return deduped


def _parse_item_datetime(item: dict, key: str, tz) -> datetime | None:
    raw = item.get(key)
    if not raw:
        return None
    try:
        dt = datetime.fromisoformat(raw)
    except ValueError:
        return None
    if dt.tzinfo is None:
        return dt.replace(tzinfo=tz)
    return dt.astimezone(tz)


def _is_item_active_now(item: dict, now: datetime) -> bool:
    start_dt = _parse_item_datetime(item, "start_dt", now.tzinfo)
    end_dt = _parse_item_datetime(item, "end_dt", now.tzinfo)
    if start_dt and end_dt:
        return start_dt <= now < end_dt

    s, e = item.get("start", ""), item.get("end", "")
    if not s or not e:
        return False
    now_hm = now.strftime("%H:%M")
    if s <= e:
        return s <= now_hm < e
    return now_hm >= s or now_hm < e


def _item_future_start(item: dict, now: datetime) -> datetime | None:
    start_dt = _parse_item_datetime(item, "start_dt", now.tzinfo)
    if start_dt:
        return start_dt if start_dt > now else None

    start_hm = item.get("start", "")
    if not start_hm:
        return None
    try:
        start_h, start_m = int(start_hm[:2]), int(start_hm[3:5])
    except (TypeError, ValueError):
        return None
    start_today = now.replace(hour=start_h, minute=start_m, second=0, microsecond=0)
    return start_today if start_today > now else None


def _item_end_after_now(item: dict, now: datetime) -> datetime | None:
    end_dt = _parse_item_datetime(item, "end_dt", now.tzinfo)
    if end_dt:
        return end_dt if end_dt > now else None

    end_hm = item.get("end", "")
    if not end_hm:
        return None
    try:
        end_h, end_m = int(end_hm[:2]), int(end_hm[3:5])
    except (TypeError, ValueError):
        return None
    end_today = now.replace(hour=end_h, minute=end_m, second=0, microsecond=0)
    if end_today <= now:
        end_today += timedelta(days=1)
    return end_today


# ========================
# X4 Device API
# ========================

@app.get("/api/x4/agenda")
async def x4_agenda(view: int = 0, day_offset: int = 0):
    day_offset = max(DAY_OFFSET_MIN, min(DAY_OFFSET_MAX, day_offset))
    view_name, cal_ids = _resolve_view(view)
    loop = asyncio.get_running_loop()
    items = await loop.run_in_executor(_executor, _get_agenda_items, cal_ids, day_offset)

    now = datetime.now().astimezone()
    target = now + timedelta(days=day_offset)

    if day_offset == 0:
        cfg = load_config()
        day_start = now.replace(hour=cfg["day_start"], minute=0, second=0, microsecond=0)
        day_end = now.replace(hour=max(cfg["day_start"] + 1, cfg["day_end"]), minute=0,
                              second=0, microsecond=0)
        progress = max(0, min(1, (now - day_start).total_seconds()
                              / (day_end - day_start).total_seconds()))
    else:
        progress = 0.0

    # Find all items currently happening (for the passive/lock screen) and
    # detect conflicts between overlapping events.
    current_index = -1
    conflict_index = -1  # Second overlapping event (conflict)
    active_indices = []
    if day_offset == 0:
        for i, item in enumerate(items):
            if _is_item_active_now(item, now):
                active_indices.append(i)

        if active_indices:
            current_index = active_indices[0]
            if len(active_indices) > 1:
                conflict_index = active_indices[1]
        else:
            # Nothing active right now — find the NEXT upcoming item
            for i, item in enumerate(items):
                if _item_future_start(item, now):
                    current_index = i
                    break

    # Calculate wake timer
    wake_in_sec = 300  # Default: 5 min
    if day_offset == 0:
        if active_indices:
            # Wake when the earliest active item ends
            earliest_end = None
            for i in active_indices:
                end_at = _item_end_after_now(items[i], now)
                if end_at and (earliest_end is None or end_at < earliest_end):
                    earliest_end = end_at
            if earliest_end:
                delta = int((earliest_end - now).total_seconds())
                if delta > 0:
                    wake_in_sec = min(delta + 10, 3600)
        elif current_index >= 0:
            # Nothing active — wake when the next item starts
            start_at = _item_future_start(items[current_index], now)
            if start_at:
                delta = int((start_at - now).total_seconds())
                if delta > 0:
                    wake_in_sec = min(delta + 10, 3600)

    return {
        "view_name": view_name,
        "view_count": len(parse_views()),
        "date": target.strftime("%Y-%m-%d"),
        "date_label": target.strftime("%A %-d %b"),
        "day_offset": day_offset,
        "items": items,
        "current_index": current_index,
        "conflict_index": conflict_index,
        "wake_in_sec": wake_in_sec,
        "day_progress": round(progress, 3),
        "generated_at": now.isoformat(),
    }


@app.post("/api/x4/complete-selected")
async def x4_complete_selected(view: int = 0, cursor: int = 0, day_offset: int = 0):
    """Compatibility no-op: every item is a calendar event here, and the firmware
    only offers Complete on items with type "task" — which never appear."""
    return JSONResponse({"error": "cannot complete calendar events"}, 400)


@app.post("/api/x4/delete-event")
async def x4_delete_event(view: int = 0, cursor: int = 0, day_offset: int = 0):
    """Delete the GCal event at the cursor position. Used when a meeting is
    canceled — the user confirms on the event via two-press and we remove it
    from Google Calendar (or decline it, for attendee-only events)."""
    day_offset = max(DAY_OFFSET_MIN, min(DAY_OFFSET_MAX, day_offset))
    _, cal_ids = _resolve_view(view)
    loop = asyncio.get_running_loop()

    items = await loop.run_in_executor(_executor, _get_agenda_items, cal_ids, day_offset)
    if cursor < 0 or cursor >= len(items):
        return JSONResponse({"error": "cursor out of range"}, 400)

    item = items[cursor]
    gcal_event_id = item.get("gcal_event_id")
    calendar_id = item.get("calendar_id")
    if not gcal_event_id or not calendar_id:
        return JSONResponse({"error": "missing event identity"}, 400)

    cfg = load_config()
    try:
        await loop.run_in_executor(
            _executor, delete_event, cfg["sa_path"], calendar_id, gcal_event_id)
    except Exception as e:
        logger.warning("Event delete failed: %s", e)
        return JSONResponse({"error": "delete failed"}, 502)

    _invalidate_agenda_cache()
    return {"deleted": gcal_event_id, "title": item["title"]}


# ========================
# Settings Web UI
# ========================

SETTINGS_HTML = """<!DOCTYPE html>
<html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Xteink Agenda Settings</title>
<style>
  * { margin: 0; padding: 0; box-sizing: border-box; }
  body { font-family: Charter, Georgia, serif; background: #fff; color: #000;
         max-width: 480px; margin: 0 auto; padding: 28px; }
  h1 { font-size: 28px; font-weight: bold; margin-bottom: 8px; }
  .sub { font-family: -apple-system, sans-serif; font-size: 12px; color: #999;
         margin-bottom: 32px; letter-spacing: 0.05em; }
  h2 { font-weight: 600; margin: 28px 0 12px; padding-top: 16px;
       border-top: 2px solid #000; font-family: -apple-system, sans-serif;
       letter-spacing: 0.08em; text-transform: uppercase; font-size: 11px; }
  label { display: block; font-size: 13px; color: #666; margin-bottom: 4px;
          font-family: -apple-system, sans-serif; }
  input, textarea { width: 100%; padding: 10px 0; border: none; border-bottom: 1px solid #ccc;
                     font-family: Charter, Georgia, serif; font-size: 16px; outline: none;
                     margin-bottom: 20px; background: transparent; }
  input:focus, textarea:focus { border-bottom-color: #000; }
  textarea { resize: vertical; min-height: 60px; }
  .row { display: flex; gap: 16px; }
  .row > div { flex: 1; }
  .reveal { position: absolute; right: 0; top: 8px; border: 1px solid #ccc;
            background: #fff; padding: 4px 10px; font-size: 11px; cursor: pointer;
            font-family: -apple-system, sans-serif; border-radius: 4px; }
  .token-field { position: relative; }
  .token-field input { padding-right: 60px; }
  button[type=submit] { background: #000; color: #fff; border: none; padding: 14px 28px;
                         font-size: 14px; cursor: pointer; border-radius: 6px; margin-top: 12px;
                         font-family: -apple-system, sans-serif; font-weight: 600;
                         letter-spacing: 0.03em; }
  button[type=submit]:hover { background: #333; }
  .success { background: #f0f0f0; padding: 12px 16px; border-radius: 6px; margin-bottom: 20px;
             font-family: -apple-system, sans-serif; font-size: 13px; }
  .hint { font-size: 11px; color: #999; margin-top: -16px; margin-bottom: 16px;
          font-family: -apple-system, sans-serif; }
</style>
</head><body>
<h1>Xteink Agenda</h1>
<div class="sub">S E T T I N G S</div>
$success
<form method="POST" action="/settings">

<h2>Google Calendar</h2>
<label>Service Account JSON Path</label>
<input type="text" name="GCAL_SERVICE_ACCOUNT" value="$GCAL_SERVICE_ACCOUNT">
<div class="hint">Path to the service account JSON file relative to project root</div>

<label>Calendar Views</label>
<textarea name="VIEWS" placeholder="All:calendar@gmail.com&#10;Work:work@company.com&#10;Personal:personal@gmail.com">$VIEWS</textarea>
<div class="hint">One view per line. Format: Name:calendar_id1,calendar_id2</div>

<h2>Day Progress</h2>
<div class="row">
  <div>
    <label>Day Start Hour</label>
    <input type="number" name="DAY_START" value="$DAY_START" min="0" max="22">
  </div>
  <div>
    <label>Day End Hour</label>
    <input type="number" name="DAY_END" value="$DAY_END" min="1" max="23">
  </div>
</div>
<div class="hint">The waking-hours window used for the on-device day-progress bar.</div>

<label>Timezone</label>
<input type="text" name="TZ" value="$TZ" placeholder="Europe/London">
<div class="hint">IANA name like Asia/Tokyo or America/New_York. Update when you travel. Leave blank to use system timezone.</div>

<h2>Device Config</h2>
<div class="hint">Pushed to the X4 on next boot via /api/x4/config. Leave blank to keep device defaults.</div>
<div class="row">
  <div>
    <label>WiFi SSID</label>
    <input type="text" name="DEVICE_WIFI_SSID" value="$DEVICE_WIFI_SSID">
  </div>
  <div>
    <label>WiFi Password</label>
    <input type="password" name="DEVICE_WIFI_PASS" value="$DEVICE_WIFI_PASS" id="wifipass">
    <button type="button" class="reveal" onclick="let t=document.getElementById('wifipass'); t.type=t.type==='password'?'text':'password'; this.textContent=t.type==='password'?'Show':'Hide';">Show</button>
  </div>
</div>
<div class="row">
  <div>
    <label>Server Host</label>
    <input type="text" name="DEVICE_SERVER_HOST" value="$DEVICE_SERVER_HOST">
  </div>
  <div>
    <label>Server Port</label>
    <input type="number" name="DEVICE_SERVER_PORT" value="$DEVICE_SERVER_PORT" min="1" max="65535">
  </div>
</div>

<button type="submit">Save</button>
</form>
</body></html>"""


@app.get("/settings", response_class=HTMLResponse)
async def settings_page(saved: str = ""):
    env = read_env_raw()
    # Build views text from VIEW_0, VIEW_1, etc.
    views_lines = []
    i = 0
    while f"VIEW_{i}" in env:
        views_lines.append(env[f"VIEW_{i}"])
        i += 1

    success = ""
    if saved == "1":
        success = '<div class="success">Settings saved.</div>'

    from html import escape
    from string import Template
    html = Template(SETTINGS_HTML).safe_substitute(
        success=success,
        GCAL_SERVICE_ACCOUNT=escape(env.get("GCAL_SERVICE_ACCOUNT", "service-account.json")),
        VIEWS=escape("\n".join(views_lines)),
        DAY_START=escape(env.get("DAY_START", "7")),
        DAY_END=escape(env.get("DAY_END", "23")),
        TZ=escape(env.get("TZ", "")),
        DEVICE_WIFI_SSID=escape(env.get("DEVICE_WIFI_SSID", "")),
        DEVICE_WIFI_PASS=escape(env.get("DEVICE_WIFI_PASS", "")),
        DEVICE_SERVER_HOST=escape(env.get("DEVICE_SERVER_HOST", "")),
        DEVICE_SERVER_PORT=escape(env.get("DEVICE_SERVER_PORT", "")),
    )
    return HTMLResponse(html)


@app.post("/settings")
async def save_settings(request: Request):
    form = await request.form()

    values = {
        "GCAL_SERVICE_ACCOUNT": form.get("GCAL_SERVICE_ACCOUNT", "service-account.json"),
        "DAY_START": form.get("DAY_START", "7"),
        "DAY_END": form.get("DAY_END", "23"),
        "TZ": form.get("TZ", ""),
        "DEVICE_WIFI_SSID": form.get("DEVICE_WIFI_SSID", ""),
        "DEVICE_WIFI_PASS": form.get("DEVICE_WIFI_PASS", ""),
        "DEVICE_SERVER_HOST": form.get("DEVICE_SERVER_HOST", ""),
        "DEVICE_SERVER_PORT": form.get("DEVICE_SERVER_PORT", ""),
    }

    # Parse views textarea into VIEW_0, VIEW_1, etc.
    views_text = form.get("VIEWS", "")
    # Clear old VIEW_* keys
    env = read_env_raw()
    i = 0
    while f"VIEW_{i}" in env:
        values[f"VIEW_{i}"] = ""  # Will be overwritten or removed
        i += 1

    # Set new views
    for idx, line in enumerate(views_text.strip().splitlines()):
        line = line.strip()
        if line:
            values[f"VIEW_{idx}"] = line

    # Remove empty VIEW_* entries
    values = {k: v for k, v in values.items() if v}

    write_env(values)

    # Reload .env into os.environ
    load_dotenv(_project_root / ".env", override=True)
    _invalidate_agenda_cache()

    logger.info("Settings saved via web UI")

    # Redirect back with success flag
    from fastapi.responses import RedirectResponse
    return RedirectResponse(url="/settings?saved=1", status_code=303)


# ========================
# Device config (WiFi + server settings pushed to device via SPIFFS)
# ========================

@app.get("/api/x4/config")
async def device_config():
    """Return device config — device saves to SPIFFS on boot if changed."""
    return {
        "wifi_ssid": os.getenv("DEVICE_WIFI_SSID", ""),
        "wifi_pass": os.getenv("DEVICE_WIFI_PASS", ""),
        "server_host": os.getenv("DEVICE_SERVER_HOST", ""),
        "server_port": int(os.getenv("DEVICE_SERVER_PORT", "0") or "0"),
    }


# ========================
# Firmware OTA
# ========================

@app.get("/api/x4/firmware/version")
async def firmware_version(request: Request):
    """Return latest firmware version and download URL for OTA."""
    import json
    manifest_path = _project_root / "firmware" / "ota" / "manifest.json"
    if not manifest_path.exists():
        return {"version": "0.0.0", "size": 0}
    manifest = json.loads(manifest_path.read_text())
    bin_path = _project_root / "firmware" / "ota" / manifest["file"]
    host = request.headers.get("host", "localhost:8080")
    return {
        "version": manifest["version"],
        "url": f"http://{host}/api/x4/firmware/download",
        "size": bin_path.stat().st_size if bin_path.exists() else 0,
    }


@app.get("/api/x4/firmware/download")
async def firmware_download():
    """Stream firmware binary for OTA update."""
    import json
    manifest_path = _project_root / "firmware" / "ota" / "manifest.json"
    if not manifest_path.exists():
        return JSONResponse({"error": "no firmware manifest"}, 404)
    manifest = json.loads(manifest_path.read_text())
    bin_path = _project_root / "firmware" / "ota" / manifest["file"]
    if not bin_path.exists():
        return JSONResponse({"error": "firmware binary not found"}, 404)
    return FileResponse(bin_path, media_type="application/octet-stream",
                        filename="firmware.bin")


@app.get("/status")
async def status():
    return {
        "views": [name for name, _ in parse_views()],
        "agenda_cache_entries": len(_agenda_cache),
    }


@app.get("/health")
async def health():
    return {"status": "ok"}
