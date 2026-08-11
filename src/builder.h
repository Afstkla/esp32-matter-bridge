#pragma once

#include <stdint.h>

// Accessories are created before the Matter stack starts and can also be added
// while it is running; both paths go through here.
void builderBegin();

void builderDraw();
void builderTouch(int16_t x, int16_t y);
bool builderNeedsRedraw();

uint8_t builderSlotCount();
uint8_t builderMaxSlots();
bool builderIsButton(uint8_t slot);
const char *builderLabel(uint8_t slot);

// Appends an accessory and brings it up immediately. False when full.
bool builderAdd(bool isButton);

// Clears the saved layout and reboots into the defaults.
void builderReset();

void builderPress(uint8_t slot);
void builderSetLevel(uint8_t slot, uint8_t value);
uint8_t builderLevel(uint8_t slot);
void builderSetOnOff(uint8_t slot, bool on);
bool builderOnOff(uint8_t slot);

// The hardware keys drive whichever level slot was last opened.
const char *builderActiveLevelLabel();
void builderNudgeLevel(int8_t direction);
