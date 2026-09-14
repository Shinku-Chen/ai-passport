#pragma once

#include <stdbool.h>
#include <stdint.h>

// Pure geometry helper kept independent from ESP-IDF/LVGL for host tests.
bool bsp_display_pixel_outside_rounded_rect(int32_t x, int32_t y,
                                            int32_t width, int32_t height,
                                            int32_t radius);
