#pragma once

#include <cstdint>

// The board's own sensors and controls, exposed as a single accessory made of
// several endpoints — a composed device, in Matter's terms. The parent carries
// the name; each child carries one function, and the parent's PartsList is what
// tells a controller they belong together.
//
// Unlike the accessories in builder.cpp these are not invented: the temperature
// is the PMU die, and the brightness really does drive the panel.
//
// Must run after esp_matter::start().
void internalsBegin();

// Publishes fresh readings and applies anything a controller asked of the
// panel, and runs the finder. Rate limited internally; call it from the ui
// task.
void internalsPoll();

// The finder: a beep every couple of seconds, and the panel flashing while it
// is awake, until something stops it. Matter starts and stops it by writing the
// Finder child's OnOff attribute, which the device writes back whenever a
// session ends by itself.
//
// A human stopping it locally goes through here. Touch stops it while the
// screen is awake; once the screen sleeps, only PWR or BOOT can stop it.
// Called from the touch and key paths rather than from a control of its own.
void internalsFinderStop();

// True while a session is running. The ui task wakes the screen on the edge:
// the finder does not own the sleep state machine, it just asks once.
bool internalsFinding();

// A short burst, for the Identify cluster. Ignores endpoints that are not part
// of this composed device.
void internalsIdentify(uint16_t endpointId);
