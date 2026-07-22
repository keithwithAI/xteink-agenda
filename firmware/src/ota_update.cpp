// OTA firmware update — uses HTTPClient + esp_ota_ops for plain HTTP OTA.
// Checks our server for a newer version, downloads and flashes if available.

#include "ota_update.h"
#include "config.h"
#include "device_config.h"
#include "agenda_renderer.h"
#include <HalPowerManager.h>

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <esp_task_wdt.h>
#include <ArduinoJson.h>
#include <HalDisplay.h>

#include "esp_ota_ops.h"

extern HalDisplay display;

// --- Semver comparison ---

static bool isNewer(const char* latest, const char* current) {
    int curMajor = 0, curMinor = 0, curPatch = 0;
    int latMajor = 0, latMinor = 0, latPatch = 0;

    sscanf(current, "%d.%d.%d", &curMajor, &curMinor, &curPatch);
    sscanf(latest, "%d.%d.%d", &latMajor, &latMinor, &latPatch);

    if (latMajor != curMajor) return latMajor > curMajor;
    if (latMinor != curMinor) return latMinor > curMinor;
    return latPatch > curPatch;
}

// --- Version check via our server ---

struct UpdateInfo {
    char version[16];
    char url[128];
    size_t size;
    bool available;
};

static bool checkForUpdate(UpdateInfo& info) {
    HTTPClient http;
    extern DeviceConfig deviceCfg;
    String url = String("http://") + deviceCfg.server_host + ":" + deviceCfg.server_port + API_FIRMWARE_VERSION;
    Serial.printf("OTA: checking %s\n", url.c_str());
    http.begin(url);
    http.setTimeout(5000);
    int code = http.GET();

    if (code != 200) {
        Serial.printf("OTA: version check HTTP %d\n", code);
        http.end();
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream());
    http.end();

    if (err) {
        Serial.printf("OTA: JSON error: %s\n", err.c_str());
        return false;
    }

    strlcpy(info.version, doc["version"] | "0.0.0", sizeof(info.version));
    strlcpy(info.url, doc["url"] | "", sizeof(info.url));
    info.size = doc["size"] | 0;
    info.available = isNewer(info.version, FIRMWARE_VERSION);

    if (info.available) {
        Serial.printf("OTA: update available %s -> %s (%zu bytes)\n",
                       FIRMWARE_VERSION, info.version, info.size);
    } else {
        Serial.printf("OTA: up to date (%s)\n", FIRMWARE_VERSION);
    }
    return true;
}

// --- Download + flash using HTTPClient + esp_ota_ops ---

static OtaResult installUpdate(const UpdateInfo& info) {
    Serial.printf("OTA: downloading from %s\n", info.url);

    // Show "Update available" on screen
    showOtaProgress(display, -1);

    // Find the next OTA partition to write to
    const esp_partition_t* target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        Serial.println("OTA: no update partition found");
        return OTA_FAILED;
    }
    Serial.printf("OTA: writing to partition '%s' at 0x%x\n", target->label, target->address);

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        Serial.printf("OTA: begin failed: %s\n", esp_err_to_name(err));
        return OTA_FAILED;
    }

    // Download binary via HTTPClient (proven to work — same as agenda fetch)
    HTTPClient http;
    http.begin(info.url);
    http.setTimeout(30000);
    int code = http.GET();

    if (code != 200) {
        Serial.printf("OTA: download HTTP %d\n", code);
        http.end();
        esp_ota_abort(ota_handle);
        return OTA_FAILED;
    }

    int contentLength = http.getSize();
    Serial.printf("OTA: downloading %d bytes\n", contentLength);

    WiFiClient* tcp = http.getStreamPtr();
    if (!tcp) {
        Serial.println("OTA: failed to get stream");
        http.end();
        esp_ota_abort(ota_handle);
        return OTA_FAILED;
    }
    uint8_t buf[4096];
    size_t written = 0;
    int lastPercent = -1;

    uint32_t lastDataTime = millis();
    while (http.connected() && (contentLength < 0 || written < (size_t)contentLength)) {
        size_t available = tcp->available();
        if (available == 0) {
            delay(1);
            if (millis() - lastDataTime > 30000) {
                Serial.println("OTA: download stalled (30s timeout)");
                http.end();
                esp_ota_abort(ota_handle);
                return OTA_FAILED;
            }
            continue;
        }
        lastDataTime = millis();
        size_t toRead = (available < sizeof(buf)) ? available : sizeof(buf);
        int bytesRead = tcp->readBytes(buf, toRead);
        if (bytesRead <= 0) break;

        err = esp_ota_write(ota_handle, buf, bytesRead);
        if (err != ESP_OK) {
            Serial.printf("OTA: write failed: %s\n", esp_err_to_name(err));
            http.end();
            esp_ota_abort(ota_handle);
            return OTA_FAILED;
        }

        written += bytesRead;
        esp_task_wdt_reset();  // OTA downloads can exceed 60s on slow networks
        if (contentLength > 0) {
            int percent = (int)((uint64_t)written * 100 / contentLength);
            if (percent / 5 != lastPercent / 5) {
                Serial.printf("OTA: %d%%\n", percent);
                showOtaProgress(display, percent);
                lastPercent = percent;
            }
        }
    }

    http.end();
    Serial.printf("OTA: wrote %zu bytes\n", written);

    if (contentLength > 0 && written != (size_t)contentLength) {
        Serial.println("OTA: incomplete download");
        esp_ota_abort(ota_handle);
        return OTA_FAILED;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        Serial.printf("OTA: end failed: %s\n", esp_err_to_name(err));
        return OTA_FAILED;
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        Serial.printf("OTA: set boot partition failed: %s\n", esp_err_to_name(err));
        return OTA_FAILED;
    }

    Serial.println("OTA: update complete, rebooting...");
    showOtaProgress(display, 100);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(500);
    return OTA_UPDATED;
}

// --- Public API ---

OtaResult checkAndApplyOta() {
    // Mark the CURRENTLY-RUNNING image valid (not the one we may flash below).
    // After a successful OTA, the new image boots in PENDING_VERIFY state — if it
    // doesn't call this within the rollback window, the bootloader reverts to the
    // previous partition on next reboot. Calling this here means: "we booted far
    // enough to reach the OTA check, this image is OK." The newly-flashed image
    // must make its own call after its own successful boot.
    esp_ota_mark_app_valid_cancel_rollback();

    UpdateInfo info = {};

    if (!checkForUpdate(info)) {
        return OTA_FAILED;
    }

    if (!info.available) {
        return OTA_UP_TO_DATE;
    }

    // Battery check — skip OTA if battery too low (risk of dying mid-flash)
    extern HalPowerManager powerManager;
    uint16_t battPct = powerManager.getBatteryPercentage();
    if (battPct > 0 && battPct < 20) {
        Serial.printf("OTA: skipping update, battery too low (%d%%)\n", battPct);
        return OTA_FAILED;
    }

    OtaResult result = installUpdate(info);
    if (result == OTA_UPDATED) {
        ESP.restart();
        // Does not return
    }
    return result;
}
