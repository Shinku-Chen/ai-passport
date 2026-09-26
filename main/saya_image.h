// main/saya_image.h —— 画面区合成:背景 JPEG + 立绘(JPEG + 1bpp 遮罩)-> 一张 RGB565 画布。
//
// 版面固定:横屏 320x240,上方 320x150 是画面区(左下角叠说话人名字),下方 82px 文本框。背景在打包
// 时就裁成 320x150,立绘只保留落在画面区里的那部分,所以设备端不做任何缩放/裁剪,
// 只需要"解码 + 按遮罩拷贝"。
#pragma once

#include "saya_pack.h"

#include <stdbool.h>
#include <stdint.h>

struct _lv_obj_t;

#define SAYA_ART_W 320
#define SAYA_ART_H 150
// 立绘宽度上限:必须与 tools/saya_pack.py 的 SPRITE_MAX_W 一致(打包时已限宽)。
#define SAYA_SPRITE_MAX_W 180

typedef struct {
    struct _lv_obj_t *canvas;   // LVGL 画布对象
    uint16_t *pixels;           // 320x136 RGB565,由调用方提供(static)
    uint8_t *sprite_scratch;    // 立绘解码缓冲,由调用方提供(static)
    uint32_t sprite_scratch_size;
    uint32_t last_decode_ms;
} saya_image_t;

// 绑定缓冲区并在 parent 上创建画布对象(需持有 LVGL 锁)。
// pixels 至少 SAYA_ART_W*SAYA_ART_H*2 字节;sprite_scratch 至少
// SAYA_ART_W*SAYA_ART_H*2 字节(立绘宽度上限 180,这里给足)。
bool saya_image_init(saya_image_t *img, struct _lv_obj_t *parent, uint16_t *pixels,
                     uint8_t *sprite_scratch, uint32_t sprite_scratch_size);
// 显示一个场景:bg_id 必填,fg_id 可以是 SAYA_NONE(只画背景)。
// 失败时画布保持原样并返回 false。
bool saya_image_show(saya_image_t *img, const saya_pack_t *pack, uint16_t bg_id, uint16_t fg_id);

// 只画背景(用于换场景)。
bool saya_image_show_bg(saya_image_t *img, const saya_pack_t *pack, uint16_t bg_id);
