// Minimal BSP/font stubs for tests that link the real image/audio modules.
#include "bsp_battery.h"
#include "bsp_display.h"
#include "lvgl.h"

const lv_font_t nine_cjk_16 = { 0 };
const lv_font_t nine_cjk_20 = { 0 };

bool bsp_lvgl_lock(int timeout_ms) { (void)timeout_ms; return true; }
void bsp_lvgl_unlock(void) {}
int bsp_battery_soc(void) { return 88; }
