#pragma once
#include <Arduino_GFX_Library.h>

bool panelInit();
Arduino_Canvas &panelCanvas();
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
