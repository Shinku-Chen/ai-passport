#pragma once
#include "esp_err.h"
#include <stdbool.h>
bool bsp_lvgl_lock(int timeout_ms);
void bsp_lvgl_unlock(void);
esp_err_t bsp_lvgl_set_landscape(bool landscape);
