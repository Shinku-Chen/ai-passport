#include "bsp_display.h"
#include "bsp_battery.h"
bool bsp_lvgl_lock(int timeout_ms) { (void)timeout_ms; return true; }
void bsp_lvgl_unlock(void) {}
int bsp_battery_soc(void) { return 77; }
