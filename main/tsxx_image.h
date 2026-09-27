// main/tsxx_image.h —— 美术层:满屏画布、背景/事件图解码、立绘遮罩合成。
//
// 版面:画布就是 TSXX_ART_W x TSXX_ART_H = 240x320,与屏幕 1:1 铺满,不再缩放 ——
// 立绘按这个尺寸存,显示时不会被重采样糊掉。背景与事件图在包里仍按更小的尺寸存
// (META 的 bg= / event=),合成时按最近邻放大到画布(见 tsxx_image.c)。
// 文字层不在画布上 —— 它由 tsxx_ui 直接在原生 240x320 上绘制。
//
// 画布要 240x320x2 = 150 KB 静态 RAM,但板上没有 PSRAM,所以解码时不再像以前那样
// 为整张立绘/补丁再开一块同样大的暂存区:全部走 tjpgd 的回调接口逐块合成。
#pragma once

#include "tsxx_pack.h"

#include <stdbool.h>
#include <stdint.h>

// LVGL 9 的缩放单位是 1/256:256 = 原始大小。画布已经与屏幕同尺寸,所以这里是 1:1,
// 不再对画布调用 lv_image_set_scale()。
#define TSXX_ART_SCALE 256

typedef struct {
    struct _lv_obj_t *canvas;   // 画布对象(直接用 pixels 当缓冲)
    uint16_t *pixels;           // TSXX_ART_W * TSXX_ART_H 个像素,由调用方静态提供
    uint32_t last_decode_ms;    // 最近一次一屏合成的耗时,仅用于日志
} tsxx_art_t;

// 建画布(需持有 LVGL 锁)。pixels 必须是 TSXX_ART_W * TSXX_ART_H 个像素的缓冲。
bool tsxx_art_init(tsxx_art_t *img, struct _lv_obj_t *parent, uint16_t *pixels);

// 把一屏画满画布:背景 -> 事件图(整帧或 基准帧+补丁) -> 立绘。
// bg 为 TSXX_NONE8 时用主题底色填充;sprite 为 TSXX_NONE8 时不画立绘;
// cg 为 NULL 时没有事件图。返回 false 表示本次没能完整画出来(调用方不要更新缓存)。
// 整张 JPEG 一次解码完才会返回,所以这里也是"拿到图就立刻解码"的窗口契约履行点。
bool tsxx_art_show(tsxx_art_t *img, const tsxx_pack_t *pack, uint8_t bg,
                     const tsxx_cg_t *cg, uint8_t sprite);

// 用主题底色填满画布(过场、没有背景的页)。
void tsxx_art_clear(tsxx_art_t *img);
