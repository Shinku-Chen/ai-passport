#pragma once
#include <stdbool.h>
bool bsp_lvgl_lock(int timeout_ms);
void bsp_lvgl_unlock(void);
