#include "panel.h"
#include <Wire.h>

static const uint8_t PIN_SDA = 15;
static const uint8_t PIN_SCL = 14;
static const uint8_t ADDR_TCA9554 = 0x20;
static const uint8_t ADDR_CST816 = 0x15;

static const int16_t PANEL_W = 368;
static const int16_t PANEL_H = 448;
static const uint8_t CO5300_COL_OFFSET = 16;
static const uint8_t PANEL_ON_BRIGHTNESS = 180;

// Arduino_Canvas::begin() allocates its framebuffer with aligned_alloc, which is
// internal RAM only — 322 KB will not fit next to the Matter stack. Pre-seeding
// _framebuffer from PSRAM makes begin() skip the allocation entirely.
class PsramCanvas : public Arduino_Canvas {
public:
  PsramCanvas(int16_t w, int16_t h, Arduino_G *output) : Arduino_Canvas(w, h, output) {
    _framebuffer = (uint16_t *)ps_malloc((size_t)w * h * sizeof(uint16_t));
  }
  bool hasFramebuffer() const {
    return _framebuffer != nullptr;
  }
};

static Arduino_ESP32QSPI *s_bus = nullptr;
static Arduino_CO5300 *s_panel = nullptr;
static PsramCanvas *s_canvas = nullptr;

static void tcaWrite(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(ADDR_TCA9554);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission();
}

// EXIO0 = LCD_RESET, EXIO1 = TP_RESET, EXIO2 = DSI_PWR_EN. Neither reset line
// reaches a GPIO on this board, so the panel driver is constructed with
// GFX_NOT_DEFINED and relies on this pulse having already run.
static void releaseResets() {
  tcaWrite(0x03, 0xF8);
  tcaWrite(0x01, 0x00);
  delay(20);
  tcaWrite(0x01, 0x07);
  delay(20);
}

static bool cstRead(uint8_t reg, uint8_t *out, uint8_t len) {
  Wire.beginTransmission(ADDR_CST816);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(ADDR_CST816, len) != len) {
    return false;
  }
  for (uint8_t i = 0; i < len; i++) {
    out[i] = Wire.read();
  }
  return true;
}

static void cstWrite(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(ADDR_CST816);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission();
}

// The controller reports nothing until its interrupt mode is configured — 0xFA
// bit 4 (motion) is what Arduino_DriveBus writes, and it is the difference
// between frozen coordinate registers and a working digitiser. The chip also
// needs a few hundred ms after reset before it answers at all.
static void touchInit() {
  uint8_t id = 0;
  for (int attempt = 0; attempt < 25; attempt++) {
    if (cstRead(0xA7, &id, 1)) {
      Serial.printf("touch: CST816 chip id 0x%02X after %d ms\n", id, attempt * 20);
      cstWrite(0xFA, 0x10);
      delay(20);
      return;
    }
    delay(20);
  }
  Serial.println("touch: no response at 0x15");
}

bool panelInit() {
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);
  releaseResets();
  touchInit();

  s_bus = new Arduino_ESP32QSPI(12, 11, 4, 5, 6, 7);
  s_panel = new Arduino_CO5300(s_bus, GFX_NOT_DEFINED, 0, PANEL_W, PANEL_H,
                               CO5300_COL_OFFSET, 0, 0, 0);
  s_canvas = new PsramCanvas(PANEL_W, PANEL_H, s_panel);
  if (!s_canvas->hasFramebuffer()) {
    Serial.println("panel: PSRAM framebuffer alloc failed");
    return false;
  }
  if (!s_canvas->begin(80000000)) {
    Serial.println("panel: canvas begin failed");
    return false;
  }
  s_panel->setBrightness(0);
  s_canvas->fillScreen(RGB565_BLACK);
  panelFlush();
  s_panel->setBrightness(PANEL_ON_BRIGHTNESS);
  return true;
}

Arduino_Canvas &panelCanvas() {
  return *s_canvas;
}

static bool s_asleep = false;

void panelBrightness(uint8_t level) {
  s_panel->setBrightness(level);
}

bool panelAsleep() {
  return s_asleep;
}

// Deliberately no displayOff(): it issues SLPIN, and the CST816 shares this
// display module's power domain, so sleeping the panel controller kills touch
// and nothing can wake us again. Blanking the framebuffer is the real saving
// anyway — an AMOLED's black pixels are simply unlit.
void panelSleep() {
  if (s_asleep) {
    return;
  }
  s_canvas->fillScreen(RGB565_BLACK);
  panelFlush();
  s_panel->setBrightness(0);
  s_asleep = true;
}

// The caller redraws; the framebuffer was blanked on the way down.
void panelWake() {
  if (!s_asleep) {
    return;
  }
  s_panel->setBrightness(PANEL_ON_BRIGHTNESS);
  s_asleep = false;
}

// CO5300 leaves the panel black if a frame arrives as many small writes, so the
// whole framebuffer goes out in a single transaction.
void panelFlush() {
  s_panel->draw16bitRGBBitmap(0, 0, s_canvas->getFramebuffer(), PANEL_W, PANEL_H);
}

// Prints the touch registers whenever they change. The digitiser reports
// nothing at all until 0xFA is configured, and this is what distinguishes that
// from a wiring or address fault.
void panelTouchDump(uint32_t durationMs) {
  uint8_t previous[6] = {0};
  uint32_t until = millis() + durationMs;
  Serial.println("DUMP start");
  while (millis() < until) {
    uint8_t now[6];
    if (!cstRead(0x01, now, sizeof(now))) {
      Serial.println("DUMP read failed");
      delay(250);
      continue;
    }
    if (memcmp(now, previous, sizeof(now)) != 0) {
      Serial.printf("DUMP %02X %02X %02X %02X %02X %02X\n", now[0], now[1], now[2], now[3],
                    now[4], now[5]);
      memcpy(previous, now, sizeof(now));
    }
    delay(50);
  }
  Serial.println("DUMP end");
}

bool panelTouch(int16_t *x, int16_t *y) {
  Wire.beginTransmission(ADDR_CST816);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(ADDR_CST816, (uint8_t)5) != 5) {
    return false;
  }
  uint8_t fingers = Wire.read();
  uint8_t xh = Wire.read();
  uint8_t xl = Wire.read();
  uint8_t yh = Wire.read();
  uint8_t yl = Wire.read();
  if (fingers == 0) {
    return false;
  }
  *x = (int16_t)(((xh & 0x0F) << 8) | xl);
  *y = (int16_t)(((yh & 0x0F) << 8) | yl);
  return true;
}
