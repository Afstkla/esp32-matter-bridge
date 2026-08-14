#pragma once

#include <stdint.h>

#include "bridge.h"

// Loads the saved layout and creates the Matter node. Accessories come up in
// builderResume(), which must run after the stack has started.
void builderBegin();
void builderResume();

void builderDraw();
void builderTouch(int16_t x, int16_t y);
bool builderNeedsRedraw();

uint8_t builderSlotCount();
uint8_t builderMaxSlots();
uint8_t builderType(uint8_t slot);
AccessoryUi builderUi(uint8_t slot);
const char *builderLabel(uint8_t slot);

// Appends an accessory and brings it up immediately. False when full.
// BUILDER_PRESET_AUTO takes the first name nothing else is using.
static const uint8_t BUILDER_PRESET_AUTO = 0xFF;
bool builderAdd(uint8_t type, uint8_t preset = BUILDER_PRESET_AUTO);

// True when the tile grid is showing, rather than a screen layered over it.
bool builderAtRoot();

// -1 pages left, +1 pages right. Ignored unless the tile grid is on screen.
void builderSwipe(int8_t direction);

// Destroys the accessory's endpoint and empties its slot. Slots are not
// renumbered, so indices stay valid for everything already holding one.
void builderRemove(uint8_t slot);

bool builderSlotUsed(uint8_t slot);

// Clears the saved layout and reboots into the defaults.
void builderReset();

void builderPress(uint8_t slot);
void builderSetLevel(uint8_t slot, uint8_t value);
uint8_t builderLevel(uint8_t slot);
void builderSetOnOff(uint8_t slot, bool on);
bool builderOnOff(uint8_t slot);

// The one-line state the tiles show, so serial and screen never disagree.
void builderDescribe(uint8_t slot, char *out, size_t n);

// UI_FLAG accessories: a sensor reading one of two states.
bool builderFlag(uint8_t slot);
void builderToggleFlag(uint8_t slot);

// UI_VALUE accessories: a sensor reading, in hundredths.
int16_t builderValue(uint8_t slot);
void builderNudgeValue(uint8_t slot, int8_t direction);

// The hardware keys drive whichever level slot was last opened.
const char *builderActiveLevelLabel();
void builderNudgeLevel(int8_t direction);
