#pragma once

#include "driver/i2c_master.h"

// One bus for the whole board: TCA9554, CST816 and AXP2101 all hang off it, so
// it has to exist before any of them is touched.
void i2cInit();
i2c_master_dev_handle_t i2cAddDevice(uint8_t address);

esp_err_t i2cWriteReg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value);
esp_err_t i2cReadReg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *out, size_t len);
