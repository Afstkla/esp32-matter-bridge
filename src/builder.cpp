#include "builder.h"

#include <Matter.h>
#include <Preferences.h>

#include "bridge.h"
#include "panel.h"
#include "theme.h"

// Six is what the board proved it can carry alongside the internals accessory:
// twelve endpoints all told, which measured stable, where fifteen did not.
static const uint8_t MAX_SLOTS = 6;

static const int16_t PANEL_W = 368;
static const int16_t PANEL_H = 448;
static const int16_t TILE_W = 167;
static const int16_t TILE_H = 100;
static const int16_t TILE_X0 = 12;
static const int16_t TILE_Y0 = 62;
static const int16_t TILE_GAP = 10;
static const uint8_t PER_PAGE = 6;

// Naming without a keyboard. The choice becomes the accessory's NodeLabel, so
// it reaches Apple Home too.
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

static const char *SLOTS_KEY = "slots";

// Persisted verbatim as one NVS blob, so the layout has to stay stable.
struct Slot {
  uint8_t type;
  uint8_t preset;
  uint16_t endpointId;
};

static Slot s_slot[MAX_SLOTS];
static uint8_t s_count = 0;
static BridgedAccessory s_accessory[MAX_SLOTS];
static Preferences s_prefs;

// Which screen is showing. Three of the four are grids that differ only in what
// fills the cells, so paging, swiping and hit testing are written once against
// the current screen instead of once per screen.
enum Screen : uint8_t {
  SCREEN_GRID,    // the accessories themselves
  SCREEN_TYPE,    // picking a device type for a new accessory
  SCREEN_NAME,    // picking a name, for a new accessory or a rename
  SCREEN_DETAIL,  // one accessory's controls
  SCREEN_COUNT
};

static Screen s_screen = SCREEN_GRID;
static uint8_t s_page[SCREEN_COUNT] = {0};
static uint8_t s_slotShown = 0;            // SCREEN_DETAIL, and what a rename renames
static uint8_t s_typeChosen = ACC_BUTTON;  // carried from SCREEN_TYPE to SCREEN_NAME
static bool s_namingNew = false;           // SCREEN_NAME is naming a new accessory
static bool s_confirmRemove = false;
static bool s_dirty = true;
static uint8_t s_activeLevel = 0;

static void show(Screen screen) {
  s_screen = screen;
  s_confirmRemove = false;
  s_dirty = true;
}

uint8_t builderSlotCount() {
  return s_count;
}

uint8_t builderMaxSlots() {
  return MAX_SLOTS;
}

bool builderSlotUsed(uint8_t slot) {
  return slot < s_count && s_slot[slot].preset != PRESET_FREE;
}

uint8_t builderType(uint8_t slot) {
  return builderSlotUsed(slot) ? s_slot[slot].type : ACC_BUTTON;
}

AccessoryUi builderUi(uint8_t slot) {
  return accessoryUi(builderType(slot));
}

static bool isUi(uint8_t slot, AccessoryUi ui) {
  return builderSlotUsed(slot) && builderUi(slot) == ui;
}

static bool hasOnOff(uint8_t slot) {
  return isUi(slot, UI_ONOFF) || isUi(slot, UI_LEVEL);
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

static uint16_t accentForType(uint8_t type) {
  switch (accessoryUi(type)) {
    case UI_PRESS:
      return COL_BUTTON;
    case UI_ONOFF:
    case UI_LEVEL:
      return COL_LEVEL;
    default:
      return COL_OK;
  }
}

static uint16_t accentFor(uint8_t slot) {
  return accentForType(builderType(slot));
}

static void persistSlots() {
  s_prefs.putBytes(SLOTS_KEY, s_slot, (size_t)s_count * sizeof(Slot));
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

void builderPress(uint8_t slot) {
  if (!isUi(slot, UI_PRESS)) {
    return;
  }
  s_accessory[slot].click();
  Serial.printf("PRESS %u %s\n", slot, builderLabel(slot));
}

uint8_t builderLevel(uint8_t slot) {
  return isUi(slot, UI_LEVEL) ? s_accessory[slot].getBrightness() : 0;
}

void builderSetLevel(uint8_t slot, uint8_t value) {
  if (!isUi(slot, UI_LEVEL)) {
    return;
  }
  s_accessory[slot].setBrightness(value);
  Serial.printf("LEVEL %u %u\n", slot, value);
  s_dirty = true;
}

void builderSetOnOff(uint8_t slot, bool on) {
  if (!hasOnOff(slot)) {
    return;
  }
  s_accessory[slot].setOnOff(on);
  Serial.printf("ONOFF %u %d\n", slot, on ? 1 : 0);
  s_dirty = true;
}

bool builderOnOff(uint8_t slot) {
  return hasOnOff(slot) && s_accessory[slot].getOnOff();
}

bool builderFlag(uint8_t slot) {
  return isUi(slot, UI_FLAG) && s_accessory[slot].flag();
}

void builderToggleFlag(uint8_t slot) {
  if (!isUi(slot, UI_FLAG)) {
    return;
  }
  bool next = !s_accessory[slot].flag();
  s_accessory[slot].setFlag(next);
  Serial.printf("FLAG %u %d\n", slot, next ? 1 : 0);
  s_dirty = true;
}

int16_t builderValue(uint8_t slot) {
  return isUi(slot, UI_VALUE) ? s_accessory[slot].value() : 0;
}

void builderNudgeValue(uint8_t slot, int8_t direction) {
  if (!isUi(slot, UI_VALUE)) {
    return;
  }
  uint8_t type = builderType(slot);
  int32_t next = (int32_t)s_accessory[slot].value() + (int32_t)direction * accessoryStep(type);
  next = constrain(next, accessoryMin(type), accessoryMax(type));
  s_accessory[slot].setValue((int16_t)next);
  Serial.printf("VALUE %u %d\n", slot, (int)next);
  s_dirty = true;
}

static void nudgeLevelSlot(uint8_t slot, int8_t direction) {
  if (!isUi(slot, UI_LEVEL)) {
    return;
  }
  int16_t percent = (int16_t)percentOf(builderLevel(slot)) + direction * 10;
  builderSetLevel(slot, levelOf((uint8_t)constrain(percent, 0, 100)));
}

void builderNudgeLevel(int8_t direction) {
  nudgeLevelSlot(s_activeLevel, direction);
}

const char *builderActiveLevelLabel() {
  return builderLabel(s_activeLevel);
}

static const char *flagLabel(uint8_t type, bool active) {
  if (type == ACC_CONTACT) {
    return active ? "closed" : "open";
  }
  return active ? "detected" : "clear";
}

// Matter scales both sensor readings by 100, so the fraction is recovered
// rather than carried. The sign needs writing out only between -1 and 0, where
// the whole part is zero and cannot show it.
static void formatValue(uint8_t slot, char *out, size_t n) {
  int16_t reading = builderValue(slot);
  int16_t whole = (int16_t)(reading / 100);
  int16_t frac = (int16_t)(abs(reading % 100) / 10);
  snprintf(out, n, "%s%d.%d %s", reading < 0 && whole == 0 ? "-" : "", whole, frac,
           accessoryUnit(builderType(slot)));
}

void builderDescribe(uint8_t slot, char *out, size_t n) {
  uint8_t type = builderType(slot);
  switch (builderUi(slot)) {
    case UI_PRESS:
      snprintf(out, n, "button");
      return;
    case UI_ONOFF:
      snprintf(out, n, "%s  %s", accessoryTypeName(type), builderOnOff(slot) ? "on" : "off");
      return;
    case UI_FLAG:
      snprintf(out, n, "%s  %s", accessoryTypeName(type), flagLabel(type, builderFlag(slot)));
      return;
    case UI_VALUE: {
      char reading[16];
      formatValue(slot, reading, sizeof(reading));
      snprintf(out, n, "%s  %s", accessoryTypeName(type), reading);
      return;
    }
    default:
      snprintf(out, n, "dimmer  %u%%  %s", percentOf(builderLevel(slot)),
               builderOnOff(slot) ? "on" : "off");
      return;
  }
}

static bool startAccessory(uint8_t slot) {
  uint16_t wanted = s_slot[slot].endpointId;
  if (!s_accessory[slot].begin(s_slot[slot].type, builderLabel(slot), wanted)) {
    return false;
  }

  uint16_t assigned = (uint16_t)s_accessory[slot].getEndPointId();
  if (assigned != wanted) {
    s_slot[slot].endpointId = assigned;
    persistSlots();
  }

  // Only the controllable types report back. A sensor is driven from this end,
  // and a button carries no state a controller could write.
  if (!hasOnOff(slot)) {
    return true;
  }
  s_accessory[slot].onChangeOnOff([slot](bool on) {
    Serial.printf("HK slot=%u onoff=%d\n", slot, on ? 1 : 0);
    s_dirty = true;
  });
  if (isUi(slot, UI_LEVEL)) {
    s_accessory[slot].onChangeBrightness([slot](uint8_t level) {
      Serial.printf("HK slot=%u level=%u\n", slot, level);
      s_dirty = true;
    });
    s_activeLevel = slot;
  }
  return true;
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

// Nothing is seeded. A fresh device carries only its own internals, and the
// grid opens on the one tile that adds something.
void builderBegin() {
  s_prefs.begin("builder", false);
  loadSlots();
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

// Two accessories sharing a name is legal but miserable to tell apart in the
// Home app, so a name already spoken for is not offered.
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

// exceptSlot keeps a slot's own name on offer while renaming it. Passed in
// rather than read from the picker's state, because this also answers for
// accessories added over serial, when no picker is open and that state is stale.
static uint8_t availableNames(uint8_t *out, int8_t exceptSlot) {
  uint8_t n = 0;
  for (uint8_t preset = 0; preset < PRESET_COUNT; preset++) {
    if (!presetTaken(preset, exceptSlot)) {
      out[n++] = preset;
    }
  }
  return n;
}

// Which names the name picker is currently offering.
static uint8_t offeredNames(uint8_t *out) {
  return availableNames(out, s_namingNew ? -1 : (int8_t)s_slotShown);
}

static uint8_t unusedPreset() {
  uint8_t available[PRESET_COUNT];
  return availableNames(available, -1) > 0 ? available[0] : 0;
}

static int8_t claimSlot() {
  for (uint8_t slot = 0; slot < s_count; slot++) {
    if (!builderSlotUsed(slot)) {
      return (int8_t)slot;
    }
  }
  return s_count < MAX_SLOTS ? (int8_t)s_count : -1;
}

// The grid keeps one spare cell for the "+" tile, so a full rack loses it.
static uint8_t tileCount() {
  uint8_t used = usedCount();
  return used < MAX_SLOTS ? (uint8_t)(used + 1) : used;
}

static uint8_t cellCount() {
  switch (s_screen) {
    case SCREEN_TYPE:
      return ACC_TYPE_COUNT;
    case SCREEN_NAME: {
      uint8_t available[PRESET_COUNT];
      return offeredNames(available);
    }
    case SCREEN_DETAIL:
      return 0;
    default:
      return tileCount();
  }
}

static uint8_t pageCount() {
  uint8_t pages = (uint8_t)((cellCount() + PER_PAGE - 1) / PER_PAGE);
  return pages < 1 ? 1 : pages;
}

// Measured, not guessed: at ~11 KB free the device stopped failing cleanly and
// started asserting inside the SPI driver instead, because a page slide is the
// largest allocation it ever asks for and mbedTLS wants memory at the same time.
// A slot is worth 1.8 KB as a sensor and up to 4 KB as something controllable.
// MAX_SLOTS is the real limit; this is the backstop for whatever else the stack
// happens to be holding at the time.
static const uint32_t MIN_FREE_HEAP = 16000;

bool builderAdd(uint8_t type, uint8_t preset) {
  uint32_t heap = ESP.getFreeHeap();
  if (heap < MIN_FREE_HEAP) {
    Serial.printf("ADD refused, %u bytes free and %u needed\n", (unsigned)heap,
                  (unsigned)MIN_FREE_HEAP);
    return false;
  }
  int8_t claimed = claimSlot();
  if (claimed < 0) {
    Serial.println("ADD full");
    return false;
  }
  uint8_t slot = (uint8_t)claimed;
  uint8_t restoreCount = s_count;
  if (slot == s_count) {
    s_count++;
  }
  s_slot[slot].type = type;
  s_slot[slot].preset = preset == BUILDER_PRESET_AUTO ? unusedPreset() : preset;
  s_slot[slot].endpointId = 0;

  // Persist only once the endpoint exists. A type the stack refuses would
  // otherwise be written down and retried at every boot, and esp_matter aborts
  // rather than returning an error, so that is a boot loop and not a bad tile.
  if (!startAccessory(slot)) {
    s_slot[slot].preset = PRESET_FREE;
    s_count = restoreCount;
    Serial.printf("ADD %s failed\n", accessoryTypeName(type));
    return false;
  }
  persistSlots();

  show(SCREEN_GRID);
  s_page[SCREEN_GRID] = (uint8_t)((usedCount() - 1) / PER_PAGE);
  Serial.printf("ADD %u %s %s\n", slot, accessoryTypeName(type), builderLabel(slot));
  return true;
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
      if (isUi(i, UI_LEVEL)) {
        s_activeLevel = i;
        break;
      }
    }
  }
  show(SCREEN_GRID);
  if (s_page[SCREEN_GRID] >= pageCount()) {
    s_page[SCREEN_GRID] = (uint8_t)(pageCount() - 1);
  }
}

void builderReset() {
  s_prefs.remove(SLOTS_KEY);
  Serial.println("RESET slots cleared, restarting");
  delay(200);
  ESP.restart();
}

bool builderAtRoot() {
  return s_screen == SCREEN_GRID;
}

bool builderNeedsRedraw() {
  bool was = s_dirty;
  s_dirty = false;
  return was;
}

static void cellRect(uint8_t index, int16_t *x, int16_t *y) {
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

// Centred inside one cell rather than the screen, for the two grids whose cells
// hold nothing but a word.
static void drawCellLabel(int16_t x, int16_t y, const char *text) {
  drawText(x + (TILE_W - (int16_t)strlen(text) * 12) / 2, y + 42, text, 2, COL_WHITE);
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
    g.fillCircle(x + page * spacing, PANEL_H - 44, 4, page == current ? COL_WHITE : COL_OUTLINE);
  }
}

// Everything that does not travel with the pages. Keeping it still during a
// slide is both cheaper and closer to how a paged home screen behaves.
static void drawChrome(uint8_t current) {
  if (s_screen == SCREEN_GRID) {
    drawText(14, 24, "Accessories", 2, COL_WHITE);
    panelCanvas().fillCircle(PANEL_W - 24, 30, 7,
                             Matter.isDeviceCommissioned() ? COL_OK : COL_ALERT);
    char hint[40];
    snprintf(hint, sizeof(hint), "BOOT +/- PWR  %s", builderActiveLevelLabel());
    drawCentred(PANEL_H - 22, hint, 1, COL_FAINT);
  } else {
    drawText(16, 24, "< back", 2, COL_MUTED);
    drawCentred(PANEL_H - 22, s_screen == SCREEN_TYPE ? "pick a device type" : "pick a name", 1,
                COL_FAINT);
  }
  drawPageDots(current);
}

static void drawAccessoryCells(uint8_t page, int16_t dx) {
  Arduino_Canvas &g = panelCanvas();
  uint8_t used[MAX_SLOTS];
  uint8_t count = usedSlots(used);
  uint8_t first = page * PER_PAGE;
  for (uint8_t i = first; i < tileCount() && i < first + PER_PAGE; i++) {
    int16_t x, y;
    cellRect(i, &x, &y);
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

    char detail[28];
    builderDescribe(slot, detail, sizeof(detail));
    drawText(x + 12, y + 62, detail, 1, COL_MUTED);
  }
}

static void drawTypeCells(uint8_t page, int16_t dx) {
  Arduino_Canvas &g = panelCanvas();
  uint8_t first = page * PER_PAGE;
  for (uint8_t i = first; i < ACC_TYPE_COUNT && i < first + PER_PAGE; i++) {
    int16_t x, y;
    cellRect(i, &x, &y);
    x += dx;
    if (x + TILE_W <= 0 || x >= PANEL_W) {
      continue;
    }
    g.fillRoundRect(x, y, TILE_W, TILE_H, 12, COL_BG);
    g.drawRoundRect(x, y, TILE_W, TILE_H, 12, accentForType(i));
    drawCellLabel(x, y, accessoryTypeName(i));
  }
}

static void drawNameCells(uint8_t page, int16_t dx) {
  Arduino_Canvas &g = panelCanvas();
  uint8_t available[PRESET_COUNT];
  uint8_t count = offeredNames(available);
  uint8_t first = page * PER_PAGE;
  for (uint8_t i = first; i < count && i < first + PER_PAGE; i++) {
    int16_t x, y;
    cellRect(i, &x, &y);
    x += dx;
    if (x + TILE_W <= 0 || x >= PANEL_W) {
      continue;
    }
    g.fillRoundRect(x, y, TILE_W, TILE_H, 12, COL_BG);
    g.drawRoundRect(x, y, TILE_W, TILE_H, 12, COL_OUTLINE);
    drawCellLabel(x, y, PRESETS[available[i]]);
  }
}

static void drawCells(uint8_t page, int16_t dx) {
  switch (s_screen) {
    case SCREEN_TYPE:
      drawTypeCells(page, dx);
      return;
    case SCREEN_NAME:
      drawNameCells(page, dx);
      return;
    default:
      drawAccessoryCells(page, dx);
      return;
  }
}

static void drawDetail(uint8_t slot) {
  Arduino_Canvas &g = panelCanvas();
  g.fillScreen(COL_BLACK);
  drawText(16, 24, "< back", 2, COL_MUTED);
  drawText(PANEL_W - 100, 26, s_confirmRemove ? "tap to confirm" : "remove", 1,
           s_confirmRemove ? COL_DANGER : COL_FAINT);
  drawCentred(72, builderLabel(slot), 3, COL_WHITE);
  drawCentred(104, "tap name to rename", 1, COL_FAINT);

  switch (builderUi(slot)) {
    case UI_PRESS:
      g.fillRoundRect(44, 150, 280, 190, 24, accentFor(slot));
      drawCentred(232, "PRESS", 3, COL_BLACK);
      return;
    case UI_ONOFF: {
      bool on = builderOnOff(slot);
      g.fillRoundRect(44, 150, 280, 190, 24, on ? accentFor(slot) : COL_WELL);
      drawCentred(232, on ? "ON" : "OFF", 3, on ? COL_BLACK : COL_MUTED);
      return;
    }
    case UI_FLAG: {
      bool active = builderFlag(slot);
      g.fillRoundRect(44, 150, 280, 190, 24, active ? accentFor(slot) : COL_WELL);
      drawCentred(232, flagLabel(builderType(slot), active), 3, active ? COL_BLACK : COL_MUTED);
      return;
    }
    case UI_VALUE: {
      g.fillRoundRect(30, 200, 90, 120, 16, COL_WELL);
      drawText(66, 248, "-", 4, COL_WHITE);
      g.fillRoundRect(248, 200, 90, 120, 16, COL_WELL);
      drawText(284, 248, "+", 4, COL_WHITE);
      char reading[16];
      formatValue(slot, reading, sizeof(reading));
      drawCentred(248, reading, 2, accentFor(slot));
      return;
    }
    default:
      break;
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
  if (s_screen == SCREEN_DETAIL) {
    drawDetail(s_slotShown);
  } else {
    panelCanvas().fillScreen(COL_BLACK);
    drawChrome(s_page[s_screen]);
    drawCells(s_page[s_screen], 0);
  }
  panelFlush();
}

// Eased travel, as a percentage of the screen width. The last step is the
// normal redraw the caller triggers, so the list stops at 100 without paying
// for a frame that draws two pages.
static const uint8_t SLIDE_STEPS[] = {34, 60, 79, 92};

void builderSwipe(int8_t direction) {
  uint8_t pages = pageCount();
  if (s_screen == SCREEN_DETAIL || pages < 2) {
    return;
  }
  uint8_t from = s_page[s_screen];
  uint8_t to = (uint8_t)((from + pages + direction) % pages);

  for (uint8_t step = 0; step < sizeof(SLIDE_STEPS) / sizeof(SLIDE_STEPS[0]); step++) {
    int16_t travelled = (int16_t)((int32_t)PANEL_W * SLIDE_STEPS[step] / 100);
    int16_t offset = direction > 0 ? -travelled : travelled;
    int16_t incoming = direction > 0 ? offset + PANEL_W : offset - PANEL_W;
    panelCanvas().fillScreen(COL_BLACK);
    drawChrome(to);
    drawCells(from, offset);
    drawCells(to, incoming);
    panelFlush();
  }

  s_page[s_screen] = to;
  s_dirty = true;
}

static bool inRect(int16_t x, int16_t y, int16_t rx, int16_t ry, int16_t rw, int16_t rh) {
  return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static bool hitBack(int16_t x, int16_t y) {
  return inRect(x, y, 0, 0, 130, 56);
}

// The cell under the point, or -1. Every grid is laid out the same way, so this
// is the only place that turns a touch into an index.
static int8_t cellAt(int16_t x, int16_t y) {
  uint8_t first = s_page[s_screen] * PER_PAGE;
  uint8_t count = cellCount();
  for (uint8_t i = first; i < count && i < first + PER_PAGE; i++) {
    int16_t cx, cy;
    cellRect(i, &cx, &cy);
    if (inRect(x, y, cx, cy, TILE_W, TILE_H)) {
      return (int8_t)i;
    }
  }
  return -1;
}

static void openName(bool forNewAccessory) {
  s_namingNew = forNewAccessory;
  s_page[SCREEN_NAME] = 0;
  show(SCREEN_NAME);
}

static void touchGrid(int16_t x, int16_t y) {
  int8_t cell = cellAt(x, y);
  if (cell < 0) {
    return;
  }
  uint8_t used[MAX_SLOTS];
  uint8_t count = usedSlots(used);
  if ((uint8_t)cell >= count) {
    s_page[SCREEN_TYPE] = 0;
    show(SCREEN_TYPE);
    return;
  }
  s_slotShown = used[cell];
  if (isUi(s_slotShown, UI_LEVEL)) {
    s_activeLevel = s_slotShown;
  }
  show(SCREEN_DETAIL);
}

static void touchType(int16_t x, int16_t y) {
  if (hitBack(x, y)) {
    show(SCREEN_GRID);
    return;
  }
  int8_t cell = cellAt(x, y);
  if (cell < 0) {
    return;
  }
  s_typeChosen = (uint8_t)cell;
  openName(true);
}

static void touchName(int16_t x, int16_t y) {
  if (hitBack(x, y)) {
    show(s_namingNew ? SCREEN_TYPE : SCREEN_DETAIL);
    return;
  }
  int8_t cell = cellAt(x, y);
  if (cell < 0) {
    return;
  }
  uint8_t available[PRESET_COUNT];
  offeredNames(available);
  uint8_t preset = available[cell];

  if (s_namingNew) {
    builderAdd(s_typeChosen, preset);
    return;
  }
  s_slot[s_slotShown].preset = preset;
  persistSlots();
  s_accessory[s_slotShown].setName(builderLabel(s_slotShown));
  Serial.printf("NAME %u %s\n", s_slotShown, builderLabel(s_slotShown));
  show(SCREEN_DETAIL);
}

static void touchDetail(uint8_t slot, int16_t x, int16_t y) {
  // Removal asks twice, and any other tap on the screen is the second answer
  // being "no".
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

  if (hitBack(x, y)) {
    show(SCREEN_GRID);
    return;
  }
  if (inRect(x, y, 40, 56, 288, 40)) {
    openName(false);
    return;
  }

  switch (builderUi(slot)) {
    case UI_PRESS:
      if (inRect(x, y, 44, 150, 280, 190)) {
        builderPress(slot);
      }
      return;
    case UI_ONOFF:
      if (inRect(x, y, 44, 150, 280, 190)) {
        builderSetOnOff(slot, !builderOnOff(slot));
      }
      return;
    case UI_FLAG:
      if (inRect(x, y, 44, 150, 280, 190)) {
        builderToggleFlag(slot);
      }
      return;
    case UI_VALUE:
      if (inRect(x, y, 30, 200, 90, 120)) {
        builderNudgeValue(slot, -1);
      } else if (inRect(x, y, 248, 200, 90, 120)) {
        builderNudgeValue(slot, 1);
      }
      return;
    default:
      break;
  }

  if (inRect(x, y, 30, 180, 90, 120)) {
    nudgeLevelSlot(slot, -1);
  } else if (inRect(x, y, 248, 180, 90, 120)) {
    nudgeLevelSlot(slot, 1);
  } else if (inRect(x, y, 60, 356, 248, 56)) {
    builderSetOnOff(slot, !builderOnOff(slot));
  }
}

void builderTouch(int16_t x, int16_t y) {
  switch (s_screen) {
    case SCREEN_TYPE:
      touchType(x, y);
      return;
    case SCREEN_NAME:
      touchName(x, y);
      return;
    case SCREEN_DETAIL:
      touchDetail(s_slotShown, x, y);
      return;
    default:
      touchGrid(x, y);
      return;
  }
}
