#include "keys.h"

#include <cstdio>

#include "driver/gpio.h"
#include "esp_log.h"

#include "console.h"
#include "i2c.h"

static const char *TAG = "keys";

static const uint8_t ADDR_AXP2101 = 0x34;
static const gpio_num_t PIN_KEY_BOOT = GPIO_NUM_0;

static const uint8_t REG_STATUS1 = 0x00;
static const uint8_t REG_STATUS2 = 0x01;
static const uint8_t REG_IC_TYPE = 0x03;
static const uint8_t REG_ADC_BATT_H = 0x34;
static const uint8_t REG_ADC_BATT_L = 0x35;
static const uint8_t REG_ADC_VBUS_H = 0x38;
static const uint8_t REG_ADC_VBUS_L = 0x39;
static const uint8_t REG_ADC_SYS_H = 0x3A;
static const uint8_t REG_ADC_SYS_L = 0x3B;
static const uint8_t REG_ADC_TEMP_H = 0x3C;
static const uint8_t REG_ADC_TEMP_L = 0x3D;
static const uint8_t REG_BAT_PERCENT = 0xA4;
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

static uint8_t pmuReadReg(uint8_t reg) {
  uint8_t value = 0;
  i2cReadReg(s_pmu, reg, &value, 1);
  return value;
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

static int pmuBattPercent() {
  return pmuBatteryPresent() ? pmuReadReg(REG_BAT_PERCENT) : -1;
}

static int cmdPower(int argc, char **argv) {
  pmuReport();
  return 0;
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
    uint8_t inten2 = pmuReadReg(REG_INTEN2);
    ESP_ERROR_CHECK(i2cWriteReg(s_pmu, REG_INTEN2, inten2 | PKEY_SHORT_IRQ_BIT));
    ESP_ERROR_CHECK(i2cWriteReg(s_pmu, REG_INTSTS1, 0xFF));
    ESP_ERROR_CHECK(i2cWriteReg(s_pmu, REG_INTSTS2, 0xFF));
    ESP_ERROR_CHECK(i2cWriteReg(s_pmu, REG_INTSTS3, 0xFF));
    ESP_LOGI(TAG, "AXP2101 ready, INTEN2=0x%02X (bit3 PEK short-press)", pmuReadReg(REG_INTEN2));
  }

  consoleRegisterCmd("power", "Print PMU status", cmdPower);
}

void pmuReport() {
  if (!s_pmuReady) {
    printf("PMU absent\n");
    return;
  }
  printf("PMU batt=%umV vbus=%umV sys=%umV pct=%d vbus_in=%d charging=%d temp=%.1fC\n",
         pmuBattMv(), pmuVbusMv(), pmuSysMv(), pmuBattPercent(), pmuVbusIn() ? 1 : 0,
         pmuCharging() ? 1 : 0, pmuTempC());
}

PmuStatus pmuStatus() {
  PmuStatus s{};
  if (!s_pmuReady) {
    return s;
  }
  s.present = true;
  s.millivolts = pmuBattMv();
  int percent = pmuBattPercent();
  s.percent = (uint8_t)(percent < 0 ? 0 : percent > 100 ? 100 : percent);
  s.celsius = pmuTempC();
  s.charging = pmuCharging();
  s.onUsb = pmuVbusIn();
  return s;
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
    i2cWriteReg(s_pmu, REG_INTSTS1, 0xFF);
    i2cWriteReg(s_pmu, REG_INTSTS2, 0xFF);
    i2cWriteReg(s_pmu, REG_INTSTS3, 0xFF);
  }
  return hit;
}
