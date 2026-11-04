// tests/esp_stubs/esp_timer.h —— 只提供 gal_image.c 用到的计时接口。
#pragma once

#include <stdint.h>

int64_t esp_timer_get_time(void);
