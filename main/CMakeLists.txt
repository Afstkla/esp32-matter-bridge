idf_component_register(
  SRCS "app_main.cpp" "console.cpp"
  INCLUDE_DIRS "."
  REQUIRES console nvs_flash
)
