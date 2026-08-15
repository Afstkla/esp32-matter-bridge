#include "panel.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

#include "console.h"
#include "gfx.h"
#include "i2c.h"
#include "theme.h"

static const char *TAG = "panel";

static const uint8_t ADDR_TCA9554 = 0x20;
static const uint8_t ADDR_CST816 = 0x15;

static const int PIN_LCD_CS = 12;
static const int PIN_LCD_SCK = 11;
static const int PIN_LCD_D0 = 4;
static const int PIN_LCD_D1 = 5;
static const int PIN_LCD_D2 = 6;
static const int PIN_LCD_D3 = 7;
// The wire would take 80 MHz, but the frame is DMA'd straight out of PSRAM and
// the EDMA cannot refill the SPI FIFO that fast: at 80 MHz every flush ends in
// "DMA TX underflow detected" and a black panel. 40 MHz is 20 MB/s, just above
// the ~18 MB/s the Arduino build actually sustained.
static const unsigned int LCD_CLOCK_HZ = 40 * 1000 * 1000;

static const int CO5300_COL_OFFSET = 16;
static const uint8_t PANEL_ON_BRIGHTNESS = 180;
static const size_t FRAME_BYTES = (size_t)PANEL_W * PANEL_H * sizeof(uint16_t);

// The QSPI command word the CO5300 expects: opcode 0x02 (write command) in the
// top byte, the register in the next one.
static const uint32_t QSPI_CMD_BRIGHTNESS = (0x02u << 24) | (0x51u << 8);

static esp_lcd_panel_io_handle_t s_io = nullptr;
static esp_lcd_panel_handle_t s_panel = nullptr;
static i2c_master_dev_handle_t s_tca = nullptr;
static i2c_master_dev_handle_t s_touch = nullptr;
static uint16_t *s_frame = nullptr;
static bool s_asleep = false;
static SemaphoreHandle_t s_flushDone = nullptr;
static uint32_t s_lastFlushUs = 0;

// EXIO0 = LCD_RESET, EXIO1 = DSI_PWR_EN, EXIO2 = TP_RESET. Neither reset line
// reaches a GPIO on this board, so the panel driver is created without a reset
// pin and relies on this pulse having already run.
static void releaseResets() {
  ESP_ERROR_CHECK(i2cWriteReg(s_tca, 0x03, 0xF8));
  ESP_ERROR_CHECK(i2cWriteReg(s_tca, 0x01, 0x00));
  vTaskDelay(pdMS_TO_TICKS(20));
  ESP_ERROR_CHECK(i2cWriteReg(s_tca, 0x01, 0x07));
  vTaskDelay(pdMS_TO_TICKS(20));
}

// The same three bits low: DSI_PWR_EN cuts VCI to the module, and the two reset
// lines go with it so the controller and the digitiser sit in their defined
// state while unpowered rather than being fed through their protection diodes.
static void cutPower() {
  ESP_ERROR_CHECK_WITHOUT_ABORT(i2cWriteReg(s_tca, 0x01, 0x00));
}

// The controller reports nothing until its interrupt mode is configured — 0xFA
// bit 4 (motion) is what Arduino_DriveBus writes, and it is the difference
// between frozen coordinate registers and a working digitiser. The chip also
// needs a few hundred ms after reset before it answers at all.
static void touchInit() {
  uint8_t id = 0;
  for (int attempt = 0; attempt < 25; attempt++) {
    if (i2cReadReg(s_touch, 0xA7, &id, 1) == ESP_OK) {
      ESP_LOGI(TAG, "CST816 chip id 0x%02X after %d ms", id, attempt * 20);
      ESP_ERROR_CHECK(i2cWriteReg(s_touch, 0xFA, 0x10));
      vTaskDelay(pdMS_TO_TICKS(20));
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  ESP_LOGE(TAG, "no touch response at 0x15");
}

// esp_lcd fires this once per frame, on the last chunk of the transfer.
static bool onFlushDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *) {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(s_flushDone, &woken);
  return woken == pdTRUE;
}

// Everything the CO5300 forgets when VCI goes: SWRESET, MADCTL/COLMOD, the
// vendor sequence (SLPOUT among it) and DISPON. Roughly 180 ms of mandated
// delays, which is most of what panelWake() costs.
static void initController() {
  ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
  ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
  ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, CO5300_COL_OFFSET, 0));
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
}

static void displayInit() {
  s_flushDone = xSemaphoreCreateBinary();

  spi_bus_config_t bus = {};
  bus.data0_io_num = PIN_LCD_D0;
  bus.data1_io_num = PIN_LCD_D1;
  bus.sclk_io_num = PIN_LCD_SCK;
  bus.data2_io_num = PIN_LCD_D2;
  bus.data3_io_num = PIN_LCD_D3;
  bus.max_transfer_sz = FRAME_BYTES;
  // SPI3 cannot DMA out of PSRAM on this chip, and the framebuffer lives there.
  ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

  esp_lcd_panel_io_spi_config_t io = CO5300_PANEL_IO_QSPI_CONFIG(PIN_LCD_CS, onFlushDone, nullptr);
  io.pclk_hz = LCD_CLOCK_HZ;
  // A frame is 329 728 bytes and the ESP32-S3's DMA caps one transaction at
  // 32 768, so esp_lcd splits it into eleven chunks. Twelve descriptors fit
  // the whole frame, so no chunk ever waits on a mid-frame recycle. (This was
  // once believed to be the wedge fix; it is not — see the drain-accounting
  // patch in components/esp_lcd/PATCH.md for the real mechanism.)
  io.trans_queue_depth = 12;
  io.flags.psram_dma_direct = 1;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io, &s_io));

  co5300_vendor_config_t vendor = {};
  vendor.flags.use_qspi_interface = 1;
  esp_lcd_panel_dev_config_t device = {};
  device.reset_gpio_num = -1;
  device.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
  device.bits_per_pixel = 16;
  device.vendor_config = &vendor;
  ESP_ERROR_CHECK(esp_lcd_new_panel_co5300(s_io, &device, &s_panel));

  initController();
}

static void registerCommands();

bool panelInit() {
  i2cInit();
  s_tca = i2cAddDevice(ADDR_TCA9554);
  s_touch = i2cAddDevice(ADDR_CST816);
  releaseResets();
  touchInit();

  // 64-byte aligned so the SPI driver DMAs the frame out of PSRAM as it stands
  // instead of copying it into an aligned bounce buffer first.
  s_frame = (uint16_t *)heap_caps_aligned_alloc(64, FRAME_BYTES, MALLOC_CAP_SPIRAM);
  if (s_frame == nullptr) {
    ESP_LOGE(TAG, "PSRAM framebuffer alloc failed");
    return false;
  }
  gfxInit(s_frame);

  displayInit();
  panelBrightness(0);
  gfxFillScreen(COL_BLACK);
  panelFlush();
  panelBrightness(PANEL_ON_BRIGHTNESS);
  registerCommands();
  return true;
}

// Asleep now means unpowered, so this and panelFlush() are where every caller's
// drawing stops: gfx writes reach PSRAM as they always did, and nothing reaches
// a module whose VCI is off. Guarding the two functions that talk to the wire
// covers pollState(), builderNeedsRedraw() and the Matter backlight endpoint at
// once, which guarding each caller would not.
void panelBrightness(uint8_t level) {
  if (s_asleep) {
    return;
  }
  // The component's own setter takes 0-100 and logs at INFO on every call; the
  // register is the 0-255 scale the rest of the firmware works in.
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_io_tx_param(s_io, QSPI_CMD_BRIGHTNESS, &level, 1));
}

bool panelAsleep() {
  return s_asleep;
}

// Brightness 0 is not off: the CO5300 keeps its charge pump running for
// ELVDD/ELVSS whatever it is showing, and this firmware used to leave DSI_PWR_EN
// high for the device's whole life. Cutting VCI is worth tens of mA — the
// larger half of the shelf drain (see the README's power section).
//
// DISPOFF first so the panel stops driving pixels before its supply is pulled;
// the settle is for the module's own decoupling to bleed down. SLPIN was
// considered and rejected: the driver's disp_on_off only sends DISPOFF, so
// SLPIN would be a raw QSPI command, and it saves nothing once VCI is gone and
// the reset lines are low.
//
// ponytail: the CST816 shares this rail, so touch dies with the panel and the
// PWR key is the only wake. Task #22 (wake on touch) would have to either keep
// the rail up — which is most of this saving — or open a timed window; both
// start from cutPower()/releaseResets() being the only pair that moves it.
void panelSleep() {
  if (s_asleep) {
    return;
  }
  gfxFillScreen(COL_BLACK);
  panelBrightness(0);
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_disp_on_off(s_panel, false));
  vTaskDelay(pdMS_TO_TICKS(20));
  cutPower();
  s_asleep = true;
}

// A full cold start of the module, because that is what it now is. The black
// framebuffer goes out before the brightness comes up so the first lit frame is
// not whatever the GRAM powered up holding; the caller then redraws. Touch is
// initialised last: it costs the longest and nobody can tap a screen they
// cannot see yet.
void panelWake() {
  if (!s_asleep) {
    return;
  }
  int64_t startedAt = esp_timer_get_time();
  releaseResets();
  initController();
  s_asleep = false;
  panelBrightness(0);
  panelFlush();
  panelBrightness(PANEL_ON_BRIGHTNESS);
  touchInit();
  ESP_LOGI(TAG, "panel wake took %u ms", (unsigned)((esp_timer_get_time() - startedAt) / 1000));
}

// The ui task is the only task that may be inside this, panelSleep(),
// panelWake(), panelBrightness() or any gfx call. esp_lcd's panel IO handle is
// shared by draw_bitmap and tx_param and is not thread safe, so a second task
// issuing a brightness command mid-frame corrupts the transfer bookkeeping.
// Console commands that want the panel leave their intent for the ui tick; see
// pollRequests() in app_main.cpp.
//
// One draw_bitmap for the whole frame: esp_lcd chunks it internally but keeps
// CS asserted across the chunks, and the CO5300 leaves the panel black if a
// frame arrives as separately addressed writes. Nothing byte-swaps on the way —
// the buffer is already big-endian (see theme.h), which is what the wire wants.
//
// draw_bitmap only queues the transfer, so the wait is not optional: the DMA is
// still reading the framebuffer when it returns, and the caller's next draw
// would race it. The timeout is a backstop so the ui task cannot hang.
//
// The frame DMAs straight out of PSRAM, and under enough bus contention the
// EDMA can fail to refill the SPI FIFO even at 40 MHz — a "DMA TX underflow",
// one garbled chunk on the wire. The wedge that used to follow (the next frame
// parking forever inside draw_bitmap) was esp_lcd losing count of a
// faulted-but-consumed result; that is fixed in components/esp_lcd (see its
// PATCH.md), which also counts those faults — so a damaged frame is now
// something this function can see and send again. An underflow costs an extra
// ~18 ms flush instead of leaving a garbled band that stands until the screen
// next changes, which on a screen that has gone quiet could be a long time.
void panelFlush() {
  if (s_asleep) {
    return;
  }
  int64_t startedAt = esp_timer_get_time();
  // Exactly two attempts: a retry that underflows too would only be a third
  // chance at the same odds, and the ui task has a watchdog to answer to.
  for (int attempt = 0; attempt < 2; attempt++) {
    uint32_t faultsBefore = esp_lcd_spi_underflow_count();
    // A flush that timed out still gets its callback eventually. Clearing that
    // stale give here is what stops the wait below from being satisfied by the
    // previous frame, which would put every flush from then on one frame out of
    // phase — drawing over a buffer the DMA is still reading.
    xSemaphoreTake(s_flushDone, 0);
    if (ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_draw_bitmap(
            s_panel, 0, 0, PANEL_W, PANEL_H, s_frame)) != ESP_OK) {
      return;
    }
    if (xSemaphoreTake(s_flushDone, pdMS_TO_TICKS(200)) != pdTRUE) {
      ESP_LOGE(TAG, "flush did not complete");
    }
    // The completion callback rides the last chunk, but the chunks' results sit
    // in esp_lcd's queue until something drains them, and the DMA fault only
    // surfaces in that drain. Unclaimed, it would surface inside the *next*
    // frame's CASET — blaming the wrong frame, and never firing at all for the
    // last frame before the screen goes quiet, which is the frame that matters.
    // A parameter transfer with no command drains and sends nothing.
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_io_tx_param(s_io, -1, nullptr, 0));
    uint32_t faulted = esp_lcd_spi_underflow_count() - faultsBefore;
    s_lastFlushUs = (uint32_t)(esp_timer_get_time() - startedAt);
    // This tick of quiet between frames is what makes underflows rare in the
    // first place: back-to-back frames keep the PSRAM bus saturated, and the
    // earlier bracket (no yield: glitch by frame 2; taskYIELD(): ~24; one tick:
    // 160+ clean) was measuring exactly that contention relief. The retry wants
    // it more than anyone: it follows the one flush known to have lost the bus.
    vTaskDelay(1);
    if (faulted == 0) {
      break;
    }
    if (attempt == 0) {
      ESP_LOGW(TAG, "frame damaged, %" PRIu32 " chunk(s) faulted; flushing it again", faulted);
    }
  }
}

uint32_t panelLastFlushUs() {
  return s_lastFlushUs;
}

uint32_t panelUnderflows() {
  return esp_lcd_spi_underflow_count();
}

// Prints the touch registers whenever they change. The digitiser reports
// nothing at all until 0xFA is configured, and this is what distinguishes that
// from a wiring or address fault.
void panelTouchDump(uint32_t durationMs) {
  uint8_t previous[6] = {0};
  int64_t until = esp_timer_get_time() + (int64_t)durationMs * 1000;
  printf("DUMP start\n");
  while (esp_timer_get_time() < until) {
    uint8_t now[6];
    if (i2cReadReg(s_touch, 0x01, now, sizeof(now)) != ESP_OK) {
      printf("DUMP read failed\n");
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }
    if (memcmp(now, previous, sizeof(now)) != 0) {
      printf("DUMP %02X %02X %02X %02X %02X %02X\n", now[0], now[1], now[2], now[3], now[4],
             now[5]);
      memcpy(previous, now, sizeof(now));
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  printf("DUMP end\n");
}

bool panelTouch(int16_t *x, int16_t *y) {
  if (s_asleep) {
    return false;
  }
  uint8_t report[5];
  if (i2cReadReg(s_touch, 0x02, report, sizeof(report)) != ESP_OK) {
    return false;
  }
  if (report[0] == 0) {
    return false;
  }
  *x = (int16_t)(((report[1] & 0x0F) << 8) | report[2]);
  *y = (int16_t)(((report[3] & 0x0F) << 8) | report[4]);
  return true;
}

static int cmdTouchdump(int argc, char **argv) {
  panelTouchDump(argc == 2 ? (uint32_t)atoi(argv[1]) : 10000);
  return 0;
}

// The framebuffer is the whole screen state, so streaming it out makes what the
// panel shows machine-checkable: tools/screenshot.py turns this into a PNG.
//
// ponytail: the one thing outside the ui task that touches the framebuffer,
// and it only reads. A dump takes seconds, so locking the ui task out for its
// duration would trip that task's watchdog. What makes it safe is that screens
// are static by design — the ui task redraws only behind builderNeedsRedraw()
// or a state change, and the single animation is over in 150 ms — so a dump
// almost always spans a still frame. A tear would be full-length and so
// invisible to screenshot.py's length check, which catches truncation only.
// Dump from a frame copy if a screen ever animates continuously.
static int cmdScreendump(int argc, char **argv) {
  const size_t chunk = 768;
  static char line[4 * (chunk / 3) + 2];
  const uint8_t *bytes = (const uint8_t *)s_frame;

  printf("SCREENDUMP %d %d\n", PANEL_W, PANEL_H);
  for (size_t offset = 0; offset < FRAME_BYTES; offset += chunk) {
    size_t take = FRAME_BYTES - offset < chunk ? FRAME_BYTES - offset : chunk;
    size_t written = 0;
    mbedtls_base64_encode((unsigned char *)line, sizeof(line), &written, bytes + offset, take);
    printf("%s\n", line);
    // The console loses whole lines on a dump this size, and pacing does not
    // stop it — smaller lines, fsync and yields were all measured and none
    // helped, so the loss is on the host side of the USB link. fsync at least
    // keeps the device's own buffer empty; screenshot.py checks the length it
    // got and asks again, which in practice takes one or two tries.
    fflush(stdout);
    fsync(fileno(stdout));
  }
  printf("SCREENDUMP END\n");
  return 0;
}

static void registerCommands() {
  consoleRegisterCmd("touchdump", "Stream touch register changes for <ms>", cmdTouchdump);
  consoleRegisterCmd("screendump", "Stream the framebuffer as base64", cmdScreendump);
}
