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

#include "bridge.h"
#include "builder.h"
#include "console.h"
#include "gfx.h"
#include "internals.h"
#include "keys.h"
#include "net.h"
#include "panel.h"
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
  if (panelAsleep()) {
    panelWake();
    drawScreen();
    return;
  }
  builderNudgeLevel(up ? 1 : -1);
}

// Acting on release rather than on contact is what makes a swipe possible at
// all: until the finger lifts there is no way to tell one from a tap.
static const int16_t SWIPE_MIN_DX = 50;
static const int16_t TAP_MAX_DRIFT = 24;

static void handleRelease(int16_t x0, int16_t y0, int16_t x, int16_t y) {
  // The tap that wakes the screen must not also hit whatever was underneath it.
  if (panelAsleep()) {
    panelWake();
    drawScreen();
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
    handleRelease(downX, downY, lastX, lastY);
  }
  wasDown = down;
}

static void pollRequests() {
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
      panelSleep();
      printf("SLEEP\n");
    } else {
      panelWake();
      // Without this the idle timer is still expired and the next tick puts the
      // screen straight back to sleep. The framebuffer was blanked on the way
      // down, so waking also has to redraw.
      noteActivity();
      drawScreen();
      printf("WAKE\n");
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
}

// The screen shows both, and neither raises anything the ui task hears about.
// Half a second is the same cadence the Arduino build polled the stack at.
static void pollState() {
  static uint32_t lastAt = 0;
  static bool lastCommissioned = false;
  static bool lastOnline = false;
  if (nowMs() - lastAt < 500) {
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
    pollRequests();
    internalsPoll();
    pollKeys();
    pollTouch();
    if (builderNeedsRedraw()) {
      drawScreen();
    }
    if (!panelAsleep() && s_idleTimeoutMs > 0 && nowMs() - s_lastActivityAt > s_idleTimeoutMs) {
      panelSleep();
      printf("SLEEP\n");
    }
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(20));
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
  return 0;
}

static int cmdBright(int argc, char **argv) {
  if (!wantArgs(argc, 2, "bright <0-255>")) {
    return 1;
  }
  s_brightnessWanted = (int16_t)std::clamp(strtol(argv[1], nullptr, 10), 0L, 255L);
  return 0;
}

static int cmdSleep(int argc, char **argv) {
  s_sleepWanted = 1;
  return 0;
}

static int cmdWake(int argc, char **argv) {
  s_sleepWanted = 0;
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
  return 0;
}

static int cmdIdle(int argc, char **argv) {
  if (!wantArgs(argc, 2, "idle <seconds>")) {
    return 1;
  }
  s_idleTimeoutMs = (uint32_t)strtoul(argv[1], nullptr, 10) * 1000;
  noteActivity();
  printf("IDLE %u\n", (unsigned)(s_idleTimeoutMs / 1000));
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
  consoleRegisterCmd("sleep", "Blank the screen", cmdSleep);
  consoleRegisterCmd("wake", "Unblank the screen and redraw", cmdWake);
  consoleRegisterCmd("qr", "Toggle the pairing screen", cmdQr);
  consoleRegisterCmd("idle", "Blank the screen after <seconds>, 0 to never", cmdIdle);
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

  s_showQr = !bridgeCommissioned();
  if (s_showQr) {
    bridgeOpenPairingWindow();
  }
  noteActivity();
  xTaskCreate(uiTask, "ui", 8192, nullptr, 4, nullptr);
  ESP_LOGI(TAG, "genie booted");
}
