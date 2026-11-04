// main/anemoi/anemoi_canvas.c —— 画布实现（不依赖 ESP-IDF；LVGL 只用描述符类型）。
#include "anemoi_canvas.h"

#include "lvgl.h"

#include <string.h>

// 静态像素缓冲：MAP 里可核的 115,200 B。
static uint16_t s_pixels[ANEM_CANVAS_W * ANEM_CANVAS_H];
static lv_image_dsc_t s_dsc;

bool anem_canvas_init(anem_canvas_t *canvas)
{
    if (canvas == NULL) {
        return false;
    }
    memset(s_pixels, 0, sizeof(s_pixels));
    memset(&s_dsc, 0, sizeof(s_dsc));
    s_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_dsc.header.w = ANEM_CANVAS_W;
    s_dsc.header.h = ANEM_CANVAS_H;
    s_dsc.header.stride = ANEM_CANVAS_W * 2;
    s_dsc.data = (const uint8_t *)s_pixels;
    s_dsc.data_size = sizeof(s_pixels);
    canvas->pixels = s_pixels;
    canvas->image_dsc = &s_dsc;
    canvas->ready = true;
    return true;
}

uint16_t *anem_canvas_pixels(anem_canvas_t *canvas)
{
    return canvas ? canvas->pixels : NULL;
}

const void *anem_canvas_dsc(const anem_canvas_t *canvas)
{
    return canvas ? canvas->image_dsc : NULL;
}

void anem_canvas_fill(anem_canvas_t *canvas, uint16_t color)
{
    if (canvas == NULL || canvas->pixels == NULL) {
        return;
    }
    for (int i = 0; i < ANEM_CANVAS_W * ANEM_CANVAS_H; i++) {
        canvas->pixels[i] = color;
    }
}

void anem_canvas_blit(anem_canvas_t *canvas, const uint16_t *src, int src_stride, int src_w,
                      int src_h, int x, int y)
{
    if (canvas == NULL || canvas->pixels == NULL || src == NULL) {
        return;
    }
    for (int row = 0; row < src_h; row++) {
        const int dy = y + row;
        if (dy < 0 || dy >= ANEM_CANVAS_H) {
            continue;
        }
        int sx0 = 0;
        int dx0 = x;
        int count = src_w;
        if (dx0 < 0) {
            sx0 = -dx0;
            count -= sx0;
            dx0 = 0;
        }
        if (dx0 + count > ANEM_CANVAS_W) {
            count = ANEM_CANVAS_W - dx0;
        }
        if (count <= 0) {
            continue;
        }
        memcpy(&canvas->pixels[dy * ANEM_CANVAS_W + dx0], &src[row * src_stride + sx0],
               (size_t)count * sizeof(uint16_t));
    }
}

void anem_canvas_blend_pixel(anem_canvas_t *canvas, int x, int y, uint16_t color, uint8_t alpha)
{
    if (canvas == NULL || canvas->pixels == NULL || alpha == 0) {
        return;
    }
    if (x < 0 || x >= ANEM_CANVAS_W || y < 0 || y >= ANEM_CANVAS_H) {
        return;
    }
    uint16_t *dst = &canvas->pixels[y * ANEM_CANVAS_W + x];
    if (alpha >= 250) {
        *dst = color;
        return;
    }
    const uint16_t d = *dst;
    const int dr = (d >> 11) & 0x1F, dg = (d >> 5) & 0x3F, db = d & 0x1F;
    const int sr = (color >> 11) & 0x1F, sg = (color >> 5) & 0x3F, sb = color & 0x1F;
    const int r = (sr * alpha + dr * (255 - alpha)) / 255;
    const int g = (sg * alpha + dg * (255 - alpha)) / 255;
    const int b = (sb * alpha + db * (255 - alpha)) / 255;
    *dst = (uint16_t)((r << 11) | (g << 5) | b);
}
