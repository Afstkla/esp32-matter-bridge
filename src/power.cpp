#include "power.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_wifi.h>

#include "keys.h"

// WiFi modem sleep is the single biggest saving available here: the radio
// idles between beacons instead of listening continuously. "max" waits for the
// router's DTIM beacon, which adds a beat of latency to anything Home sends —
// drop to "min" if that ever grates.
static const wifi_ps_type_t DEFAULT_PS = WIFI_PS_MAX_MODEM;

// The screen is dark for almost all of the device's life, and nothing that
// runs in the dark needs more than WiFi's floor.
static const uint32_t DEFAULT_AWAKE_MHZ = 240;
static const uint32_t ASLEEP_MHZ = 80;

// A day of samples. The AXP2101 has no current ADC, so drain rate has to be
// read off voltage and fuel-gauge percent over time.
static const uint32_t SAMPLE_INTERVAL_MS = 10UL * 60 * 1000;
static const uint16_t LOG_ENTRIES = 144;

struct BattSample {
  uint16_t mv;  // 0 marks an unused slot
  uint8_t pct;
  uint8_t flags;
};
static const uint8_t FLAG_USB = 1;
static const uint8_t FLAG_CHARGING = 2;
static const uint8_t FLAG_BOOT = 4;

static Preferences s_prefs;
static BattSample s_log[LOG_ENTRIES];
static uint16_t s_head = 0;
static uint32_t s_awakeMhz = DEFAULT_AWAKE_MHZ;

static const char *psName(wifi_ps_type_t ps) {
  return ps == WIFI_PS_NONE ? "none" : ps == WIFI_PS_MIN_MODEM ? "min" : "max";
}

void powerBegin() {
  s_prefs.begin("power", false);
  s_awakeMhz = s_prefs.getUShort("mhz", DEFAULT_AWAKE_MHZ);
  setCpuFrequencyMhz(s_awakeMhz);
  esp_wifi_set_ps((wifi_ps_type_t)s_prefs.getUChar("ps", DEFAULT_PS));
  if (s_prefs.getBytesLength("log") == sizeof(s_log)) {
    s_prefs.getBytes("log", s_log, sizeof(s_log));
    s_head = s_prefs.getUShort("head", 0) % LOG_ENTRIES;
  } else {
    memset(s_log, 0, sizeof(s_log));
  }
}

static void sample(bool boot) {
  PmuStatus pmu = pmuStatus();
  if (!pmu.present || pmu.millivolts == 0) {
    return;
  }
  BattSample &s = s_log[s_head];
  s.mv = pmu.millivolts;
  s.pct = pmu.percent;
  s.flags = (pmu.onUsb ? FLAG_USB : 0) | (pmu.charging ? FLAG_CHARGING : 0) |
            (boot ? FLAG_BOOT : 0);
  s_head = (s_head + 1) % LOG_ENTRIES;
  s_prefs.putBytes("log", s_log, sizeof(s_log));
  s_prefs.putUShort("head", s_head);
}

void powerPoll(bool panelAsleep) {
  static bool wasAsleep = false;
  if (panelAsleep != wasAsleep) {
    wasAsleep = panelAsleep;
    setCpuFrequencyMhz(panelAsleep ? ASLEEP_MHZ : s_awakeMhz);
  }

  static uint32_t lastAt = 0;
  static bool firstSample = true;
  if (!firstSample && millis() - lastAt < SAMPLE_INTERVAL_MS) {
    return;
  }
  lastAt = millis();
  sample(firstSample);
  firstSample = false;
}

bool powerSetWifiPs(const char *mode) {
  wifi_ps_type_t ps;
  if (strcmp(mode, "none") == 0) {
    ps = WIFI_PS_NONE;
  } else if (strcmp(mode, "min") == 0) {
    ps = WIFI_PS_MIN_MODEM;
  } else if (strcmp(mode, "max") == 0) {
    ps = WIFI_PS_MAX_MODEM;
  } else {
    return false;
  }
  if (esp_wifi_set_ps(ps) != ESP_OK) {
    return false;
  }
  s_prefs.putUChar("ps", (uint8_t)ps);
  return true;
}

bool powerSetCpuMhz(uint32_t mhz) {
  if (mhz != 80 && mhz != 160 && mhz != 240) {
    return false;
  }
  if (!setCpuFrequencyMhz(mhz)) {
    return false;
  }
  s_awakeMhz = mhz;
  s_prefs.putUShort("mhz", (uint16_t)mhz);
  return true;
}

void powerReport() {
  wifi_ps_type_t ps = WIFI_PS_NONE;
  esp_wifi_get_ps(&ps);
  Serial.printf("POWER cpu=%luMHz awake=%luMHz wifi_ps=%s heap=%u\n",
                (unsigned long)getCpuFrequencyMhz(), (unsigned long)s_awakeMhz, psName(ps),
                (unsigned)ESP.getFreeHeap());
  pmuReport();
}

void powerDumpLog() {
  Serial.println("BATTLOG oldest first, one sample per 10 min, gap-free while powered");
  for (uint16_t i = 0; i < LOG_ENTRIES; i++) {
    const BattSample &s = s_log[(s_head + i) % LOG_ENTRIES];
    if (s.mv == 0) {
      continue;
    }
    Serial.printf("BATT %umV %u%%%s%s%s\n", s.mv, s.pct, (s.flags & FLAG_USB) ? " usb" : "",
                  (s.flags & FLAG_CHARGING) ? " chg" : "", (s.flags & FLAG_BOOT) ? " boot" : "");
  }
}
