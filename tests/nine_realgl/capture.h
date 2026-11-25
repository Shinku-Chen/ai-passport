// Minimal host display capture for real-LVGL regression/screenshot binaries.
// The framebuffer is the full 320x240 RGB565 target layout; the flush callback
// copies submitted areas into it. This is a host render, not a device capture.
#pragma once
#include "lvgl.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CAPTURE_W 320
#define CAPTURE_H 240
static uint16_t capture_pixels[CAPTURE_W * CAPTURE_H];
static uint32_t capture_draw[CAPTURE_W * CAPTURE_H / 2];
static lv_display_t *capture_display;

static void capture_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    if (area && px_map) {
        const int32_t w = lv_area_get_width(area);
        const size_t row_bytes = (size_t)w * 2u;
        for (int32_t y = 0; y < lv_area_get_height(area); ++y) {
            const int32_t dy = area->y1 + y;
            if (dy < 0 || dy >= CAPTURE_H) continue;
            const int32_t dx = area->x1;
            for (int32_t x = 0; x < w; ++x) {
                const int32_t sx = dx + x;
                if (sx >= 0 && sx < CAPTURE_W)
                    capture_pixels[dy * CAPTURE_W + sx] =
                        ((const uint16_t *)(px_map + (size_t)y * row_bytes))[x];
            }
        }
    }
    lv_display_flush_ready(disp);
}

static lv_display_t *capture_init(void)
{
    lv_init();
    capture_display = lv_display_create(CAPTURE_W, CAPTURE_H);
    if (!capture_display) return NULL;
    lv_display_set_color_format(capture_display, LV_COLOR_FORMAT_RGB565);
    memset(capture_pixels, 0, sizeof(capture_pixels));
    memset(capture_draw, 0, sizeof(capture_draw));
    lv_display_set_buffers(capture_display, capture_draw, NULL, sizeof(capture_draw),
                           LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(capture_display, capture_flush);
    return capture_display;
}

static void capture_render(void)
{
    if (!capture_display) return;
    lv_obj_update_layout(lv_screen_active());
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(capture_display);
}

static uint16_t __attribute__((unused)) capture_pixel(int x, int y)
{
    if (x < 0 || x >= CAPTURE_W || y < 0 || y >= CAPTURE_H) return 0;
    return capture_pixels[y * CAPTURE_W + x];
}

static int __attribute__((unused)) capture_write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fprintf(f, "P6\n%d %d\n255\n", CAPTURE_W, CAPTURE_H);
    for (int y = 0; y < CAPTURE_H; ++y) {
        for (int x = 0; x < CAPTURE_W; ++x) {
            const uint16_t p = capture_pixels[y * CAPTURE_W + x];
            const unsigned r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
            const unsigned char rgb[3] = {
                (unsigned char)((r << 3) | (r >> 2)),
                (unsigned char)((g << 2) | (g >> 4)),
                (unsigned char)((b << 3) | (b >> 2)),
            };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    return 1;
}
