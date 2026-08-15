#pragma once

#include <stdint.h>

void keysInit();

// Edge triggered: true once per press.
bool keyBootPressed();
bool keyPowerPressed();

// The AXP2101 has no current ADC, so this is voltages and state only.
void pmuReport();

// Which regulators the PMU is running, for the drain hunt: nothing here ever
// configures one, so every rail is at its power-on default and a rail with no
// load on this board is still a rail that is on.
void pmuDumpRails();

struct PmuStatus {
  bool present;
  uint16_t millivolts;
  uint8_t percent;
  float celsius;  // the PMU die, not the room
  bool charging;
  bool onUsb;
};

PmuStatus pmuStatus();
