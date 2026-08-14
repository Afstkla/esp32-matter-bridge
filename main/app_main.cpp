#include "esp_log.h"
#include "nvs_flash.h"

#include "console.h"
#include "panel.h"

static const char *TAG = "genie";

static void initNvs() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
}

extern "C" void app_main(void) {
  initNvs();
  if (!panelInit()) {
    ESP_LOGE(TAG, "panel init failed");
  }
  consoleStart();
  ESP_LOGI(TAG, "genie booted");
}
