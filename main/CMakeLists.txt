idf_component_register(
  SRCS "app_main.cpp" "console.cpp" "i2c.cpp" "gfx.cpp" "panel.cpp" "keys.cpp" "bridge.cpp" "net.cpp" "builder.cpp" "internals.cpp" "qrcode.c"
  INCLUDE_DIRS "."
  REQUIRES console nvs_flash esp_driver_gpio esp_driver_i2c esp_driver_spi esp_lcd esp_timer mbedtls
           esp_wifi esp_netif espressif__esp_matter
)

# qrcode.c is vendored verbatim from ricmoo/QRCode, so its warnings are silenced
# here rather than patched out of a file we want to keep diffable upstream.
set_source_files_properties(qrcode.c PROPERTIES
  COMPILE_OPTIONS "-Wno-unknown-pragmas;-Wno-type-limits")
