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

// The CO5300 datasheet allows SLPOUT 120 ms to settle before the next command;
// the vendor init sequence waits 60. The conservative one is the default until
// a human has confirmed the picture comes back clean from a shorter one — there
// is no readback on a QSPI panel to confirm it from the firmware side.
constexpr uint32_t PANEL_SLPOUT_SETTLE_MS = 120;

// Tier 2: the module's rail goes down, which takes the digitiser with it, so the
// PWR key is the only way back and the way back is a cold start (~261 ms).
void panelSleep();
void panelWake();
bool panelAsleep();

// Tier 1: the rail stays up, the controller goes into SLPIN and the digitiser
// into standby, so a finger on the glass can wake the device through its INT
// line and the way back is a settle rather than a module cold start (~200 ms).
// Same ui-task-only rule as the pair above.
//
// A rouse that fails leaves the module in tier 2 — rail down, panelAsleep() —
// so the caller's next step is panelWake() and the full re-init.
void panelDoze();
void panelRouse(uint32_t settleMs);
bool panelDozing();

// Either tier: the screen is off, and nothing drawn reaches the glass.
bool panelDark();

// True while a finger is down; coordinates are panel pixels (368x448).
bool panelTouch(int16_t *x, int16_t *y);

// Dumps the touch registers as they change, for diagnosing a silent digitiser.
void panelTouchDump(uint32_t durationMs);
