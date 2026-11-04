#include "esp_timer.h"
int64_t esp_timer_stub_now_us = 1000000;
int64_t esp_timer_get_time(void) { return esp_timer_stub_now_us; }
