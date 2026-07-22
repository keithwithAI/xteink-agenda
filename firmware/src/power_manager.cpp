#include "power_manager.h"
#include "config.h"

#include <Arduino.h>
#include <esp_sleep.h>
#include <WiFi.h>
#include <HalGPIO.h>

RTC_DATA_ATTR static int8_t _cursor = -1;
RTC_DATA_ATTR static uint8_t _view = 0;
RTC_DATA_ATTR static uint8_t _item_count = 0;
RTC_DATA_ATTR static int8_t _day_offset = 0;

RtcState rtcLoad() {
    return { _cursor, _view, _item_count, _day_offset };
}

void rtcSave(const RtcState& state) {
    _cursor = state.cursor;
    _view = state.view;
    _item_count = state.item_count;
    _day_offset = state.day_offset;
}

void enterDeepSleep() {
    Serial.println("Powering off...");
    Serial.flush();

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

    // X4 hardware: power button cuts power entirely (cold boot on wake).
    // Deep sleep is effectively power-off. No timer wake possible.
    extern HalGPIO gpio;
    gpio.startDeepSleep();
}
