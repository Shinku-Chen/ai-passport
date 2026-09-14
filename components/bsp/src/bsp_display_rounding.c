#include "bsp_display_rounding.h"

bool bsp_display_pixel_outside_rounded_rect(int32_t x, int32_t y,
                                            int32_t width, int32_t height,
                                            int32_t radius)
{
    if (x < 0 || y < 0 || x >= width || y >= height) return true;
    if (radius <= 0 || width <= 0 || height <= 0) return false;

    int32_t max_radius = (width < height ? width : height) / 2;
    if (radius > max_radius) radius = max_radius;

    int32_t dx;
    if (x < radius) {
        dx = radius - x;
    } else if (x >= width - radius) {
        dx = x - (width - 1 - radius);
    } else {
        return false;
    }

    int32_t dy;
    if (y < radius) {
        dy = radius - y;
    } else if (y >= height - radius) {
        dy = y - (height - 1 - radius);
    } else {
        return false;
    }

    return dx * dx + dy * dy > radius * radius;
}
