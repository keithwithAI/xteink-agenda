#include "agenda_renderer.h"
#include "config.h"

#include <GfxRenderer.h>
#include <EpdFont.h>
#include <EpdFontFamily.h>
#include <FontDecompressor.h>
#include <FontCacheManager.h>
#include <HalDisplay.h>
#include <HalPowerManager.h>

// Noto Sans (body/titles) + Inter (UI labels)
#include <builtinFonts/inter_10_regular.h>
#include <builtinFonts/notosans_12_regular.h>
#include <builtinFonts/notosans_12_bold.h>
#include <builtinFonts/notosans_24_bold.h>

// Font IDs
static const int FONT_SMALL = 10;     // Inter 10pt — UI labels, buttons, badges
static const int FONT_REGULAR = 12;   // Noto Sans 12pt — body text (times, durations)
static const int FONT_TITLE = 13;     // Noto Sans 12pt bold — titles, event names
static const int FONT_DISPLAY = 24;   // Noto Sans 24pt bold — passive screen headline

// Layout constants (480x800 portrait)
static const int16_t MARGIN_X = 24;
static const int16_t MARGIN_Y = 16;
static const int16_t TIME_COL_W = 70;  // Wide enough for "13:00" + padding before separator
static const int16_t SEP_X = MARGIN_X + TIME_COL_W;
static const int16_t BLOCK_X = SEP_X + 14;
static const int16_t BLOCK_W = 480 - BLOCK_X - MARGIN_X;
static const int16_t BLOCK_GAP = 10;
static const int16_t PROGRESS_H = 6;
static const int16_t BTN_BAR_H = 36;
static const int16_t HEADER_H = 60;
static const int16_t LIST_TOP = HEADER_H + 4;
static const int16_t LIST_BOTTOM = 700;  // Leave room for progress bar + button bar

// Track scroll state across renders
static uint8_t _scrollOffset = 0;

// Persistent renderer — avoids recreating GfxRenderer + reinserting fonts every frame
static GfxRenderer* _gfx = nullptr;

static GfxRenderer& getRenderer(HalDisplay& display) {
    if (!_gfx) {
        static GfxRenderer gfx(display);
        _gfx = &gfx;
        _gfx->begin();
        _gfx->setOrientation(GfxRenderer::Orientation::Portrait);

        static EpdFont fontSmall(&inter_10_regular);
        static EpdFont fontRegular(&notosans_12_regular);
        static EpdFont fontBold(&notosans_12_bold);
        static EpdFont fontDisplay(&notosans_24_bold);
        _gfx->insertFont(FONT_SMALL, EpdFontFamily(&fontSmall));
        _gfx->insertFont(FONT_REGULAR, EpdFontFamily(&fontRegular));
        _gfx->insertFont(FONT_TITLE, EpdFontFamily(&fontBold));
        _gfx->insertFont(FONT_DISPLAY, EpdFontFamily(&fontDisplay));

        // Initialize font decompression pipeline (required for 16pt+ compressed fonts)
        static FontDecompressor decompressor;
        decompressor.init();
        static FontCacheManager cacheManager(_gfx->getFontMap());
        cacheManager.setFontDecompressor(&decompressor);
        _gfx->setFontCacheManager(&cacheManager);
    }
    return *_gfx;
}

static int16_t clampBlockHeight(uint16_t duration_min) {
    int16_t h = duration_min * 2.5;
    if (h < 85) h = 85;   // Min height: 2-line title (10+16+2+16) + gap + duration label (14+6)
    if (h > 160) h = 160;
    return h;
}

static void drawDashedRect(GfxRenderer& gfx, int x, int y, int w, int h) {
    for (int dx = 0; dx < w; dx += 6) {
        gfx.drawLine(x + dx, y, x + dx + 3, y);
        gfx.drawLine(x + dx, y + h - 1, x + dx + 3, y + h - 1);
    }
    for (int dy = 0; dy < h; dy += 6) {
        gfx.drawLine(x, y + dy, x, y + dy + 3);
        gfx.drawLine(x + w - 1, y + dy, x + w - 1, y + dy + 3);
    }
}

static void drawDottedRect(GfxRenderer& gfx, int x, int y, int w, int h) {
    for (int dx = 0; dx < w; dx += 4) gfx.drawPixel(x + dx, y);
    for (int dx = 0; dx < w; dx += 4) gfx.drawPixel(x + dx, y + h - 1);
    for (int dy = 0; dy < h; dy += 4) gfx.drawPixel(x, y + dy);
    for (int dy = 0; dy < h; dy += 4) gfx.drawPixel(x + w - 1, y + dy);
}

static void drawEventBlock(GfxRenderer& gfx, int x, int y, int w, int h,
                           const AgendaItem& item, bool selected) {
    if (selected) {
        gfx.fillRect(x, y, w, h);
    } else if (item.completed) {
        drawDottedRect(gfx, x, y, w, h);
    } else if (item.is_task) {
        drawDashedRect(gfx, x, y, w, h);
    } else {
        gfx.drawRect(x, y, w, h);
    }

    bool whiteText = selected;
    int titleX = x + 12;
    int titleY = y + 10;
    int maxTitleW = w - 24;
    int lineH = gfx.getLineHeight(FONT_TITLE);

    // Word-wrap to 2 lines with ellipsis
    // Second line leaves room for duration label at bottom-right
    char dur[8];
    snprintf(dur, sizeof(dur), "%dm", item.duration_min);
    int durW = gfx.getTextWidth(FONT_SMALL, dur);
    int line2MaxW = maxTitleW - durW - 16;  // Leave gap before duration

    const char* titleText = item.title;
    char buf[72];
    if (selected) {
        snprintf(buf, sizeof(buf), "> %s", item.title);
        titleText = buf;
    }
    auto lines = gfx.wrappedText(FONT_TITLE, titleText, maxTitleW, 2);
    // Truncate second line if it would overlap with duration
    if (lines.size() > 1 && line2MaxW > 0) {
        lines[1] = gfx.truncatedText(FONT_TITLE, lines[1].c_str(), line2MaxW);
    }
    for (size_t i = 0; i < lines.size(); i++) {
        gfx.drawText(FONT_TITLE, titleX, titleY + i * (lineH + 2), lines[i].c_str(), !whiteText);
    }

    if (item.completed && !selected && !lines.empty()) {
        // Strikethrough on first line
        int tw = gfx.getTextWidth(FONT_TITLE, lines[0].c_str());
        int strikeY = titleY + lineH / 2;
        gfx.drawLine(titleX, strikeY, titleX + tw, strikeY);
    }

    // Duration label (dur/durW already computed above for line2 truncation)
    gfx.drawText(FONT_SMALL, x + w - durW - 12,
                 y + h - gfx.getLineHeight(FONT_SMALL) - 6,
                 dur, !whiteText);
}

static void drawProgressBar(GfxRenderer& gfx, int y, float progress) {
    int barX = MARGIN_X;
    int barW = 480 - 2 * MARGIN_X;
    gfx.drawRect(barX, y, barW, PROGRESS_H);
    int fillW = (int)(barW * progress);
    if (fillW > 0) {
        gfx.fillRect(barX, y, fillW, PROGRESS_H);
    }
}

static void drawButtonBar(GfxRenderer& gfx, const char* labels[4]) {
    int btnW = 106;
    int btnY = 800 - BTN_BAR_H - 14;
    int positions[] = {25, 131, 245, 350};
    int lineH = gfx.getLineHeight(FONT_SMALL);

    for (int i = 0; i < 4; i++) {
        if (!labels[i] || labels[i][0] == '\0') continue;
        gfx.fillRect(positions[i], btnY, btnW, BTN_BAR_H, false);
        gfx.drawRect(positions[i], btnY, btnW, BTN_BAR_H);
        int tw = gfx.getTextWidth(FONT_SMALL, labels[i]);
        int tx = positions[i] + (btnW - tw) / 2;
        int ty = btnY + (BTN_BAR_H - lineH) / 2 + 1;
        gfx.drawText(FONT_SMALL, tx, ty, labels[i]);
    }
}

static void drawBattery(GfxRenderer& gfx) {
    extern HalPowerManager powerManager;
    uint16_t pct = powerManager.getBatteryPercentage();

    // Draw percentage as text, same font as date, right-aligned
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    int tw = gfx.getTextWidth(FONT_TITLE, buf);
    gfx.drawText(FONT_TITLE, 480 - MARGIN_X - tw, MARGIN_Y, buf);
}

// Adjust scroll offset so cursor is visible. Modifies _scrollOffset in place.
static void ensureCursorVisible(const Agenda& agenda, int8_t cursor) {
    if (cursor < 0 || agenda.item_count == 0) {
        _scrollOffset = 0;
        return;
    }

    uint8_t target = (uint8_t)cursor;

    // Scroll up if cursor is above visible area
    if (target < _scrollOffset) {
        _scrollOffset = target;
        return;
    }

    // Iteratively scroll down until cursor fits in view
    for (int attempt = 0; attempt < agenda.item_count; attempt++) {
        int y = LIST_TOP;
        bool fits = false;
        for (uint8_t i = _scrollOffset; i < agenda.item_count; i++) {
            int h = clampBlockHeight(agenda.items[i].duration_min);
            if (y + h > LIST_BOTTOM) {
                if (target >= i) { _scrollOffset++; break; }  // Scroll down, retry
                return;  // Cursor is above this point, already visible
            }
            if (i == target) { fits = true; break; }
            y += h + BLOCK_GAP;
        }
        if (fits) return;
    }
}

void resetScroll() { _scrollOffset = 0; }
void setScroll(uint8_t offset) { _scrollOffset = offset; }

void renderAgenda(HalDisplay& display, const Agenda& agenda, int8_t cursor) {
    GfxRenderer& gfx = getRenderer(display);

    // Update scroll offset
    ensureCursorVisible(agenda, cursor);

    gfx.clearScreen();

    // Header: date + battery (vertically centered with text)
    gfx.drawText(FONT_TITLE, MARGIN_X, MARGIN_Y, agenda.date_label);
    drawBattery(gfx);

    // Divider
    gfx.drawLine(MARGIN_X, HEADER_H - 2, 480 - MARGIN_X, HEADER_H - 2);

    // Timeline (scrolled)
    int y = LIST_TOP;
    for (uint8_t i = _scrollOffset; i < agenda.item_count; i++) {
        const AgendaItem& item = agenda.items[i];
        int blockH = clampBlockHeight(item.duration_min);

        if (y + blockH > LIST_BOTTOM) break;

        bool selected = (cursor >= 0 && i == (uint8_t)cursor);

        gfx.drawText(FONT_REGULAR, MARGIN_X, y + 10, item.start);
        gfx.drawLine(SEP_X, y, SEP_X, y + blockH);
        drawEventBlock(gfx, BLOCK_X, y, BLOCK_W, blockH, item, selected);

        y += blockH + BLOCK_GAP;
    }

    // Day progress bar (above button bar)
    drawProgressBar(gfx, 800 - (cursor >= 0 ? 60 : 50), agenda.day_progress);

    if (cursor >= 0) {
        // Interactive: show button bar. Confirm label is context-aware —
        // "Delete" when cursor is on a calendar event (destructive), else "Complete".
        bool onEvent = (cursor < (int8_t)agenda.item_count
                        && !agenda.items[cursor].is_task);
        const char* confirmLabel = onEvent ? "Delete" : "Complete";
        const char* labels[] = {"Resched.", confirmLabel, "Up", "Down"};
        drawButtonBar(gfx, labels);
    }

    display.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

// Compute Y position and height for item at index (relative to current scroll)
// Returns false if item is not visible (scrolled off screen)
static bool getItemGeometry(const Agenda& agenda, uint8_t index, int& outY, int& outH) {
    if (index < _scrollOffset) return false;
    int y = LIST_TOP;
    for (uint8_t i = _scrollOffset; i <= index && i < agenda.item_count; i++) {
        int h = clampBlockHeight(agenda.items[i].duration_min);
        if (i == index) {
            if (y + h > LIST_BOTTOM) return false;  // Below visible area
            outY = y;
            outH = h;
            return true;
        }
        y += h + BLOCK_GAP;
    }
    return false;
}

// Redraw a single item block (clear its area first, then redraw)
static bool redrawItem(GfxRenderer& gfx, const Agenda& agenda, uint8_t index, bool selected) {
    if (index >= agenda.item_count) return false;
    int y, h;
    if (!getItemGeometry(agenda, index, y, h)) return false;
    const AgendaItem& item = agenda.items[index];

    // Clear the block area (white fill)
    gfx.fillRect(BLOCK_X, y, BLOCK_W, h, false);

    // Redraw the block
    drawEventBlock(gfx, BLOCK_X, y, BLOCK_W, h, item, selected);
    return true;
}

void updateCursor(HalDisplay& display, const Agenda& agenda, int8_t oldCursor, int8_t newCursor) {
    if (oldCursor == newCursor) return;

    // Check if scrolling is needed
    uint8_t oldScroll = _scrollOffset;
    ensureCursorVisible(agenda, newCursor);
    if (_scrollOffset != oldScroll) {
        // Scroll changed — need full redraw
        renderAgenda(display, agenda, newCursor);
        return;
    }

    GfxRenderer& gfx = getRenderer(display);

    // Un-highlight old cursor position
    if (oldCursor >= 0 && oldCursor < (int8_t)agenda.item_count) {
        redrawItem(gfx, agenda, oldCursor, false);
    }

    // Highlight new cursor position
    if (newCursor >= 0 && newCursor < (int8_t)agenda.item_count) {
        redrawItem(gfx, agenda, newCursor, true);
    }

    // Refresh the button bar in case the Confirm label changed between
    // "Complete" (on task) and "Delete" (on event) as the cursor moved.
    bool oldOnEvent = (oldCursor >= 0 && oldCursor < (int8_t)agenda.item_count
                       && !agenda.items[oldCursor].is_task);
    bool newOnEvent = (newCursor >= 0 && newCursor < (int8_t)agenda.item_count
                       && !agenda.items[newCursor].is_task);
    if (oldOnEvent != newOnEvent) {
        int btnY = 800 - BTN_BAR_H - 14;
        gfx.fillRect(0, btnY, 480, BTN_BAR_H, false);
        const char* confirmLabel = newOnEvent ? "Delete" : "Complete";
        const char* labels[] = {"Resched.", confirmLabel, "Up", "Down"};
        drawButtonBar(gfx, labels);
    }

    display.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

void showSplash(HalDisplay& display, const char* message, bool inverted) {
    GfxRenderer& gfx = getRenderer(display);

    if (inverted) {
        // Sleep splash: clear to white first (wipes all ghosting), then go black
        gfx.clearScreen(0xFF);
        display.displayBuffer(HalDisplay::RefreshMode::HALF_REFRESH);
        // Now draw black splash
        gfx.clearScreen(0x00);
        gfx.drawCenteredText(FONT_TITLE, 380, message, false);
        char ver[16];
        snprintf(ver, sizeof(ver), "v%s", FIRMWARE_VERSION);
        gfx.drawCenteredText(FONT_SMALL, 420, ver, false);
        display.displayBuffer(HalDisplay::RefreshMode::HALF_REFRESH);
    } else {
        // Boot splash: white background
        gfx.clearScreen(0xFF);
        gfx.drawCenteredText(FONT_TITLE, 380, message);
        char ver[16];
        snprintf(ver, sizeof(ver), "v%s", FIRMWARE_VERSION);
        gfx.drawCenteredText(FONT_SMALL, 420, ver);
        display.displayBuffer(HalDisplay::RefreshMode::HALF_REFRESH);
    }
}

void renderPassive(HalDisplay& display, const Agenda& agenda) {
    GfxRenderer& gfx = getRenderer(display);

    const int16_t M = 28;
    const int16_t W = 480 - 2 * M;

    // Black background
    gfx.clearScreen(0x00);

    // All text white (black=false)

    // --- Date, top-left ---
    gfx.drawText(FONT_SMALL, M, M, agenda.date_label, false);

    // --- Find current (happening now) and next items ---
    const AgendaItem* current = nullptr;
    const AgendaItem* next = nullptr;
    if (agenda.current_index >= 0 && agenda.current_index < agenda.item_count) {
        current = &agenda.items[agenda.current_index];
        // Next = first uncompleted item starting after current ends
        for (uint8_t i = 0; i < agenda.item_count; i++) {
            if ((int8_t)i == agenda.current_index) continue;
            if (agenda.items[i].completed) continue;
            if (strcmp(agenda.items[i].start, current->end) >= 0) {
                next = &agenda.items[i];
                break;
            }
        }
    } else {
        // No current item — show first uncompleted as "up next"
        for (uint8_t i = 0; i < agenda.item_count; i++) {
            if (agenda.items[i].completed) continue;
            if (!current) current = &agenda.items[i];
            else if (!next) { next = &agenda.items[i]; break; }
        }
    }

    if (current) {
        // --- Current task: large 18pt Noto Sans Bold (compressed, needs FontDecompressor) ---
        auto lines = gfx.wrappedText(FONT_DISPLAY, current->title, W, 3);
        int lineH = gfx.getLineHeight(FONT_DISPLAY);
        bool hasConflict = (agenda.conflict_index >= 0 && agenda.conflict_index < agenda.item_count);
        // Reserve the conflict block's height so the whole thing stays vertically balanced
        int conflictH = hasConflict
            ? 30 + gfx.getLineHeight(FONT_SMALL) + 2 + gfx.getLineHeight(FONT_TITLE) + 2
                 + gfx.getLineHeight(FONT_SMALL)
            : 0;
        int totalH = lines.size() * (lineH + 6) + 50 + conflictH;
        int y = (800 - totalH) / 2 - 40;

        // Type label
        gfx.drawText(FONT_SMALL, M, y, current->is_task ? "TASK" : "EVENT", false);
        y += 28;

        // Large title
        for (size_t i = 0; i < lines.size(); i++) {
            gfx.drawText(FONT_DISPLAY, M, y, lines[i].c_str(), false);
            y += lineH + 6;
        }
        y += 16;

        // Time range
        char timeRange[16];
        snprintf(timeRange, sizeof(timeRange), "%s - %s", current->start, current->end);
        gfx.drawText(FONT_REGULAR, M, y, timeRange, false);

        // Conflict warning — second overlapping event.
        // Advance by each line's real height (getLineHeight = font advanceY) so the
        // label/title/time never overlap — the fonts are taller than the old fixed 18px.
        if (hasConflict) {
            const AgendaItem& conflict = agenda.items[agenda.conflict_index];
            y += 30;
            gfx.drawText(FONT_SMALL, M, y, "CONFLICT", false);
            y += gfx.getLineHeight(FONT_SMALL) + 2;
            auto conflictTitle = gfx.truncatedText(FONT_TITLE, conflict.title, W);
            gfx.drawText(FONT_TITLE, M, y, conflictTitle.c_str(), false);
            y += gfx.getLineHeight(FONT_TITLE) + 2;
            char conflictTime[16];
            snprintf(conflictTime, sizeof(conflictTime), "%s - %s", conflict.start, conflict.end);
            gfx.drawText(FONT_SMALL, M, y, conflictTime, false);
        }
    } else {
        gfx.drawText(FONT_TITLE, M, 370, "All done for today", false);
    }

    // --- Divider above "Next" ---
    gfx.drawLine(M, 660, 480 - M, 660, false);

    // --- Next item, bottom ---
    if (next) {
        gfx.drawText(FONT_SMALL, M, 675, "NEXT", false);
        auto truncTitle = gfx.truncatedText(FONT_TITLE, next->title, W);
        gfx.drawText(FONT_TITLE, M, 700, truncTitle.c_str(), false);
        char nextTime[16];
        snprintf(nextTime, sizeof(nextTime), "%s - %s", next->start, next->end);
        gfx.drawText(FONT_SMALL, M, 735, nextTime, false);
    }

    display.displayBuffer(HalDisplay::RefreshMode::FULL_REFRESH);
}

void showReschedulingProgress(HalDisplay& display, int percent, const char* message) {
    GfxRenderer& gfx = getRenderer(display);
    gfx.clearScreen();

    gfx.drawCenteredText(FONT_TITLE, 340, "Rescheduling...");
    gfx.drawCenteredText(FONT_SMALL, 375, message);

    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", percent);
    gfx.drawCenteredText(FONT_REGULAR, 400, pct);

    int barW = 300, barH = 8;
    int barX = (480 - barW) / 2, barY = 440;
    gfx.drawRect(barX, barY, barW, barH);
    int fillW = barW * percent / 100;
    if (fillW > 0) gfx.fillRect(barX, barY, fillW, barH);

    display.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

void showToast(HalDisplay& display, const char* message) {
    GfxRenderer& gfx = getRenderer(display);
    gfx.clearScreen();
    gfx.drawCenteredText(FONT_TITLE, 390, message);
    display.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
    delay(1200);
}

void showLoading(HalDisplay& display, const char* message) {
    GfxRenderer& gfx = getRenderer(display);
    // Clear only the schedule area — keep header (date, battery) and footer (buttons)
    gfx.fillRect(0, LIST_TOP, 480, LIST_BOTTOM - LIST_TOP, false);
    int centerY = LIST_TOP + (LIST_BOTTOM - LIST_TOP) / 2;
    gfx.drawCenteredText(FONT_REGULAR, centerY, message);
    display.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

// ============================================================================
// WiFi setup wizard screens
// ============================================================================

namespace WifiKeyboard {
    // iPhone-style QWERTY. Three layers (letters / numbers+symbols / more
    // symbols), 4 rows each, variable column count per row:
    //
    //   Layer 0 (letters, shift toggles case):
    //     Row 0 (10): q w e r t y u i o p
    //     Row 1 (9):    a s d f g h j k l         (visually indented)
    //     Row 2 (9):  ⇧ z x c v b n m ⌫           (shift + 7 letters + backspace)
    //     Row 3 (3):  123  [space]  OK
    //
    //   Layer 1 (numbers + main symbols):
    //     Row 0 (10): 1 2 3 4 5 6 7 8 9 0
    //     Row 1 (10): - / : ; ( ) $ & @ "
    //     Row 2 (9):  #+= . , ? ! ' " ` ⌫
    //     Row 3 (3):  ABC  [space]  OK
    //
    //   Layer 2 (more symbols, ASCII-only — no €£¥•):
    //     Row 0 (10): [ ] { } # % ^ * + =
    //     Row 1 (6):  _ \ | ~ < >          (centered, wider keys)
    //     Row 2 (9):  123 . , ? ! ' " ` ⌫
    //     Row 3 (3):  ABC  [space]  OK
    //
    // Cursor walks the cells in reading order (left-to-right, wrapping at the
    // last cell back to the first). The wifi_setup wizard owns the cursor
    // state and the shift toggle.
    static const Cell kSpaceCell { "space", nullptr, ' ', CELL_CHAR,      0 };
    static const Cell kOkCell    { "OK",    nullptr, 0,   CELL_SUBMIT,    0 };
    static const Cell kShiftCell { "shift", nullptr, 0,   CELL_SHIFT,     0 };
    static const Cell kBackCell  { "del",   nullptr, 0,   CELL_BACKSPACE, 0 };
    static const Cell k123Cell   { "123",   nullptr, 0,   CELL_LAYER,     1 };
    static const Cell kSymCell   { "#+=",   nullptr, 0,   CELL_LAYER,     2 };
    static const Cell kAbcCell   { "ABC",   nullptr, 0,   CELL_LAYER,     0 };

    #define CCHAR(L, U, X) { (L), (U), (X), CELL_CHAR, 0 }
    #define SCHAR(L, X)    { (L), nullptr, (X), CELL_CHAR, 0 }

    // Layer 0 — letters. altLabel = uppercase form (shown when shift is on).
    static const Cell kLetters[ROWS][10] = {
        { CCHAR("q","Q",'q'), CCHAR("w","W",'w'), CCHAR("e","E",'e'),
          CCHAR("r","R",'r'), CCHAR("t","T",'t'), CCHAR("y","Y",'y'),
          CCHAR("u","U",'u'), CCHAR("i","I",'i'), CCHAR("o","O",'o'),
          CCHAR("p","P",'p') },
        { CCHAR("a","A",'a'), CCHAR("s","S",'s'), CCHAR("d","D",'d'),
          CCHAR("f","F",'f'), CCHAR("g","G",'g'), CCHAR("h","H",'h'),
          CCHAR("j","J",'j'), CCHAR("k","K",'k'), CCHAR("l","L",'l') },
        { kShiftCell,
          CCHAR("z","Z",'z'), CCHAR("x","X",'x'), CCHAR("c","C",'c'),
          CCHAR("v","V",'v'), CCHAR("b","B",'b'), CCHAR("n","N",'n'),
          CCHAR("m","M",'m'),
          kBackCell },
        { k123Cell, kSpaceCell, kOkCell },
    };
    static const uint8_t kLetterRowCols[ROWS] = { 10, 9, 9, 3 };

    // Layer 1 — numbers + main symbols.
    static const Cell kNumbers[ROWS][10] = {
        { SCHAR("1",'1'), SCHAR("2",'2'), SCHAR("3",'3'), SCHAR("4",'4'),
          SCHAR("5",'5'), SCHAR("6",'6'), SCHAR("7",'7'), SCHAR("8",'8'),
          SCHAR("9",'9'), SCHAR("0",'0') },
        { SCHAR("-",'-'), SCHAR("/",'/'), SCHAR(":",':'), SCHAR(";",';'),
          SCHAR("(",'('), SCHAR(")",')'), SCHAR("$",'$'), SCHAR("&",'&'),
          SCHAR("@",'@'), SCHAR("\"",'"') },
        { kSymCell,
          SCHAR(".",'.'), SCHAR(",",','), SCHAR("?",'?'), SCHAR("!",'!'),
          SCHAR("'",'\''), SCHAR("\"",'"'), SCHAR("`",'`'),
          kBackCell },
        { kAbcCell, kSpaceCell, kOkCell },
    };
    static const uint8_t kNumberRowCols[ROWS] = { 10, 10, 9, 3 };

    // Layer 2 — additional symbols.
    static const Cell kSymbols[ROWS][10] = {
        { SCHAR("[",'['), SCHAR("]",']'), SCHAR("{",'{'), SCHAR("}",'}'),
          SCHAR("#",'#'), SCHAR("%",'%'), SCHAR("^",'^'), SCHAR("*",'*'),
          SCHAR("+",'+'), SCHAR("=",'=') },
        { SCHAR("_",'_'), SCHAR("\\",'\\'), SCHAR("|",'|'),
          SCHAR("~",'~'), SCHAR("<",'<'), SCHAR(">",'>') },
        { k123Cell,
          SCHAR(".",'.'), SCHAR(",",','), SCHAR("?",'?'), SCHAR("!",'!'),
          SCHAR("'",'\''), SCHAR("\"",'"'), SCHAR("`",'`'),
          kBackCell },
        { kAbcCell, kSpaceCell, kOkCell },
    };
    static const uint8_t kSymbolRowCols[ROWS] = { 10, 6, 9, 3 };

    #undef CCHAR
    #undef SCHAR

    uint8_t colsInRow(uint8_t layer, uint8_t row) {
        if (row >= ROWS) return 0;
        switch (layer) {
            case 0: return kLetterRowCols[row];
            case 1: return kNumberRowCols[row];
            case 2: return kSymbolRowCols[row];
        }
        return kLetterRowCols[row];
    }

    uint16_t totalCells(uint8_t layer) {
        uint16_t total = 0;
        for (uint8_t r = 0; r < ROWS; r++) total += colsInRow(layer, r);
        return total;
    }

    Cell cellAt(uint8_t layer, uint8_t row, uint8_t col) {
        if (row >= ROWS) row = 0;
        uint8_t n = colsInRow(layer, row);
        if (n == 0) return kAbcCell;  // fallback (shouldn't happen)
        if (col >= n) col = (uint8_t)(n - 1);
        switch (layer) {
            case 0: return kLetters[row][col];
            case 1: return kNumbers[row][col];
            case 2: return kSymbols[row][col];
        }
        return kLetters[row][col];
    }

    void indexToRowCol(uint8_t layer, uint16_t index, uint8_t* outRow, uint8_t* outCol) {
        for (uint8_t r = 0; r < ROWS; r++) {
            uint8_t n = colsInRow(layer, r);
            if (index < n) { *outRow = r; *outCol = (uint8_t)index; return; }
            index = (uint16_t)(index - n);
        }
        *outRow = 0;
        *outCol = 0;
    }

    Cell cellAtIndex(uint8_t layer, uint16_t index) {
        uint8_t r = 0, c = 0;
        indexToRowCol(layer, index, &r, &c);
        return cellAt(layer, r, c);
    }

    // Geometry — see comments in showWifiPasswordEntry for layout rationale.
    static constexpr int16_t KB_LEFT  = 16;
    static constexpr int16_t KB_TOP   = 260;
    static constexpr int16_t KB_WIDTH = 480 - 2 * KB_LEFT;  // 448
    static constexpr int16_t KB_ROW_H = 64;
    static constexpr int16_t KEY_STD  = 44;   // standard key width
    static constexpr int16_t KEY_WIDE = 66;   // shift, backspace, layer-toggle
    static constexpr int16_t KEY_SPACE_W = KB_WIDTH - 2 * KEY_WIDE;  // 316
    static constexpr int16_t KEY_WIDE_SYM = 64;  // layer 2 row 1 keys

    KeyRect keyRect(uint8_t layer, uint8_t row, uint8_t col) {
        int16_t y = (int16_t)(KB_TOP + row * KB_ROW_H + 2);
        int16_t h = KB_ROW_H - 4;
        uint8_t n = colsInRow(layer, row);

        // Row 3 across all layers: 3 keys (wide-toggle | space | wide-OK)
        if (row == 3) {
            switch (col) {
                case 0: return { KB_LEFT,                         y, KEY_WIDE,    h };
                case 1: return { (int16_t)(KB_LEFT + KEY_WIDE),   y, KEY_SPACE_W, h };
                default: return { (int16_t)(KB_LEFT + KEY_WIDE + KEY_SPACE_W), y, KEY_WIDE, h };
            }
        }

        // Row 2 in any layer with 9 cells: wide-toggle | 7 std | wide-backspace
        if (row == 2 && n == 9) {
            int16_t indent = (int16_t)((KB_WIDTH - (2 * KEY_WIDE + 7 * KEY_STD)) / 2);
            if (col == 0) return { (int16_t)(KB_LEFT + indent), y, KEY_WIDE, h };
            if (col == 8) return { (int16_t)(KB_LEFT + indent + KEY_WIDE + 7 * KEY_STD), y, KEY_WIDE, h };
            return { (int16_t)(KB_LEFT + indent + KEY_WIDE + (col - 1) * KEY_STD), y, KEY_STD, h };
        }

        // Layer 2 row 1: 6 wider keys centered
        if (n == 6) {
            int16_t indent = (int16_t)((KB_WIDTH - 6 * KEY_WIDE_SYM) / 2);
            return { (int16_t)(KB_LEFT + indent + col * KEY_WIDE_SYM), y, KEY_WIDE_SYM, h };
        }

        // Uniform standard-width rows (10 keys, or 9 letters indented)
        int16_t indent = (int16_t)((KB_WIDTH - n * KEY_STD) / 2);
        return { (int16_t)(KB_LEFT + indent + col * KEY_STD), y, KEY_STD, h };
    }
}

// Network list layout
static const int16_t WIFI_LIST_ROW_H = 56;
static const int16_t WIFI_LIST_TOP = 80;
static const uint8_t WIFI_LIST_VISIBLE = 10;  // 56*10 = 560, fits below header

void showWifiScanning(HalDisplay& display) {
    GfxRenderer& gfx = getRenderer(display);
    gfx.clearScreen();
    gfx.drawCenteredText(FONT_TITLE, 360, "Scanning Wi-Fi networks");
    gfx.drawCenteredText(FONT_SMALL, 400, "This takes a few seconds...");
    display.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

void showWifiNetworkList(HalDisplay& display,
                         const char* const* ssids,
                         const int8_t* rssi,
                         const bool* secured,
                         uint8_t count,
                         int8_t selected,
                         uint8_t scrollOffset) {
    GfxRenderer& gfx = getRenderer(display);
    gfx.clearScreen();

    // Header
    gfx.drawText(FONT_TITLE, MARGIN_X, MARGIN_Y, "Choose Wi-Fi network");
    char foundBuf[24];
    snprintf(foundBuf, sizeof(foundBuf), "%u found", (unsigned)count);
    int fw = gfx.getTextWidth(FONT_SMALL, foundBuf);
    gfx.drawText(FONT_SMALL, 480 - MARGIN_X - fw, MARGIN_Y + 2, foundBuf);
    gfx.drawLine(MARGIN_X, HEADER_H - 2, 480 - MARGIN_X, HEADER_H - 2);

    // List
    if (count == 0) {
        gfx.drawCenteredText(FONT_REGULAR, 360, "No networks found");
        gfx.drawCenteredText(FONT_SMALL, 400, "Press BACK to scan again");
    } else {
        for (uint8_t i = 0; i < WIFI_LIST_VISIBLE; i++) {
            uint8_t idx = scrollOffset + i;
            if (idx >= count) break;
            int y = WIFI_LIST_TOP + i * WIFI_LIST_ROW_H;
            bool isSelected = (selected >= 0 && (uint8_t)selected == idx);

            if (isSelected) {
                gfx.fillRect(MARGIN_X, y, 480 - 2 * MARGIN_X, WIFI_LIST_ROW_H - 6);
            } else {
                gfx.drawLine(MARGIN_X, y + WIFI_LIST_ROW_H - 6,
                             480 - MARGIN_X, y + WIFI_LIST_ROW_H - 6);
            }

            // SSID (truncated)
            bool drawBlack = !isSelected;  // selected row is black bg → draw labels white
            std::string truncated = gfx.truncatedText(FONT_TITLE, ssids[idx], 480 - 2 * MARGIN_X - 90);
            gfx.drawText(FONT_TITLE, MARGIN_X + 12, y + 14, truncated.c_str(), drawBlack);

            // Lock + signal indicator on the right
            int rightX = 480 - MARGIN_X - 16;
            if (secured && secured[idx]) {
                gfx.drawText(FONT_SMALL, rightX - 100, y + 18, "secured", drawBlack);
            }
            int barX = rightX - 24;
            int barY = y + WIFI_LIST_ROW_H - 14;
            int8_t signalDb = rssi ? rssi[idx] : -70;
            int bars;
            if (signalDb >= -55) bars = 4;
            else if (signalDb >= -70) bars = 3;
            else if (signalDb >= -80) bars = 2;
            else if (signalDb >= -90) bars = 1;
            else bars = 0;
            for (int b = 0; b < 4; b++) {
                int h = 4 + b * 4;
                int bx = barX + b * 6;
                int by = barY - h;
                gfx.drawRect(bx, by, 4, h, drawBlack);
                if (b < bars) gfx.fillRect(bx, by, 4, h, drawBlack);
            }
        }

        // Scroll hint
        if (count > WIFI_LIST_VISIBLE) {
            char scrollBuf[16];
            snprintf(scrollBuf, sizeof(scrollBuf), "%u/%u",
                     (unsigned)((selected >= 0 ? selected : scrollOffset) + 1), (unsigned)count);
            gfx.drawCenteredText(FONT_SMALL,
                                 WIFI_LIST_TOP + WIFI_LIST_VISIBLE * WIFI_LIST_ROW_H + 10,
                                 scrollBuf);
        }
    }

    // Help text (button bar style at bottom)
    int helpY = 800 - 56;
    gfx.drawText(FONT_SMALL, MARGIN_X, helpY,
                 "<- / -> select   OK connect   BACK rescan   PWR skip");
    display.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

void showWifiPasswordEntry(HalDisplay& display,
                           const char* ssid,
                           const char* password,
                           uint8_t cursorRow,
                           uint8_t cursorCol,
                           uint8_t layer,
                           bool shift) {
    GfxRenderer& gfx = getRenderer(display);
    gfx.clearScreen();

    // Header
    char header[80];
    snprintf(header, sizeof(header), "Password for %s", ssid);
    std::string truncHeader = gfx.truncatedText(FONT_TITLE, header, 480 - 2 * MARGIN_X);
    gfx.drawText(FONT_TITLE, MARGIN_X, MARGIN_Y, truncHeader.c_str());
    gfx.drawLine(MARGIN_X, HEADER_H - 2, 480 - MARGIN_X, HEADER_H - 2);

    // Password field — show last chars that fit, then cursor "▎"
    int fieldY = HEADER_H + 16;
    int fieldH = 56;
    gfx.drawRect(MARGIN_X, fieldY, 480 - 2 * MARGIN_X, fieldH);

    size_t passLen = strlen(password);
    int maxFieldW = 480 - 2 * MARGIN_X - 24;
    int startIdx = 0;
    for (int s = 0; s <= (int)passLen; s++) {
        int w = gfx.getTextWidth(FONT_REGULAR, password + s) + 8;
        if (w <= maxFieldW) { startIdx = s; break; }
    }
    int textX = MARGIN_X + 12;
    int textY = fieldY + 16;
    if (passLen == 0) {
        gfx.drawText(FONT_SMALL, textX, textY + 4, "(enter password)");
    } else {
        gfx.drawText(FONT_REGULAR, textX, textY, password + startIdx);
    }
    int textW = gfx.getTextWidth(FONT_REGULAR, password + startIdx);
    int cursorX = textX + textW + 2;
    gfx.fillRect(cursorX, textY - 2, 2, gfx.getLineHeight(FONT_REGULAR) + 4);

    // Char count + layer indicator
    const char* layerLabel = (layer == 0) ? (shift ? "ABC" : "abc")
                           : (layer == 1) ? "123"
                           : "#+=";
    char counter[32];
    snprintf(counter, sizeof(counter), "%u chars  [%s]", (unsigned)passLen, layerLabel);
    int counterW = gfx.getTextWidth(FONT_SMALL, counter);
    gfx.drawText(FONT_SMALL, 480 - MARGIN_X - counterW, fieldY + fieldH + 8, counter);

    // Keyboard
    for (uint8_t r = 0; r < WifiKeyboard::ROWS; r++) {
        uint8_t cols = WifiKeyboard::colsInRow(layer, r);
        for (uint8_t c = 0; c < cols; c++) {
            WifiKeyboard::Cell cell = WifiKeyboard::cellAt(layer, r, c);
            WifiKeyboard::KeyRect rect = WifiKeyboard::keyRect(layer, r, c);

            // Label flips to uppercase when shift is armed and we're on a letter.
            const char* label = (shift && cell.altLabel) ? cell.altLabel : cell.label;

            bool isSelected = (r == cursorRow && c == cursorCol);
            bool shiftArmed = (cell.type == WifiKeyboard::CELL_SHIFT && shift);
            bool filled = isSelected || shiftArmed;

            if (filled) gfx.fillRect(rect.x, rect.y, rect.w, rect.h);
            else        gfx.drawRect(rect.x, rect.y, rect.w, rect.h);

            // Single-char labels render in title font; multi-char in small font.
            int fontId = (label && label[0] && label[1] == '\0') ? FONT_TITLE : FONT_SMALL;
            int lw = gfx.getTextWidth(fontId, label);
            int lh = gfx.getLineHeight(fontId);
            int lx = rect.x + (rect.w - lw) / 2;
            int ly = rect.y + (rect.h - lh) / 2;
            gfx.drawText(fontId, lx, ly, label, !filled);
        }
    }

    // Bottom hint — only the buttons we actually use, so users don't go hunting
    // for volume buttons that do nothing.
    int hintY = 260 + WifiKeyboard::ROWS * 64 + 12;  // KB_TOP + ROWS*KB_ROW_H + pad
    gfx.drawText(FONT_SMALL, MARGIN_X, hintY,
                 "<- / -> step   OK type   BACK / PWR cancel");
    display.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

void showWifiConnecting(HalDisplay& display, const char* ssid) {
    GfxRenderer& gfx = getRenderer(display);
    gfx.clearScreen();
    gfx.drawCenteredText(FONT_TITLE, 360, "Connecting...");
    char buf[80];
    snprintf(buf, sizeof(buf), "%s", ssid);
    std::string trunc = gfx.truncatedText(FONT_REGULAR, buf, 480 - 2 * MARGIN_X);
    gfx.drawCenteredText(FONT_REGULAR, 400, trunc.c_str());
    display.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

void showWifiResult(HalDisplay& display, bool success, const char* message) {
    GfxRenderer& gfx = getRenderer(display);
    gfx.clearScreen();
    gfx.drawCenteredText(FONT_TITLE, 360, success ? "Connected" : "Could not connect");
    if (message && message[0]) {
        gfx.drawCenteredText(FONT_SMALL, 400, message);
    }
    display.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

void showWifiCaptiveStatus(HalDisplay& display, const char* status) {
    GfxRenderer& gfx = getRenderer(display);
    gfx.clearScreen();
    gfx.drawCenteredText(FONT_TITLE, 360, "Captive portal");
    if (status && status[0]) {
        gfx.drawCenteredText(FONT_REGULAR, 400, status);
    }
    display.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

void showWifiCaptiveError(HalDisplay& display, const char* ssid,
                          const char* macAddress, const char* portalUrl) {
    GfxRenderer& gfx = getRenderer(display);
    gfx.clearScreen();

    int y = MARGIN_Y;
    gfx.drawText(FONT_TITLE, MARGIN_X, y, "Sign-in required");
    y += 28;
    gfx.drawLine(MARGIN_X, y, 480 - MARGIN_X, y);
    y += 18;

    char ssidLine[80];
    snprintf(ssidLine, sizeof(ssidLine), "\"%s\" needs a browser sign-in", ssid);
    auto wrapped = gfx.wrappedText(FONT_REGULAR, ssidLine, 480 - 2 * MARGIN_X, 2);
    int lineH = gfx.getLineHeight(FONT_REGULAR);
    for (auto& line : wrapped) {
        gfx.drawText(FONT_REGULAR, MARGIN_X, y, line.c_str());
        y += lineH + 2;
    }
    y += 10;

    gfx.drawText(FONT_REGULAR, MARGIN_X, y, "I tried to auto-accept and");
    y += lineH + 2;
    gfx.drawText(FONT_REGULAR, MARGIN_X, y, "the portal still won't let me");
    y += lineH + 2;
    gfx.drawText(FONT_REGULAR, MARGIN_X, y, "through. Try one of these:");
    y += lineH + 16;

    gfx.drawText(FONT_REGULAR, MARGIN_X, y, "1. Some networks register a");
    y += lineH + 2;
    gfx.drawText(FONT_REGULAR, MARGIN_X + 14, y, "device by its MAC address.");
    y += lineH + 2;
    gfx.drawText(FONT_REGULAR, MARGIN_X + 14, y, "Add this one to the network:");
    y += lineH + 6;
    gfx.drawText(FONT_TITLE, MARGIN_X + 14, y, macAddress);
    y += lineH + 16;

    gfx.drawText(FONT_REGULAR, MARGIN_X, y, "2. Otherwise, pick a different");
    y += lineH + 2;
    gfx.drawText(FONT_REGULAR, MARGIN_X + 14, y, "network — phone hotspots and");
    y += lineH + 2;
    gfx.drawText(FONT_REGULAR, MARGIN_X + 14, y, "home Wi-Fi work without this.");
    y += lineH + 18;

    if (portalUrl && portalUrl[0]) {
        gfx.drawText(FONT_SMALL, MARGIN_X, y, "Portal URL (for reference):");
        y += gfx.getLineHeight(FONT_SMALL) + 2;
        auto urlLines = gfx.wrappedText(FONT_SMALL, portalUrl, 480 - 2 * MARGIN_X, 2);
        for (auto& line : urlLines) {
            gfx.drawText(FONT_SMALL, MARGIN_X, y, line.c_str());
            y += gfx.getLineHeight(FONT_SMALL) + 2;
        }
    }

    int helpY = 800 - 56;
    gfx.drawText(FONT_SMALL, MARGIN_X, helpY,
                 "OK / BACK = pick another network    PWR = skip");

    display.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

void showOtaProgress(HalDisplay& display, int percent) {
    GfxRenderer& gfx = getRenderer(display);
    gfx.clearScreen();

    if (percent < 0) {
        // Discovery phase
        gfx.drawCenteredText(FONT_TITLE, 360, "Update available");
        gfx.drawCenteredText(FONT_SMALL, 400, "Downloading...");
    } else {
        // Download progress
        gfx.drawCenteredText(FONT_TITLE, 340, "Installing update");

        char pct[8];
        snprintf(pct, sizeof(pct), "%d%%", percent);
        gfx.drawCenteredText(FONT_REGULAR, 380, pct);

        // Progress bar
        int barW = 300, barH = 8;
        int barX = (480 - barW) / 2, barY = 420;
        gfx.drawRect(barX, barY, barW, barH);
        int fillW = barW * percent / 100;
        if (fillW > 0) gfx.fillRect(barX, barY, fillW, barH);
    }

    display.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}
