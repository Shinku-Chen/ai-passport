// main/atri_image.h —— 画面区(240x320 RGB565 画布)、背景 JPEG 与立绘合成。
//
// 本文件从 ATRI 阅读器移植(main/atri_image.c),资源来源接到《星空列车与白的旅行》的
// 资源包上(背景 = 整屏 JPEG,立绘 = 无损 RGB565 + 4bpp 遮罩,没有叠加段)。
//
// 版面:画布铺满整屏,正文带压在画面下沿(与 main/atri_ui.h 的 ATRI_BOX_Y 一致)。
//   - 背景整图 JPEG 解码进画布(尺寸必须等于画布,解码目标就是画布本身,无额外缓冲);
//   - 立绘是无损 RGB565 + 4bpp 遮罩,逐行直接从 Flash 读进画布 —— 没有解码、没有
//     临时缓冲,因此运行时不再需要 84KB 的立绘缓冲。
#pragma once

#include "starry_pack.h"

#include <stdbool.h>
#include <stdint.h>

// 画面区尺寸,必须与 tools/starry_pack.py 的 SCREEN_W/SCREEN_H 一致。
// 画布铺满整屏;底部的文本框是压在画布上的一条半透明带(见 ATRI_BAND_Y),
// 与源工程一样,立绘可以一直画到屏幕底部。
#define ATRI_ART_W 240
#define ATRI_ART_H 320
// 正文带的顶边与高度(与 main/atri_ui.h 的 ATRI_BOX_Y/ATRI_BOX_H 一致)。
#define ATRI_BAND_Y 210
#define ATRI_BAND_H (ATRI_ART_H - ATRI_BAND_Y)
// 立绘上限:必须与 tools/starry_pack.py 的 SPRITE_MAX_W / SPRITE_MAX_H 一致。
// 打包器把立绘裁到不透明边界后等比缩放进这个框,贴右边、底边贴屏幕底;
// 改这里必须同步改打包器(tests/test_starry_pack.py 会核对位置约束)。
#define STARRY_SPRITE_MAX_W 168
#define STARRY_SPRITE_H 252

typedef struct {
    struct _lv_obj_t *canvas;   // 画布对象(直接显示 pixels)
    uint16_t *pixels;           // 画布缓冲,由调用方提供并保证生命周期
    uint32_t last_decode_ms;    // 最近一次背景解码耗时,仅用于日志
} atri_image_t;

// 建画布(需持有 LVGL 锁)。pixels 必须是 ATRI_ART_W * ATRI_ART_H 个像素的缓冲。
bool atri_image_init(atri_image_t *img, struct _lv_obj_t *parent, uint16_t *pixels);

// 画一整屏:背景(STARRY_NONE = 黑屏)-> 正文带(半透明蓝底,源工程的 text_bg)
// -> 角色立绘 -> 文字区浅暗帘。立绘压在带子之上,所以不会被"对话框挡住";
// 浅暗帘只盖在文字那几行上,保证白字清晰。
// chr 为 STARRY_FG_KEEP 时不画立绘。背景解码失败时画布内容保持上一次的画面。
bool atri_image_show(atri_image_t *img, const starry_pack_t *pack, uint16_t bg, uint16_t chr);

// 黑屏(章节过场等)。
void atri_image_clear(atri_image_t *img);
