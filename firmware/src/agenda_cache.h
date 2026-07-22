#pragma once

#include "http_client.h"

/// Save agenda to SPIFFS for offline display.
bool cacheAgenda(const Agenda& agenda);

/// Load cached agenda from SPIFFS. Returns false if no cache exists.
bool loadCachedAgenda(Agenda& out);

/// Store a pending task completion for retry when WiFi is available.
/// Saves view + cursor to a queue file on SPIFFS.
bool queueCompletion(uint8_t view, uint8_t cursor);

/// Replay any pending completions via the server API.
/// Returns number of completions successfully replayed.
int drainPendingCompletions();
