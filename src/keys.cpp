#include "keys.h"

#include <Arduino.h>
#include <Wire.h>
#include <XPowersLib.h>

static const uint8_t PIN_KEY_BOOT = 0;
static const uint8_t PIN_SDA = 15;
static const uint8_t PIN_SCL = 14;

static XPowersAXP2101 s_pmu;
static bool s_pmuReady = false;

// Only the short-press IRQ is enabled; a long press stays with the PMU's own
// power-off behaviour rather than becoming a UI event.
void keysInit() {
  pinMode(PIN_KEY_BOOT, INPUT_PULLUP);
  s_pmuReady = s_pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, PIN_SDA, PIN_SCL);
  if (!s_pmuReady) {
    Serial.println("keys: AXP2101 not found, power key disabled");
    return;
  }
  s_pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ);
  s_pmu.clearIrqStatus();
}

void pmuReport() {
  if (!s_pmuReady) {
    Serial.println("PMU absent");
    return;
  }
  Serial.printf("PMU batt=%umV vbus=%umV sys=%umV pct=%d vbus_in=%d charging=%d temp=%.1fC\n",
                s_pmu.getBattVoltage(), s_pmu.getVbusVoltage(), s_pmu.getSystemVoltage(),
                s_pmu.getBatteryPercent(), s_pmu.isVbusIn() ? 1 : 0,
                s_pmu.isCharging() ? 1 : 0, s_pmu.getTemperature());
}

PmuStatus pmuStatus() {
  PmuStatus s{};
  if (!s_pmuReady) {
    return s;
  }
  s.present = true;
  s.percent = (uint8_t)constrain(s_pmu.getBatteryPercent(), 0, 100);
  s.celsius = s_pmu.getTemperature();
  s.charging = s_pmu.isCharging();
  s.onUsb = s_pmu.isVbusIn();
  return s;
}

bool keyBootPressed() {
  static bool wasDown = false;
  bool down = digitalRead(PIN_KEY_BOOT) == LOW;
  bool released = !down && wasDown;
  wasDown = down;
  return released;
}

bool keyPowerPressed() {
  if (!s_pmuReady) {
    return false;
  }
  s_pmu.getIrqStatus();
  bool hit = s_pmu.isPekeyShortPressIrq();
  if (hit) {
    s_pmu.clearIrqStatus();
  }
  return hit;
}
