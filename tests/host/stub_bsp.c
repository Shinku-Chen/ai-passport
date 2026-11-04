// tests/host/stub_bsp.c —— BSP/LVGL 锁与字体符号的宿主替身。
#include "bsp_display.h"
#include "lvgl.h"

const lv_font_t gal_cjk_16;

static bool s_landscape;
esp_err_t bsp_lvgl_set_landscape(bool landscape) { s_landscape = landscape; return 0; }
bool bsp_lvgl_lock(int timeout_ms) { (void)timeout_ms; return true; }
void bsp_lvgl_unlock(void) {}
void bsp_display_backlight(uint8_t percent) { (void)percent; }
