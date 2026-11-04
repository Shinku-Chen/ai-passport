// main/stella_image.h —— 横屏 320x240 画面合成：背景/标题/CG JPEG + 立绘 JPEG+1bpp 遮罩。
//
// 解码走 esp_jpeg 组件自带的 TJpgDec 回调接口：每解出一块 MCU 就直接写进
// 320x240 RGB565 画布，不需要整张图的解码输出缓冲，也不需要第二块画布。
#pragma once

#include "stella_pack.h"

#include <stdbool.h>
#include <stdint.h>

struct _lv_obj_t;

#define STELLA_SCREEN_W 320
#define STELLA_SCREEN_H 240
#define STELLA_BG_W 320
#define STELLA_BG_H 180
#define STELLA_PANEL_H 90
#define STELLA_PANEL_Y (STELLA_SCREEN_H - STELLA_PANEL_H)

#define STELLA_IMAGE_THEME_TOP 0x08C2u
#define STELLA_IMAGE_THEME_BOTTOM 0x11C6u

typedef struct {
    struct _lv_obj_t *canvas;
    uint16_t *pixels;
} stella_image_t;

bool stella_image_init(stella_image_t *img, struct _lv_obj_t *parent, uint16_t *pixels);

// 清成主题深色渐变（没有背景时使用）。
void stella_image_clear(stella_image_t *img, uint16_t top, uint16_t bottom);

// 场景：背景铺在 0..179，立绘按 alpha 锚点缩放后叠上去（最多 3 个）。
bool stella_image_show_scene(stella_image_t *img, const stella_pack_t *pack,
                             uint16_t bg, const uint16_t bodies[3], uint8_t body_count);

// 标题图铺满 320x240。
bool stella_image_show_title(stella_image_t *img, const stella_pack_t *pack, uint16_t asset);

// CG 鉴赏：320x180 居中（上下各 30 像素黑边），完整显示不裁切。
bool stella_image_show_cg(stella_image_t *img, const stella_pack_t *pack, uint16_t asset);

// 不写画布的完整性/JPEG 预检：用于读档前拒绝坏图，避免先破坏当前进度。
bool stella_image_validate_asset(const stella_pack_t *pack, uint16_t asset);
bool stella_image_validate_scene(const stella_pack_t *pack, uint16_t bg,
                                 const uint16_t bodies[3], uint8_t body_count);

// 立绘缩放/锚点盒子；公开给宿主几何回归，不写画布。
void stella_image_portrait_box(uint8_t count, uint8_t index, const stella_asset_t *asset,
                               int *dx, int *dy, int *dw, int *dh);
