#pragma once

#include <Arduino.h>
#include <HalDisplay.h>

struct DeviceConfig {
    char wifi_ssid[64];
    char wifi_pass[64];
    char server_host[128];
    uint16_t server_port;
};

// Load from SPIFFS /config.json, falling back to compiled defaults in config.h
DeviceConfig loadDeviceConfig();

// Save to SPIFFS /config.json
bool saveDeviceConfig(const DeviceConfig& cfg);

// Fetch config from server /api/x4/config, save to SPIFFS if changed.
// Returns true if config was updated (takes effect on next boot).
bool fetchAndUpdateConfig(const DeviceConfig& current);
