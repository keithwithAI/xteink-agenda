#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HalDisplay.h>

#include "config.h"

struct AgendaItem {
    char id[32];
    char title[64];
    char start[6];    // "HH:MM"
    char end[6];
    uint16_t duration_min;
    bool is_task;
    bool completed;
};

struct Agenda {
    char view_name[16];
    char date_label[20];  // "Friday 10 Apr"
    uint8_t view_count;
    int8_t day_offset;
    AgendaItem items[MAX_ITEMS];
    uint8_t item_count;
    int8_t current_index;   // Item happening now (-1 if none)
    int8_t conflict_index;  // Second overlapping event (-1 if none)
    uint16_t wake_in_sec;  // Seconds until current task ends (for precise sleep timer)
    float day_progress;
};

bool fetchAgenda(uint8_t view, int8_t day_offset, Agenda& out);
bool postComplete(uint8_t view, uint8_t cursor, int8_t day_offset = 0);
bool postDeleteEvent(uint8_t view, uint8_t cursor, int8_t day_offset = 0);
bool postReschedule(HalDisplay& display);
