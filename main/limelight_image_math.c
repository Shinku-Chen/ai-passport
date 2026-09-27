// main/limelight_image_math.c —— 画面区纯算术(不依赖 LVGL / esp_jpeg)。
#include "limelight_image_math.h"

int lime_sprite_origin_x(int sprite_w)
{
    // 立绘水平居中:人物站在画面中间,下半身落在对话框后面。
    int x = (LIME_ART_W - sprite_w) / 2;
    if (x < 0) x = 0;
    return x;
}

int lime_sprite_origin_y(int sprite_h)
{
    // 默认:底边贴**屏幕**底,人物站在屏幕最下方,下摆落在对话框后面(与 ATRI 一致)。
    int y = LIME_SCREEN_H - sprite_h - LIME_SPRITE_EDGE_MARGIN;
    if (y < 0) return 0;                                  // 比屏幕还高:顶到上沿
    const int limit = LIME_ART_H - LIME_SPRITE_MIN_VISIBLE;
    if (y > limit) y = limit;                             // 矮立绘:别整个躲进对话框
    return y;
}

int lime_sprite_rows_in_canvas(int sprite_h)
{
    const int y = lime_sprite_origin_y(sprite_h);
    int rows = LIME_ART_H - y;                 // 画布里能放下的行
    if (rows > sprite_h) rows = sprite_h;
    if (rows < 0) rows = 0;
    return rows;
}

int lime_sprite_rows_below_canvas(int sprite_h)
{
    return sprite_h - lime_sprite_rows_in_canvas(sprite_h);
}

uint32_t lime_sprite_blit(uint16_t *canvas, int canvas_w, int canvas_h,
                          const uint8_t *rgb565, int sprite_w, int sprite_h,
                          const uint8_t *mask, int x, int y)
{
    if (!canvas || !rgb565 || !mask) return 0;
    if (sprite_w <= 0 || sprite_h <= 0 || canvas_w <= 0 || canvas_h <= 0) return 0;

    uint32_t written = 0;
    const int mask_stride = (sprite_w + 7) / 8;
    for (int sy = 0; sy < sprite_h; sy++) {
        const int dy = y + sy;
        if (dy < 0) continue;
        if (dy >= canvas_h) break;
        uint16_t *dst_row = canvas + (size_t)dy * canvas_w;
        const uint8_t *mask_row = mask + (size_t)sy * mask_stride;
        const uint8_t *src_row = rgb565 + (size_t)sy * sprite_w * 2u;
        for (int sx = 0; sx < sprite_w; sx++) {
            const int dx = x + sx;
            if (dx < 0) continue;
            if (dx >= canvas_w) break;
            const uint8_t packed = mask_row[sx >> 3];
            if (!((packed >> (7 - (sx & 7))) & 1u)) continue;   // 透明
            const uint16_t src = (uint16_t)(src_row[sx * 2] | ((uint16_t)src_row[sx * 2 + 1] << 8));
            dst_row[dx] = src;
            written++;
        }
    }
    return written;
}

uint32_t lime_sprite_buffer_size(int max_w, int max_h)
{
    if (max_w <= 0 || max_h <= 0) return 0;
    return (uint32_t)max_w * (uint32_t)max_h * 2u;
}
