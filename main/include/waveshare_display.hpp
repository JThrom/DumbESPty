#pragma once
#include "esp_err.h"
#include "lvgl.h"
esp_err_t waveshare_display_init(lv_disp_t **disp);
int waveshare_display_width(void);
int waveshare_display_height(void);
