// main/atri_image.h —— 画面区(240x210 RGB565 画布)、背景 JPEG 与叠加合成。
//
// 版面:画布固定在屏幕顶部,下面 110px 是文本框。背景与叠加都按 tools/atri_pack.py
// 的换算裁到 240x210,所以这里只负责"解码 + 合成":
//   - 背景整图 JPEG 解码进画布(尺寸必须等于画布,解码目标就是画布本身,无额外缓冲);
//   - 叠加是资源包里的无损 RGB565 + 4bpp 遮罩,逐行直接从 Flash 读进画布 ——
//     没有解码、没有临时缓冲,因此在只有几十 KB 堆的 C3 上也不会失败。
#pragma once

#include "atri_pack.h"

#include <stdbool.h>
#include <stdint.h>

// 画面区尺寸,必须与 tools/atri_pack.py 的 ART_W/ART_H 一致。
// 画布铺满整屏;底部的文本框是压在画布上的一条半透明带(见 ATRI_BAND_Y),
// 与源工程一样,立绘可以一直画到屏幕底部。
#define ATRI_ART_W 240
#define ATRI_ART_H 320
// 正文带的顶边与高度(与 main/atri_ui.h 的 ATRI_BOX_Y/ATRI_BOX_H 一致)。
#define ATRI_BAND_Y 210
#define ATRI_BAND_H (ATRI_ART_H - ATRI_BAND_Y)

typedef struct {
    struct _lv_obj_t *canvas;   // 画布对象(直接显示 pixels)
    uint16_t *pixels;           // 画布缓冲,由调用方提供并保证生命周期
    uint32_t last_decode_ms;    // 最近一次背景解码耗时,仅用于日志
} atri_image_t;

// 建画布(需持有 LVGL 锁)。pixels 必须是 ATRI_ART_W * ATRI_ART_H 个像素的缓冲。
bool atri_image_init(atri_image_t *img, struct _lv_obj_t *parent, uint16_t *pixels);

// 画一整屏:先铺背景(ATRI_NONE = 黑屏),再把叠加按 (x, y) 合成上去,
// 最后把正文带(半透明蓝底,自上而下渐深)画进画布 —— 它就是源工程的 text_bg。
// 背景解码失败时画布内容保持上一次的画面。
bool atri_image_show(atri_image_t *img, const atri_pack_t *pack, uint16_t bg, uint16_t ovl,
                     int16_t x, int16_t y);

// 黑屏(章节过场等)。
void atri_image_clear(atri_image_t *img);
