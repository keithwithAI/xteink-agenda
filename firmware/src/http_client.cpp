#include "http_client.h"
#include "device_config.h"
#include "agenda_renderer.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <esp_task_wdt.h>

extern DeviceConfig deviceCfg;

static String buildUrl(const char* path, uint8_t view) {
    return String("http://") + deviceCfg.server_host + ":" + deviceCfg.server_port + path + "?view=" + view;
}

bool fetchAgenda(uint8_t view, int8_t day_offset, Agenda& out) {
    HTTPClient http;
    String url = buildUrl(API_AGENDA, view);
    url += "&day_offset=" + String(day_offset);
    http.begin(url);
    http.setTimeout(5000);
    int code = http.GET();

    if (code != 200) {
        Serial.printf("fetchAgenda: HTTP %d\n", code);
        http.end();
        return false;
    }

    // Buffer the body so we can echo a snippet if JSON parsing fails. The agenda
    // response is small (a few KB for 32 items), so one String read is safe.
    String body = http.getString();
    http.end();

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);

    if (err) {
        Serial.printf("fetchAgenda: JSON error: %s (body %u bytes)\n", err.c_str(), body.length());
        Serial.printf("fetchAgenda: first 200 bytes: %s\n", body.substring(0, 200).c_str());
        return false;
    }

    if (!doc["items"].is<JsonArray>()) {
        Serial.println("fetchAgenda: response missing 'items' array");
        Serial.printf("fetchAgenda: first 200 bytes: %s\n", body.substring(0, 200).c_str());
        return false;
    }

    memset(&out, 0, sizeof(out));
    strlcpy(out.view_name, doc["view_name"] | "All", sizeof(out.view_name));
    strlcpy(out.date_label, doc["date_label"] | "Today", sizeof(out.date_label));
    out.view_count = doc["view_count"] | 1;
    out.day_offset = doc["day_offset"] | 0;
    out.day_progress = doc["day_progress"] | 0.0f;
    out.current_index = doc["current_index"] | -1;
    out.conflict_index = doc["conflict_index"] | -1;
    out.wake_in_sec = doc["wake_in_sec"] | 300;
    out.item_count = 0;

    for (JsonObject item : doc["items"].as<JsonArray>()) {
        if (out.item_count >= MAX_ITEMS) break;
        AgendaItem& a = out.items[out.item_count];
        strlcpy(a.id, item["id"] | "", sizeof(a.id));
        strlcpy(a.title, item["title"] | "", sizeof(a.title));
        strlcpy(a.start, item["start"] | "", sizeof(a.start));
        strlcpy(a.end, item["end"] | "", sizeof(a.end));
        a.duration_min = item["duration_min"] | 30;
        a.is_task = strcmp(item["type"] | "event", "task") == 0;
        a.completed = item["completed"] | false;
        out.item_count++;
    }

    Serial.printf("fetchAgenda: %d items, view=%s\n", out.item_count, out.view_name);
    return true;
}

bool postReschedule(HalDisplay& display) {
    HTTPClient http;
    String url = String("http://") + deviceCfg.server_host + ":" + deviceCfg.server_port + "/api/schedule";
    http.begin(url);
    http.setTimeout(30000);

    showReschedulingProgress(display, 0, "Connecting...");

    int code = http.POST("");
    if (code != 200) {
        Serial.printf("postReschedule: HTTP %d\n", code);
        http.end();
        return false;
    }

    WiFiClient* tcp = http.getStreamPtr();
    if (!tcp) {
        Serial.println("postReschedule: failed to get stream");
        http.end();
        return false;
    }
    tcp->setTimeout(15);  // 15 seconds — GCal fetch can be slow

    bool success = false;
    uint32_t lastDisplayMs = 0;

    while (http.connected()) {
        esp_task_wdt_reset();  // Reschedule can take 30s+ with many tasks
        String line = tcp->readStringUntil('\n');
        if (line.length() == 0) {
            if (!http.connected()) break;
            continue;
        }

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, line);
        if (err) {
            Serial.printf("postReschedule: JSON parse error: %s\n", err.c_str());
            continue;
        }

        if (doc["done"] | false) {
            int scheduled = doc["scheduled"] | 0;
            Serial.printf("postReschedule: done, %d tasks scheduled\n", scheduled);
            showReschedulingProgress(display, 100, "Done!");
            success = true;
            break;
        }

        int pct = doc["pct"] | 0;
        const char* msg = doc["msg"] | "Working...";
        Serial.printf("postReschedule: %d%% %s\n", pct, msg);

        uint32_t now = millis();
        if (now - lastDisplayMs >= 400) {
            showReschedulingProgress(display, pct, msg);
            lastDisplayMs = now;
        }
    }

    http.end();

    if (!success) {
        Serial.println("postReschedule: stream ended without done signal");
    }
    return success;
}

bool postComplete(uint8_t view, uint8_t cursor, int8_t day_offset) {
    HTTPClient http;
    String url = buildUrl(API_COMPLETE, view);
    url += "&cursor=" + String(cursor) + "&day_offset=" + String(day_offset);
    http.begin(url);
    http.setTimeout(5000);
    esp_task_wdt_reset();  // Reset before the blocking POST — 5s is within WDT but be safe
    int code = http.POST("");
    esp_task_wdt_reset();
    http.end();

    if (code != 200) {
        Serial.printf("postComplete: HTTP %d\n", code);
        return false;
    }
    Serial.println("postComplete: ok");
    return true;
}

bool postDeleteEvent(uint8_t view, uint8_t cursor, int8_t day_offset) {
    HTTPClient http;
    String url = buildUrl(API_DELETE_EVENT, view);
    url += "&cursor=" + String(cursor) + "&day_offset=" + String(day_offset);
    http.begin(url);
    http.setTimeout(5000);
    esp_task_wdt_reset();
    int code = http.POST("");
    esp_task_wdt_reset();
    http.end();

    if (code != 200) {
        Serial.printf("postDeleteEvent: HTTP %d\n", code);
        return false;
    }
    Serial.println("postDeleteEvent: ok");
    return true;
}
