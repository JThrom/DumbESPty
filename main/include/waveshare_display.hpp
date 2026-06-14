#pragma once
#include "esp_err.h"
#include "lvgl.h"
esp_err_t waveshare_display_init(lv_disp_t **disp);
int waveshare_display_width(void);
int waveshare_display_height(void);
bool waveshare_display_brightness_supported(void);
int waveshare_display_get_brightness(void);
esp_err_t waveshare_display_set_brightness(int percent);
