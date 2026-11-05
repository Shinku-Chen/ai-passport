#pragma once
#include <stdbool.h>
static inline bool bsp_lvgl_lock(int timeout) { (void)timeout; return true; }
static inline void bsp_lvgl_unlock(void) {}
