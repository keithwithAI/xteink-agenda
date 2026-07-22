#include <Arduino.h>
#include <WiFi.h>
#include <atomic>
#include <esp_task_wdt.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>

#include "config.h"
#include "device_config.h"
#include "http_client.h"
#include "agenda_renderer.h"
#include "agenda_cache.h"
#include "power_manager.h"
#include "ota_update.h"
#include "wifi_setup.h"

// CrossPoint global hardware instances (defined in HAL .cpp files, declared extern in headers)

// Application state
static Agenda agenda = {};
static RtcState state;
static bool usingCachedData = false;
DeviceConfig deviceCfg;  // Runtime config — loaded from SPIFFS, falls back to config.h defaults

// Background button polling — accumulates press counts during display refresh
// so that presses landing during the ~600 ms e-ink update don't get lost.
static std::atomic<int8_t> pendingBtn[HalGPIO::BTN_POWER + 1] = {};
static TaskHandle_t btnTaskHandle = nullptr;

static void buttonPollTask(void* param) {
    while (true) {
        gpio.update();
        if (gpio.wasPressed(HalGPIO::BTN_LEFT))    pendingBtn[HalGPIO::BTN_LEFT]++;
        if (gpio.wasPressed(HalGPIO::BTN_RIGHT))   pendingBtn[HalGPIO::BTN_RIGHT]++;
        if (gpio.wasPressed(HalGPIO::BTN_CONFIRM))  pendingBtn[HalGPIO::BTN_CONFIRM]++;
        if (gpio.wasPressed(HalGPIO::BTN_BACK))    pendingBtn[HalGPIO::BTN_BACK]++;
        if (gpio.wasPressed(HalGPIO::BTN_UP))      pendingBtn[HalGPIO::BTN_UP]++;
        if (gpio.wasPressed(HalGPIO::BTN_DOWN))    pendingBtn[HalGPIO::BTN_DOWN]++;
        if (gpio.wasPressed(HalGPIO::BTN_POWER))   pendingBtn[HalGPIO::BTN_POWER]++;

        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

// Non-static so wifi_setup.cpp can drain button presses during setup wizard.
int8_t consumeBtn(uint8_t btn) {
    return pendingBtn[btn].exchange(0);
}

static int8_t clampDayOffset(int value) {
    if (value < DAY_OFFSET_MIN) return DAY_OFFSET_MIN;
    if (value > DAY_OFFSET_MAX) return DAY_OFFSET_MAX;
    return (int8_t)value;
}

// Connect using the SSID/password stored in DeviceConfig only — no UI
// fallback. Used in the steady-state loop (reconnectWifi) where we don't want
// to launch the setup wizard for transient drops.
static bool tryConnectSavedCreds() {
    if (strlen(deviceCfg.wifi_ssid) == 0) return false;
    WiFi.mode(WIFI_STA);
    WiFi.begin(deviceCfg.wifi_ssid, deviceCfg.wifi_pass);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
        delay(100);
    }
    return WiFi.status() == WL_CONNECTED;
}

// Boot-time WiFi: try saved creds first, otherwise launch the on-device setup
// wizard so the user can pick a network + type a password without a phone.
// Returns true iff WiFi is connected on exit.
static bool connectWifi() {
    if (tryConnectSavedCreds()) {
        Serial.printf("WiFi: %s\n", WiFi.localIP().toString().c_str());
        return true;
    }
    Serial.println("WiFi: saved creds missing or failed → on-device setup");
    if (runWifiSetup(deviceCfg)) {
        Serial.printf("WiFi: %s (after setup)\n", WiFi.localIP().toString().c_str());
        return true;
    }
    return false;
}

static uint32_t wifiBackoffUntil = 0;
static uint32_t wifiBackoffMs = WIFI_BACKOFF_MIN_MS;

// Mid-session reconnect: silent retry with backoff. Never launches the setup
// wizard — that would hijack the screen for routine router blips.
static bool reconnectWifi() {
    if (WiFi.status() == WL_CONNECTED) {
        wifiBackoffMs = WIFI_BACKOFF_MIN_MS;
        return true;
    }
    if (millis() < wifiBackoffUntil) return false;
    bool ok = tryConnectSavedCreds();
    if (ok) {
        wifiBackoffMs = WIFI_BACKOFF_MIN_MS;
    } else {
        wifiBackoffUntil = millis() + wifiBackoffMs;
        Serial.printf("WiFi backoff %u ms\n", wifiBackoffMs);
        wifiBackoffMs = wifiBackoffMs * 2;
        if (wifiBackoffMs > WIFI_BACKOFF_MAX_MS) wifiBackoffMs = WIFI_BACKOFF_MAX_MS;
    }
    return ok;
}

static void disconnectWifi() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}

// Shared temp buffer for agenda fetches — too large for stack (~3.6KB with MAX_ITEMS=32)
static Agenda _fetchBuf;

static bool fetchAndRender() {
    Serial.println("  fetchAgenda..."); Serial.flush();
    memset(&_fetchBuf, 0, sizeof(_fetchBuf));
    if (!fetchAgenda(state.view, state.day_offset, _fetchBuf)) {
        Serial.println("  Fetch FAILED, keeping current display");
        return false;
    }
    agenda = _fetchBuf;
    Serial.printf("  Fetched %d items\n", agenda.item_count); Serial.flush();
    usingCachedData = false;
    cacheAgenda(agenda);  // Save for offline use
    state.item_count = agenda.item_count;
    if (state.cursor >= 0) {
        // Start at current task (the one happening now) if available
        if (agenda.current_index >= 0 && agenda.current_index < (int8_t)state.item_count) {
            state.cursor = agenda.current_index;
            setScroll(agenda.current_index);  // Current task at top of screen
        } else if (state.cursor >= (int8_t)state.item_count) {
            state.cursor = state.item_count > 0 ? (int8_t)(state.item_count - 1) : -1;
        }
    }
    rtcSave(state);
    Serial.println("  renderAgenda..."); Serial.flush();
    renderAgenda(display, agenda, state.cursor);
    Serial.println("  render DONE"); Serial.flush();
    return true;
}

void setup() {
    Serial.begin(115200);
    delay(500);  // Extra delay for USB CDC to stabilize
    Serial.println("\n=== xteink-agenda X4 ===");
    Serial.flush();

    // Init hardware (CrossPoint HAL) — order matters
    Serial.println("INIT 1: gpio.begin()"); Serial.flush();
    gpio.begin();
    Serial.println("INIT 2: powerManager.begin()"); Serial.flush();
    powerManager.begin();
    Serial.println("INIT 3: display.begin()"); Serial.flush();
    display.begin();
    // Show splash immediately — user sees feedback before WiFi connects
    showSplash(display, "Agenda");

    // Start button polling task EARLY — must run even if WiFi fails or OTA triggers
    Serial.println("INIT 4: buttonPollTask"); Serial.flush();
    if (xTaskCreate(buttonPollTask, "btn", 3072, NULL, 1, &btnTaskHandle) != pdPASS) {
        Serial.println("ERROR: button task creation failed (OOM?)");
    }

    Serial.println("INIT 5: loadDeviceConfig()"); Serial.flush();
    deviceCfg = loadDeviceConfig();

    Serial.println("INIT 6: rtcLoad()"); Serial.flush();
    state = rtcLoad();

    // Every boot = interactive mode (X4 power button cuts power entirely —
    // no deep sleep timer wake possible, every wake is a cold boot)
    Serial.println("INIT 7: interactive mode (cold boot)"); Serial.flush();
    if (state.cursor < 0) state.cursor = 0;
    state.day_offset = 0;

    Serial.println("INIT 8: connectWifi()"); Serial.flush();
    if (!connectWifi()) {
        // Offline: show cached agenda instead of blank screen
        if (loadCachedAgenda(agenda)) {
            usingCachedData = true;
            state.item_count = agenda.item_count;
            rtcSave(state);
            renderAgenda(display, agenda, state.cursor);
            Serial.println("Showing cached agenda (offline)");
        } else {
            rtcSave(state);
            display.deepSleep();
            enterDeepSleep();
        }
        return;
    }

    // Online: drain pending completions first, then OTA check, then fetch
    Serial.println("INIT 9: drainPendingCompletions()"); Serial.flush();
    int replayed = drainPendingCompletions();
    if (replayed > 0) {
        Serial.printf("Replayed %d offline completions\n", replayed);
    }

    // Check for OTA update before fetching agenda
    Serial.println("INIT 10: checkAndApplyOta()"); Serial.flush();
    if (checkAndApplyOta() == OTA_UPDATED) return;  // Rebooting

    // Check if server has updated device config (WiFi, server host, etc.)
    Serial.println("INIT 11: fetchAndUpdateConfig()"); Serial.flush();
    if (fetchAndUpdateConfig(deviceCfg)) {
        Serial.println("Config updated — will apply on next boot");
    }

    Serial.println("INIT 12: fetchAndRender()"); Serial.flush();
    resetScroll();
    fetchAndRender();
    Serial.printf("  cursor=%d, current_index=%d, item_count=%d\n",
                  state.cursor, agenda.current_index, agenda.item_count);

    // Discard any power button press accumulated during boot — the press that
    // woke the device would otherwise be consumed as "go to sleep" in loop().
    consumeBtn(HalGPIO::BTN_POWER);

    // Watchdog: auto-reboot if loop() hangs for >60s (e.g., stuck HTTP request)
    esp_task_wdt_config_t wdt_cfg = { .timeout_ms = 60000, .idle_core_mask = 0, .trigger_panic = true };
    esp_task_wdt_reconfigure(&wdt_cfg);
    esp_task_wdt_add(NULL);

    Serial.println("INIT 13: done, WiFi stays on for interactive mode"); Serial.flush();
}

void loop() {
    esp_task_wdt_reset();
    static uint32_t lastActivity = millis();

    // Button task accumulates presses in pendingBtn[] — read counters here
    // No gpio.update() in main loop (task handles it)

    // --- Cursor UP (left rocker) ---
    int8_t ups = consumeBtn(HalGPIO::BTN_LEFT);
    if (ups > 0) {
        lastActivity = millis();
        int8_t oldCursor = state.cursor;
        if (state.cursor < 0) {
            state.cursor = 0;
        } else if (state.cursor > 0) {
            for (int i = 0; i < ups && state.cursor > 0; i++) state.cursor--;
        } else {
            // At first item: go to previous day, cursor at last item
            int8_t prev = state.day_offset;
            state.day_offset = clampDayOffset(state.day_offset - 1);
            if (state.day_offset == prev) {
                // Already at DAY_OFFSET_MIN — nothing to do
                showToast(display, "Earliest day");
                renderAgenda(display, agenda, state.cursor);
                oldCursor = -99;
            } else {
                resetScroll();
                showLoading(display, "Loading...");
                if (reconnectWifi() && fetchAndRender()) {
                    state.cursor = state.item_count > 0 ? (int8_t)(state.item_count - 1) : 0;
                    rtcSave(state);
                    renderAgenda(display, agenda, state.cursor);
                } else {
                    state.day_offset = prev;  // Revert — fetch failed
                    showToast(display, "No WiFi");
                    renderAgenda(display, agenda, state.cursor);
                }
                oldCursor = -99;
            }
        }
        if (oldCursor != -99) {
            rtcSave(state);
            if (oldCursor < 0) renderAgenda(display, agenda, state.cursor);
            else updateCursor(display, agenda, oldCursor, state.cursor);
        }
    }

    // --- Cursor DOWN (right rocker) ---
    int8_t downs = consumeBtn(HalGPIO::BTN_RIGHT);
    if (downs > 0) {
        lastActivity = millis();
        int8_t oldCursor = state.cursor;
        if (state.cursor < 0) {
            state.cursor = 0;
        } else {
            for (int i = 0; i < downs; i++) {
                if (state.cursor < (int8_t)(state.item_count - 1)) {
                    state.cursor++;
                } else {
                    // Past last item: advance to next day (if not at horizon)
                    int8_t prev = state.day_offset;
                    state.day_offset = clampDayOffset(state.day_offset + 1);
                    if (state.day_offset == prev) {
                        showToast(display, "Furthest day");
                        renderAgenda(display, agenda, state.cursor);
                        oldCursor = -99;
                        break;
                    }
                    state.cursor = 0;
                    resetScroll();
                    showLoading(display, "Loading...");
                    if (reconnectWifi() && fetchAndRender()) {
                        rtcSave(state);
                    } else {
                        state.day_offset = prev;  // Revert — fetch failed
                        state.cursor = (int8_t)(state.item_count - 1);
                        showToast(display, "Server offline");
                        renderAgenda(display, agenda, state.cursor);
                    }
                    oldCursor = -99;  // Signal: already rendered
                    break;
                }
            }
        }
        if (oldCursor != -99) {
            rtcSave(state);
            if (oldCursor < 0) renderAgenda(display, agenda, state.cursor);
            else updateCursor(display, agenda, oldCursor, state.cursor);
        }
    }

    // --- Confirm button (context-aware: complete task OR delete event) ---
    // Two-press guard state for event deletion — armed on first press, executes
    // on second press within 2s. Resets if cursor moves or window expires.
    static uint32_t lastDeletePressMs = 0;
    static int8_t lastDeleteCursor = -1;
    if (consumeBtn(HalGPIO::BTN_CONFIRM) > 0) {
        lastActivity = millis();
        if (state.cursor >= 0 && state.cursor < (int8_t)agenda.item_count) {
            auto& item = agenda.items[state.cursor];
            if (item.is_task && !item.completed) {
                Serial.printf("Completing: %s\n", item.title);
                uint8_t completedIdx = state.cursor;

                // Optimistic: mark completed + advance cursor + render
                item.completed = true;
                if (state.cursor < (int8_t)(state.item_count - 1)) {
                    state.cursor++;
                }
                cacheAgenda(agenda);
                rtcSave(state);
                renderAgenda(display, agenda, state.cursor);

                // Sync to server (no fetchAndRender — local state is correct)
                if (reconnectWifi()) {
                    if (!postComplete(state.view, completedIdx, state.day_offset)) {
                        queueCompletion(state.view, completedIdx);
                        Serial.println("Queued completion for retry (server error)");
                    }
                } else {
                    queueCompletion(state.view, completedIdx);
                    Serial.println("Queued completion for retry (offline)");
                }
            } else if (!item.is_task) {
                // Event deletion requires two-press confirmation: destructive and
                // online-only, so we don't want to queue it or one-tap it.
                uint32_t nowMs = millis();
                bool second = (lastDeleteCursor == state.cursor
                               && nowMs - lastDeletePressMs < 2000);
                if (!second) {
                    lastDeletePressMs = nowMs;
                    lastDeleteCursor = state.cursor;
                    showToast(display, "Press again to delete");
                    renderAgenda(display, agenda, state.cursor);
                } else {
                    lastDeletePressMs = 0;
                    lastDeleteCursor = -1;
                    Serial.printf("Deleting event: %s\n", item.title);
                    uint8_t deletedIdx = state.cursor;
                    if (reconnectWifi()) {
                        if (postDeleteEvent(state.view, deletedIdx, state.day_offset)) {
                            showToast(display, "Event deleted");
                            fetchAndRender();
                        } else {
                            showToast(display, "Delete failed");
                            renderAgenda(display, agenda, state.cursor);
                        }
                    } else {
                        showToast(display, "No WiFi");
                        renderAgenda(display, agenda, state.cursor);
                    }
                }
            }
        }
    }

    // --- Reschedule (back button) ---
    if (consumeBtn(HalGPIO::BTN_BACK) > 0) {
        lastActivity = millis();
        Serial.println("Reschedule all...");
        if (reconnectWifi()) {
            if (postReschedule(display)) {
                fetchAndRender();
            } else {
                showToast(display, "Reschedule failed");
                renderAgenda(display, agenda, state.cursor);
            }
        } else {
            showToast(display, "No WiFi");
            renderAgenda(display, agenda, state.cursor);
        }
        lastActivity = millis();  // Reset — don't count reschedule time as idle
    }

    // --- Previous day (volume up) — batch rapid presses ---
    {
        int8_t presses = consumeBtn(HalGPIO::BTN_UP);
        if (presses > 0) {
            lastActivity = millis();
            int8_t prev = state.day_offset;
            state.day_offset = clampDayOffset(state.day_offset - presses);
            if (state.day_offset == prev) {
                showToast(display, "Earliest day");
                renderAgenda(display, agenda, state.cursor);
            } else {
                state.cursor = state.cursor >= 0 ? 0 : -1;
                resetScroll();
                showLoading(display, "Loading...");
                if (reconnectWifi() && fetchAndRender()) {
                    rtcSave(state);
                } else {
                    state.day_offset = prev;  // Revert
                    showToast(display, "Server offline");
                    renderAgenda(display, agenda, state.cursor);
                }
            }
        }
    }

    // --- Next day (volume down) — batch rapid presses ---
    {
        int8_t presses = consumeBtn(HalGPIO::BTN_DOWN);
        if (presses > 0) {
            lastActivity = millis();
            int8_t prev = state.day_offset;
            state.day_offset = clampDayOffset(state.day_offset + presses);
            if (state.day_offset == prev) {
                showToast(display, "Furthest day");
                renderAgenda(display, agenda, state.cursor);
            } else {
                state.cursor = state.cursor >= 0 ? 0 : -1;
                resetScroll();
                showLoading(display, "Loading...");
                if (reconnectWifi() && fetchAndRender()) {
                    rtcSave(state);
                } else {
                    state.day_offset = prev;  // Revert
                    showToast(display, "Server offline");
                    renderAgenda(display, agenda, state.cursor);
                }
            }
        }
    }

    // --- Sleep (power button) ---
    if (consumeBtn(HalGPIO::BTN_POWER) > 0) {
        Serial.println("Power button -> sleep");
        state.cursor = -1;
        if (state.day_offset != 0) {
            state.day_offset = 0;
            if (reconnectWifi()) {
                memset(&_fetchBuf, 0, sizeof(_fetchBuf));
                if (fetchAgenda(state.view, 0, _fetchBuf)) {
                    agenda = _fetchBuf;
                    cacheAgenda(agenda);
                }
            }
        }
        rtcSave(state);
        // Single FULL_REFRESH directly to black passive screen
        renderPassive(display, agenda);
        if (btnTaskHandle) { vTaskDelete(btnTaskHandle); btnTaskHandle = nullptr; }
        enterDeepSleep();
    }

    // Inactivity timeout
    if (millis() - lastActivity > INACTIVITY_TIMEOUT_MS) {
        Serial.println("Inactivity timeout -> sleep");
        state.cursor = -1;
        if (state.day_offset != 0) {
            state.day_offset = 0;
            if (reconnectWifi()) {
                memset(&_fetchBuf, 0, sizeof(_fetchBuf));
                if (fetchAgenda(state.view, 0, _fetchBuf)) {
                    agenda = _fetchBuf;
                    cacheAgenda(agenda);
                }
            }
        }
        rtcSave(state);
        // Single FULL_REFRESH directly to black passive screen
        renderPassive(display, agenda);
        if (btnTaskHandle) { vTaskDelete(btnTaskHandle); btnTaskHandle = nullptr; }
        enterDeepSleep();
    }

    delay(10);
}
