// tests/app_stubs/bsp_display.h —— LVGL 互斥锁桩。
#pragma once

#include <stdbool.h>

bool bsp_lvgl_lock(int timeout_ms);
void bsp_lvgl_unlock(void);
