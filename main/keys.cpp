#include "keys.h"

#include <cstdio>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "i2c.h"

static const char *TAG = "keys";

static const uint8_t ADDR_AXP2101 = 0x34;
static const gpio_num_t PIN_KEY_BOOT = GPIO_NUM_0;

static const uint8_t REG_STATUS1 = 0x00;
static const uint8_t REG_STATUS2 = 0x01;
static const uint8_t REG_IC_TYPE = 0x03;
static const uint8_t REG_ADC_CHANNEL_CTRL = 0x30;
static const uint8_t REG_TS_PIN_CTRL = 0x50;
static const uint8_t REG_ADC_BATT_H = 0x34;
static const uint8_t REG_ADC_BATT_L = 0x35;
static const uint8_t REG_ADC_VBUS_H = 0x38;
static const uint8_t REG_ADC_VBUS_L = 0x39;
static const uint8_t REG_ADC_SYS_H = 0x3A;
static const uint8_t REG_ADC_SYS_L = 0x3B;
static const uint8_t REG_ADC_TEMP_H = 0x3C;
static const uint8_t REG_ADC_TEMP_L = 0x3D;
static const uint8_t REG_INTEN2 = 0x41;
static const uint8_t REG_INTSTS1 = 0x48;
static const uint8_t REG_INTSTS2 = 0x49;
static const uint8_t REG_INTSTS3 = 0x4A;

static const uint8_t CHIP_ID_AXP2101 = 0x4A;
// INTEN2/INTSTS2 bit 3: PEK short-press IRQ enable/flag (long press is the
// PMU's own power-off and stays disabled here, per src/keys.cpp).
static const uint8_t PKEY_SHORT_IRQ_BIT = 1 << 3;

static i2c_master_dev_handle_t s_pmu = nullptr;
static bool s_pmuReady = false;

// Set on any failed read since the last resetPmuFault(), so pmuReport() and
// pmuStatus() can tell a real reading from a bus glitch that would otherwise
// look like a plausible "0 mV, not charging" battery. Logged once per failure
// burst rather than once per register to avoid flooding the console when the
// bus is down for a whole report (multiple registers each fail once).
static bool s_pmuFault = false;
static bool s_pmuFaultLogged = false;

static void resetPmuFault() {
  s_pmuFault = false;
}

static uint8_t pmuReadReg(uint8_t reg) {
  uint8_t value = 0;
  esp_err_t err = i2cReadReg(s_pmu, reg, &value, 1);
  if (err != ESP_OK) {
    s_pmuFault = true;
    if (!s_pmuFaultLogged) {
      ESP_LOGE(TAG, "AXP2101 read of reg 0x%02X failed: %s", reg, esp_err_to_name(err));
      s_pmuFaultLogged = true;
    }
    return 0;
  }
  s_pmuFaultLogged = false;
  return value;
}

static void pmuClearIrqStatus() {
  esp_err_t a = i2cWriteReg(s_pmu, REG_INTSTS1, 0xFF);
  esp_err_t b = i2cWriteReg(s_pmu, REG_INTSTS2, 0xFF);
  esp_err_t c = i2cWriteReg(s_pmu, REG_INTSTS3, 0xFF);
  if (a != ESP_OK || b != ESP_OK || c != ESP_OK) {
    ESP_LOGE(TAG, "AXP2101 IRQ status clear failed");
  }
}

// Battery voltage is a 13-bit ADC (5-bit high byte), vbus/sys/temp are 14-bit
// (6-bit high byte); both report their raw value directly in the target unit.
static uint16_t pmuReadAdc(uint8_t highReg, uint8_t lowReg, uint8_t highMask) {
  uint16_t high = pmuReadReg(highReg) & highMask;
  uint8_t low = pmuReadReg(lowReg);
  return (uint16_t)((high << 8) | low);
}

static bool pmuBatteryPresent() {
  return (pmuReadReg(REG_STATUS1) >> 3) & 1;
}

static bool pmuVbusIn() {
  bool vbusGood = (pmuReadReg(REG_STATUS1) >> 5) & 1;
  bool vbusAbsentBit = (pmuReadReg(REG_STATUS2) >> 3) & 1;
  return vbusGood && !vbusAbsentBit;
}

static bool pmuCharging() {
  return (pmuReadReg(REG_STATUS2) >> 5) == 1;
}

static uint16_t pmuBattMv() {
  return pmuBatteryPresent() ? pmuReadAdc(REG_ADC_BATT_H, REG_ADC_BATT_L, 0x1F) : 0;
}

static uint16_t pmuVbusMv() {
  return pmuVbusIn() ? pmuReadAdc(REG_ADC_VBUS_H, REG_ADC_VBUS_L, 0x3F) : 0;
}

static uint16_t pmuSysMv() {
  return pmuReadAdc(REG_ADC_SYS_H, REG_ADC_SYS_L, 0x3F);
}

// Die temperature conversion straight from the XPowersLib constant.
static float pmuTempC() {
  uint16_t raw = pmuReadAdc(REG_ADC_TEMP_H, REG_ADC_TEMP_L, 0x3F);
  return 22.0f + (7274.0f - raw) / 20.0f;
}

// The AXP2101's own fuel gauge has no battery model programmed — battlog has it
// reporting 52% at 3.58 V on a cell that died half an hour later, and 50% at
// 4.19 V — so percent is derived from voltage instead. Resting-OCV curve for a
// single LiPo cell, which is only true at rest: under load the pack sags and
// this reads low, and on USB the cell sits at charge voltage, so it reads high
// — a cell at 60% shows ~100% while charging. Only unflagged battlog rows are
// state of charge; usb/chg rows are the charger's voltage, not the cell's.
static int percentFromMv(uint16_t mv) {
  static const struct {
    uint16_t mv;
    uint8_t pct;
  } CURVE[] = {{3300, 0},  {3500, 8},  {3600, 20}, {3700, 38},
               {3800, 55}, {3900, 70}, {4000, 85}, {4150, 100}};

  if (mv <= CURVE[0].mv) {
    return 0;
  }
  for (size_t i = 1; i < sizeof(CURVE) / sizeof(CURVE[0]); i++) {
    if (mv < CURVE[i].mv) {
      uint16_t loMv = CURVE[i - 1].mv, hiMv = CURVE[i].mv;
      uint8_t loPct = CURVE[i - 1].pct, hiPct = CURVE[i].pct;
      return loPct + (mv - loMv) * (hiPct - loPct) / (hiMv - loMv);
    }
  }
  return 100;
}

void keysInit() {
  gpio_config_t bootCfg = {};
  bootCfg.pin_bit_mask = 1ULL << PIN_KEY_BOOT;
  bootCfg.mode = GPIO_MODE_INPUT;
  bootCfg.pull_up_en = GPIO_PULLUP_ENABLE;
  ESP_ERROR_CHECK(gpio_config(&bootCfg));

  s_pmu = i2cAddDevice(ADDR_AXP2101);
  s_pmuReady = pmuReadReg(REG_IC_TYPE) == CHIP_ID_AXP2101;
  if (!s_pmuReady) {
    printf("keys: AXP2101 not found, power key disabled\n");
  } else {
    // XPowersLib's begin() runs this on every chip it recognises: with no TS
    // thermistor wired on this board, leaving the TS pin in its reset default
    // (NTC input) can make the charger read a bogus temperature off a
    // floating pin and refuse to charge. Fixing it to "external input" here
    // takes the TS pin out of the charge-safety decision entirely.
    uint8_t tsPinCtrl = pmuReadReg(REG_TS_PIN_CTRL);
    ESP_ERROR_CHECK(i2cWriteReg(s_pmu, REG_TS_PIN_CTRL, (tsPinCtrl & 0xF0) | 0x10));
    uint8_t adcChannelCtrl = pmuReadReg(REG_ADC_CHANNEL_CTRL);
    ESP_ERROR_CHECK(i2cWriteReg(s_pmu, REG_ADC_CHANNEL_CTRL, adcChannelCtrl & ~(1 << 1)));

    uint8_t inten2 = pmuReadReg(REG_INTEN2);
    ESP_ERROR_CHECK(i2cWriteReg(s_pmu, REG_INTEN2, inten2 | PKEY_SHORT_IRQ_BIT));
    pmuClearIrqStatus();
    ESP_LOGI(TAG, "AXP2101 ready, INTEN2=0x%02X (bit3 PEK short-press), TS_PIN_CTRL=0x%02X",
             pmuReadReg(REG_INTEN2), pmuReadReg(REG_TS_PIN_CTRL));
  }
}

void pmuReport() {
  if (!s_pmuReady) {
    printf("PMU absent\n");
    return;
  }
  resetPmuFault();
  uint16_t battMv = pmuBattMv();
  uint16_t vbusMv = pmuVbusMv();
  uint16_t sysMv = pmuSysMv();
  int pct = battMv == 0 ? -1 : percentFromMv(battMv);
  bool vbusIn = pmuVbusIn();
  bool charging = pmuCharging();
  float tempC = pmuTempC();
  if (s_pmuFault) {
    printf("PMU absent\n");
    return;
  }
  printf("PMU batt=%umV vbus=%umV sys=%umV pct=%d vbus_in=%d charging=%d temp=%.1fC\n", battMv,
         vbusMv, sysMv, pct, vbusIn ? 1 : 0, charging ? 1 : 0, tempC);
}

// Two callers on the ui task want this on two different schedules — the drain
// log every ten minutes, the Matter sensors every thirty seconds, the light
// sleep decision every tick. One cached burst serves all of them: the AXP2101's
// voltages and fuel gauge do not move faster than this, and the alternative is
// three polling cadences on a bus the panel also uses.
static const int64_t CACHE_US = 5 * 1000 * 1000;

PmuStatus pmuStatus() {
  static PmuStatus cached{};
  static int64_t readAt = 0;

  PmuStatus s{};
  if (!s_pmuReady) {
    return s;
  }
  int64_t now = esp_timer_get_time();
  if (readAt != 0 && now - readAt < CACHE_US) {
    return cached;
  }
  readAt = now;
  resetPmuFault();
  uint16_t battMv = pmuBattMv();
  float tempC = pmuTempC();
  bool charging = pmuCharging();
  bool onUsb = pmuVbusIn();
  if (!s_pmuFault) {
    s.present = true;
    s.millivolts = battMv;
    s.percent = (uint8_t)(battMv == 0 ? 0 : percentFromMv(battMv));
    s.celsius = tempC;
    s.charging = charging;
    s.onUsb = onUsb;
  }
  cached = s;
  return cached;
}

bool keyBootPressed() {
  static bool wasDown = false;
  bool down = gpio_get_level(PIN_KEY_BOOT) == 0;
  bool released = !down && wasDown;
  wasDown = down;
  return released;
}

bool keyPowerPressed() {
  if (!s_pmuReady) {
    return false;
  }
  bool hit = (pmuReadReg(REG_INTSTS2) & PKEY_SHORT_IRQ_BIT) != 0;
  if (hit) {
    pmuClearIrqStatus();
  }
  return hit;
}
