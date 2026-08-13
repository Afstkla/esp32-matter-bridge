#include "builder.h"

#include <Matter.h>
#include <Preferences.h>

#include "bridge.h"
#include "panel.h"
#include "theme.h"

static const uint8_t MAX_SLOTS = 12;

// The aggregator takes the first endpoint after the root node, so accessories
// start one past it.
static const uint16_t FIRST_ACCESSORY_ENDPOINT_ID = 2;

// Renaming without a keyboard: tap the title to cycle through these. The
// chosen name becomes the accessory's NodeLabel, so it reaches Apple Home too.
static const char *PRESETS[] = {"Lights", "Lamp",    "Music",  "Volume", "Scene",
                                "Movie",  "Blinds",  "Shades", "Coffee", "Kettle",
                                "Fan",    "Heater",  "Away",   "Night",  "Morning",
                                "Desk",   "Kitchen", "Living", "Office", "Garden"};
static const uint8_t PRESET_COUNT = sizeof(PRESETS) / sizeof(PRESETS[0]);

// A removed accessory leaves its slot empty rather than shifting the ones after
// it down. Each live endpoint holds a pointer to its BridgedAccessory as
// priv_data, and its NodeLabel points into that object's name buffer, so
// nothing in the array may move while the stack is running.
static const uint8_t PRESET_FREE = 0xFF;

// Persisted verbatim as one NVS blob, so the layout must stay stable. It is
// held under its own key rather than versioned in-band: the old two-byte layout
// and this one share divisors, so a length alone cannot tell them apart.
static const char *SLOTS_KEY = "slots2";
static const char *LEGACY_SLOTS_KEY = "slots";

struct Slot {
  uint8_t isButton;
  uint8_t preset;
  uint16_t endpointId;
};

struct LegacySlot {
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
static bool s_confirmRemove = false;
static bool s_pickOpen = false;
static int8_t s_pickSlot = -1;  // -1 while choosing the name for a new accessory
static bool s_pickIsButton = false;
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
static const int16_t PICK_Y0 = 62;
static const int16_t PICK_ROW_H = 34;

uint8_t builderSlotCount() {
  return s_count;
}

uint8_t builderMaxSlots() {
  return MAX_SLOTS;
}

bool builderSlotUsed(uint8_t slot) {
  return slot < s_count && s_slot[slot].preset != PRESET_FREE;
}

bool builderIsButton(uint8_t slot) {
  return builderSlotUsed(slot) && s_slot[slot].isButton;
}

const char *builderLabel(uint8_t slot) {
  return builderSlotUsed(slot) ? PRESETS[s_slot[slot].preset % PRESET_COUNT] : "";
}

// Tiles show only the slots still in use, so a tile index is not a slot index.
static uint8_t usedSlots(uint8_t *out) {
  uint8_t n = 0;
  for (uint8_t slot = 0; slot < s_count; slot++) {
    if (builderSlotUsed(slot)) {
      out[n++] = slot;
    }
  }
  return n;
}

static uint8_t usedCount() {
  uint8_t used[MAX_SLOTS];
  return usedSlots(used);
}

static uint16_t accentFor(uint8_t slot) {
  return builderIsButton(slot) ? COL_BUTTON : COL_LEVEL;
}

static void persistSlots() {
  s_prefs.putBytes(SLOTS_KEY, s_slot, (size_t)s_count * sizeof(Slot));
}

void builderPress(uint8_t slot) {
  if (!builderIsButton(slot)) {
    return;
  }
  s_accessory[slot].click();
  Serial.printf("PRESS %u %s\n", slot, builderLabel(slot));
}

uint8_t builderLevel(uint8_t slot) {
  return builderSlotUsed(slot) && !builderIsButton(slot) ? s_accessory[slot].getBrightness() : 0;
}

void builderSetLevel(uint8_t slot, uint8_t value) {
  if (!builderSlotUsed(slot) || builderIsButton(slot)) {
    return;
  }
  s_accessory[slot].setBrightness(value);
  Serial.printf("LEVEL %u %u\n", slot, value);
  s_dirty = true;
}

void builderSetOnOff(uint8_t slot, bool on) {
  if (!builderSlotUsed(slot) || builderIsButton(slot)) {
    return;
  }
  s_accessory[slot].setOnOff(on);
  Serial.printf("ONOFF %u %d\n", slot, on ? 1 : 0);
  s_dirty = true;
}

bool builderOnOff(uint8_t slot) {
  return builderSlotUsed(slot) && !builderIsButton(slot) && s_accessory[slot].getOnOff();
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
  if (!builderSlotUsed(slot) || builderIsButton(slot)) {
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
  uint16_t wanted = s_slot[slot].endpointId;

  bool ok = s_slot[slot].isButton ? s_accessory[slot].beginSwitch(name, wanted)
                                  : s_accessory[slot].beginLight(name, true, 128, wanted);
  if (!ok) {
    return;
  }

  uint16_t assigned = (uint16_t)s_accessory[slot].getEndPointId();
  if (assigned != wanted) {
    s_slot[slot].endpointId = assigned;
    persistSlots();
  }
  if (s_slot[slot].isButton) {
    return;
  }

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
  static const Slot DEFAULTS[] = {{1, 0, 0}, {1, 2, 0}, {1, 4, 0},
                                  {1, 5, 0}, {0, 3, 0}, {0, 6, 0}};
  s_count = sizeof(DEFAULTS) / sizeof(DEFAULTS[0]);
  memcpy(s_slot, DEFAULTS, sizeof(DEFAULTS));
  persistSlots();
}

static bool loadSlots() {
  size_t stored = s_prefs.getBytesLength(SLOTS_KEY);
  if (stored < sizeof(Slot) || stored > sizeof(s_slot) || stored % sizeof(Slot) != 0) {
    return false;
  }
  s_prefs.getBytes(SLOTS_KEY, s_slot, stored);
  s_count = (uint8_t)(stored / sizeof(Slot));
  return true;
}

// Accessories saved before endpoint ids were persisted were created in list
// order straight after the aggregator, so that is the id each one still holds
// in every controller that has already seen it.
static bool migrateLegacySlots() {
  size_t stored = s_prefs.getBytesLength(LEGACY_SLOTS_KEY);
  if (stored < sizeof(LegacySlot) || stored % sizeof(LegacySlot) != 0) {
    return false;
  }
  uint8_t count = (uint8_t)(stored / sizeof(LegacySlot));
  if (count > MAX_SLOTS) {
    return false;
  }

  LegacySlot legacy[MAX_SLOTS];
  s_prefs.getBytes(LEGACY_SLOTS_KEY, legacy, stored);
  s_count = count;
  for (uint8_t slot = 0; slot < count; slot++) {
    s_slot[slot].isButton = legacy[slot].isButton;
    s_slot[slot].preset = legacy[slot].preset;
    s_slot[slot].endpointId = (uint16_t)(FIRST_ACCESSORY_ENDPOINT_ID + slot);
  }
  persistSlots();
  s_prefs.remove(LEGACY_SLOTS_KEY);
  Serial.printf("SLOTS migrated %u accessories to persisted endpoint ids\n", count);
  return true;
}

void builderBegin() {
  s_prefs.begin("builder", false);
  if (!loadSlots() && !migrateLegacySlots()) {
    seedDefaults();
  }
  bridgeBegin();
}

// Deliberately after esp_matter::start(): reclaiming an endpoint id needs the
// stack's counter restored from NVS first, so accessories cannot come up with
// the node.
void builderResume() {
  for (uint8_t slot = 0; slot < s_count; slot++) {
    if (builderSlotUsed(slot)) {
      startAccessory(slot);
    }
  }
}

// Two accessories sharing a name is legal but miserable to pick apart in the
// Home app, so a new one takes the first preset nothing else is using.
static uint8_t unusedPreset() {
  for (uint8_t preset = 0; preset < PRESET_COUNT; preset++) {
    bool taken = false;
    for (uint8_t slot = 0; slot < s_count && !taken; slot++) {
      taken = builderSlotUsed(slot) && s_slot[slot].preset == preset;
    }
    if (!taken) {
      return preset;
    }
  }
  return 0;
}

static int8_t claimSlot() {
  for (uint8_t slot = 0; slot < s_count; slot++) {
    if (!builderSlotUsed(slot)) {
      return (int8_t)slot;
    }
  }
  return s_count < MAX_SLOTS ? (int8_t)s_count : -1;
}

bool builderAdd(bool isButton, uint8_t preset) {
  int8_t claimed = claimSlot();
  if (claimed < 0) {
    Serial.println("ADD full");
    return false;
  }
  uint8_t slot = (uint8_t)claimed;
  if (slot == s_count) {
    s_count++;
  }
  s_slot[slot].isButton = isButton ? 1 : 0;
  s_slot[slot].preset = preset == BUILDER_PRESET_AUTO ? unusedPreset() : preset;
  s_slot[slot].endpointId = 0;
  persistSlots();
  startAccessory(slot);

  s_addOpen = false;
  s_page = (uint8_t)((usedCount() - 1) / PER_PAGE);
  s_dirty = true;
  Serial.printf("ADD %u %s %s\n", slot, isButton ? "button" : "level", builderLabel(slot));
  return true;
}

static uint8_t tileCount() {
  uint8_t used = usedCount();
  return used < MAX_SLOTS ? (uint8_t)(used + 1) : used;
}

static uint8_t pageCount() {
  return (uint8_t)((tileCount() + PER_PAGE - 1) / PER_PAGE);
}

// The slot is emptied rather than reclaimed: startAccessory() would otherwise
// hand a stale endpoint id to a different accessory.
void builderRemove(uint8_t slot) {
  if (!builderSlotUsed(slot)) {
    return;
  }
  if (!s_accessory[slot].remove()) {
    Serial.printf("REMOVE %u failed\n", slot);
    return;
  }
  Serial.printf("REMOVE %u %s\n", slot, builderLabel(slot));

  s_slot[slot].preset = PRESET_FREE;
  s_slot[slot].endpointId = 0;
  while (s_count > 0 && !builderSlotUsed((uint8_t)(s_count - 1))) {
    s_count--;
  }
  persistSlots();

  if (s_activeLevel == slot) {
    s_activeLevel = 0;
    for (uint8_t i = 0; i < s_count; i++) {
      if (builderSlotUsed(i) && !builderIsButton(i)) {
        s_activeLevel = i;
        break;
      }
    }
  }
  s_openSlot = -1;
  s_confirmRemove = false;
  if (s_page >= pageCount()) {
    s_page = (uint8_t)(pageCount() - 1);
  }
  s_dirty = true;
}

void builderReset() {
  s_prefs.remove(SLOTS_KEY);
  s_prefs.remove(LEGACY_SLOTS_KEY);
  Serial.println("RESET slots cleared, restarting");
  delay(200);
  ESP.restart();
}

bool builderAtRoot() {
  return !s_addOpen && !s_pickOpen && s_openSlot < 0;
}

bool builderNeedsRedraw() {
  bool was = s_dirty;
  s_dirty = false;
  return was;
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

static void drawPageDots(uint8_t current) {
  uint8_t pages = pageCount();
  if (pages < 2) {
    return;
  }
  Arduino_Canvas &g = panelCanvas();
  const int16_t spacing = 18;
  int16_t x = (PANEL_W - (pages - 1) * spacing) / 2;
  for (uint8_t page = 0; page < pages; page++) {
    g.fillCircle(x + page * spacing, PANEL_H - 44, 4,
                 page == current ? COL_WHITE : COL_OUTLINE);
  }
}

// Everything that does not travel with the pages. Keeping it still during a
// slide is both cheaper and closer to how a paged home screen behaves.
static void drawChrome(uint8_t current) {
  Arduino_Canvas &g = panelCanvas();
  drawText(14, 24, "Accessories", 2, COL_WHITE);
  g.fillCircle(PANEL_W - 24, 30, 7,
               Matter.isDeviceCommissioned() ? COL_OK : COL_ALERT);
  drawPageDots(current);

  char hint[40];
  snprintf(hint, sizeof(hint), "BOOT +/- PWR  %s", builderActiveLevelLabel());
  drawCentred(PANEL_H - 22, hint, 1, COL_FAINT);
}

static void drawTiles(uint8_t page, int16_t dx) {
  Arduino_Canvas &g = panelCanvas();
  uint8_t used[MAX_SLOTS];
  uint8_t count = usedSlots(used);
  uint8_t first = page * PER_PAGE;
  for (uint8_t i = first; i < tileCount() && i < first + PER_PAGE; i++) {
    int16_t x, y;
    tileRect(i, &x, &y);
    x += dx;
    if (x + TILE_W <= 0 || x >= PANEL_W) {
      continue;
    }

    if (i >= count) {
      g.drawRoundRect(x, y, TILE_W, TILE_H, 12, COL_OUTLINE);
      drawText(x + TILE_W / 2 - 12, y + 34, "+", 4, COL_MUTED);
      continue;
    }

    uint8_t slot = used[i];
    g.fillRoundRect(x, y, TILE_W, TILE_H, 12, COL_BG);
    g.drawRoundRect(x, y, TILE_W, TILE_H, 12, accentFor(slot));
    drawText(x + 12, y + 18, builderLabel(slot), 2, COL_WHITE);

    char detail[24];
    if (builderIsButton(slot)) {
      snprintf(detail, sizeof(detail), "button");
    } else {
      snprintf(detail, sizeof(detail), "level  %u%%  %s", percentOf(builderLevel(slot)),
               builderOnOff(slot) ? "on" : "off");
    }
    drawText(x + 12, y + 62, detail, 1, COL_MUTED);
  }
}

static void drawList() {
  panelCanvas().fillScreen(COL_BLACK);
  drawChrome(s_page);
  drawTiles(s_page, 0);
}

// Two accessories sharing a name is legal but miserable to tell apart in the
// Home app, so a name already spoken for is shown but cannot be picked.
static bool presetTaken(uint8_t preset, int8_t exceptSlot) {
  for (uint8_t slot = 0; slot < s_count; slot++) {
    if ((int8_t)slot == exceptSlot || !builderSlotUsed(slot)) {
      continue;
    }
    if (s_slot[slot].preset == preset) {
      return true;
    }
  }
  return false;
}

static void pickCell(uint8_t index, int16_t *x, int16_t *y) {
  *x = TILE_X0 + (index % 2) * (TILE_W + TILE_GAP);
  *y = PICK_Y0 + (index / 2) * PICK_ROW_H;
}

static void drawPicker() {
  Arduino_Canvas &g = panelCanvas();
  g.fillScreen(COL_BLACK);
  drawText(16, 24, "< back", 2, COL_MUTED);

  for (uint8_t preset = 0; preset < PRESET_COUNT; preset++) {
    int16_t x, y;
    pickCell(preset, &x, &y);
    bool mine = s_pickSlot >= 0 && s_slot[s_pickSlot].preset == preset;
    if (mine) {
      g.fillRoundRect(x, y, TILE_W, PICK_ROW_H - 4, 8, COL_BG);
    }
    drawText(x + 12, y + 8, PRESETS[preset], 2,
             presetTaken(preset, s_pickSlot) ? COL_OUTLINE : COL_WHITE);
  }
}

static void drawAdd() {
  Arduino_Canvas &g = panelCanvas();
  g.fillScreen(COL_BLACK);
  drawText(16, 24, "< back", 2, COL_MUTED);
  drawCentred(80, "New accessory", 2, COL_WHITE);

  g.fillRoundRect(44, 140, 280, 110, 20, COL_BUTTON);
  drawCentred(184, "BUTTON", 3, COL_BLACK);
  g.fillRoundRect(44, 270, 280, 110, 20, COL_LEVEL);
  drawCentred(314, "LEVEL", 3, COL_BLACK);
}

static void drawDetail(uint8_t slot) {
  Arduino_Canvas &g = panelCanvas();
  g.fillScreen(COL_BLACK);
  drawText(16, 24, "< back", 2, COL_MUTED);
  drawText(PANEL_W - 100, 26, s_confirmRemove ? "tap to confirm" : "remove", 1,
           s_confirmRemove ? COL_DANGER : COL_FAINT);
  drawCentred(72, builderLabel(slot), 3, COL_WHITE);
  drawCentred(104, "tap name to rename", 1, COL_FAINT);

  if (builderIsButton(slot)) {
    g.fillRoundRect(44, 150, 280, 190, 24, accentFor(slot));
    drawCentred(232, "PRESS", 3, COL_BLACK);
    return;
  }

  uint8_t level = builderLevel(slot);
  g.fillRoundRect(30, 180, 90, 120, 16, COL_WELL);
  drawText(66, 228, "-", 4, COL_WHITE);
  g.fillRoundRect(248, 180, 90, 120, 16, COL_WELL);
  drawText(284, 228, "+", 4, COL_WHITE);

  g.drawRoundRect(134, 180, 100, 120, 12, COL_OUTLINE);
  int16_t fill = (int16_t)((uint32_t)level * 116 / 255);
  g.fillRoundRect(136, 180 + 2 + (116 - fill), 96, fill, 10, accentFor(slot));

  char buf[8];
  snprintf(buf, sizeof(buf), "%u%%", percentOf(level));
  drawCentred(322, buf, 2, COL_WHITE);

  bool on = builderOnOff(slot);
  g.fillRoundRect(60, 356, 248, 56, 16, on ? accentFor(slot) : COL_WELL);
  drawCentred(376, on ? "ON" : "OFF", 3, on ? COL_BLACK : COL_MUTED);
}

void builderDraw() {
  if (s_pickOpen) {
    drawPicker();
  } else if (s_addOpen) {
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

// Eased travel, as a percentage of the screen width. The last step is the
// normal redraw the caller triggers, so the list stops at 100 without paying
// for a frame that draws two pages.
static const uint8_t SLIDE_STEPS[] = {34, 60, 79, 92};

void builderSwipe(int8_t direction) {
  uint8_t pages = pageCount();
  if (!builderAtRoot() || pages < 2) {
    return;
  }
  uint8_t from = s_page;
  uint8_t to = (uint8_t)((s_page + pages + direction) % pages);

  for (uint8_t step = 0; step < sizeof(SLIDE_STEPS) / sizeof(SLIDE_STEPS[0]); step++) {
    int16_t travelled = (int16_t)((int32_t)PANEL_W * SLIDE_STEPS[step] / 100);
    int16_t offset = direction > 0 ? -travelled : travelled;
    panelCanvas().fillScreen(COL_BLACK);
    drawChrome(to);
    drawTiles(from, offset);
    drawTiles(to, direction > 0 ? offset + PANEL_W : offset - PANEL_W);
    panelFlush();
  }

  s_page = to;
  s_dirty = true;
}

static void touchList(int16_t x, int16_t y) {
  uint8_t used[MAX_SLOTS];
  uint8_t count = usedSlots(used);
  uint8_t first = s_page * PER_PAGE;
  for (uint8_t i = first; i < tileCount() && i < first + PER_PAGE; i++) {
    int16_t tx, ty;
    tileRect(i, &tx, &ty);
    if (!inRect(x, y, tx, ty, TILE_W, TILE_H)) {
      continue;
    }
    if (i >= count) {
      s_addOpen = true;
    } else {
      s_openSlot = (int8_t)used[i];
      if (!builderIsButton(used[i])) {
        s_activeLevel = used[i];
      }
    }
    s_dirty = true;
    return;
  }
}

static void touchPicker(int16_t x, int16_t y) {
  if (inRect(x, y, 0, 0, 130, 56)) {
    s_pickOpen = false;
    s_addOpen = s_pickSlot < 0;
    s_dirty = true;
    return;
  }

  for (uint8_t preset = 0; preset < PRESET_COUNT; preset++) {
    int16_t cx, cy;
    pickCell(preset, &cx, &cy);
    if (!inRect(x, y, cx, cy, TILE_W, PICK_ROW_H)) {
      continue;
    }
    if (presetTaken(preset, s_pickSlot)) {
      return;
    }
    s_pickOpen = false;
    if (s_pickSlot < 0) {
      builderAdd(s_pickIsButton, preset);
      return;
    }
    s_slot[s_pickSlot].preset = preset;
    persistSlots();
    s_accessory[s_pickSlot].setName(builderLabel((uint8_t)s_pickSlot));
    Serial.printf("NAME %d %s\n", s_pickSlot, builderLabel((uint8_t)s_pickSlot));
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
  s_pickIsButton = isButton;
  s_pickSlot = -1;
  s_addOpen = false;
  s_pickOpen = true;
  s_dirty = true;
}

static void touchDetail(uint8_t slot, int16_t x, int16_t y) {
  bool confirming = s_confirmRemove;
  s_confirmRemove = false;

  if (inRect(x, y, PANEL_W - 112, 4, 112, 52)) {
    if (confirming) {
      builderRemove(slot);
    } else {
      s_confirmRemove = true;
      s_dirty = true;
    }
    return;
  }
  if (confirming) {
    s_dirty = true;
  }

  if (inRect(x, y, 0, 0, 130, 56)) {
    s_openSlot = -1;
    s_dirty = true;
    return;
  }
  if (inRect(x, y, 40, 56, 288, 40)) {
    s_pickSlot = (int8_t)slot;
    s_pickOpen = true;
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
  if (s_pickOpen) {
    touchPicker(x, y);
  } else if (s_addOpen) {
    touchAdd(x, y);
  } else if (s_openSlot < 0) {
    touchList(x, y);
  } else {
    touchDetail((uint8_t)s_openSlot, x, y);
  }
}
