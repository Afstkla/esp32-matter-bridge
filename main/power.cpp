#include "power.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#include "driver/gpio.h"
#include "esp_pm.h"
#include "esp_private/wifi.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"

#include "console.h"
#include "keys.h"
#include "panel.h"

// WiFi modem sleep is the single biggest saving available here: the radio idles
// between beacons instead of listening continuously. "max" waits for the
// router's DTIM beacon, which adds a beat of latency to anything Home sends —
// drop to "min" if that ever grates.
static const wifi_ps_type_t DEFAULT_PS = WIFI_PS_MAX_MODEM;

// Every time the station goes active it holds APB at 80 MHz — which blocks
// light sleep outright — for at least this long, refreshed by any packet either
// way. It looks like the obvious lever on the `wifi` lock's 25% residency, and
// it is not: four interleaved four-minute windows with the screen asleep and
// `usbsim on` put 50 ms at 26.7/24.9% and the driver's 8 ms floor at 25.5/27.4%
// — no effect outside the window-to-window spread. So this stays at IDF's own
// default and `power active <ms>` is left as the instrument that says so; the
// holds are the radio's traffic-driven active windows, not this tail.
static const uint32_t DEFAULT_MIN_ACTIVE_MS = 50;
static uint32_t s_minActiveMs = DEFAULT_MIN_ACTIVE_MS;

static void setMinActive(uint32_t ms) {
  s_minActiveMs = ms;
  esp_wifi_set_sleep_min_active_time(ms * 1000);
}

// The screen is dark for almost all of the device's life, and nothing that runs
// in the dark needs more than WiFi's floor. Under DFS these are the ends of the
// range, not a policy: a driver that needs the CPU takes a lock and gets 240,
// everything else idles at 80.
static const int MAX_MHZ = 240;
static const int MIN_MHZ = 80;

// A day of samples. The AXP2101 has no current ADC, so drain rate has to be
// read off the voltage slope over time. `pct` is interpolated from that same
// voltage, so it is a readability aid, not a second measurement — and it is
// only state of charge on rows without the usb/chg flags.
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

static const char *NAMESPACE = "power";
static const char *LOG_KEY = "log";
static const char *HEAD_KEY = "head";
static const char *PS_KEY = "ps";

static nvs_handle_t s_nvs = 0;
static BattSample s_log[LOG_ENTRIES];
static uint16_t s_head = 0;
static esp_pm_lock_handle_t s_awakeOnUsb = nullptr;
// The ui task writes this; `power` reads it from the console task.
static volatile bool s_awakeHeld = false;

// Bench instrument, never persisted and off at boot: on USB the PMU reports
// VBUS, the usb lock is held, and the chip never so much as attempts a light
// sleep — so the one state worth measuring is the one a cable makes
// unobservable. With this on the device behaves as if it were on battery.
//
// Written by the console task, read by the ui task, exactly as app_main.cpp
// routes every other console intent: the ui tick owns the lock and applies
// this, so `power usbsim` never touches it. An esp_pm lock is refcounted, and
// two tasks running holdAwake()'s read-decide-write concurrently can both
// acquire and only one release — a leaked lock that pins the chip awake while
// every readout swears it is free.
static volatile bool s_pretendBattery = false;

// Only ever called from the ui task, plus once from powerBegin() before that
// task exists.
static void holdAwake(bool want) {
  if (s_awakeOnUsb == nullptr || want == s_awakeHeld) {
    return;
  }
  esp_err_t err = want ? esp_pm_lock_acquire(s_awakeOnUsb) : esp_pm_lock_release(s_awakeOnUsb);
  if (err == ESP_OK) {
    s_awakeHeld = want;
  }
}

static uint32_t nowMs() {
  return (uint32_t)(esp_timer_get_time() / 1000);
}

static const char *psName(wifi_ps_type_t ps) {
  return ps == WIFI_PS_NONE ? "none" : ps == WIFI_PS_MIN_MODEM ? "min" : "max";
}

static const char *modeName(wifi_mode_t mode) {
  return mode == WIFI_MODE_STA     ? "sta"
         : mode == WIFI_MODE_AP    ? "ap"
         : mode == WIFI_MODE_APSTA ? "apsta"
                                   : "null";
}

// esp_wifi persists its mode in NVS and restores it at init, so an APSTA left
// behind by some earlier firmware outlives every reflash. CHIP only ever asks
// for station mode (EnableStationMode upgrades AP to APSTA but never downgrades
// it), so the leftover SoftAP kept beaconing on the station's channel, held the
// wifi APB_FREQ_MAX lock permanently, and neither modem nor light sleep could
// engage — measured as 12/12 lock samples Active, and 0/12 the moment the mode
// was forced back to STA on the same boot. Writing it once heals NVS too.
static void dropStaleSoftAp() {
  wifi_mode_t mode = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&mode) == ESP_OK && mode != WIFI_MODE_STA) {
    printf("POWER wifi mode was %s, forcing station-only\n", modeName(mode));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_STA));
  }
}

static bool setWifiPs(const char *mode) {
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
  nvs_set_u8(s_nvs, PS_KEY, (uint8_t)ps);
  nvs_commit(s_nvs);
  return true;
}

static void report() {
  wifi_ps_type_t ps = WIFI_PS_NONE;
  esp_wifi_get_ps(&ps);
  esp_pm_config_t pm = {};
  esp_pm_get_configuration(&pm);
  wifi_mode_t mode = WIFI_MODE_NULL;
  esp_wifi_get_mode(&mode);
  printf("POWER dfs=%d-%dMHz light_sleep=%d usb_lock=%d usbsim=%d wifi_ps=%s active=%ums "
         "wifi_mode=%s flush=%.1fms underflows=%u\n",
         pm.min_freq_mhz, pm.max_freq_mhz, pm.light_sleep_enable ? 1 : 0, s_awakeHeld ? 1 : 0,
         s_pretendBattery ? 1 : 0, psName(ps), (unsigned)s_minActiveMs, modeName(mode),
         panelLastFlushUs() / 1000.0, (unsigned)panelUnderflows());
  pmuReport();
}

static void dumpLog() {
  printf("BATTLOG oldest first, one sample per 10 min, gap-free while powered\n");
  for (uint16_t i = 0; i < LOG_ENTRIES; i++) {
    const BattSample &s = s_log[(s_head + i) % LOG_ENTRIES];
    if (s.mv == 0) {
      continue;
    }
    printf("BATT %umV %u%%%s%s%s\n", s.mv, s.pct, (s.flags & FLAG_USB) ? " usb" : "",
           (s.flags & FLAG_CHARGING) ? " chg" : "", (s.flags & FLAG_BOOT) ? " boot" : "");
  }
}

// Debug surface, kept on purpose. Pinning the clock or dropping light sleep at
// runtime is what made the flush wedge measurable — one binary, one variable at
// a time — and it is the escape hatch if that path misbehaves again. Delete it
// once the shelf-drain numbers are in and this configuration is settled.
static void configure(int maxMhz, int minMhz, bool lightSleep) {
  esp_pm_config_t pm = {
      .max_freq_mhz = maxMhz,
      .min_freq_mhz = minMhz,
      .light_sleep_enable = lightSleep,
  };
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_pm_configure(&pm));
}

// TP_INT from the CST820, idle-high through an off-chip 10 kΩ pull-up to
// VCC3V3. It lives here rather than in panel.cpp because what this probe
// measures is the sleep plumbing, not the digitiser.
static const gpio_num_t PIN_TP_INT = GPIO_NUM_21;
static bool s_intArmed = false;

// Nothing has ever configured this pad, and an unconfigured ESP32-S3 GPIO has
// its input buffer off — gpio_get_level() then reports 0 whatever the wire is
// doing, which reads exactly like a line stuck low.
static void openTpInt() {
  gpio_config_t cfg = {};
  cfg.pin_bit_mask = 1ULL << PIN_TP_INT;
  cfg.mode = GPIO_MODE_INPUT;
  ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&cfg));
}

// gpio_wakeup_enable() is also what exempts the pin from
// CONFIG_PM_SLP_DISABLE_GPIO, which is on (selected by the light-sleep GPIO
// reset workaround) and isolates every other pad on the way into automatic
// sleep. Arming by hand through rtc_gpio_wakeup_enable() would skip that and
// the pin would be deaf.
static void armTpInt(bool on) {
  openTpInt();
  if (on) {
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_wakeup_enable(PIN_TP_INT, GPIO_INTR_LOW_LEVEL));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_sleep_enable_gpio_wakeup());
  } else {
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_wakeup_disable(PIN_TP_INT));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO));
  }
  s_intArmed = on;
}

// Open drain, so the bench can pull the line low exactly as the digitiser does
// without ever fighting it for the high level.
static bool driveTpInt(const char *how) {
  if (strcasecmp(how, "z") == 0) {
    return gpio_set_direction(PIN_TP_INT, GPIO_MODE_INPUT) == ESP_OK;
  }
  if (strcmp(how, "0") != 0 && strcmp(how, "1") != 0) {
    return false;
  }
  gpio_set_direction(PIN_TP_INT, GPIO_MODE_INPUT_OUTPUT_OD);
  return gpio_set_level(PIN_TP_INT, how[0] - '0') == ESP_OK;
}

// The one measurement a serial cable cannot sit through: light sleep kills
// USB-CDC, so the window has to be opened, sampled and closed without a byte
// crossing the wire. esp_sleep_get_wakeup_causes() is a register read of what
// woke the chip last, and it is valid after light sleep, not only deep — so
// sampling it says which source the automatic path actually honoured.
struct WatchResult {
  uint32_t ms, samples, gpio, timer, other, low, mask;
  bool armed;
};
static WatchResult s_watch = {};

// Kept rather than only printed: light sleep stutters USB-CDC, and the line
// that matters is the one the window itself swallows.
static void reportWatch() {
  if (s_watch.samples == 0) {
    return;
  }
  printf("TPINT watch %ums armed=%d samples=%u gpio=%u timer=%u other=%u int_low=%u mask=0x%08X\n",
         (unsigned)s_watch.ms, s_watch.armed ? 1 : 0, (unsigned)s_watch.samples,
         (unsigned)s_watch.gpio, (unsigned)s_watch.timer, (unsigned)s_watch.other,
         (unsigned)s_watch.low, (unsigned)s_watch.mask);
}

static void watchWakeups(uint32_t ms) {
  bool restore = s_pretendBattery;
  s_pretendBattery = true;
  s_watch = {};
  s_watch.ms = ms;
  s_watch.armed = s_intArmed;
  int64_t until = esp_timer_get_time() + (int64_t)ms * 1000;
  while (esp_timer_get_time() < until) {
    vTaskDelay(pdMS_TO_TICKS(50));
    s_watch.samples++;
    if (gpio_get_level(PIN_TP_INT) == 0) {
      s_watch.low++;
    }
    uint32_t cause = esp_sleep_get_wakeup_causes();
    s_watch.mask |= cause;
    if (cause & BIT(ESP_SLEEP_WAKEUP_GPIO)) {
      s_watch.gpio++;
    } else if (cause & BIT(ESP_SLEEP_WAKEUP_TIMER)) {
      s_watch.timer++;
    } else {
      s_watch.other++;
    }
  }
  s_pretendBattery = restore;
  reportWatch();
}

// Watching the line itself rather than what it woke: a millisecond of
// granularity is enough for an interrupt meant to be serviced by a polling
// touch driver, and it costs no interrupt handler competing with the low-level
// trigger the wake path needs on the same pad.
static void scopeTpInt(uint32_t ms) {
  uint32_t samples = 0, low = 0, falls = 0;
  bool wasLow = gpio_get_level(PIN_TP_INT) == 0;
  int64_t until = esp_timer_get_time() + (int64_t)ms * 1000;
  while (esp_timer_get_time() < until) {
    vTaskDelay(1);
    bool isLow = gpio_get_level(PIN_TP_INT) == 0;
    samples++;
    low += isLow ? 1 : 0;
    falls += (isLow && !wasLow) ? 1 : 0;
    wasLow = isLow;
  }
  printf("TPINT scope %ums samples=%u low=%u falls=%u\n", (unsigned)ms, (unsigned)samples,
         (unsigned)low, (unsigned)falls);
}

static int cmdTpint(int argc, char **argv) {
  if (argc == 1) {
    printf("TPINT level=%d armed=%d\n", gpio_get_level(PIN_TP_INT), s_intArmed ? 1 : 0);
    reportWatch();
    return 0;
  }
  if (argc == 2 && strcasecmp(argv[1], "arm") == 0) {
    armTpInt(true);
  } else if (argc == 2 && strcasecmp(argv[1], "disarm") == 0) {
    armTpInt(false);
  } else if (argc == 3 && strcasecmp(argv[1], "drive") == 0) {
    if (!driveTpInt(argv[2])) {
      printf("ERR usage: tpint drive <0|1|z>\n");
      return 1;
    }
  } else if (argc == 3 && strcasecmp(argv[1], "watch") == 0) {
    watchWakeups((uint32_t)strtoul(argv[2], nullptr, 10));
    return 0;
  } else if (argc == 3 && strcasecmp(argv[1], "scope") == 0) {
    scopeTpInt((uint32_t)strtoul(argv[2], nullptr, 10));
    return 0;
  } else {
    printf("ERR usage: tpint [arm|disarm|drive <0|1|z>|scope <ms>|watch <ms>]\n");
    return 1;
  }
  printf("TPINT level=%d armed=%d\n", gpio_get_level(PIN_TP_INT), s_intArmed ? 1 : 0);
  return 0;
}

static int cmdPower(int argc, char **argv) {
  if (argc == 1) {
    report();
    return 0;
  }
  if (strcasecmp(argv[1], "usbsim") == 0) {
    if (argc != 3 || (strcasecmp(argv[2], "on") != 0 && strcasecmp(argv[2], "off") != 0)) {
      printf("ERR usage: power usbsim <on|off>\n");
      return 1;
    }
    s_pretendBattery = strcasecmp(argv[2], "on") == 0;
    // The next ui tick moves the lock, so `usb_lock=` below still reads the old
    // state for up to one tick. `usbsim=` is the intent; run `power` again for
    // the settled answer.
    report();
    return 0;
  }
  if (strcasecmp(argv[1], "active") == 0) {
    if (argc != 3) {
      printf("ERR usage: power active <ms>\n");
      return 1;
    }
    setMinActive((uint32_t)strtoul(argv[2], nullptr, 10));
    report();
    return 0;
  }

  const char *arg = argc == 2 ? argv[1] : "";
  long fixed = strtol(arg, nullptr, 10);
  if (strcasecmp(arg, "locks") == 0) {
    return esp_pm_dump_locks(stdout) == ESP_OK ? 0 : 1;
  }
  if (strcasecmp(arg, "timers") == 0) {
    return esp_timer_dump(stdout) == ESP_OK ? 0 : 1;
  }
  if (strcasecmp(arg, "rails") == 0) {
    pmuDumpRails();
    return 0;
  }
  if (strcasecmp(arg, "on") == 0) {
    configure(MAX_MHZ, MIN_MHZ, true);
  } else if (strcasecmp(arg, "off") == 0) {
    configure(MAX_MHZ, MAX_MHZ, false);
  } else if (fixed == 80 || fixed == 160 || fixed == 240) {
    configure((int)fixed, (int)fixed, false);
  } else {
    printf("ERR usage: power [locks|timers|rails|on|off|80|160|240|active <ms>|usbsim "
           "<on|off>]\n");
    return 1;
  }
  report();
  return 0;
}

static int cmdPs(int argc, char **argv) {
  if (argc != 2 || !setWifiPs(argv[1])) {
    printf("ERR usage: ps <none|min|max>\n");
    return 1;
  }
  printf("PS %s\n", argv[1]);
  return 0;
}

static int cmdBattlog(int argc, char **argv) {
  dumpLog();
  return 0;
}

void powerBegin() {
  configure(MAX_MHZ, MIN_MHZ, true);

  // USB-Serial-JTAG does not survive light sleep: the console goes dead and
  // stays dead. Held from here rather than from the first poll so a PMU that
  // cannot be read leaves the console working — only a reading that genuinely
  // says "no VBUS" ever lets the device sleep.
  ESP_ERROR_CHECK_WITHOUT_ABORT(
      esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "usb", &s_awakeOnUsb));
  holdAwake(true);

  dropStaleSoftAp();

  ESP_ERROR_CHECK(nvs_open(NAMESPACE, NVS_READWRITE, &s_nvs));
  uint8_t ps = (uint8_t)DEFAULT_PS;
  nvs_get_u8(s_nvs, PS_KEY, &ps);
  esp_wifi_set_ps((wifi_ps_type_t)ps);
  setMinActive(DEFAULT_MIN_ACTIVE_MS);

  size_t stored = sizeof(s_log);
  if (nvs_get_blob(s_nvs, LOG_KEY, s_log, &stored) != ESP_OK || stored != sizeof(s_log)) {
    memset(s_log, 0, sizeof(s_log));
  } else {
    nvs_get_u16(s_nvs, HEAD_KEY, &s_head);
    s_head %= LOG_ENTRIES;
  }

  consoleRegisterCmd("power",
                     "Show power state; 'locks' dumps PM locks and sleep stats, 'timers' dumps "
                     "esp_timer, 'rails' the PMU regulators, 'on' restores DFS and light sleep, "
                     "'off'|'80'|'160'|'240' pin the clock instead, 'active <ms>' sets the WiFi "
                     "minimum active time, 'usbsim on|off' fakes the cable being out (debug)",
                     cmdPower);
  openTpInt();
  consoleRegisterCmd("tpint",
                     "Touch INT (GPIO21) wake probe: no args reads the line, 'arm'|'disarm' the "
                     "light-sleep wake source, 'drive <0|1|z>' pulls it open-drain, 'scope <ms>' "
                     "samples the line every ms, 'watch <ms>' runs a battery-simulated window and "
                     "counts what woke the chip",
                     cmdTpint);
  consoleRegisterCmd("ps", "Set WiFi power save: none | min | max", cmdPs);
  consoleRegisterCmd("battlog", "Dump the battery drain log, oldest first", cmdBattlog);
}

static void sample(const PmuStatus &pmu, bool boot) {
  BattSample &s = s_log[s_head];
  s.mv = pmu.millivolts;
  s.pct = pmu.percent;
  s.flags = (uint8_t)((pmu.onUsb ? FLAG_USB : 0) | (pmu.charging ? FLAG_CHARGING : 0) |
                      (boot ? FLAG_BOOT : 0));
  s_head = (uint16_t)((s_head + 1) % LOG_ENTRIES);
  nvs_set_blob(s_nvs, LOG_KEY, s_log, sizeof(s_log));
  nvs_set_u16(s_nvs, HEAD_KEY, s_head);
  nvs_commit(s_nvs);
}

void powerPoll() {
  PmuStatus pmu = pmuStatus();
  if (!pmu.present) {
    return;
  }

  // Ahead of the battery check, because the lock only needs to know about VBUS:
  // a board running with no battery reads 0 mV, and gating this on that reading
  // pinned such a board awake for its whole life while `power` cheerfully
  // reported light sleep enabled.
  holdAwake(pmu.onUsb && !s_pretendBattery);

  if (pmu.millivolts == 0) {
    return;
  }

  static uint32_t lastAt = 0;
  static bool firstSample = true;
  if (!firstSample && nowMs() - lastAt < SAMPLE_INTERVAL_MS) {
    return;
  }
  lastAt = nowMs();
  sample(pmu, firstSample);
  firstSample = false;
}
