# xteink-agenda

Your Google Calendar on an [Xteink X4](https://xteink.com) e-ink reader.

A tiny FastAPI server on your home network fetches your Google Calendar and serves a
compact agenda JSON; ESP32-C3 firmware on the X4 wakes from deep sleep, pulls the
day's agenda over WiFi, renders it natively on the 480×800 e-ink panel, and goes
back to sleep. Battery lasts weeks.

```
Google Calendar ──► server (FastAPI, your Mac/Pi) ──► WiFi ──► Xteink X4 (ESP32-C3 e-ink)
```

## Features

- **Day agenda** — today's events, current event highlighted, conflict detection
- **Passive glance screen** — the current/next event with a day-progress bar; the
  device wakes itself exactly when the current event ends or the next one starts
- **Multiple calendar views** — cycle Work/Personal/All with the Back button
- **Day navigation** — flip ±days with the volume rocker (-7 to +30)
- **Delete events from the device** — two-press confirm removes a canceled meeting
  from Google Calendar (attendee-only events are declined instead)
- **OTA firmware updates** — the device checks the server for new firmware on boot
- **Web settings page** — calendars, views, device WiFi config at `/settings`

## Hardware

- Xteink X4: ESP32-C3, 480×800 1-bit e-ink (SSD1677), 5-way d-pad + back + power
- Any always-on machine on the same LAN for the server (a Mac mini, Raspberry Pi…)

The firmware builds on [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader)'s
hardware abstraction (MIT) and drivers from the
[open-x4-sdk community SDK](https://github.com/open-x4-epaper/community-sdk) (MIT, vendored
in `firmware/open-x4-sdk/`).

Xteink has reportedly blocked third-party firmware on some X3 and X4 units bought
from Chinese retailers such as Taobao. It says overseas versions bought from its
official website aren't restricted. Check yours before relying on this project.

## Security

The server has no login and listens on all network interfaces, so anyone who can
reach it can use it. `/settings` can change its configuration, `/api/x4/delete-event`
can delete events from your calendar, and `/api/x4/config` returns the device WiFi
password if you've set one. Firmware updates travel over plain HTTP. Run the server
on a home network you trust, and don't port-forward it or expose it to the internet.

## Server setup

Requires Python 3.12+ and [uv](https://docs.astral.sh/uv/).

1. **Google service account**: in Google Cloud Console create a project, enable the
   Calendar API, create a service account, and download its JSON key to
   `service-account.json` in the repo root. Then share each calendar you want to see
   with the service account's email address ("See all event details" is enough for
   the agenda; "Make changes to events" if you want delete-from-device).
2. **Configure**: `cp .env.example .env` and set your `VIEW_*` calendar views.
3. **Run**:

```bash
uv sync
uv run xteink-agenda serve        # http://0.0.0.0:8080
```

Open `http://localhost:8080/settings` to tweak config from a browser, and
`http://localhost:8080/api/x4/agenda` to see what the device sees.

To keep it running on macOS, add a LaunchAgent; on Linux, a systemd unit. The server
is stateless — safe to restart any time.

## Firmware setup

Requires [PlatformIO](https://platformio.org).

1. `cp firmware/src/secrets.h.example firmware/src/secrets.h` and fill in your WiFi
   SSID/password and the server host (e.g. `mymac.local`) and port.
2. Build and flash over USB-C:

```bash
cd firmware
pio run                    # build
pio run --target upload    # flash
pio device monitor         # serial debug
```

After the first flash, updates can go over the air: build, copy
`.pio/build/*/firmware.bin` to `firmware/ota/firmware.bin`, and write
`firmware/ota/manifest.json` (`{"version": "x.y.z", "file": "firmware.bin"}`).
The device checks `/api/x4/firmware/version` on boot and updates itself.

### Going back to the official firmware

Flash the official image with the web flasher at
<https://crosspointreader.com/#flash-tools>.

This firmware keeps your WiFi password and cached agenda in flash, and installing new
firmware may leave them there. If you're selling or giving away the device, erase the
NVS and SPIFFS partitions before you flash:

```bash
esptool.py --chip esp32c3 --port <port> erase_region 0x9000 0x5000     # NVS
esptool.py --chip esp32c3 --port <port> erase_region 0xc90000 0x360000 # SPIFFS
```

### Buttons

| Button | Action |
|---|---|
| Left/right rocker | Cursor up / down |
| Confirm | Delete event (two-press confirm) |
| Back | Cycle calendar view |
| Volume up / down | Previous / next day |
| Power | Wake (500 ms hold) / sleep |

## How it works

1. Wake from deep sleep (RTC timer or power button)
2. Connect WiFi → fetch agenda JSON → disconnect WiFi (battery saving)
3. Render natively with CrossPoint's GfxRenderer + EpdFont
4. Sleep until the agenda's `wake_in_sec` (the next event boundary) or button press

The server caches agenda responses for 60 s so button-mashing doesn't hammer the
Google Calendar API.

## Development

```bash
uv run pytest             # server tests
uv run ruff check .       # lint
```

Design mockups live in `design/`.

## License

MIT. Bundled libraries (CrossPoint HAL, open-x4-sdk, fonts) keep their own notices.
