#include "device_config.h"
#include "config.h"

#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>

static const char* CONFIG_PATH = "/config.json";

static bool ensureSpiffs() {
    static bool ready = false;
    if (ready) return true;
    if (!SPIFFS.begin(true)) {
        Serial.println("DeviceConfig: SPIFFS mount failed");
        return false;
    }
    ready = true;
    return true;
}

DeviceConfig loadDeviceConfig() {
    DeviceConfig cfg;
    // Start with compiled defaults
    strlcpy(cfg.wifi_ssid, WIFI_SSID, sizeof(cfg.wifi_ssid));
    strlcpy(cfg.wifi_pass, WIFI_PASS, sizeof(cfg.wifi_pass));
    strlcpy(cfg.server_host, SERVER_HOST, sizeof(cfg.server_host));
    cfg.server_port = SERVER_PORT;

    if (!ensureSpiffs()) return cfg;
    if (!SPIFFS.exists(CONFIG_PATH)) {
        Serial.println("DeviceConfig: no /config.json, using compiled defaults");
        return cfg;
    }

    File f = SPIFFS.open(CONFIG_PATH, "r");
    if (!f) return cfg;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err) {
        Serial.printf("DeviceConfig: JSON parse error: %s, using defaults\n", err.c_str());
        return cfg;
    }

    if (doc["wifi_ssid"].is<const char*>())
        strlcpy(cfg.wifi_ssid, doc["wifi_ssid"], sizeof(cfg.wifi_ssid));
    if (doc["wifi_pass"].is<const char*>())
        strlcpy(cfg.wifi_pass, doc["wifi_pass"], sizeof(cfg.wifi_pass));
    if (doc["server_host"].is<const char*>())
        strlcpy(cfg.server_host, doc["server_host"], sizeof(cfg.server_host));
    if (doc["server_port"].is<uint16_t>())
        cfg.server_port = doc["server_port"];

    Serial.printf("DeviceConfig: loaded from SPIFFS (host=%s:%d)\n", cfg.server_host, cfg.server_port);
    return cfg;
}

bool saveDeviceConfig(const DeviceConfig& cfg) {
    if (!ensureSpiffs()) return false;

    JsonDocument doc;
    doc["wifi_ssid"] = cfg.wifi_ssid;
    doc["wifi_pass"] = cfg.wifi_pass;
    doc["server_host"] = cfg.server_host;
    doc["server_port"] = cfg.server_port;

    File f = SPIFFS.open(CONFIG_PATH, "w");
    if (!f) {
        Serial.println("DeviceConfig: failed to open for write");
        return false;
    }
    serializeJson(doc, f);
    f.close();
    Serial.println("DeviceConfig: saved to SPIFFS");
    return true;
}

bool fetchAndUpdateConfig(const DeviceConfig& current) {
    HTTPClient http;
    String url = String("http://") + current.server_host + ":" + current.server_port + "/api/x4/config";
    http.begin(url);
    http.setTimeout(5000);
    int code = http.GET();

    if (code != 200) {
        http.end();
        return false;  // Server doesn't have config endpoint or is unreachable
    }

    String body = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, body)) return false;

    // Check if anything changed
    const char* newSsid = doc["wifi_ssid"] | "";
    const char* newPass = doc["wifi_pass"] | "";
    const char* newHost = doc["server_host"] | "";
    uint16_t newPort = doc["server_port"] | (uint16_t)0;

    // Only update if server sent non-empty values
    if (strlen(newSsid) == 0 && strlen(newHost) == 0) return false;

    bool changed = false;
    DeviceConfig updated = current;

    if (strlen(newSsid) > 0 && strcmp(newSsid, current.wifi_ssid) != 0) {
        strlcpy(updated.wifi_ssid, newSsid, sizeof(updated.wifi_ssid));
        changed = true;
    }
    if (strlen(newPass) > 0 && strcmp(newPass, current.wifi_pass) != 0) {
        strlcpy(updated.wifi_pass, newPass, sizeof(updated.wifi_pass));
        changed = true;
    }
    if (strlen(newHost) > 0 && strcmp(newHost, current.server_host) != 0) {
        strlcpy(updated.server_host, newHost, sizeof(updated.server_host));
        changed = true;
    }
    if (newPort > 0 && newPort != current.server_port) {
        updated.server_port = newPort;
        changed = true;
    }

    if (changed) {
        Serial.println("DeviceConfig: server sent updated config, saving to SPIFFS");
        saveDeviceConfig(updated);
        return true;
    }
    return false;
}
