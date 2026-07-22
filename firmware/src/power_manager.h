#pragma once

#include <cstdint>

struct RtcState {
    int8_t cursor;
    uint8_t view;
    uint8_t item_count;
    int8_t day_offset;
};

RtcState rtcLoad();
void rtcSave(const RtcState& state);
void enterDeepSleep();
