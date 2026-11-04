#pragma once
#include <stdint.h>
typedef int esp_err_t;
#include <stdbool.h>
#define ESP_OK 0
esp_err_t bsp_lvgl_set_landscape(bool landscape);
bool bsp_lvgl_lock(int timeout_ms);
void bsp_lvgl_unlock(void);
void bsp_display_backlight(uint8_t percent);
