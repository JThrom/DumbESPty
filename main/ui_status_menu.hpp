#pragma once

#include "esp_err.h"
#include "lvgl.h"

esp_err_t ui_status_menu_init(lv_obj_t *parent);
void ui_status_menu_update(void);
