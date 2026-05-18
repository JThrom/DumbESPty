#pragma once
#include "esp_err.h"

esp_err_t touch_init(void);
void touch_read_task(void *arg);
