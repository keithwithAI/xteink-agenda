# xteink-agenda X4 Firmware

Firmware for the Xteink X4 e-reader. Fetches agenda JSON from the xteink-agenda
server and renders the calendar natively on the e-ink display.

Built on [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader)'s
hardware abstraction (MIT) — battle-tested HalDisplay, HalGPIO, GfxRenderer, and
EpdFont libraries. Hardware drivers from
[open-x4-sdk](https://github.com/open-x4-epaper/community-sdk) (vendored in
`open-x4-sdk/`).

## Hardware

- Xteink X4: ESP32-C3, 480x800 1-bit e-ink (SSD1677), 5-way d-pad + back + power
- Connects to the xteink-agenda server on local WiFi

## Setup

1. Install [PlatformIO CLI](https://docs.platformio.org/en/latest/core/installation.html) or the VS Code extension
2. `cp src/secrets.h.example src/secrets.h` — set WiFi SSID, password, and server host/port

## Build & Flash

```bash
cd firmware/
pio run                    # Build
pio run --target upload    # Flash via USB-C
pio device monitor         # Serial debug
```

## How It Works

1. Wake from deep sleep (event-boundary timer or power button held 500ms)
2. Connect WiFi → fetch JSON from server → disconnect WiFi (battery saving)
3. Render calendar UI natively using CrossPoint's GfxRenderer + EpdFont
4. Wait for button input (inactivity → sleep)

## Buttons

| Button | Action |
|---|---|
| Left rocker | Cursor up |
| Right rocker | Cursor down |
| Confirm | Delete event (two-press confirm) |
| Back | Cycle calendar view |
| Volume up | Previous day |
| Volume down | Next day |
| Power | Wake (500ms hold) / Sleep |

## Design notes

- Pull-based WiFi — radio on only during fetch, off during render + idle
- Power button verification — 500ms hold required to prevent accidental wake
- Battery monitoring via HalPowerManager (ADC on GPIO 0)
- OTA-capable partition table; the device checks the server for new firmware on boot

## Returning to CrossPoint

Flash back from [CrossPoint releases](https://github.com/crosspoint-reader/crosspoint-reader/releases).
