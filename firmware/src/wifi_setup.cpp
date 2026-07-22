#include "wifi_setup.h"

#include <WiFi.h>
#include <HalDisplay.h>
#include <HalGPIO.h>

#include "agenda_renderer.h"
#include "captive_portal.h"
#include "device_config.h"

// Defined in main.cpp — exchanges the per-button press counter accumulated by
// the FreeRTOS button polling task.
int8_t consumeBtn(uint8_t btn);

namespace {

constexpr uint8_t MAX_NETWORKS = 16;
constexpr uint8_t LIST_VISIBLE = 10;        // Must match WIFI_LIST_VISIBLE in renderer
constexpr uint16_t CONNECT_TIMEOUT_MS = 15000;
constexpr uint8_t MAX_PASS_LEN = 63;        // WPA2 max
constexpr uint8_t MIN_PASS_LEN = 8;         // WPA2 min

struct ScannedNet {
    char ssid[33];
    int8_t rssi;
    bool secured;
};

ScannedNet g_nets[MAX_NETWORKS];
uint8_t g_netCount = 0;

void drainAllButtons() {
    for (uint8_t i = 0; i <= HalGPIO::BTN_POWER; i++) consumeBtn(i);
}

void scanNetworks() {
    showWifiScanning(display);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/false,
                              /*passive=*/false, /*max_ms_per_chan=*/300);
    if (n < 0) n = 0;

    g_netCount = 0;
    for (int i = 0; i < n && g_netCount < MAX_NETWORKS; i++) {
        const String s = WiFi.SSID(i);
        if (s.length() == 0) continue;
        // Dedup by SSID (multiple BSSIDs of the same name return separately)
        bool dup = false;
        for (uint8_t j = 0; j < g_netCount; j++) {
            if (s.equals(g_nets[j].ssid)) { dup = true; break; }
        }
        if (dup) continue;

        strlcpy(g_nets[g_netCount].ssid, s.c_str(), sizeof(g_nets[g_netCount].ssid));
        g_nets[g_netCount].rssi = (int8_t)WiFi.RSSI(i);
        g_nets[g_netCount].secured = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
        g_netCount++;
    }
    WiFi.scanDelete();
    Serial.printf("WiFi: scanned %d networks (%u unique)\n", n, g_netCount);
}

void renderList(int8_t selected, uint8_t scrollOffset) {
    const char* ssidPtrs[MAX_NETWORKS] = {nullptr};
    int8_t rssiArr[MAX_NETWORKS] = {0};
    bool securedArr[MAX_NETWORKS] = {false};
    for (uint8_t i = 0; i < g_netCount; i++) {
        ssidPtrs[i] = g_nets[i].ssid;
        rssiArr[i] = g_nets[i].rssi;
        securedArr[i] = g_nets[i].secured;
    }
    showWifiNetworkList(display, ssidPtrs, rssiArr, securedArr,
                        g_netCount, selected, scrollOffset);
}

// Returns selected network index (0..g_netCount-1) or -1 if user cancelled.
// Only rockers move the selection (volume buttons are silent). Each rocker tap
// moves the cursor exactly one row — queued taps accumulated during the e-ink
// refresh are all applied in a single update.
int8_t selectNetwork() {
    int8_t selected = g_netCount > 0 ? 0 : -1;
    uint8_t scrollOffset = 0;
    renderList(selected, scrollOffset);

    auto move = [&](int delta) {
        if (g_netCount == 0) return false;
        int8_t prev = selected;
        // Wrap-around modular arithmetic
        selected = (int8_t)(((selected + delta) % g_netCount + g_netCount) % g_netCount);
        if (selected < (int8_t)scrollOffset) scrollOffset = (uint8_t)selected;
        else if (selected >= (int8_t)(scrollOffset + LIST_VISIBLE))
            scrollOffset = (uint8_t)(selected - LIST_VISIBLE + 1);
        return prev != selected;
    };

    while (true) {
        if (consumeBtn(HalGPIO::BTN_POWER) > 0) return -1;

        if (consumeBtn(HalGPIO::BTN_BACK) > 0) {
            scanNetworks();
            selected = g_netCount > 0 ? 0 : -1;
            scrollOffset = 0;
            renderList(selected, scrollOffset);
            continue;
        }

        bool changed = false;
        int8_t leftTaps = consumeBtn(HalGPIO::BTN_LEFT);
        int8_t rightTaps = consumeBtn(HalGPIO::BTN_RIGHT);
        if (leftTaps > 0)  changed = move(-leftTaps) || changed;
        if (rightTaps > 0) changed = move(rightTaps) || changed;

        if (changed) renderList(selected, scrollOffset);

        if (g_netCount > 0 && consumeBtn(HalGPIO::BTN_CONFIRM) > 0) return selected;

        delay(20);
    }
}

// Returns true if user submitted a password; copies into outPass.
// Returns false if user cancelled.
//
// Cursor walks the keyboard in reading order (left-to-right, top-to-bottom,
// wrapping at the ends) — like the iPod Click Wheel scrubbing through its
// alphabet strip. Only the rockers move the cursor; volume buttons are
// deliberately unused so users mashing them don't trigger surprises.
bool enterPassword(const char* ssid, char* outPass, size_t outPassLen) {
    char buf[MAX_PASS_LEN + 1] = {0};
    size_t len = 0;
    uint8_t layer = 0;
    bool shift = false;  // Sticky for one keypress, then auto-reverts (iPhone-style)
    int cursor = 0;
    auto clampCursor = [&]() {
        int total = (int)WifiKeyboard::totalCells(layer);
        if (total <= 0) { cursor = 0; return; }
        if (cursor >= total) cursor = total - 1;
        if (cursor < 0) cursor = 0;
    };

    auto render = [&]() {
        uint8_t r = 0, c = 0;
        WifiKeyboard::indexToRowCol(layer, (uint16_t)cursor, &r, &c);
        showWifiPasswordEntry(display, ssid, buf, r, c, layer, shift);
    };
    render();

    while (true) {
        // BACK = cancel back to network list. Power = also cancel.
        if (consumeBtn(HalGPIO::BTN_POWER) > 0) return false;
        if (consumeBtn(HalGPIO::BTN_BACK) > 0)  return false;

        bool changed = false;

        // Each rocker tap moves the cursor one cell, wrapping at the ends.
        // Queued taps accumulated during a refresh apply in a single update.
        int total = (int)WifiKeyboard::totalCells(layer);
        if (total > 0) {
            int8_t leftTaps = consumeBtn(HalGPIO::BTN_LEFT);
            int8_t rightTaps = consumeBtn(HalGPIO::BTN_RIGHT);
            if (leftTaps > 0) {
                cursor = ((cursor - leftTaps) % total + total) % total;
                changed = true;
            }
            if (rightTaps > 0) {
                cursor = (cursor + rightTaps) % total;
                changed = true;
            }
        }

        // Confirm = act on the cell at cursor.
        if (consumeBtn(HalGPIO::BTN_CONFIRM) > 0) {
            WifiKeyboard::Cell cell = WifiKeyboard::cellAtIndex(layer, (uint16_t)cursor);
            switch (cell.type) {
                case WifiKeyboard::CELL_CHAR:
                    if (len < MAX_PASS_LEN) {
                        char ch = cell.ch;
                        if (shift && ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
                        buf[len++] = ch;
                        buf[len] = '\0';
                        if (shift) shift = false;  // auto-revert after one char
                    }
                    break;
                case WifiKeyboard::CELL_BACKSPACE:
                    if (len > 0) buf[--len] = '\0';
                    break;
                case WifiKeyboard::CELL_SHIFT:
                    shift = !shift;
                    break;
                case WifiKeyboard::CELL_LAYER:
                    layer = cell.targetLayer;
                    if (layer >= WifiKeyboard::LAYERS) layer = 0;
                    shift = false;  // letter-shift doesn't carry into symbol layers
                    clampCursor();
                    break;
                case WifiKeyboard::CELL_SUBMIT:
                    if (len >= MIN_PASS_LEN) {
                        strlcpy(outPass, buf, outPassLen);
                        return true;
                    }
                    showWifiResult(display, false, "Need at least 8 characters");
                    delay(1500);
                    break;  // re-render below
            }
            changed = true;
        }

        if (changed) render();
        delay(20);
    }
}

// Connect status, including the captive-portal stage. We split the "associated
// to AP" check from the "internet actually works" check because hotel/airport
// portals let you associate but block all real traffic until you sign in.
enum class ConnectOutcome {
    Connected,        // Real internet works
    NoSuchSsid,       // Network not found
    AuthFailed,       // Wrong password / association timed out
    CaptivePortal,    // Associated but stuck behind a portal we can't dismiss
    Cancelled,        // User pressed power
};

ConnectOutcome tryConnect(const char* ssid, const char* pass) {
    showWifiConnecting(display, ssid);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
    if (pass && pass[0]) WiFi.begin(ssid, pass);
    else WiFi.begin(ssid);

    // Phase 1: wait for AP association.
    uint32_t start = millis();
    while (millis() - start < CONNECT_TIMEOUT_MS) {
        wl_status_t s = WiFi.status();
        if (s == WL_CONNECTED) break;
        if (s == WL_NO_SSID_AVAIL) {
            WiFi.disconnect(true);  // Don't leave half-failed STA state for the retry
            return ConnectOutcome::NoSuchSsid;
        }
        if (consumeBtn(HalGPIO::BTN_POWER) > 0) {
            WiFi.disconnect(true);
            return ConnectOutcome::Cancelled;
        }
        delay(150);
    }
    if (WiFi.status() != WL_CONNECTED) {
        WiFi.disconnect(true);  // Auth timed out — clear the slate before the user retries
        return ConnectOutcome::AuthFailed;
    }

    // Phase 2: probe for real internet, and try to auto-accept any captive
    // portal we land behind. This is best-effort — works on simple "click I
    // accept" portals; bails on anything that needs human input.
    showWifiCaptiveStatus(display, "Checking connection...");
    auto portalResult = CaptivePortal::tryAutoAccept();
    switch (portalResult) {
        case CaptivePortal::NotPresent:
            return ConnectOutcome::Connected;
        case CaptivePortal::Resolved:
            showWifiCaptiveStatus(display, "Auto-accepted terms");
            delay(1200);
            return ConnectOutcome::Connected;
        case CaptivePortal::Unresolvable:
            return ConnectOutcome::CaptivePortal;
    }
    return ConnectOutcome::Connected;
}

}  // namespace

bool runWifiSetup(DeviceConfig& cfg) {
    drainAllButtons();
    scanNetworks();

    while (true) {
        int8_t idx = selectNetwork();
        if (idx < 0) {
            Serial.println("WiFi setup: cancelled");
            return false;
        }

        const ScannedNet& net = g_nets[idx];
        Serial.printf("WiFi setup: selected %s (%s)\n",
                      net.ssid, net.secured ? "secured" : "open");
        char password[MAX_PASS_LEN + 1] = {0};

        if (net.secured) {
            if (!enterPassword(net.ssid, password, sizeof(password))) {
                // User cancelled out of password screen — back to network list
                continue;
            }
        }

        ConnectOutcome outcome = tryConnect(net.ssid, password);
        if (outcome == ConnectOutcome::Connected) {
            strlcpy(cfg.wifi_ssid, net.ssid, sizeof(cfg.wifi_ssid));
            strlcpy(cfg.wifi_pass, password, sizeof(cfg.wifi_pass));
            saveDeviceConfig(cfg);

            char ipMsg[40];
            snprintf(ipMsg, sizeof(ipMsg), "%s", WiFi.localIP().toString().c_str());
            showWifiResult(display, true, ipMsg);
            delay(1500);
            return true;
        }

        if (outcome == ConnectOutcome::CaptivePortal) {
            // Don't save creds — silent reconnects to a captive network are useless.
            // Show the user a useful page (MAC for "register a device" flows, portal
            // URL for reference) and wait for them to acknowledge.
            String mac = WiFi.macAddress();
            const char* portal = CaptivePortal::lastPortalUrl();
            WiFi.disconnect(true);
            showWifiCaptiveError(display, net.ssid, mac.c_str(), portal);
            // Wait for any button to dismiss
            drainAllButtons();
            while (consumeBtn(HalGPIO::BTN_CONFIRM) == 0 &&
                   consumeBtn(HalGPIO::BTN_BACK) == 0 &&
                   consumeBtn(HalGPIO::BTN_POWER) == 0) {
                delay(50);
            }
            continue;  // Back to network list
        }

        const char* msg = (outcome == ConnectOutcome::NoSuchSsid)
                              ? "Network out of range"
                              : "Wrong password or auth failed";
        showWifiResult(display, false, msg);
        delay(1800);
    }
}
