#pragma once
#include <stdint.h>
int64_t esp_timer_get_time(void);
void esp_timer_stub_set_us(int64_t us);
void esp_timer_get_time_set_ms(int64_t ms);
