// main/saya_image.h —— 画面区合成:背景 JPEG + 立绘(JPEG + 1bpp 遮罩)-> 一张 RGB565 画布。
//
// 版面固定:横屏 320x240。整幅背景铺满全屏,分成上下两块画布(上 320x150 + 下 320x90,
// 都是 1:1 原尺寸),半透明文本框直接叠在下半块之上 —— 所以整幅画面完整可见,设备端不做
// 任何缩放/裁剪,只需要"解码 + 按遮罩拷贝"。
#pragma once

#include "saya_pack.h"

#include <stdbool.h>
#include <stdint.h>

struct _lv_obj_t;

#define SAYA_ART_W 320
#define SAYA_ART_H 150
// 下半块画面(文本框背后):整幅画面的第 150..239 行,1:1 原尺寸,不平铺不拉伸。
#define SAYA_STRIP_W 320
#define SAYA_STRIP_H 90
// 立绘宽度上限:必须与 tools/saya_pack.py 的 SPRITE_MAX_W 一致(打包时已限宽)。
#define SAYA_SPRITE_MAX_W 180

typedef struct {
    struct _lv_obj_t *canvas;   // LVGL 画布对象(画面区 320x150)
    uint16_t *pixels;           // SAYA_ART_W x SAYA_ART_H RGB565,由调用方提供(static)
    struct _lv_obj_t *strip_canvas;  // 下半块画面画布(文本框背后,1:1)
    uint16_t *strip_pixels;
    uint8_t *sprite_scratch;    // 立绘解码缓冲,由调用方提供(static)
    uint32_t sprite_scratch_size;
    uint32_t last_decode_ms;
} saya_image_t;

// 绑定缓冲区并在 parent 上创建画布对象(需持有 LVGL 锁)。
// pixels 至少 SAYA_ART_W*SAYA_ART_H*2 字节(上半块);sprite_scratch 至少
// SAYA_STRIP_W*SAYA_STRIP_H*2 字节(下半块,同时用作立绘下半段的解码暂存;
// 立绘上半段复用下半块画布缓冲,见 saya_image.c)。
bool saya_image_init(saya_image_t *img, struct _lv_obj_t *parent, uint16_t *pixels,
                     uint8_t *sprite_scratch, uint32_t sprite_scratch_size);
// 显示一个场景:bg_id 必填,fg_id 可以是 SAYA_NONE(只画背景)。
// 失败时画布保持原样并返回 false。
bool saya_image_show(saya_image_t *img, const saya_pack_t *pack, uint16_t bg_id, uint16_t fg_id);

// 只画背景(用于换场景)。
bool saya_image_show_bg(saya_image_t *img, const saya_pack_t *pack, uint16_t bg_id);
