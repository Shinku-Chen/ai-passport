// main/ui_eatwhat_render.c —— 「今天吃啥」候选快速渲染器(局部 + 隔行)实现。
//
// 该模块生产两种可对比的刷新路径，仅供上板 A/B 测试：
//   路A 非隔行: eatwhat_render_draw_frame(interlaced=false) —— 只刷图片矩形(局部)。
//   路B 隔行:   eatwhat_render_draw_frame(interlaced=true) 先推偶行,
//              再 eatwhat_render_finish_odd() 推奇行,两趟各半像素。
//
// 实现要点:
//   - 避免依赖 LVGL 渲染缓冲,直接用 bsp_display_panel() 裸推 ST7789。
//   - 帧数据 = 素材 bin 里的 I8(调色板 B,G,R,A 前置 + RES*RES 像素索引)。
//     逐行解码为 RGB565,再用 esp_lcd_panel_draw_bitmap 推一个 1 行高的矩形。
//   - 隔行 = 一行的矩形(1px 高),故每趟是若干次「1 行」draw_bitmap。
//     ⚠ 每行一次 SPI 命令有固定开销;隔行真正的收益是每趟数据量减半,撕裂感更明显。
//     实际快慢需上板测量。
//   - 字节序:ST7789 SPI 期望大端 RGB565(高字节先)。panel 层不负责 swap
//     (swap 只是 esp_lvgl_port 的 flag),故这里显式按大端写。
#include "ui_eatwhat_render.h"
#include "eat_what_assets.h"

#include <stdint.h>
#include <stdbool.h>
#include "esp_lcd_panel_ops.h"

// 行缓冲:一行 RES 像素的 RGB565(大端)。
static uint8_t s_line_buf[EAT_WHAT_RES * 2];
static esp_lcd_panel_handle_t s_panel;

bool eatwhat_render_init(esp_lcd_panel_handle_t panel)
{
    if (!panel) return false;
    s_panel = panel;
    return true;
}

void eatwhat_render_deinit(void)
{
    s_panel = NULL;
}

// 把 I8 帧的第 row 行解码成 RGB565(大端)存入 s_line_buf。
//   data: 帧开头(I8 调色板 + 索引);row: 0..RES-1。
static void decode_row_to_rgb565(const uint8_t *data, int row)
{
    const uint8_t *pal = data;                    // 256*4 B,G,R,A
    const uint8_t *idx = data + EAT_WHAT_PALETTE_BYTES;
    const uint8_t *row_idx = idx + (size_t)row * EAT_WHAT_RES;

    for (int col = 0; col < EAT_WHAT_RES; col++) {
        uint8_t pi = row_idx[col];
        uint8_t b = pal[4 * pi + 0];
        uint8_t g = pal[4 * pi + 1];
        uint8_t r = pal[4 * pi + 2];
        uint16_t rgb = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        s_line_buf[2 * col]     = (uint8_t)(rgb >> 8);   // 高字节先(大端)
        s_line_buf[2 * col + 1] = (uint8_t)(rgb & 0xFF);
    }
}

// 推单行(y 由 origin.y + row 给出)到 panel。origin 为图片矩形左上角。
static void push_row(const uint8_t *data, eatwhat_rect_origin_t origin, int row)
{
    if (!s_panel) return;
    decode_row_to_rgb565(data, row);
    // 1 行高的矩形 → 只画这一行。x1/x2 用图片左右边界,y 用当前行。
    esp_lcd_panel_draw_bitmap(s_panel, origin.x, origin.y + row,
                              origin.x + EAT_WHAT_RES, origin.y + row + 1,
                              s_line_buf);
}

void eatwhat_render_draw_frame(const uint8_t *data, eatwhat_rect_origin_t origin,
                               bool interlaced)
{
    if (!s_panel) return;
    // 偶行趟(第 0,2,4,... 行)。
    for (int row = 0; row < EAT_WHAT_RES; row += 2) {
        push_row(data, origin, row);
    }
    // 非隔行:补齐剩余(所有行都发);隔行:只发偶行,奇行留给 finish_odd。
    if (!interlaced) {
        for (int row = 1; row < EAT_WHAT_RES; row += 2) {
            push_row(data, origin, row);
        }
    }
}

void eatwhat_render_finish_odd(const uint8_t *data, eatwhat_rect_origin_t origin)
{
    if (!s_panel) return;
    // 奇行趟(第 1,3,5,... 行)。
    for (int row = 1; row < EAT_WHAT_RES; row += 2) {
        push_row(data, origin, row);
    }
}
