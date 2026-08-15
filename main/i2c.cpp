#include "i2c.h"

static const gpio_num_t PIN_SDA = GPIO_NUM_15;
static const gpio_num_t PIN_SCL = GPIO_NUM_14;
static const uint32_t BUS_HZ = 400000;
static const int TIMEOUT_MS = 100;

static i2c_master_bus_handle_t s_bus = nullptr;

void i2cInit() {
  i2c_master_bus_config_t config = {};
  config.i2c_port = I2C_NUM_0;
  config.sda_io_num = PIN_SDA;
  config.scl_io_num = PIN_SCL;
  config.clk_source = I2C_CLK_SRC_DEFAULT;
  config.glitch_ignore_cnt = 7;
  config.flags.enable_internal_pullup = true;
  ESP_ERROR_CHECK(i2c_new_master_bus(&config, &s_bus));
}

i2c_master_bus_handle_t i2cBus() {
  return s_bus;
}

i2c_master_dev_handle_t i2cAddDevice(uint8_t address) {
  i2c_device_config_t config = {};
  config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  config.device_address = address;
  config.scl_speed_hz = BUS_HZ;
  i2c_master_dev_handle_t dev = nullptr;
  ESP_ERROR_CHECK(i2c_master_bus_add_device(s_bus, &config, &dev));
  return dev;
}

esp_err_t i2cWriteReg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value) {
  uint8_t frame[2] = {reg, value};
  return i2c_master_transmit(dev, frame, sizeof(frame), TIMEOUT_MS);
}

esp_err_t i2cReadReg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *out, size_t len) {
  return i2c_master_transmit_receive(dev, &reg, 1, out, len, TIMEOUT_MS);
}
