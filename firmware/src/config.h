#pragma once

// --- WiFi credentials + server address (gitignored secrets.h) ---
// Copy src/secrets.h.example to src/secrets.h and fill in your network + server.
#if __has_include("secrets.h")
    #include "secrets.h"
#else
    #error "Missing firmware/src/secrets.h — copy secrets.h.example to secrets.h and fill in WIFI_SSID / WIFI_PASS / SERVER_HOST."
#endif

#define WIFI_TIMEOUT_MS 8000

// --- Server (defaults if secrets.h doesn't define them) ---
#ifndef SERVER_HOST
#define SERVER_HOST "xteink-agenda.local"
#endif
#ifndef SERVER_PORT
#define SERVER_PORT 8080
#endif
#define API_AGENDA "/api/x4/agenda"
#define API_COMPLETE "/api/x4/complete-selected"
#define API_DELETE_EVENT "/api/x4/delete-event"
#define API_FIRMWARE_VERSION "/api/x4/firmware/version"

// --- Firmware version (also set via -DFIRMWARE_VERSION in platformio.ini) ---
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "1.0.0"
#endif

// --- Timing ---
#define INACTIVITY_TIMEOUT_MS 120000      // 2 min idle → power off

// --- Day offset bounds — keep in sync with server DAY_OFFSET_MIN/MAX ---
#define DAY_OFFSET_MIN (-7)
#define DAY_OFFSET_MAX 30

// --- WiFi reconnect backoff ---
#define WIFI_BACKOFF_MIN_MS 30000       // 30s after first failure
#define WIFI_BACKOFF_MAX_MS 600000      // cap at 10 min

// --- Agenda ---
#define MAX_ITEMS 32
