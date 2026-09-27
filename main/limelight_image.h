// main/limelight_image.h —— 画面区:背景 JPEG + 立绘(JPEG + 1bpp 遮罩)合成。
//
// 版面(竖屏 240x320):
//   y   0..213   画面区 = 一张 240x214 的 RGB565 画布(背景/CG + 立绘)
//   y 210..319   正文带:由 LVGL 画的半透明渐变蓝底 + 文字(见 limelight_ui.c)
//
// 合成顺序:背景 -> 立绘(立绘压在带子"下面",与源移植版一致:人物下半身被带子盖住)。
//
// 缓冲策略(没有 PSRAM,DRAM 只有 313.8 KiB):
//   - 画布 240x214 = 100.3 KiB,背景/CG 的 JPEG 直接解进画布(紧密排列,正好对得上)。
//   - 立绘先解到调用方提供的临时缓冲(≤23,000 像素 = 45 KiB),再按遮罩合成进画布;
//     遮罩解出来的 1bpp 位图只需要 ≤3 KiB。
#pragma once

#include "limelight_assets.h"
#include "limelight_image_math.h"

#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>

// 一个都不画 / 没有背景。
#define LIME_ASSET_NONE (-1)

typedef struct {
    struct _lv_obj_t *canvas;    // 直接显示 pixels 的 LVGL 画布对象
    uint16_t *pixels;            // 画布缓冲,由调用方提供并保证生命周期
    uint8_t *sprite_buffer;      // 立绘解码临时缓冲,由调用方提供
    uint32_t sprite_capacity;
    uint8_t *mask_buffer;        // 遮罩解出来的 1bpp 位图缓冲(≤3 KiB),由调用方提供
    uint32_t mask_capacity;
    uint32_t last_decode_ms;     // 最近一次图片解码耗时,仅用于日志

    // 立绘下摆(屏幕 y>=214 的那几行):画布只有 214 行,这部分改用 LVGL 图像对象画,
    // 数据直接指向立绘解码缓冲里的中间位置(自定义 stride),所以不额外占 RAM;
    // 绘制顺序是 画布 -> 这个对象 -> 正文带,于是下摆被对话框盖住。
    struct _lv_obj_t *sprite_lower;
    lv_image_dsc_t sprite_lower_dsc;
} lime_image_t;

// 建画布(需持有 LVGL 锁)。pixels 必须是 LIME_SCREEN_W*LIME_SCREEN_H 个像素;
// sprite_buffer 由调用方静态提供,容量需 ≥ lime_image_required_sprite_bytes()。
bool lime_image_init(lime_image_t *img, struct _lv_obj_t *parent, uint16_t *pixels,
                     uint8_t *sprite_buffer, uint32_t sprite_capacity,
                     uint8_t *mask_buffer, uint32_t mask_capacity);

// 画一整屏:背景(assets 里的 JPEG,<0 = 黑屏)-> 立绘(<0 = 不画)-> 正文带 -> 暗帘。
// 解码失败时保留上一次的画面并返回 false。
bool lime_image_show(lime_image_t *img, const lime_assets_t *assets, int bg_index,
                     int sprite_index);

// 只铺背景(标题页/过场用),不画立绘与正文带。
bool lime_image_show_backdrop(lime_image_t *img, const lime_assets_t *assets, int bg_index);

// 黑屏。
void lime_image_clear(lime_image_t *img);

// 按素材包算立绘解码缓冲与遮罩缓冲需要多大(调用方按返回值申请静态缓冲)。
uint32_t lime_image_required_sprite_bytes(const lime_assets_t *assets);
uint32_t lime_image_required_mask_bytes(const lime_assets_t *assets);
