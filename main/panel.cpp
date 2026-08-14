#include "panel.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_co5300.h"
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

// EXIO0 = LCD_RESET, EXIO1 = TP_RESET, EXIO2 = DSI_PWR_EN. Neither reset line
// reaches a GPIO on this board, so the panel driver is created without a reset
// pin and relies on this pulse having already run.
static void releaseResets() {
  ESP_ERROR_CHECK(i2cWriteReg(s_tca, 0x03, 0xF8));
  ESP_ERROR_CHECK(i2cWriteReg(s_tca, 0x01, 0x00));
  vTaskDelay(pdMS_TO_TICKS(20));
  ESP_ERROR_CHECK(i2cWriteReg(s_tca, 0x01, 0x07));
  vTaskDelay(pdMS_TO_TICKS(20));
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

  ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
  ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
  ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, CO5300_COL_OFFSET, 0));
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
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

void panelBrightness(uint8_t level) {
  // The component's own setter takes 0-100 and logs at INFO on every call; the
  // register is the 0-255 scale the rest of the firmware works in.
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_io_tx_param(s_io, QSPI_CMD_BRIGHTNESS, &level, 1));
}

bool panelAsleep() {
  return s_asleep;
}

// Deliberately no disp_on_off(false): it issues SLPIN, and the CST816 shares
// this display module's power domain, so sleeping the panel controller kills
// touch and nothing can wake us again. Blanking the framebuffer is the real
// saving anyway — an AMOLED's black pixels are simply unlit.
void panelSleep() {
  if (s_asleep) {
    return;
  }
  gfxFillScreen(COL_BLACK);
  panelFlush();
  panelBrightness(0);
  s_asleep = true;
}

// The caller redraws; the framebuffer was blanked on the way down.
void panelWake() {
  if (!s_asleep) {
    return;
  }
  panelBrightness(PANEL_ON_BRIGHTNESS);
  s_asleep = false;
}

// One draw_bitmap for the whole frame: esp_lcd chunks it internally but keeps
// CS asserted across the chunks, and the CO5300 leaves the panel black if a
// frame arrives as separately addressed writes. Nothing byte-swaps on the way —
// the buffer is already big-endian (see theme.h), which is what the wire wants.
//
// draw_bitmap only queues the transfer, so the wait is not optional: the DMA is
// still reading the framebuffer when it returns, and the caller's next draw
// would race it. The timeout is a backstop so the ui task cannot hang.
void panelFlush() {
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

static int cmdPattern(int argc, char **argv) {
  static const uint16_t bars[] = {COL_ALERT, COL_LEVEL, COL_OK,     COL_BUTTON,
                                  COL_MUTED, COL_WELL,  COL_WHITE};
  const int barCount = sizeof(bars) / sizeof(bars[0]);
  const int barHeight = PANEL_H / 2 / barCount;

  gfxFillScreen(COL_BG);
  for (int i = 0; i < barCount; i++) {
    gfxFillRect(0, i * barHeight, PANEL_W, barHeight, bars[i]);
  }
  gfxText(8, PANEL_H / 2 + 8, "GENIE PATTERN", COL_WHITE, 3);
  gfxText(8, PANEL_H / 2 + 40, "368x448 rgb565 be", COL_FAINT, 2);
  for (int inset = 0; inset < 60; inset += 10) {
    gfxDrawRect(inset + 8, PANEL_H / 2 + 70 + inset, PANEL_W - 16 - 2 * inset,
                PANEL_H / 2 - 78 - 2 * inset, inset % 20 == 0 ? COL_OUTLINE : COL_BUTTON);
  }
  gfxFillRect(0, PANEL_H - 8, PANEL_W, 8, COL_NET);

  int64_t start = esp_timer_get_time();
  panelFlush();
  printf("PATTERN flush %lld us\n", esp_timer_get_time() - start);
  return 0;
}

static int cmdBright(int argc, char **argv) {
  if (argc != 2) {
    printf("usage: bright <0-255>\n");
    return 1;
  }
  int level = atoi(argv[1]);
  uint8_t clamped = (uint8_t)(level < 0 ? 0 : level > 255 ? 255 : level);
  panelBrightness(clamped);
  printf("BRIGHT %d\n", clamped);
  return 0;
}

static int cmdSleep(int argc, char **argv) {
  panelSleep();
  printf("SLEEP\n");
  return 0;
}

static int cmdWake(int argc, char **argv) {
  panelWake();
  printf("WAKE\n");
  return 0;
}

static int cmdTouchdump(int argc, char **argv) {
  panelTouchDump(argc == 2 ? (uint32_t)atoi(argv[1]) : 10000);
  return 0;
}

// The framebuffer is the whole screen state, so streaming it out makes what the
// panel shows machine-checkable: tools/screenshot.py turns this into a PNG.
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
  consoleRegisterCmd("pattern", "Draw a test pattern and flush it", cmdPattern);
  consoleRegisterCmd("bright", "Set panel brightness 0-255", cmdBright);
  consoleRegisterCmd("sleep", "Blank the screen", cmdSleep);
  consoleRegisterCmd("wake", "Unblank the screen", cmdWake);
  consoleRegisterCmd("touchdump", "Stream touch register changes for <ms>", cmdTouchdump);
  consoleRegisterCmd("screendump", "Stream the framebuffer as base64", cmdScreendump);
}
