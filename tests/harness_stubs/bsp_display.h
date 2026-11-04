// tests/harness_stubs/bsp_display.h —— 只为让 nn_app.c 的头文件链完整。
#pragma once
bool bsp_lvgl_lock(int timeout_ms);
void bsp_lvgl_unlock(void);
