// 宿主桩：单调计时。测试里只关心“有没有调用”，返回固定值即可。
#pragma once

#include <stdint.h>

int64_t esp_timer_get_time(void);
