// main/starry_gfx.h —— 逐行合成用的绘图原语(纯逻辑,不依赖 ESP-IDF)。
//
// 这块板子没有 PSRAM,整屏 240x320 RGB565 就是 150KB,放不下(见
// docs/reference/shinku-chen/release-artifact-verification.md 的教训)。所以画面
// 不在内存里整张拼,而是"逐条带"画:每个函数都作用在一块条带上,屏幕坐标由
// y0 偏移换算,y 超出条带的行直接丢弃。宿主机测试可以直接给它一小块缓冲区。
#pragma once

#include "starry_font.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 一条带缓冲:覆盖屏幕的 [y0, y0+height) 行。
typedef struct {
    uint16_t *pixels;   // 顶行指针
    int stride;         // 每行像素数(>= width)
    int width;
    int height;
    int y0;             // 这块缓冲区对应的屏幕起始行
} starry_surface_t;

static inline uint16_t starry_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((uint16_t)(r & 0xF8u) << 8) | ((uint16_t)(g & 0xFCu) << 3) |
                      ((uint16_t)b >> 3));
}

static inline uint8_t starry_rgb565_r(uint16_t c) { return (uint8_t)(((c >> 11) & 0x1Fu) << 3); }
static inline uint8_t starry_rgb565_g(uint16_t c) { return (uint8_t)(((c >> 5) & 0x3Fu) << 2); }
static inline uint8_t starry_rgb565_b(uint16_t c) { return (uint8_t)((c & 0x1Fu) << 3); }

// 前景 over 背景:alpha 0 = 全背景,255 = 全前景。分量按 5/6/5 分别插值。
uint16_t starry_blend565(uint16_t dst, uint16_t src, uint8_t alpha);

void starry_surface_init(starry_surface_t *s, uint16_t *pixels, int stride, int width, int height,
                         int y0);

// 实心填充 / 半透明覆盖;坐标是屏幕坐标,超出条带的行会被裁掉。
void starry_fill(starry_surface_t *s, int x, int y, int w, int h, uint16_t color);
// 圆角实心块(菜单选中条用):半径自动收敛到 min(w,h)/2。
void starry_fill_rounded(starry_surface_t *s, int x, int y, int w, int h, int radius,
                         uint16_t color);
void starry_blend(starry_surface_t *s, int x, int y, int w, int h, uint16_t color, uint8_t alpha);

// 圆角半透明覆盖(圆角几何与 starry_fill_rounded 完全一致)。
void starry_blend_rounded(starry_surface_t *s, int x, int y, int w, int h, int radius,
                          uint16_t color, uint8_t alpha);

// 圆角遮罩:第 y 行可见区间 [x1, x2](闭区间);不可见返回 false。
// 与 components/bsp/src/bsp_display_rounding.c 同一套几何,
// tests/test_starry_gfx.c 会逐行断言两者完全一致。
bool starry_round_row_span(int y, int width, int height, int radius, int *x1, int *x2);
// 把条带里落在圆角之外的像素刷黑(必须在推屏之前调用)。
void starry_round_mask(starry_surface_t *s, int screen_height, int radius, uint16_t black);

// 单个码位的墨迹宽度与整段文本的像素宽度(按 starry_char_units 的宽度模型)。
int starry_text_px(const char *utf8, size_t len, const starry_font_t *font);

// 绘制一行文本:遇到 '\n'、len 结束或超过 max_px 停止。返回绘制消耗的像素宽度。
int starry_text_line(starry_surface_t *s, int x, int y, const char *utf8, size_t len,
                     const starry_font_t *font, uint16_t color, int max_px);
