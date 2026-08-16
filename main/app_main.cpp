#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "audio.h"
#include "bridge.h"
#include "builder.h"
#include "console.h"
#include "gfx.h"
#include "internals.h"
#include "keys.h"
#include "net.h"
#include "netconsole.h"
#include "panel.h"
#include "power.h"
#include "qrcode.h"
#include "theme.h"

static const char *TAG = "genie";

static bool s_showQr = true;
static uint32_t s_lastActivityAt = 0;
static uint32_t s_idleTimeoutMs = 60000;

// Console commands run on the REPL task, and only the ui task may touch the
// panel. Anything a command wants drawn is left here for the next ui tick.
static volatile int8_t s_swipeWanted = 0;
static volatile bool s_qrToggleWanted = false;
static volatile int16_t s_tapX = -1;
static volatile int16_t s_tapY = 0;
static volatile int16_t s_brightnessWanted = -1;
static volatile int8_t s_sleepWanted = -1;  // 0 wake, 1 sleep
static volatile uint32_t s_rouseSettleMs = PANEL_SLPOUT_SETTLE_MS;

// Commands record intent and the ui tick applies it, so the work — and the line
// it prints — happen after the command has already returned. Waiting for the
// tick that applies it is what keeps a command's output inside its own reply
// instead of surfacing under whichever command comes next, which is what made
// the first `doze` measurements read as if they were off by one.
static volatile uint32_t s_requestsApplied = 0;

static void awaitUi() {
  uint32_t before = s_requestsApplied;
  for (int i = 0; i < 200 && s_requestsApplied == before; i++) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// How long tier 1 holds the module's rail up after the screen goes dark. The
// tier buys a fast touch wake for an idle draw nothing on this board can
// measure — no current sense, no current ADC in the PMU — so the window is what
// bounds the exposure to that unmeasured number. Grow it once a battery soak has
// priced it (`battlog`), not before — and since that soak has to run on the live
// Genie, where reflashing to change one number is the expensive part, `idle`
// takes the window as a second argument for the length of a session.
static const uint32_t TIER1_WINDOW_MS = 60000;
static uint32_t s_tier1WindowMs = TIER1_WINDOW_MS;
static uint32_t s_tier1StartedAt = 0;

static uint32_t nowMs() {
  return (uint32_t)(esp_timer_get_time() / 1000);
}

static void noteActivity() {
  s_lastActivityAt = nowMs();
}

static void drawCentredText(int16_t y, const char *text, uint8_t size, uint16_t colour) {
  gfxText((PANEL_W - gfxTextWidth(text, size)) / 2, y, text, colour, size);
}

static void formatPairingCode(char *out, size_t size) {
  char raw[32] = {0};
  if (!bridgePairingCode(raw, sizeof(raw))) {
    out[0] = '\0';
    return;
  }
  if (strlen(raw) != 11) {
    snprintf(out, size, "%s", raw);
    return;
  }
  snprintf(out, size, "%.4s-%.3s-%s", raw, raw + 4, raw + 7);
}

// The payload decides how much grid it needs. A larger version than it calls
// for is not more robust, only finer: the same picture in smaller modules, which
// is harder for a phone to resolve. So take the first one the payload fits in.
static const uint8_t QR_MAX_VERSION = 4;
static const uint8_t QR_MAX_MODULES = 4 * QR_MAX_VERSION + 17;
static const int16_t QR_BOX_PX = 340;

static uint8_t s_qrModules[(QR_MAX_MODULES * QR_MAX_MODULES + 7) / 8];

static bool buildQr(QRCode *qr, const char *payload) {
  for (uint8_t version = 1; version <= QR_MAX_VERSION; version++) {
    if (qrcode_initText(qr, s_qrModules, version, ECC_LOW, payload) == 0) {
      return true;
    }
  }
  return false;
}

static void drawPairingScreen() {
  gfxFillScreen(COL_BLACK);

  char payload[128] = {0};
  QRCode qr;
  if (!bridgePairingPayload(payload, sizeof(payload)) || !buildQr(&qr, payload)) {
    drawCentredText(200, "pairing code too long to show", 1, COL_WARN);
    panelFlush();
    return;
  }

  // A quiet zone of four modules a side is what the spec asks for, so the code
  // occupies size + 8 modules and the module size falls out of the space.
  const int16_t scale = QR_BOX_PX / (qr.size + 8);
  const int16_t quiet = 4 * scale;
  const int16_t box = qr.size * scale + 2 * quiet;
  const int16_t x0 = (PANEL_W - box) / 2;
  const int16_t y0 = 34;

  gfxFillRect(x0, y0, box, box, COL_WHITE);
  for (uint8_t y = 0; y < qr.size; y++) {
    for (uint8_t x = 0; x < qr.size; x++) {
      if (qrcode_getModule(&qr, x, y)) {
        gfxFillRect(x0 + quiet + x * scale, y0 + quiet + y * scale, scale, scale, COL_BLACK);
      }
    }
  }

  char code[16] = {0};
  formatPairingCode(code, sizeof(code));
  drawCentredText(y0 + box + 16, code, 2, COL_WHITE);

  NetStatus net = netStatus();
  bool listening = bridgePairingWindowOpen();
  const char *hint = !net.connected ? "No WiFi - a commissioner can supply it"
                     : listening    ? "Scan in Apple Home"
                                    : "Pairing window shut - reopen from the grid";
  drawCentredText(y0 + box + 40, hint, 1, listening ? COL_DIM : COL_WARN);
  if (net.connected) {
    char line[64];
    snprintf(line, sizeof(line), "%s  %s", net.ssid, net.ip);
    drawCentredText(y0 + box + 54, line, 1, COL_NET);
  }
  drawCentredText(PANEL_H - 10, "tap to continue", 1, COL_FAINT);
  panelFlush();
}

static void drawScreen() {
  if (s_showQr) {
    drawPairingScreen();
  } else {
    builderDraw();
  }
}

static void showQr(bool on) {
  s_showQr = on;
  if (on) {
    bridgeOpenPairingWindow();
  }
  drawScreen();
  printf("SCREEN %s\n", on ? "qr" : "builder");
}

// The three transitions between the screen's states, and the only places a wake
// source is armed. Every one of them runs on the ui task: the panel, the
// expander and the light-sleep wake bitmap are all single-writer state, and the
// ui task is that writer.
//
// Tier 1 leaves the module powered with the panel in SLPIN and the digitiser in
// standby, so a finger wakes the screen through the INT line. Tier 2 is what the
// firmware did before wake-on-touch: rail down, PWR key only.
static void enterTier1() {
  panelDoze();
  powerArmTouchWake(true);
  s_tier1StartedAt = nowMs();
  printf("SLEEP tier=1\n");
}

static void enterTier2() {
  powerArmTouchWake(false);
  panelSleep();
  printf("SLEEP tier=2\n");
}

// Disarm first: the wake source has done its job and a level trigger left armed
// against a line the digitiser is about to drive is a wake loop. A rouse that
// fails leaves the rail down, which is what the panelAsleep() branch then picks
// up — the cold start is the tier-1 failure path, not a separate one.
static void wakeScreen() {
  powerArmTouchWake(false);
  if (panelDozing()) {
    panelRouse(s_rouseSettleMs);
  }
  if (panelAsleep()) {
    panelWake();
  }
  // Without this the idle timer is still expired and the next tick puts the
  // screen straight back to sleep. The framebuffer was blanked on the way down,
  // so waking also has to redraw.
  noteActivity();
  drawScreen();
}

static void runSwipe(int8_t direction) {
  uint32_t at = nowMs();
  builderSwipe(direction);
  printf("SWIPE %s %ums\n", direction > 0 ? "left" : "right", (unsigned)(nowMs() - at));
}

static void pollKeys() {
  bool up = keyBootPressed();
  bool down = keyPowerPressed();
  if (!up && !down) {
    return;
  }
  noteActivity();
  // Ahead of the wake, so that one press always silences a beeping Genie —
  // including one beeping in the dark, where waking first would cost a second
  // press. Same rule as the tap that wakes the screen otherwise: the press that
  // stops the finder must not also land on whatever it was pointing at.
  if (internalsFinding()) {
    internalsFinderStop();
    return;
  }
  if (panelDark()) {
    wakeScreen();
    return;
  }
  builderNudgeLevel(up ? 1 : -1);
}

// Acting on release rather than on contact is what makes a swipe possible at
// all: until the finger lifts there is no way to tell one from a tap.
static const int16_t SWIPE_MIN_DX = 50;
static const int16_t TAP_MAX_DRIFT = 24;

static void handleRelease(int16_t x0, int16_t y0, int16_t x, int16_t y) {
  // The tap that wakes the screen must not also hit whatever was underneath it,
  // and neither may the one that stops the finder.
  if (internalsFinding()) {
    internalsFinderStop();
    return;
  }
  if (panelDark()) {
    wakeScreen();
    return;
  }

  int16_t dx = x - x0;
  int16_t dy = y - y0;
  if (abs(dx) >= SWIPE_MIN_DX && abs(dx) > abs(dy)) {
    runSwipe(dx < 0 ? 1 : -1);
    return;
  }
  if (abs(dx) > TAP_MAX_DRIFT || abs(dy) > TAP_MAX_DRIFT) {
    return;
  }

  printf("TAP %d %d\n", x0, y0);
  if (s_showQr) {
    s_showQr = false;
    drawScreen();
  } else if (builderAtRoot() && x0 > PANEL_W - 60 && y0 < 60) {
    // Only the tile grid gives up its top right corner. Any screen below it
    // owns that space, and a global hotspot silently eats whatever it lands on.
    showQr(true);
  } else {
    builderTouch(x0, y0);
  }
}

// Tier 1 wakes on the INT line rather than on a touch report — the digitiser has
// no I2C interface in standby — so by the time it can be read, the finger that
// woke the device is already on the glass and its release is the wake. Swallowed
// here, and only until the glass goes quiet, so a finger lifted before the
// digitiser came back never eats the next real tap.
static bool s_swallowRelease = false;

static void pollTouch() {
  static bool wasDown = false;
  static int16_t downX = 0, downY = 0;
  static int16_t lastX = 0, lastY = 0;
  static uint32_t lastAt = 0;

  int16_t x = 0, y = 0;
  bool down = panelTouch(&x, &y);

  if (down) {
    noteActivity();
    if (!wasDown) {
      downX = x;
      downY = y;
    }
    lastX = x;
    lastY = y;
  } else if (wasDown && nowMs() - lastAt > 120) {
    lastAt = nowMs();
    noteActivity();
    if (s_swallowRelease) {
      s_swallowRelease = false;
    } else {
      handleRelease(downX, downY, lastX, lastY);
    }
  } else if (!wasDown) {
    s_swallowRelease = false;
  }
  wasDown = down;
}

// The other half of tier 1: the latch the INT handler sets, read on the tick the
// wake itself brought forward.
//
// ponytail: polled, so a touch costs up to one asleep tick (250 ms) before the
// rouse even starts. A task notification from the handler would take that to
// zero — worth it if the measured wake ever feels slow, and not before.
static void pollTouchWake() {
  if (!panelDozing() || !powerTouchWoke()) {
    return;
  }
  wakeScreen();
  s_swallowRelease = true;
  printf("WAKE touch\n");
}

// A dark device is a hard device to find, so a finder session asks for the
// screen once, on the edge, through the same intent flag `wake` uses. Asking
// once rather than holding it is what keeps the finder out of the sleep state
// machine: the idle timer takes the screen back after its usual minute and the
// beeping carries on regardless.
static void pollFinder() {
  static bool wasFinding = false;
  bool finding = internalsFinding();
  if (finding && !wasFinding && panelDark()) {
    s_sleepWanted = 0;
  }
  wasFinding = finding;
}

static void pollRequests() {
  bool applied = s_brightnessWanted >= 0 || s_sleepWanted >= 0 || s_swipeWanted != 0 ||
                 s_qrToggleWanted || s_tapX >= 0;
  if (s_brightnessWanted >= 0) {
    uint8_t level = (uint8_t)s_brightnessWanted;
    s_brightnessWanted = -1;
    panelBrightness(level);
    printf("BRIGHT %u\n", level);
  }
  if (s_sleepWanted >= 0) {
    bool wantsSleep = s_sleepWanted == 1;
    s_sleepWanted = -1;
    if (wantsSleep) {
      enterTier1();
    } else {
      wakeScreen();
      printf("%s\n", panelDark() ? "WAKE failed" : "WAKE");
    }
  }
  if (s_swipeWanted != 0) {
    int8_t direction = s_swipeWanted;
    s_swipeWanted = 0;
    runSwipe(direction);
  }
  if (s_qrToggleWanted) {
    s_qrToggleWanted = false;
    showQr(!s_showQr);
  }
  if (s_tapX >= 0) {
    int16_t x = s_tapX, y = s_tapY;
    s_tapX = -1;
    noteActivity();
    handleRelease(x, y, x, y);
  }
  if (applied) {
    s_requestsApplied = s_requestsApplied + 1;
  }
}

// The screen shows both, and neither raises anything the ui task hears about.
// Half a second is the same cadence the Arduino build polled the stack at.
//
// Nothing to poll for while the screen is off in either tier: the redraw this
// would trigger goes nowhere, and every wake path redraws anyway. netStatus() is
// a WiFi API call, so this is also the one place the ui task touches the radio
// on a fixed cadence.
static void pollState() {
  static uint32_t lastAt = 0;
  static bool lastCommissioned = false;
  static bool lastOnline = false;
  if (panelDark() || nowMs() - lastAt < 500) {
    return;
  }
  lastAt = nowMs();

  bool commissioned = bridgeCommissioned();
  bool online = netStatus().connected;
  if (commissioned == lastCommissioned && online == lastOnline) {
    return;
  }
  lastCommissioned = commissioned;
  lastOnline = online;
  if (commissioned) {
    s_showQr = false;
  }
  drawScreen();
}

// A dark screen is the device's normal state, and there is nothing to look at
// between ticks: dropping to four passes a second is what lets tickless idle
// find a sleep worth taking. The cost is anything shorter than a tick: a quick
// BOOT press can fall between two polls, so waking the screen sometimes takes a
// second try. The other two wakes are latched and so cannot be missed — the
// power key by the PMU's interrupt, a touch in tier 1 by the INT handler.
static const uint32_t TICK_AWAKE_MS = 20;
static const uint32_t TICK_ASLEEP_MS = 250;

// One pass is 150 ms at its very worst (a swipe is four flushes), so the 5 s
// watchdog only fires on something genuinely stuck. It logs a backtrace rather
// than rebooting, which is the difference between a diagnosable stall and the
// screen simply going quiet.
static void uiTask(void *) {
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_task_wdt_add(nullptr));
  drawScreen();
  printf("ready\n");
  while (true) {
    pollState();
    pollFinder();
    pollRequests();
    internalsPoll();
    powerPoll();
    pollKeys();
    pollTouchWake();
    pollTouch();
    if (builderNeedsRedraw()) {
      drawScreen();
    }
    if (!panelDark() && s_idleTimeoutMs > 0 && nowMs() - s_lastActivityAt > s_idleTimeoutMs) {
      enterTier1();
    }
    if (panelDozing() && nowMs() - s_tier1StartedAt > s_tier1WindowMs) {
      enterTier2();
    }
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(panelDark() ? TICK_ASLEEP_MS : TICK_AWAKE_MS));
  }
}

static bool wantArgs(int argc, int expected, const char *usage) {
  if (argc == expected) {
    return true;
  }
  printf("ERR usage: %s\n", usage);
  return false;
}

static uint8_t slotArg(const char *text) {
  return (uint8_t)strtoul(text, nullptr, 10);
}

static int cmdAdd(int argc, char **argv) {
  if (!wantArgs(argc, 2, "add <type>")) {
    return 1;
  }
  for (uint8_t type = 0; type < ACC_TYPE_COUNT; type++) {
    if (strcasecmp(argv[1], accessoryTypeName(type)) == 0) {
      builderAdd(type);
      return 0;
    }
  }
  printf("ERR unknown type '%s'\n", argv[1]);
  return 1;
}

static int cmdRemove(int argc, char **argv) {
  if (!wantArgs(argc, 2, "remove <slot>")) {
    return 1;
  }
  builderRemove(slotArg(argv[1]));
  return 0;
}

static int cmdClick(int argc, char **argv) {
  if (!wantArgs(argc, 2, "click <slot>")) {
    return 1;
  }
  builderPress(slotArg(argv[1]));
  return 0;
}

static int cmdOn(int argc, char **argv) {
  if (!wantArgs(argc, 3, "on <slot> <0|1>")) {
    return 1;
  }
  builderSetOnOff(slotArg(argv[1]), strtoul(argv[2], nullptr, 10) != 0);
  return 0;
}

static int cmdLevel(int argc, char **argv) {
  if (!wantArgs(argc, 3, "level <slot> <0-255>")) {
    return 1;
  }
  builderSetLevel(slotArg(argv[1]), (uint8_t)strtoul(argv[2], nullptr, 10));
  return 0;
}

static int cmdFlag(int argc, char **argv) {
  if (!wantArgs(argc, 2, "flag <slot>")) {
    return 1;
  }
  builderToggleFlag(slotArg(argv[1]));
  return 0;
}

static int cmdValue(int argc, char **argv) {
  if (!wantArgs(argc, 3, "value <slot> <up|down>")) {
    return 1;
  }
  builderNudgeValue(slotArg(argv[1]), strcasecmp(argv[2], "up") == 0 ? 1 : -1);
  return 0;
}

static int cmdSwipe(int argc, char **argv) {
  if (!wantArgs(argc, 2, "swipe <left|right>")) {
    return 1;
  }
  s_swipeWanted = strcasecmp(argv[1], "left") == 0 ? 1 : -1;
  awaitUi();
  return 0;
}

// The digitiser is the only part of the touch path this skips: a tap arrives
// here exactly as a released finger does, which is what makes the screens
// reachable from a test harness.
static int cmdTap(int argc, char **argv) {
  if (!wantArgs(argc, 3, "tap <x> <y>")) {
    return 1;
  }
  s_tapY = (int16_t)strtol(argv[2], nullptr, 10);
  s_tapX = (int16_t)strtol(argv[1], nullptr, 10);
  awaitUi();
  return 0;
}

static int cmdBright(int argc, char **argv) {
  if (!wantArgs(argc, 2, "bright <0-255>")) {
    return 1;
  }
  s_brightnessWanted = (int16_t)std::clamp(strtol(argv[1], nullptr, 10), 0L, 255L);
  awaitUi();
  return 0;
}

static int cmdSleep(int argc, char **argv) {
  s_sleepWanted = 1;
  awaitUi();
  return 0;
}

static int cmdWake(int argc, char **argv) {
  s_sleepWanted = 0;
  awaitUi();
  return 0;
}

// `sleep`/`wake` with the SLPOUT settle exposed, so what a settle bisect
// measures is the shipped path rather than a bench-only copy of it. The value
// sticks for the session; a reboot goes back to PANEL_SLPOUT_SETTLE_MS.
static int cmdDoze(int argc, char **argv) {
  if (argc < 2 || (strcmp(argv[1], "0") != 0 && strcmp(argv[1], "1") != 0)) {
    printf("ERR usage: doze <0|1> [settle-ms]\n");
    return 1;
  }
  if (argc == 3) {
    s_rouseSettleMs = (uint32_t)strtoul(argv[2], nullptr, 10);
  }
  s_sleepWanted = argv[1][0] - '0';
  awaitUi();
  return 0;
}

static int cmdSlots(int argc, char **argv) {
  for (uint8_t i = 0; i < builderSlotCount(); i++) {
    if (!builderSlotUsed(i)) {
      continue;
    }
    char state[28];
    builderDescribe(i, state, sizeof(state));
    printf("SLOT %u %s %s | %s\n", i, accessoryTypeName(builderType(i)), builderLabel(i), state);
  }
  return 0;
}

static int cmdQr(int argc, char **argv) {
  s_qrToggleWanted = true;
  awaitUi();
  return 0;
}

// The second argument is how long tier 1 then holds the rail up: 0 cuts it
// straight away (the pre-wake-on-touch behaviour), and a long one is what a
// drain soak of tier 1 needs.
static int cmdIdle(int argc, char **argv) {
  if (argc < 2 || argc > 3) {
    printf("ERR usage: idle <seconds> [tier1-seconds]\n");
    return 1;
  }
  s_idleTimeoutMs = (uint32_t)strtoul(argv[1], nullptr, 10) * 1000;
  if (argc == 3) {
    s_tier1WindowMs = (uint32_t)strtoul(argv[2], nullptr, 10) * 1000;
  }
  noteActivity();
  printf("IDLE %u tier1=%u\n", (unsigned)(s_idleTimeoutMs / 1000),
         (unsigned)(s_tier1WindowMs / 1000));
  return 0;
}

static int cmdReset(int argc, char **argv) {
  if (argc != 2 || strcasecmp(argv[1], "slots") != 0) {
    printf("ERR usage: reset slots\n");
    return 1;
  }
  builderReset();
  return 0;
}

static void registerCommands() {
  consoleRegisterCmd("add", "Add an accessory of <type>", cmdAdd);
  consoleRegisterCmd("remove", "Remove the accessory in <slot>", cmdRemove);
  consoleRegisterCmd("click", "Send a press from the button in <slot>", cmdClick);
  consoleRegisterCmd("on", "Set the on/off state of <slot>", cmdOn);
  consoleRegisterCmd("level", "Set the brightness of <slot>", cmdLevel);
  consoleRegisterCmd("flag", "Toggle the sensor state of <slot>", cmdFlag);
  consoleRegisterCmd("value", "Nudge the sensor reading of <slot>", cmdValue);
  consoleRegisterCmd("swipe", "Page the grid left or right", cmdSwipe);
  consoleRegisterCmd("tap", "Inject a tap at <x> <y>", cmdTap);
  consoleRegisterCmd("slots", "List the accessories in use", cmdSlots);
  consoleRegisterCmd("bright", "Set panel brightness 0-255", cmdBright);
  consoleRegisterCmd("sleep", "Blank the screen (tier 1: rail up, touch wakes it)", cmdSleep);
  consoleRegisterCmd("wake", "Unblank the screen and redraw", cmdWake);
  consoleRegisterCmd("doze", "sleep/wake with the SLPOUT settle exposed: doze <0|1> [settle-ms]",
                     cmdDoze);
  consoleRegisterCmd("qr", "Toggle the pairing screen", cmdQr);
  consoleRegisterCmd("idle",
                     "Blank the screen after <seconds>, 0 to never; [tier1-seconds] sets how "
                     "long the rail then stays up for touch wake",
                     cmdIdle);
  consoleRegisterCmd("reset", "reset slots: clear the layout and restart", cmdReset);
}

static void initNvs() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
}

extern "C" void app_main(void) {
  initNvs();
  if (!panelInit()) {
    ESP_LOGE(TAG, "panel init failed");
  }
  keysInit();

  consoleStart();
  registerCommands();
  // Owns its own task: a beep is a two-second cycle of blocking writes, which
  // has no business inside the ui task's 20 ms pass.
  audioBegin();

  // builderBegin() creates the node; a second bridgeBegin() here would create a
  // second one and log a failure that is not real.
  builderBegin();
  // bridgeStart() is what registers the Matter commands, so a silent failure
  // here would look like a firmware that shipped without them.
  if (!bridgeStart()) {
    ESP_LOGE(TAG, "Matter did not come up; its commands are missing");
  }
  builderResume();
  internalsBegin();
  // After esp_matter::start(): the WiFi driver has to exist before its
  // power-save mode can be set.
  powerBegin();
  // Last of the console registrations, so a TCP session finds every command.
  netconsoleBegin();

  s_showQr = !bridgeCommissioned();
  if (s_showQr) {
    bridgeOpenPairingWindow();
  }
  noteActivity();
  // Deliberately unpinned. Core 1 would be the tidy answer — WiFi and the BLE
  // controller are both pinned to core 0 — but the SPI completion interrupt is
  // registered from here, on core 0, so pinning the ui task to core 1 makes
  // every flush handoff a cross-core signal. That handoff is the one that
  // intermittently wedges inside esp_lcd_panel_draw_bitmap (see panelFlush),
  // and pinning to core 0 instead would put an 18 ms DMA wait on the radio's
  // core. Neither is worth choosing blind while the wedge is unexplained.
  xTaskCreate(uiTask, "ui", 8192, nullptr, 4, nullptr);
  ESP_LOGI(TAG, "genie booted");
}
