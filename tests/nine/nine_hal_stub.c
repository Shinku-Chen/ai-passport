#include "bsp_battery.h"
#include "bsp_display.h"
#include "esp_err.h"

bool bsp_lvgl_lock(int timeout_ms) { (void)timeout_ms; return true; }
void bsp_lvgl_unlock(void) {}
esp_err_t bsp_lvgl_set_landscape(bool landscape) { (void)landscape; return ESP_OK; }
int bsp_battery_soc(void) { return 88; }
