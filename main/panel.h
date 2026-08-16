#pragma once

#include <cstdint>

constexpr int16_t PANEL_W = 368;
constexpr int16_t PANEL_H = 448;

bool panelInit();

// The two calls below run on the ui task and nowhere else: one esp_lcd panel IO
// handle carries both the frame and the brightness command, and it is not
// thread safe. Console commands that want the panel record their intent and
// let the ui tick apply it.
void panelFlush();
void panelBrightness(uint8_t level);

// How long the last full-frame transfer took. Frequency scaling moves this, so
// it is the number to watch when the power configuration changes.
uint32_t panelLastFlushUs();

// PSRAM DMA underflows the display driver has caught since boot. Each one cost
// a re-flush; a number that climbs is the bus asking for a slower pixel clock.
uint32_t panelUnderflows();

// Screen only — the CPU stays awake because Matter needs the WiFi link up.
void panelSleep();
void panelWake();
bool panelAsleep();

// The shallower pair: the rail stays up and the controller goes into SLPIN, so
// the digitiser on the same rail stays alive and the way back is a settle
// rather than a module cold start. Same ui-task-only rule as the pair above.
void panelDoze();
void panelRouse(uint32_t settleMs);
bool panelDozing();

// True while a finger is down; coordinates are panel pixels (368x448).
bool panelTouch(int16_t *x, int16_t *y);

// Dumps the touch registers as they change, for diagnosing a silent digitiser.
void panelTouchDump(uint32_t durationMs);
