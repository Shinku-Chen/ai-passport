// main/eden_image.h —— 画面区合成层(横屏 320x240)。
//
// 版面(与 main/eden_ui.h 保持一致):
//   画面区  x 28..291, y 0..164 :264x165 原生画面,1:1 显示,不缩放
//   左右留边 28px               :黑底(源作画幅是 16:10,本屏是 4:3)
//   正文带  y 165..239          :半透明压住画面下沿
//
// 解码路径只有一条"低内存"链路,不做整帧 RGBA、不做整屏 canvas、不缓存多帧:
//   底图   JPEG(ROM TJpgDec)→ 直接解进画面缓冲(输出缓冲就是画布,没有中转)
//   补丁   JPEG → 解进一条窄暂存(104x168,约 35 KB),再逐行贴进画面
// 画面缓冲 264x168 是 8 对齐的: TJpgDec 按 MCU 出块,存储尺寸对齐后解码器不会越界;
// 多出来的 3 行落在正文带下面,看不见。
#pragma once

#include "eden_pack.h"

#include <stdbool.h>
#include <stdint.h>

struct _lv_obj_t;

// 画面内容尺寸(源作画幅 16:10)。
#define EDEN_FRAME_W 264
#define EDEN_FRAME_H 165
// 画面缓冲尺寸(高度向上 8 对齐,供 TJpgDec 整 MCU 写入)。
#define EDEN_CANVAS_W 264
#define EDEN_CANVAS_H 168
// 画面在屏幕上的位置:水平居中,顶到屏幕最上面。
#define EDEN_CANVAS_X 28
#define EDEN_CANVAS_Y 0
// CG 全屏分带：真机最大连续块只有 ~47 KB，320x240=153,600 / 320x120=76,800 都拿不到。
// 用 320x60 = 38,400 B（47,104 的 81%，留 19% 余量）分 4 带画满 0..239。
#define EDEN_CG_BAND_W 320
#define EDEN_CG_BAND_H 60
#define EDEN_CG_BANDS 4

// 补丁暂存条:打包器把宽补丁按 104 列切开,单条补丁不会比它更宽。
#define EDEN_PATCH_W 104
#define EDEN_PATCH_H EDEN_CANVAS_H

typedef struct {
    struct _lv_obj_t *canvas;   // LVGL 画布(直接显示 canvas_pixels)
    uint16_t *pixels;           // 画面缓冲,由调用方提供并保证生命周期
    uint16_t *patch_pixels;     // 补丁暂存条,同上
    const eden_pack_t *pack;
} eden_image_t;

// 建画布(需持有 LVGL 锁)。
bool eden_image_init(eden_image_t *image, struct _lv_obj_t *parent,
                     uint16_t *canvas_pixels, uint16_t *patch_pixels);

void eden_image_set_pack(eden_image_t *image, const eden_pack_t *pack);

// 把画面清成黑色(左右留边与无画面时的底色)。
void eden_image_clear(eden_image_t *image);

// 画一帧:底图 + 全部补丁。失败时返回 false 并把画面清黑,便于在日志里定位坏包。
bool eden_image_draw(eden_image_t *image, uint16_t frame_index);

// 把已解码到 image->pixels 的画面按 cover 铺到 320x240，只输出第 band 条带
// （band*EDEN_CG_BAND_H 起 60 行）到 band_pixels。最近邻，无额外大缓冲。
bool eden_image_cg_band(eden_image_t *image, uint16_t *band_pixels, int band);

// 画整屏底色(鉴赏/结局等不需要画面的页面复用同一块缓冲,省一次分配)。
void eden_image_fill(eden_image_t *image, uint16_t color565);
