#include "esp_timer.h"
static int64_t s_us = 1000000;
int64_t esp_timer_get_time(void) { return s_us; }
void esp_timer_stub_set_us(int64_t us) { s_us = us; }
void esp_timer_get_time_set_ms(int64_t ms) { s_us = ms * 1000; }
