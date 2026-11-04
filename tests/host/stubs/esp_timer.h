#pragma once
#include <stdint.h>
int64_t esp_timer_get_time(void);
void host_timer_advance_us(int64_t us);
