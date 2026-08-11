#include "builder.h"

#include <Matter.h>
#include <Preferences.h>

#include "bridge.h"
#include "panel.h"

static const uint8_t MAX_SLOTS = 12;

// Renaming without a keyboard: tap the title to cycle through these. The
// chosen name becomes the accessory's NodeLabel, so it reaches Apple Home too.
static const char *PRESETS[] = {"Lights", "Lamp",    "Music",  "Volume", "Scene",
                                "Movie",  "Blinds",  "Shades", "Coffee", "Kettle",
                                "Fan",    "Heater",  "Away",   "Night",  "Morning",
                                "Desk",   "Kitchen", "Living", "Office", "Garden"};
static const uint8_t PRESET_COUNT = sizeof(PRESETS) / sizeof(PRESETS[0]);

// Persisted verbatim as one NVS blob, so the layout must stay stable.
struct Slot {
  uint8_t isButton;
  uint8_t preset;
};

static Slot s_slot[MAX_SLOTS];
static uint8_t s_count = 0;
static BridgedAccessory s_accessory[MAX_SLOTS];

static Preferences s_prefs;
static bool s_dirty = true;
static int8_t s_openSlot = -1;
static bool s_addOpen = false;
static uint8_t s_page = 0;
static uint8_t s_activeLevel = 0;

static const int16_t PANEL_W = 368;
static const int16_t PANEL_H = 448;
static const int16_t TILE_W = 167;
static const int16_t TILE_H = 100;
static const int16_t TILE_X0 = 12;
static const int16_t TILE_Y0 = 62;
static const int16_t TILE_GAP = 10;
static const uint8_t PER_PAGE = 6;

static const uint16_t COL_BG = RGB565(28, 28, 32);
static const uint16_t COL_MUTED = RGB565(150, 150, 160);
static const uint16_t COL_FAINT = RGB565(110, 110, 120);
static const uint16_t COL_BUTTON = RGB565(70, 140, 240);
static const uint16_t COL_LEVEL = RGB565(240, 160, 60);

uint8_t builderSlotCount() {
  return s_count;
}

uint8_t builderMaxSlots() {
  return MAX_SLOTS;
}

bool builderIsButton(uint8_t slot) {
  return slot < s_count && s_slot[slot].isButton;
}

const char *builderLabel(uint8_t slot) {
  return slot < s_count ? PRESETS[s_slot[slot].preset % PRESET_COUNT] : "";
}

static uint16_t accentFor(uint8_t slot) {
  return builderIsButton(slot) ? COL_BUTTON : COL_LEVEL;
}

static void persistSlots() {
  s_prefs.putBytes("slots", s_slot, (size_t)s_count * sizeof(Slot));
}

void builderPress(uint8_t slot) {
  if (!builderIsButton(slot)) {
    return;
  }
  s_accessory[slot].click();
  Serial.printf("PRESS %u %s\n", slot, builderLabel(slot));
}

uint8_t builderLevel(uint8_t slot) {
  return slot < s_count && !builderIsButton(slot) ? s_accessory[slot].getBrightness() : 0;
}

void builderSetLevel(uint8_t slot, uint8_t value) {
  if (slot >= s_count || builderIsButton(slot)) {
    return;
  }
  s_accessory[slot].setBrightness(value);
  Serial.printf("LEVEL %u %u\n", slot, value);
  s_dirty = true;
}

void builderSetOnOff(uint8_t slot, bool on) {
  if (slot >= s_count || builderIsButton(slot)) {
    return;
  }
  s_accessory[slot].setOnOff(on);
  Serial.printf("ONOFF %u %d\n", slot, on ? 1 : 0);
  s_dirty = true;
}

bool builderOnOff(uint8_t slot) {
  return slot < s_count && !builderIsButton(slot) && s_accessory[slot].getOnOff();
}

const char *builderActiveLevelLabel() {
  return builderLabel(s_activeLevel);
}

// Matter brightness is 0..255, but the UI and the keys work in whole percent.
// Both conversions round rather than truncate, so a 10% step is exactly 10% in
// both directions instead of drifting by the 0.5 that 255/10 leaves over.
static uint8_t percentOf(uint8_t level) {
  return (uint8_t)(((uint16_t)level * 100 + 127) / 255);
}

static uint8_t levelOf(uint8_t percent) {
  return (uint8_t)(((uint16_t)percent * 255 + 50) / 100);
}

static void nudgeSlot(uint8_t slot, int8_t direction) {
  if (slot >= s_count || builderIsButton(slot)) {
    return;
  }
  int16_t percent = (int16_t)percentOf(builderLevel(slot)) + direction * 10;
  builderSetLevel(slot, levelOf((uint8_t)constrain(percent, 0, 100)));
}

void builderNudgeLevel(int8_t direction) {
  nudgeSlot(s_activeLevel, direction);
}

static void startAccessory(uint8_t slot) {
  const char *name = builderLabel(slot);
  if (s_slot[slot].isButton) {
    s_accessory[slot].beginSwitch(name);
    return;
  }

  s_accessory[slot].beginLight(name, true, 128);
  s_accessory[slot].onChangeOnOff([slot](bool on) {
    Serial.printf("HK slot=%u onoff=%d\n", slot, on ? 1 : 0);
    s_dirty = true;
  });
  s_accessory[slot].onChangeBrightness([slot](uint8_t level) {
    Serial.printf("HK slot=%u level=%u\n", slot, level);
    s_dirty = true;
  });
  s_activeLevel = slot;
}

static void seedDefaults() {
  static const Slot DEFAULTS[] = {{1, 0}, {1, 2}, {1, 4}, {1, 5}, {0, 3}, {0, 6}};
  s_count = sizeof(DEFAULTS) / sizeof(DEFAULTS[0]);
  memcpy(s_slot, DEFAULTS, sizeof(DEFAULTS));
  persistSlots();
}

void builderBegin() {
  s_prefs.begin("builder", false);

  size_t stored = s_prefs.getBytesLength("slots");
  if (stored >= sizeof(Slot) && stored <= sizeof(s_slot) && stored % sizeof(Slot) == 0) {
    s_prefs.getBytes("slots", s_slot, stored);
    s_count = (uint8_t)(stored / sizeof(Slot));
  } else {
    seedDefaults();
  }

  bridgeBegin();
  for (uint8_t slot = 0; slot < s_count; slot++) {
    startAccessory(slot);
  }
}

// Two accessories sharing a name is legal but miserable to pick apart in the
// Home app, so a new one takes the first preset nothing else is using.
static uint8_t unusedPreset() {
  for (uint8_t preset = 0; preset < PRESET_COUNT; preset++) {
    bool taken = false;
    for (uint8_t slot = 0; slot < s_count && !taken; slot++) {
      taken = s_slot[slot].preset == preset;
    }
    if (!taken) {
      return preset;
    }
  }
  return 0;
}

// Restarting rather than bringing the accessory up live is deliberate. An
// endpoint created while the stack is running takes the next free id, which is
// not the id the same accessory gets from a cold boot's creation order, so it
// would change identity on the next restart and controllers would re-add it.
// Recreating everything in order keeps ids stable for good.
bool builderAdd(bool isButton) {
  if (s_count >= MAX_SLOTS) {
    Serial.println("ADD full");
    return false;
  }
  uint8_t slot = s_count;
  s_slot[slot].isButton = isButton ? 1 : 0;
  s_slot[slot].preset = unusedPreset();
  s_count++;
  persistSlots();
  Serial.printf("ADD %u %s %s, restarting\n", slot, isButton ? "button" : "level",
                builderLabel(slot));
  delay(200);
  ESP.restart();
  return true;
}

void builderReset() {
  s_prefs.remove("slots");
  Serial.println("RESET slots cleared, restarting");
  delay(200);
  ESP.restart();
}

bool builderNeedsRedraw() {
  bool was = s_dirty;
  s_dirty = false;
  return was;
}

static uint8_t tileCount() {
  return s_count < MAX_SLOTS ? (uint8_t)(s_count + 1) : s_count;
}

static uint8_t pageCount() {
  return (uint8_t)((tileCount() + PER_PAGE - 1) / PER_PAGE);
}

static void tileRect(uint8_t index, int16_t *x, int16_t *y) {
  uint8_t cell = index % PER_PAGE;
  *x = TILE_X0 + (cell % 2) * (TILE_W + TILE_GAP);
  *y = TILE_Y0 + (cell / 2) * (TILE_H + TILE_GAP);
}

static void drawText(int16_t x, int16_t y, const char *text, uint8_t size, uint16_t colour) {
  Arduino_Canvas &g = panelCanvas();
  g.setTextSize(size);
  g.setTextColor(colour);
  g.setCursor(x, y);
  g.print(text);
}

static void drawCentred(int16_t y, const char *text, uint8_t size, uint16_t colour) {
  drawText((PANEL_W - (int16_t)strlen(text) * 6 * size) / 2, y, text, size, colour);
}

static void drawList() {
  Arduino_Canvas &g = panelCanvas();
  g.fillScreen(RGB565_BLACK);
  drawText(14, 24, "Accessories", 2, RGB565_WHITE);
  g.fillCircle(PANEL_W - 24, 30, 7,
               Matter.isDeviceCommissioned() ? RGB565(90, 220, 130) : RGB565(230, 90, 90));

  if (pageCount() > 1) {
    char page[8];
    snprintf(page, sizeof(page), "%u/%u", s_page + 1, pageCount());
    drawText(240, 24, "<", 2, COL_MUTED);
    drawText(264, 26, page, 1, COL_MUTED);
    drawText(304, 24, ">", 2, COL_MUTED);
  }

  uint8_t first = s_page * PER_PAGE;
  for (uint8_t i = first; i < tileCount() && i < first + PER_PAGE; i++) {
    int16_t x, y;
    tileRect(i, &x, &y);

    if (i >= s_count) {
      g.drawRoundRect(x, y, TILE_W, TILE_H, 12, RGB565(70, 70, 80));
      drawText(x + TILE_W / 2 - 12, y + 34, "+", 4, COL_MUTED);
      continue;
    }

    g.fillRoundRect(x, y, TILE_W, TILE_H, 12, COL_BG);
    g.drawRoundRect(x, y, TILE_W, TILE_H, 12, accentFor(i));
    drawText(x + 12, y + 18, builderLabel(i), 2, RGB565_WHITE);

    char detail[24];
    if (builderIsButton(i)) {
      snprintf(detail, sizeof(detail), "button");
    } else {
      snprintf(detail, sizeof(detail), "level  %u%%  %s", percentOf(builderLevel(i)),
               builderOnOff(i) ? "on" : "off");
    }
    drawText(x + 12, y + 62, detail, 1, COL_MUTED);
  }

  char hint[40];
  snprintf(hint, sizeof(hint), "BOOT +/- PWR  %s", builderActiveLevelLabel());
  drawCentred(PANEL_H - 22, hint, 1, COL_FAINT);
}

static void drawAdd() {
  Arduino_Canvas &g = panelCanvas();
  g.fillScreen(RGB565_BLACK);
  drawText(16, 24, "< back", 2, COL_MUTED);
  drawCentred(80, "New accessory", 2, RGB565_WHITE);

  g.fillRoundRect(44, 140, 280, 110, 20, COL_BUTTON);
  drawCentred(184, "BUTTON", 3, RGB565_BLACK);
  g.fillRoundRect(44, 270, 280, 110, 20, COL_LEVEL);
  drawCentred(314, "LEVEL", 3, RGB565_BLACK);
}

static void drawDetail(uint8_t slot) {
  Arduino_Canvas &g = panelCanvas();
  g.fillScreen(RGB565_BLACK);
  drawText(16, 24, "< back", 2, COL_MUTED);
  drawCentred(72, builderLabel(slot), 3, RGB565_WHITE);
  drawCentred(104, "tap name to rename", 1, COL_FAINT);

  if (builderIsButton(slot)) {
    g.fillRoundRect(44, 150, 280, 190, 24, accentFor(slot));
    drawCentred(232, "PRESS", 3, RGB565_BLACK);
    return;
  }

  uint8_t level = builderLevel(slot);
  g.fillRoundRect(30, 180, 90, 120, 16, RGB565(40, 40, 46));
  drawText(66, 228, "-", 4, RGB565_WHITE);
  g.fillRoundRect(248, 180, 90, 120, 16, RGB565(40, 40, 46));
  drawText(284, 228, "+", 4, RGB565_WHITE);

  g.drawRoundRect(134, 180, 100, 120, 12, RGB565(70, 70, 80));
  int16_t fill = (int16_t)((uint32_t)level * 116 / 255);
  g.fillRoundRect(136, 180 + 2 + (116 - fill), 96, fill, 10, accentFor(slot));

  char buf[8];
  snprintf(buf, sizeof(buf), "%u%%", percentOf(level));
  drawCentred(322, buf, 2, RGB565_WHITE);

  bool on = builderOnOff(slot);
  g.fillRoundRect(60, 356, 248, 56, 16, on ? accentFor(slot) : RGB565(40, 40, 46));
  drawCentred(376, on ? "ON" : "OFF", 3, on ? RGB565_BLACK : COL_MUTED);
}

void builderDraw() {
  if (s_addOpen) {
    drawAdd();
  } else if (s_openSlot < 0) {
    drawList();
  } else {
    drawDetail((uint8_t)s_openSlot);
  }
  panelFlush();
}

static bool inRect(int16_t x, int16_t y, int16_t rx, int16_t ry, int16_t rw, int16_t rh) {
  return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static void cycleName(uint8_t slot) {
  s_slot[slot].preset = (uint8_t)((s_slot[slot].preset + 1) % PRESET_COUNT);
  persistSlots();
  s_accessory[slot].setName(builderLabel(slot));
  Serial.printf("NAME %u %s\n", slot, builderLabel(slot));
}

static void touchList(int16_t x, int16_t y) {
  if (pageCount() > 1 && inRect(x, y, 230, 4, 50, 52)) {
    s_page = (uint8_t)((s_page + pageCount() - 1) % pageCount());
    s_dirty = true;
    return;
  }
  if (pageCount() > 1 && inRect(x, y, 294, 4, 50, 52)) {
    s_page = (uint8_t)((s_page + 1) % pageCount());
    s_dirty = true;
    return;
  }

  uint8_t first = s_page * PER_PAGE;
  for (uint8_t i = first; i < tileCount() && i < first + PER_PAGE; i++) {
    int16_t tx, ty;
    tileRect(i, &tx, &ty);
    if (!inRect(x, y, tx, ty, TILE_W, TILE_H)) {
      continue;
    }
    if (i >= s_count) {
      s_addOpen = true;
    } else {
      s_openSlot = (int8_t)i;
      if (!builderIsButton(i)) {
        s_activeLevel = i;
      }
    }
    s_dirty = true;
    return;
  }
}

static void touchAdd(int16_t x, int16_t y) {
  if (inRect(x, y, 0, 0, 130, 56)) {
    s_addOpen = false;
    s_dirty = true;
    return;
  }

  bool isButton = inRect(x, y, 44, 140, 280, 110);
  if (!isButton && !inRect(x, y, 44, 270, 280, 110)) {
    return;
  }

  // builderAdd restarts the device, so say so before the screen goes dark.
  Arduino_Canvas &g = panelCanvas();
  g.fillScreen(RGB565_BLACK);
  drawCentred(200, "Adding accessory", 2, RGB565_WHITE);
  drawCentred(236, "restarting", 1, COL_MUTED);
  panelFlush();
  builderAdd(isButton);
}

static void touchDetail(uint8_t slot, int16_t x, int16_t y) {
  if (inRect(x, y, 0, 0, 130, 56)) {
    s_openSlot = -1;
    s_dirty = true;
    return;
  }
  if (inRect(x, y, 40, 56, 288, 40)) {
    cycleName(slot);
    s_dirty = true;
    return;
  }
  if (builderIsButton(slot)) {
    if (inRect(x, y, 44, 150, 280, 190)) {
      builderPress(slot);
    }
    return;
  }
  if (inRect(x, y, 30, 180, 90, 120)) {
    nudgeSlot(slot, -1);
  } else if (inRect(x, y, 248, 180, 90, 120)) {
    nudgeSlot(slot, 1);
  } else if (inRect(x, y, 60, 356, 248, 56)) {
    builderSetOnOff(slot, !builderOnOff(slot));
  }
}

void builderTouch(int16_t x, int16_t y) {
  if (s_addOpen) {
    touchAdd(x, y);
  } else if (s_openSlot < 0) {
    touchList(x, y);
  } else {
    touchDetail((uint8_t)s_openSlot, x, y);
  }
}
