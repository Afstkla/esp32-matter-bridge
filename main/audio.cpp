#include "audio.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

#include "console.h"
#include "i2c.h"

static const char *TAG = "audio";

static const gpio_num_t PIN_MCLK = GPIO_NUM_16;
static const gpio_num_t PIN_BCLK = GPIO_NUM_9;
static const gpio_num_t PIN_WS = GPIO_NUM_45;
static const gpio_num_t PIN_DOUT = GPIO_NUM_8;
static const gpio_num_t PIN_DIN = GPIO_NUM_10;
static const gpio_num_t PIN_PA = GPIO_NUM_46;

static const uint32_t SAMPLE_RATE = 16000;
static const uint32_t TONE_HZ = 2000;
static const int16_t TONE_AMPLITUDE = 20000;
static const uint32_t BURST_MS = 200;
// Beep, then wait: with the amplifier's warm-up and the drain below, this is a
// beep every two seconds.
static const uint32_t GAP_MS = 1500;
static const int OUT_VOLUME = 80;
// Only the microphone capture uses this, and what it is pointed at is
// centimetres away: at 24 dB a recording of the beep clips flat and the
// analogue front end takes tens of milliseconds to recover, which reads as a
// beep that stopped early. Zero is level enough for the loudest thing this
// device can hear.
static const float MIC_GAIN_DB = 0.0f;

// Measured on this board: the amplifier passes nothing for about 175 ms after
// its enable pin goes high, so a burst written straight after the enable is
// heard as its last 25 ms only. Waking it this far ahead of the tone is what
// makes the whole burst audible.
static const uint32_t PA_SETTLE_MS = 200;

// The codec is a mono part, but esp_codec_dev rejects an odd channel count, so
// both slots are carried and the ES8311 takes the left one.
static const uint8_t SLOTS = 2;

// 2 kHz at 16 kHz is eight samples a period, so a 20 ms chunk is forty whole
// periods and repeats without a step in the waveform.
static const uint32_t CHUNK_MS = 20;
static const size_t CHUNK_FRAMES = SAMPLE_RATE * CHUNK_MS / 1000;

// A write returns once the bytes are in the DMA buffers, not once they have
// been clocked out, so the amplifier has to outlive the last write by the
// length of those buffers or the tail of the burst is cut off.
static const uint32_t DMA_DRAIN_MS = 100;

static int16_t s_tone[CHUNK_FRAMES * SLOTS];

static i2s_chan_handle_t s_tx = nullptr;
static i2s_chan_handle_t s_rx = nullptr;
static esp_codec_dev_handle_t s_codec = nullptr;
static const audio_codec_if_t *s_codecIf = nullptr;
static const audio_codec_ctrl_if_t *s_ctrlIf = nullptr;
static const audio_codec_data_if_t *s_dataIf = nullptr;

static TaskHandle_t s_task = nullptr;
// The audio task alone brings the chain up and down. Either flag holds the
// session open; `micdump` runs on the console task and takes the second one so
// that it can read the microphone while the audio task is playing a burst.
static volatile bool s_beepWanted = false;
static volatile bool s_captureWanted = false;
static volatile bool s_sessionUp = false;

static void buildTone() {
  for (size_t frame = 0; frame < CHUNK_FRAMES; frame++) {
    float phase = 2.0f * (float)M_PI * TONE_HZ * frame / SAMPLE_RATE;
    int16_t sample = (int16_t)(TONE_AMPLITUDE * sinf(phase));
    s_tone[frame * SLOTS] = sample;
    s_tone[frame * SLOTS + 1] = sample;
  }
}

// Hi-Z rather than a driven level: WS is GPIO45 and it picks VDD_SPI's voltage
// at reset, so leaving it driven would let a reboot land on whatever level the
// last frame happened to end on. Released, the board's own resistors decide, as
// they do before this firmware runs at all.
static void releasePin(gpio_num_t pin) {
  gpio_config_t config = {};
  config.pin_bit_mask = 1ULL << pin;
  config.mode = GPIO_MODE_DISABLE;
  gpio_config(&config);
}

static bool sessionUp() {
  gpio_config_t pa = {};
  pa.pin_bit_mask = 1ULL << PIN_PA;
  pa.mode = GPIO_MODE_OUTPUT;
  gpio_config(&pa);
  // PA_CTRL is GPIO46, a strapping pin, and low is both "amplifier off" and the
  // level the board boots with. It is driven high only inside a burst.
  gpio_set_level(PIN_PA, 0);

  i2s_chan_config_t channels = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  channels.auto_clear = true;
  if (i2s_new_channel(&channels, &s_tx, &s_rx) != ESP_OK) {
    ESP_LOGE(TAG, "i2s channels unavailable");
    return false;
  }
  i2s_std_config_t std = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = PIN_MCLK,
              .bclk = PIN_BCLK,
              .ws = PIN_WS,
              .dout = PIN_DOUT,
              .din = PIN_DIN,
              .invert_flags = {},
          },
  };
  ESP_ERROR_CHECK_WITHOUT_ABORT(i2s_channel_init_std_mode(s_tx, &std));
  ESP_ERROR_CHECK_WITHOUT_ABORT(i2s_channel_init_std_mode(s_rx, &std));
  ESP_ERROR_CHECK_WITHOUT_ABORT(i2s_channel_enable(s_tx));
  ESP_ERROR_CHECK_WITHOUT_ABORT(i2s_channel_enable(s_rx));

  audio_codec_i2c_cfg_t i2c = {};
  i2c.port = I2C_NUM_0;
  i2c.addr = ES8311_CODEC_DEFAULT_ADDR;
  i2c.bus_handle = i2cBus();
  s_ctrlIf = audio_codec_new_i2c_ctrl(&i2c);

  audio_codec_i2s_cfg_t i2s = {};
  i2s.port = I2S_NUM_0;
  i2s.rx_handle = s_rx;
  i2s.tx_handle = s_tx;
  s_dataIf = audio_codec_new_i2s_data(&i2s);

  es8311_codec_cfg_t es8311 = {};
  es8311.ctrl_if = s_ctrlIf;
  es8311.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
  // The driver's own PA handling holds the amplifier on for the whole session;
  // this code gates it per burst instead, which is also what makes "no tone
  // with the amplifier off" a control the microphone can be shown.
  es8311.pa_pin = -1;
  es8311.use_mclk = true;
  // Otherwise the right slot of a recording is the DAC's own output, and a
  // capture would show the tone whether or not the speaker ever moved.
  es8311.no_dac_ref = true;
  s_codecIf = es8311_codec_new(&es8311);

  esp_codec_dev_cfg_t device = {};
  device.dev_type = ESP_CODEC_DEV_TYPE_IN_OUT;
  device.codec_if = s_codecIf;
  device.data_if = s_dataIf;
  s_codec = esp_codec_dev_new(&device);
  if (s_codec == nullptr) {
    ESP_LOGE(TAG, "codec create failed");
    return false;
  }

  esp_codec_dev_sample_info_t format = {};
  format.bits_per_sample = 16;
  format.channel = SLOTS;
  format.channel_mask = 0x03;
  format.sample_rate = SAMPLE_RATE;
  if (esp_codec_dev_open(s_codec, &format) != ESP_CODEC_DEV_OK) {
    ESP_LOGE(TAG, "codec open failed");
    return false;
  }
  esp_codec_dev_set_out_vol(s_codec, OUT_VOLUME);
  esp_codec_dev_set_in_gain(s_codec, MIC_GAIN_DB);
  s_sessionUp = true;
  return true;
}

static void sessionDown() {
  s_sessionUp = false;
  gpio_set_level(PIN_PA, 0);
  // Closing the codec disables both i2s channels on the way out, and disabling
  // an already disabled channel is an error the driver logs.
  bool channelsDisabled = false;
  if (s_codec != nullptr) {
    esp_codec_dev_close(s_codec);
    esp_codec_dev_delete(s_codec);
    s_codec = nullptr;
    channelsDisabled = true;
  }
  if (s_codecIf != nullptr) {
    audio_codec_delete_codec_if(s_codecIf);
    s_codecIf = nullptr;
  }
  if (s_dataIf != nullptr) {
    audio_codec_delete_data_if(s_dataIf);
    s_dataIf = nullptr;
  }
  if (s_ctrlIf != nullptr) {
    audio_codec_delete_ctrl_if(s_ctrlIf);
    s_ctrlIf = nullptr;
  }
  i2s_chan_handle_t channels[2] = {s_tx, s_rx};
  for (i2s_chan_handle_t channel : channels) {
    if (channel != nullptr) {
      if (!channelsDisabled) {
        i2s_channel_disable(channel);
      }
      i2s_del_channel(channel);
    }
  }
  s_tx = nullptr;
  s_rx = nullptr;
  gpio_num_t pins[5] = {PIN_MCLK, PIN_BCLK, PIN_WS, PIN_DOUT, PIN_DIN};
  for (gpio_num_t pin : pins) {
    releasePin(pin);
  }
}

static void burst() {
  gpio_set_level(PIN_PA, 1);
  vTaskDelay(pdMS_TO_TICKS(PA_SETTLE_MS));
  for (uint32_t played = 0; played < BURST_MS; played += CHUNK_MS) {
    size_t written = 0;
    esp_err_t err = i2s_channel_write(s_tx, s_tone, sizeof(s_tone), &written, portMAX_DELAY);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "write failed: %s", esp_err_to_name(err));
      break;
    }
  }
  vTaskDelay(pdMS_TO_TICKS(DMA_DRAIN_MS));
  gpio_set_level(PIN_PA, 0);
}

static void audioTask(void *) {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (!s_beepWanted && !s_captureWanted) {
      continue;
    }
    if (!sessionUp()) {
      s_beepWanted = false;
      s_captureWanted = false;
      sessionDown();
      continue;
    }
    printf("BEEP session up\n");
    while (s_beepWanted || s_captureWanted) {
      if (s_beepWanted) {
        burst();
      }
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(s_beepWanted ? GAP_MS : 50));
    }
    sessionDown();
    printf("BEEP session down\n");
  }
}

void audioBeep(bool on) {
  s_beepWanted = on;
  xTaskNotifyGive(s_task);
}

static int cmdBeep(int argc, char **argv) {
  if (argc == 2 && strcasecmp(argv[1], "on") == 0) {
    audioBeep(true);
  } else if (argc == 2 && strcasecmp(argv[1], "off") == 0) {
    audioBeep(false);
  } else if (argc != 1) {
    printf("ERR usage: beep [on|off]\n");
    return 1;
  }
  printf("BEEP %s\n", s_beepWanted ? "on" : "off");
  return 0;
}

// Debug surface, and the only way this repo can hear its own speaker: the
// microphone hangs off the same codec, so a capture taken while the beep runs
// is the machine-checkable proof that the amplifier and the speaker work.
// tools/beeptest.py turns the dump into per-chunk tone energy.
static int cmdMicdump(int argc, char **argv) {
  uint32_t durationMs = argc == 2 ? (uint32_t)strtoul(argv[1], nullptr, 10) : 2500;
  size_t bytes = (size_t)(SAMPLE_RATE * durationMs / 1000) * SLOTS * sizeof(int16_t);
  uint8_t *samples = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  if (samples == nullptr) {
    printf("MICDUMP ERR out of memory\n");
    return 1;
  }

  s_captureWanted = true;
  xTaskNotifyGive(s_task);
  for (int wait = 0; !s_sessionUp && wait < 100; wait++) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  size_t got = 0;
  esp_err_t err = s_sessionUp ? i2s_channel_read(s_rx, samples, bytes, &got,
                                                 pdMS_TO_TICKS(durationMs + 2000))
                              : ESP_ERR_INVALID_STATE;
  s_captureWanted = false;
  xTaskNotifyGive(s_task);
  if (err != ESP_OK) {
    printf("MICDUMP ERR %s\n", esp_err_to_name(err));
    free(samples);
    return 1;
  }

  // Same shape as screendump, plus a byte offset per line: the console drops
  // whole lines on a dump this size, and an offset lets the host skip a hole
  // rather than mistake it for a shift in the audio.
  const size_t chunk = 768;
  static char line[4 * (chunk / 3) + 2];
  printf("MICDUMP %u %u 16 %u\n", (unsigned)SAMPLE_RATE, SLOTS, (unsigned)got);
  for (size_t offset = 0; offset < got; offset += chunk) {
    size_t take = got - offset < chunk ? got - offset : chunk;
    size_t written = 0;
    mbedtls_base64_encode((unsigned char *)line, sizeof(line), &written, samples + offset, take);
    printf("MIC %u %s\n", (unsigned)offset, line);
    fflush(stdout);
    fsync(fileno(stdout));
  }
  printf("MICDUMP END\n");
  free(samples);
  return 0;
}

void audioBegin() {
  buildTone();
  xTaskCreate(audioTask, "audio", 4096, nullptr, 4, &s_task);
  consoleRegisterCmd("beep", "Beep until told to stop: beep [on|off]", cmdBeep);
  consoleRegisterCmd("micdump", "Record <ms> from the microphone and stream it as base64",
                     cmdMicdump);
}
