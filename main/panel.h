#pragma once

#include <cstdint>

constexpr int16_t PANEL_W = 368;
constexpr int16_t PANEL_H = 448;

bool panelInit();
void panelFlush();
void panelBrightness(uint8_t level);

// Screen only — the CPU stays awake because Matter needs the WiFi link up.
void panelSleep();
void panelWake();
bool panelAsleep();

// True while a finger is down; coordinates are panel pixels (368x448).
bool panelTouch(int16_t *x, int16_t *y);

// Dumps the touch registers as they change, for diagnosing a silent digitiser.
void panelTouchDump(uint32_t durationMs);
