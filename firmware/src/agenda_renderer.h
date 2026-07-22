#pragma once

#include "http_client.h"
#include <HalDisplay.h>

// Full redraw — use when data changes (fetch, completion, view/day change, sleep)
void renderAgenda(HalDisplay& display, const Agenda& agenda, int8_t cursor);

// Fast cursor-only update — redraws only old and new cursor blocks
void updateCursor(HalDisplay& display, const Agenda& agenda, int8_t oldCursor, int8_t newCursor);

// Reset scroll to top (call when switching days/views)
void resetScroll();
void setScroll(uint8_t offset);

// Passive screen — stitch editorial design, shown during deep sleep
void renderPassive(HalDisplay& display, const Agenda& agenda);

// Splash screens (no WiFi needed, renders immediately)
void showSplash(HalDisplay& display, const char* message, bool inverted = false);
void showReschedulingProgress(HalDisplay& display, int percent, const char* message);
void showOtaProgress(HalDisplay& display, int percent);

// Brief error/info toast — renders message, delays 1.2s, then caller re-renders
void showToast(HalDisplay& display, const char* message);

// Instant loading indicator — renders and returns immediately (no delay)
void showLoading(HalDisplay& display, const char* message);

// --- WiFi setup wizard screens (called from wifi_setup.cpp) ---

namespace WifiKeyboard {
    // iPhone-style QWERTY. Three layers, 4 rows each, variable column counts:
    //   Layer 0 — letters (case driven by `shift` state, not a separate layer)
    //   Layer 1 — numbers + main symbols
    //   Layer 2 — additional ASCII symbols (no €£¥ — out of WPA2 ASCII range)
    constexpr uint8_t LAYERS = 3;
    constexpr uint8_t ROWS = 4;

    enum CellType : uint8_t {
        CELL_CHAR,
        CELL_BACKSPACE,
        CELL_SHIFT,
        CELL_LAYER,    // Jump to targetLayer (123 / #+= / ABC keys)
        CELL_SUBMIT,
    };

    struct Cell {
        const char* label;   // Default label (lowercase letters / digit / symbol)
        const char* altLabel;  // Label when shift is active (only used in letter layer); nullptr otherwise
        char ch;             // ASCII inserted on tap (only valid for CELL_CHAR)
        CellType type;
        uint8_t targetLayer; // For CELL_LAYER only
    };

    // Geometry of a single key (used by both the renderer and any hit-test logic).
    struct KeyRect { int16_t x, y, w, h; };

    uint8_t colsInRow(uint8_t layer, uint8_t row);
    uint16_t totalCells(uint8_t layer);
    Cell cellAt(uint8_t layer, uint8_t row, uint8_t col);
    Cell cellAtIndex(uint8_t layer, uint16_t index);
    void indexToRowCol(uint8_t layer, uint16_t index, uint8_t* outRow, uint8_t* outCol);
    KeyRect keyRect(uint8_t layer, uint8_t row, uint8_t col);
}

// "Scanning networks..." splash before list appears
void showWifiScanning(HalDisplay& display);

// Render scrollable network list. selected = currently focused row (0-indexed
// across the full list, not just visible window). scrollOffset = top of window.
// secured[] indicates a lock icon. rssi[] is signal strength dBm (e.g. -65).
void showWifiNetworkList(HalDisplay& display,
                         const char* const* ssids,
                         const int8_t* rssi,
                         const bool* secured,
                         uint8_t count,
                         int8_t selected,
                         uint8_t scrollOffset);

// Render password entry screen: SSID header, password buffer, on-screen keyboard
// with the cell at (cursorRow, cursorCol) of `layer` highlighted. When `shift`
// is true, letter keys render uppercase and the shift cell renders armed.
void showWifiPasswordEntry(HalDisplay& display,
                           const char* ssid,
                           const char* password,
                           uint8_t cursorRow,
                           uint8_t cursorCol,
                           uint8_t layer,
                           bool shift);

// "Connecting to <ssid>..." status
void showWifiConnecting(HalDisplay& display, const char* ssid);

// "Connected!" / "Failed" — caller sleeps after.
void showWifiResult(HalDisplay& display, bool success, const char* message);

// Generic "doing something with the captive portal" status
void showWifiCaptiveStatus(HalDisplay& display, const char* status);

// Detailed error when auto-accept failed — shows MAC + portal URL +
// instructions for the user to sign in on another device.
void showWifiCaptiveError(HalDisplay& display, const char* ssid,
                          const char* macAddress, const char* portalUrl);
