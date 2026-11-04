#include "bsp_display.h"
#include "lvgl.h"
static int s_depth;
bool bsp_lvgl_lock(int timeout_ms) { (void)timeout_ms; s_depth++; lv_stub_set_locked(true); return true; }
void bsp_lvgl_unlock(void) { if (s_depth) s_depth--; lv_stub_set_locked(s_depth > 0); }
void bsp_display_backlight(unsigned char percent) { (void)percent; }
int bsp_stub_lock_depth(void) { return s_depth; }
