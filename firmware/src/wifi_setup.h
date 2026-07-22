#pragma once

#include "device_config.h"

// On-device WiFi provisioning wizard. Replaces WiFiManager captive portal so
// the user never needs a phone or browser — they pick the network and type
// the password using the X4's buttons + e-ink display.
//
// Blocks until either:
//   - A connection succeeds (creds copied into cfg, persisted to SPIFFS) → true
//   - The user cancels with the power button or no networks found → false
//
// Caller is responsible for the next step: on true, proceed with normal app
// flow; on false, fall back to cached agenda or sleep.
//
// Preconditions:
//   - display is initialized
//   - HalGPIO button polling task is running (consumeBtn() reachable)
bool runWifiSetup(DeviceConfig& cfg);
