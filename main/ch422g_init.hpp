#pragma once
#include "esp_err.h"
#include "driver/i2c_master.h"

esp_err_t ch422g_init(void);
void ch422g_set_backlight(bool on);
i2c_master_bus_handle_t ch422g_get_i2c_bus(void);
