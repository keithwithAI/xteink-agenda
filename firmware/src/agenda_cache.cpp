// Offline resilience: cache agenda to SPIFFS and queue completions for retry.
// SPIFFS partition is 3.5MB — plenty for a small JSON cache + queue file.

#include "agenda_cache.h"
#include "http_client.h"
#include "config.h"

#include <Arduino.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>

static bool spiffsReady = false;

static bool ensureSpiffs() {
    if (spiffsReady) return true;
    if (!SPIFFS.begin(true)) {  // true = format if mount fails
        Serial.println("Cache: SPIFFS mount failed");
        return false;
    }
    spiffsReady = true;
    return true;
}

// --- Agenda cache ---

static const char* CACHE_PATH = "/agenda.json";

bool cacheAgenda(const Agenda& agenda) {
    if (!ensureSpiffs()) return false;

    JsonDocument doc;
    doc["view_name"] = agenda.view_name;
    doc["date_label"] = agenda.date_label;
    doc["view_count"] = agenda.view_count;
    doc["day_offset"] = agenda.day_offset;
    doc["day_progress"] = agenda.day_progress;
    doc["item_count"] = agenda.item_count;

    JsonArray items = doc["items"].to<JsonArray>();
    for (uint8_t i = 0; i < agenda.item_count; i++) {
        JsonObject obj = items.add<JsonObject>();
        obj["id"] = agenda.items[i].id;
        obj["title"] = agenda.items[i].title;
        obj["start"] = agenda.items[i].start;
        obj["end"] = agenda.items[i].end;
        obj["duration_min"] = agenda.items[i].duration_min;
        obj["is_task"] = agenda.items[i].is_task;
        obj["completed"] = agenda.items[i].completed;
    }

    File f = SPIFFS.open(CACHE_PATH, "w");
    if (!f) {
        Serial.println("Cache: failed to open for write");
        return false;
    }
    serializeJson(doc, f);
    f.close();

    Serial.printf("Cache: saved %d items\n", agenda.item_count);
    return true;
}

bool loadCachedAgenda(Agenda& out) {
    if (!ensureSpiffs()) return false;
    if (!SPIFFS.exists(CACHE_PATH)) return false;

    File f = SPIFFS.open(CACHE_PATH, "r");
    if (!f) return false;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err) {
        Serial.printf("Cache: JSON error: %s\n", err.c_str());
        return false;
    }

    strlcpy(out.view_name, doc["view_name"] | "Cached", sizeof(out.view_name));
    strlcpy(out.date_label, doc["date_label"] | "", sizeof(out.date_label));
    out.view_count = doc["view_count"] | 1;
    out.day_offset = doc["day_offset"] | 0;
    out.day_progress = doc["day_progress"] | 0.0f;
    out.item_count = 0;

    for (JsonObject item : doc["items"].as<JsonArray>()) {
        if (out.item_count >= MAX_ITEMS) break;
        AgendaItem& a = out.items[out.item_count];
        strlcpy(a.id, item["id"] | "", sizeof(a.id));
        strlcpy(a.title, item["title"] | "", sizeof(a.title));
        strlcpy(a.start, item["start"] | "", sizeof(a.start));
        strlcpy(a.end, item["end"] | "", sizeof(a.end));
        a.duration_min = item["duration_min"] | 30;
        a.is_task = item["is_task"] | false;
        a.completed = item["completed"] | false;
        out.item_count++;
    }

    Serial.printf("Cache: loaded %d items\n", out.item_count);
    return true;
}

// --- Pending completions queue ---

static const char* QUEUE_PATH = "/pending.bin";

// Queue format: pairs of (uint8_t view, uint8_t cursor), max 16 entries
static const int MAX_PENDING = 16;

bool queueCompletion(uint8_t view, uint8_t cursor) {
    if (!ensureSpiffs()) return false;

    // Read existing queue
    uint8_t buf[MAX_PENDING * 2];
    int count = 0;

    if (SPIFFS.exists(QUEUE_PATH)) {
        File f = SPIFFS.open(QUEUE_PATH, "r");
        if (f) {
            size_t fileSize = f.size();
            if (fileSize % 2 != 0) {
                // Corrupted queue file (odd size) — discard
                Serial.println("Cache: corrupted queue file, discarding");
                f.close();
                SPIFFS.remove(QUEUE_PATH);
            } else {
                count = fileSize / 2;
                if (count > MAX_PENDING) count = MAX_PENDING;
                size_t bytesRead = f.read(buf, count * 2);
                f.close();
                if (bytesRead != (size_t)(count * 2)) {
                    Serial.println("Cache: incomplete queue read, discarding");
                    count = 0;
                }
            }
        }
    }

    if (count >= MAX_PENDING) {
        Serial.println("Cache: pending queue full");
        return false;
    }

    // Append new entry
    buf[count * 2] = view;
    buf[count * 2 + 1] = cursor;
    count++;

    File f = SPIFFS.open(QUEUE_PATH, "w");
    if (!f) return false;
    f.write(buf, count * 2);
    f.close();

    Serial.printf("Cache: queued completion (view=%d, cursor=%d), %d pending\n",
                  view, cursor, count);
    return true;
}

int drainPendingCompletions() {
    if (!ensureSpiffs()) return 0;
    if (!SPIFFS.exists(QUEUE_PATH)) return 0;

    File f = SPIFFS.open(QUEUE_PATH, "r");
    if (!f) return 0;

    size_t fileSize = f.size();
    if (fileSize < 2 || fileSize % 2 != 0) {
        f.close();
        SPIFFS.remove(QUEUE_PATH);
        return 0;
    }

    int count = fileSize / 2;
    if (count > MAX_PENDING) count = MAX_PENDING;

    uint8_t buf[MAX_PENDING * 2];
    size_t bytesRead = f.read(buf, count * 2);
    f.close();

    if (bytesRead != (size_t)(count * 2)) {
        Serial.println("Cache: incomplete queue read during drain");
        SPIFFS.remove(QUEUE_PATH);
        return 0;
    }

    Serial.printf("Cache: replaying %d pending completions\n", count);

    int success = 0;
    for (int i = 0; i < count; i++) {
        uint8_t view = buf[i * 2];
        uint8_t cursor = buf[i * 2 + 1];
        if (postComplete(view, cursor)) {
            success++;
            Serial.printf("Cache: replayed completion %d/%d ok\n", i + 1, count);
        } else {
            Serial.printf("Cache: replayed completion %d/%d FAILED\n", i + 1, count);
        }
    }

    // Clear queue regardless (failed ones will re-appear next sync)
    SPIFFS.remove(QUEUE_PATH);

    return success;
}
