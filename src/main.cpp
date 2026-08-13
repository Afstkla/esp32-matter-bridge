#include <Matter.h>
#include <WiFi.h>
#include <nvs.h>
#include <app/server/Server.h>
#include <lwip/netif.h>
#include <platform/CHIPDeviceLayer.h>
#include <qrcode.h>

#include "bridge.h"
#include "builder.h"
#include "keys.h"
#include "netcfg.h"
#include "panel.h"

// The prebuilt Arduino lwIP is compiled with CONFIG_LWIP_HOOK_IP6_INPUT_CUSTOM,
// so the application must supply this hook or the link fails. Dropping packets
// that arrive before the interface has a link-local address is the stock
// ESP-IDF implementation.
extern "C" int lwip_hook_ip6_input(struct pbuf *p, struct netif *inp) {
  if (ip6_addr_isany_val(inp->ip6_addr[0].u_addr.ip6)) {
    pbuf_free(p);
    return 1;
  }
  return 0;
}

static const int16_t PANEL_W = 368;
static const int16_t PANEL_H = 448;

static bool s_showQr = true;
static uint32_t s_lastActivityAt = 0;
static uint32_t s_idleTimeoutMs = 60000;

static void noteActivity() {
  s_lastActivityAt = millis();
}

static void drawCentredText(int16_t y, const String &text, uint8_t size, uint16_t colour) {
  Arduino_Canvas &g = panelCanvas();
  g.setTextSize(size);
  g.setTextColor(colour);
  g.setCursor((PANEL_W - (int16_t)text.length() * 6 * size) / 2, y);
  g.print(text);
}

// Apple Home scans the raw "MT:..." payload, not the project-chip URL that
// wraps it, so strip everything up to the query parameter.
static String pairingPayload() {
  String url = Matter.getOnboardingQRCodeUrl();
  int at = url.indexOf("data=");
  return at < 0 ? url : url.substring(at + 5);
}

static String formattedPairingCode() {
  String raw = Matter.getManualPairingCode();
  if (raw.length() != 11) {
    return raw;
  }
  return raw.substring(0, 4) + "-" + raw.substring(4, 7) + "-" + raw.substring(7);
}

static void drawPairingScreen() {
  QRCode qr;
  uint8_t buffer[qrcode_getBufferSize(4)];
  qrcode_initText(&qr, buffer, 4, ECC_LOW, pairingPayload().c_str());

  Arduino_Canvas &g = panelCanvas();
  g.fillScreen(RGB565_BLACK);

  const int16_t scale = 8;
  const int16_t quiet = 4 * scale;
  const int16_t box = qr.size * scale + 2 * quiet;
  const int16_t x0 = (PANEL_W - box) / 2;
  const int16_t y0 = 34;

  g.fillRect(x0, y0, box, box, RGB565_WHITE);
  for (uint8_t y = 0; y < qr.size; y++) {
    for (uint8_t x = 0; x < qr.size; x++) {
      if (qrcode_getModule(&qr, x, y)) {
        g.fillRect(x0 + quiet + x * scale, y0 + quiet + y * scale, scale, scale, RGB565_BLACK);
      }
    }
  }

  drawCentredText(y0 + box + 22, formattedPairingCode(), 2, RGB565_WHITE);

  bool online = WiFi.status() == WL_CONNECTED;
  const char *hint = !online          ? "No WiFi - pairing cannot work"
                     : Matter.isDeviceCommissioned() ? "Already paired"
                                                     : "Scan in Apple Home";
  drawCentredText(y0 + box + 52, hint, 1, online ? RGB565(140, 140, 140) : RGB565(255, 90, 90));
  if (online) {
    drawCentredText(y0 + box + 68, WiFi.SSID() + "  " + WiFi.localIP().toString(), 1,
                    RGB565(90, 130, 90));
  }
  drawCentredText(PANEL_H - 20, "tap to continue", 1, RGB565(110, 110, 120));
  panelFlush();
}

static void drawScreen() {
  if (s_showQr) {
    drawPairingScreen();
  } else {
    builderDraw();
  }
}

// Commissioning crosses four hand-offs (BLE discovery, credential transfer,
// operational IPv6/mDNS, fabric join) and the prebuilt CHIP libs are compiled
// at log level ERROR, so progress logs do not exist. Polling the stack's own
// state is the only way to see which hand-off a failed attempt reached.
struct CommissioningStage {
  bool bleAdvertising;
  bool windowOpen;
  bool wifiProvisioned;
  bool wifiConnected;
  uint8_t fabrics;
};

static CommissioningStage readStage() {
  CommissioningStage s{};
  chip::DeviceLayer::PlatformMgr().LockChipStack();
  s.bleAdvertising = chip::DeviceLayer::ConnectivityMgr().IsBLEAdvertisingEnabled();
  s.windowOpen = chip::Server::GetInstance().GetCommissioningWindowManager().IsCommissioningWindowOpen();
  s.wifiProvisioned = chip::DeviceLayer::ConnectivityMgr().IsWiFiStationProvisioned();
  s.wifiConnected = chip::DeviceLayer::ConnectivityMgr().IsWiFiStationConnected();
  s.fabrics = chip::Server::GetInstance().GetFabricTable().FabricCount();
  chip::DeviceLayer::PlatformMgr().UnlockChipStack();
  return s;
}

static void printStage(const CommissioningStage &s) {
  Serial.printf("DIAG t=%lus ble_adv=%d window=%d wifi_prov=%d wifi_conn=%d fabrics=%u\n",
                (unsigned long)(millis() / 1000), s.bleAdvertising, s.windowOpen,
                s.wifiProvisioned, s.wifiConnected, s.fabrics);
  Serial.printf("DIAG ssid=%s rssi=%d ch=%d ip=%s ll6=%s g6=%s\n", WiFi.SSID().c_str(),
                WiFi.RSSI(), WiFi.channel(), WiFi.localIP().toString().c_str(),
                WiFi.linkLocalIPv6().toString().c_str(),
                WiFi.globalIPv6().toString().c_str());
}

static void pollDiag() {
  static uint32_t lastAt = 0;
  static CommissioningStage prev{};
  static bool seeded = false;
  if (millis() - lastAt < 500) {
    return;
  }
  lastAt = millis();
  CommissioningStage now = readStage();
  if (seeded && memcmp(&now, &prev, sizeof(now)) == 0) {
    return;
  }
  seeded = true;
  prev = now;
  printStage(now);
}

static void runCommand(const String &cmd) {
  if (cmd == "diag") {
    printStage(readStage());
    Serial.printf("DIAG commissioned=%d heap=%u\n", Matter.isDeviceCommissioned() ? 1 : 0,
                  (unsigned)ESP.getFreeHeap());
    return;
  }
  if (cmd.startsWith("click ")) {
    builderPress((uint8_t)cmd.substring(6).toInt());
  } else if (cmd.startsWith("level ")) {
    int space = cmd.indexOf(' ', 6);
    if (space < 0) {
      Serial.println("ERR usage: level <slot> <0-255>");
      return;
    }
    builderSetLevel((uint8_t)cmd.substring(6, space).toInt(),
                    (uint8_t)cmd.substring(space + 1).toInt());
  } else if (cmd.startsWith("on ")) {
    int space = cmd.indexOf(' ', 3);
    if (space < 0) {
      Serial.println("ERR usage: on <slot> <0|1>");
      return;
    }
    builderSetOnOff((uint8_t)cmd.substring(3, space).toInt(),
                    cmd.substring(space + 1).toInt() != 0);
  } else if (cmd.startsWith("remove ")) {
    builderRemove((uint8_t)cmd.substring(7).toInt());
  } else if (cmd == "swipe left" || cmd == "swipe right") {
    uint32_t at = millis();
    builderSwipe(cmd.endsWith("left") ? 1 : -1);
    Serial.printf("SWIPE %s %lums\n", cmd.endsWith("left") ? "left" : "right",
                  (unsigned long)(millis() - at));
  } else if (cmd == "reset slots") {
    builderReset();
  } else if (cmd == "add button") {
    builderAdd(true);
  } else if (cmd == "add level") {
    builderAdd(false);
  } else if (cmd == "slots") {
    for (uint8_t i = 0; i < builderSlotCount(); i++) {
      if (!builderSlotUsed(i)) {
        continue;
      }
      Serial.printf("SLOT %u %s %s %u on=%d\n", i, builderIsButton(i) ? "button" : "level",
                    builderLabel(i), builderLevel(i), builderOnOff(i) ? 1 : 0);
    }
  } else if (cmd == "qr") {
    s_showQr = !s_showQr;
    drawScreen();
    Serial.printf("SCREEN %s\n", s_showQr ? "qr" : "builder");
  } else if (cmd == "state") {
    Serial.printf("COMMISSIONED %d\n", Matter.isDeviceCommissioned() ? 1 : 0);
  } else if (cmd == "pairing") {
    Serial.printf("CODE %s\n", Matter.getManualPairingCode().c_str());
    Serial.printf("QR %s\n", Matter.getOnboardingQRCodeUrl().c_str());
  } else if (cmd == "window") {
    chip::DeviceLayer::PlatformMgr().LockChipStack();
    CHIP_ERROR err = chip::Server::GetInstance().GetCommissioningWindowManager().OpenBasicCommissioningWindow(
        chip::System::Clock::Seconds32(180));
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
    Serial.printf("WINDOW %s\n", err == CHIP_NO_ERROR ? "open" : "failed");
  } else if (cmd == "decommission") {
    Matter.decommission();
    Serial.println("DECOMMISSIONED");
  } else if (cmd == "touchdump") {
    panelTouchDump(15000);
  } else if (cmd == "sleep") {
    panelSleep();
    Serial.println("SLEEP");
  } else if (cmd == "wake") {
    panelWake();
    noteActivity();
    Serial.println("WAKE");
  } else if (cmd.startsWith("idle ")) {
    s_idleTimeoutMs = (uint32_t)cmd.substring(5).toInt() * 1000;
    noteActivity();
    Serial.printf("IDLE %u\n", s_idleTimeoutMs / 1000);
  } else if (cmd.startsWith("ssid ")) {
    netcfgSetSsid(cmd.substring(5));
  } else if (cmd.startsWith("pass ")) {
    netcfgSetPassword(cmd.substring(5));
  } else if (cmd == "wifi") {
    Serial.printf("WIFI stored='%s' status=%d ip=%s\n", netcfgSsid().c_str(),
                  (int)WiFi.status(), WiFi.localIP().toString().c_str());
  } else if (cmd == "forget") {
    netcfgForget();
  } else if (cmd == "nvs") {
    nvs_stats_t st;
    if (nvs_get_stats(NULL, &st) == ESP_OK) {
      Serial.printf("NVS used=%u free=%u total=%u namespaces=%u\n", (unsigned)st.used_entries,
                    (unsigned)st.free_entries, (unsigned)st.total_entries,
                    (unsigned)st.namespace_count);
    } else {
      Serial.println("NVS stats unavailable");
    }
  } else if (cmd == "power") {
    pmuReport();
  } else if (cmd == "ping") {
    Serial.println("PONG");
  } else if (cmd.length()) {
    Serial.printf("ERR unknown '%s'\n", cmd.c_str());
  }
}

static void pollSerial() {
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (line.length()) {
        runCommand(line);
        line = "";
      }
    } else if (line.length() < 64) {
      line += c;
    }
  }
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
    uint32_t at = millis();
    builderSwipe(dx < 0 ? 1 : -1);
    Serial.printf("SWIPE %s %lums\n", dx < 0 ? "left" : "right",
                  (unsigned long)(millis() - at));
    return;
  }
  if (abs(dx) > TAP_MAX_DRIFT || abs(dy) > TAP_MAX_DRIFT) {
    return;
  }

  Serial.printf("TAP %d %d\n", x0, y0);
  if (s_showQr) {
    s_showQr = false;
    drawScreen();
  } else if (x0 > PANEL_W - 60 && y0 < 60) {
    s_showQr = true;
    drawScreen();
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
  } else if (wasDown && millis() - lastAt > 120) {
    lastAt = millis();
    noteActivity();
    handleRelease(downX, downY, lastX, lastY);
  }
  wasDown = down;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== matter builder ===");

  if (!panelInit()) {
    Serial.println("panel init FAILED");
  }
  keysInit();

  builderBegin();
  netcfgJoin(0);
  bridgeStart();
  builderResume();

  s_showQr = !Matter.isDeviceCommissioned();
  if (!Matter.isDeviceCommissioned()) {
    Serial.printf("CODE %s\n", Matter.getManualPairingCode().c_str());
    Serial.printf("QR %s\n", Matter.getOnboardingQRCodeUrl().c_str());
  }
  drawScreen();
  Serial.println("ready");
}

void loop() {
  static bool lastCommissioned = false;
  static bool lastOnline = false;
  bool commissioned = Matter.isDeviceCommissioned();
  bool online = WiFi.status() == WL_CONNECTED;
  if (commissioned != lastCommissioned || online != lastOnline) {
    lastCommissioned = commissioned;
    lastOnline = online;
    if (commissioned) {
      s_showQr = false;
    }
    drawScreen();
  }

  pollSerial();
  pollDiag();
  pollKeys();
  pollTouch();
  if (builderNeedsRedraw()) {
    drawScreen();
  }
  if (!panelAsleep() && s_idleTimeoutMs > 0 && millis() - s_lastActivityAt > s_idleTimeoutMs) {
    panelSleep();
    Serial.println("SLEEP");
  }
  delay(20);
}
