#pragma once

#include <stdint.h>

// Runtime power knobs. Automatic light sleep is off the table — the prebuilt
// Arduino libs ship without CONFIG_PM_ENABLE — so this is WiFi modem sleep
// plus CPU frequency scaling, which is everything the stock libs allow.
void powerBegin();

// Applies the asleep/awake CPU policy and samples the battery log.
void powerPoll(bool panelAsleep);

bool powerSetWifiPs(const char *mode);  // "none" | "min" | "max"
bool powerSetCpuMhz(uint32_t mhz);      // awake frequency: 80 | 160 | 240

void powerReport();
void powerDumpLog();
