#pragma once

// Turns on dynamic frequency scaling and automatic light sleep, restores the
// persisted WiFi power-save mode and loads the battery drain log. Call after
// the Matter stack has started: esp_wifi_set_ps needs a WiFi driver to exist.
void powerBegin();

// Holds light sleep off while USB is attached and feeds the drain log. Called
// from the ui task on every tick — it reads the PMU through pmuStatus(), which
// is the firmware's one place that talks to the AXP2101 on a schedule.
void powerPoll();

// The digitiser's INT line (GPIO21) as tier 1's wake: armed on the way into the
// tier, disarmed on every way out. Arming clears the latch, so powerTouchWoke()
// only ever reports a fall that happened since. Both are for the ui task alone —
// it owns the panel, and it owns the wake sources with it.
void powerArmTouchWake(bool on);
bool powerTouchWoke();
