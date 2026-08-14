#pragma once

#include <stdint.h>

void keysInit();

// Edge triggered: true once per press.
bool keyBootPressed();
bool keyPowerPressed();

// The AXP2101 has no current ADC, so this is voltages and state only.
void pmuReport();

struct PmuStatus {
  bool present;
  uint16_t millivolts;
  uint8_t percent;
  float celsius;  // the PMU die, not the room
  bool charging;
  bool onUsb;
};

PmuStatus pmuStatus();
