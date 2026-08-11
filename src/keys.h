#pragma once

void keysInit();

// Edge triggered: true once per press.
bool keyBootPressed();
bool keyPowerPressed();

// The AXP2101 has no current ADC, so this is voltages and state only.
void pmuReport();
