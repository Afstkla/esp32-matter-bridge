idf_component_register(
  SRCS "app_main.cpp" "console.cpp" "i2c.cpp" "gfx.cpp" "panel.cpp" "keys.cpp"
  INCLUDE_DIRS "."
  REQUIRES console nvs_flash esp_driver_i2c esp_driver_gpio esp_driver_spi esp_lcd esp_timer mbedtls
)
