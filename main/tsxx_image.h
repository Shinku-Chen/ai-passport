// main/tsxx_image.h —— 美术层:180x240 画布、背景/事件帧解码、立绘遮罩合成。
//
// 版面:资源包里的背景、事件帧、立绘都是 180x240 的"美术层"坐标(见 tsxx_pack.h 的
// TSXX_ART_W/TSXX_ART_H);固件把整层用 LVGL 放大 4/3 铺满 240x320 的屏幕。
// 文字层不在画布上 —— 它由 tsxx_ui 在原生 240x320 上绘制,因此不会被放大糊掉。
//
// 为什么用缩放而不是解到整屏:
//   整屏 RGB565 缓冲要 240x320x2 = 150 KB,本板没有 PSRAM,C3 的静态 RAM 也放不下;
//   美术层只要 180x240x2 = 84 KB,而且所有素材在包里就是按这个尺寸编码的(更小、更快)。
#pragma once

#include "tsxx_pack.h"

#include <stdbool.h>
#include <stdint.h>

// LVGL 9 的缩放单位是 1/256(256 = 原始大小),4/3 ≈ 341。取整误差 (341/256 = 1.332)
// 让放大后的宽度是 239.8 px,四舍五入正好铺满 240。
#define TSXX_ART_SCALE ((256 * TSXX_SCREEN_W + TSXX_ART_W / 2) / TSXX_ART_W)

typedef struct {
    struct _lv_obj_t *canvas;   // 画布对象(直接用 pixels 当缓冲)
    uint16_t *pixels;           // TSXX_ART_W * TSXX_ART_H 个像素,由调用方静态提供
    uint8_t *sprite;            // 立绘/事件补丁的解码暂存区(堆分配)
    uint32_t sprite_size;       // 暂存区字节数;0 = 没分配成功,此时不画立绘
    uint32_t last_decode_ms;    // 最近一次背景解码耗时,仅用于日志
} tsxx_art_t;

// 建画布(需持有 LVGL 锁)。pixels 必须是 TSXX_ART_W * TSXX_ART_H 个像素的缓冲。
bool tsxx_art_init(tsxx_art_t *img, struct _lv_obj_t *parent, uint16_t *pixels);

// 扫描立绘表与事件补丁表,按最大 w*h*2 分配暂存区(并打到日志)。
// 分配失败不算致命:阅读照常,只是不画立绘/补丁。返回是否拿到缓冲。
bool tsxx_art_prepare(tsxx_art_t *img, const tsxx_pack_t *pack);

// 把一屏画满画布:背景 -> 事件图(整帧或 基准帧+补丁) -> 立绘。
// bg 为 TSXX_NONE8 时用主题底色填充;sprite 为 TSXX_NONE8 时不画立绘;
// cg 为 NULL 时没有事件图。返回 false 表示本次没能完整画出来(调用方不要更新缓存)。
bool tsxx_art_show(tsxx_art_t *img, const tsxx_pack_t *pack, uint8_t bg,
                     const tsxx_cg_t *cg, uint8_t sprite);

// 用主题底色填满画布(过场、没有背景的页)。
void tsxx_art_clear(tsxx_art_t *img);
