// main/ui_eatwhat_render.h —— 「今天吃啥」的候选快速渲染器(局部 + 隔行)。
//
// 目的:对比测试用。LVGL 的 PARTIAL 局部刷新已只刷图片,但经过 LVGL 渲染缓冲
// 再到 SPI,仍有排队延迟。本模块直接拿 bsp_display_panel(),用
// esp_lcd_panel_draw_bitmap 只把【图片矩形】裸推到屏,并支持「隔行」趟:
// 一帧分偶行/奇行两趟,每趟只有一半行 → 每趟 SPI 传输时间减半,刷新感知更快,
// 代价是两趟之间出现半帧撕裂(这正是用户接受的权衡)。
//
// 约定:
//   - 只负责图片区域(固定在屏幕居中,尺寸 EAT_WHAT_RES,原点由调用方给出)。
//   - 必须在持有 bsp_lvgl_lock() 时调用(它直接碰 panel,须与 LVGL 串行)。
//   - 不参与 LVGL 对象管理;图片区在 LVGL 界面里留透明占位即可,像素由本模块直画。
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_lcd_types.h"

// 图片矩形在屏幕上的位置(左上角)。调用方与 LVGL 界面布局保持一致。
typedef struct {
    int x;
    int y;
} eatwhat_rect_origin_t;

// 初始化快速渲染器:拿到 panel 句柄并准备一张行缓冲。
//   panel 由 bsp_display_panel() 提供;buffer 用于以 RGB565 行暂存,大小 = 一行。
// 返回 false 表示 panel 不可用(如显示未初始化)。
bool eatwhat_render_init(esp_lcd_panel_handle_t panel);

// 在一个持有 bsp_lvgl_lock() 的上下文里,把指定帧【只刷图片矩形】推到屏。
//   anim_data 指向该帧的 I8 数据(调色板 + 像素索引),origin 是图片左上角,
//   interlaced=true 时只发该帧的【偶数行】(隔行第一趟);配合 eatwhat_render_finish
//   完成整帧。false 时整帧一次发完(仍是局部,只刷图片矩形)。
void eatwhat_render_draw_frame(const uint8_t *anim_data, eatwhat_rect_origin_t origin,
                               bool interlaced);

// 隔行模式第二趟:发【奇数行】,补全上一趟未刷的行,完成整帧。
//   interlaced 趟的上一次调用后,再调本函数补全。非隔行模式无需调用。
void eatwhat_render_finish_odd(const uint8_t *anim_data, eatwhat_rect_origin_t origin);

// 释放行缓冲(退出页面时调用)。之后不能再 draw。
void eatwhat_render_deinit(void);
