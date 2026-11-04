#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
void bsp_display_backlight(uint8_t percent);
bool bsp_lvgl_lock(int timeout_ms);
void bsp_lvgl_unlock(void);
esp_err_t bsp_lvgl_set_landscape(bool landscape);
