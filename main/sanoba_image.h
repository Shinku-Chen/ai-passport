// main/sanoba_image.h —— 画面区(240x320 RGB565 画布)合成层。
//
// 本文件从 ATRI 阅读器移植(main/sanoba_image.c),资源来源接到《千恋＊万花》的图片包。
// 版面与源工程一致:画面铺满整屏,正文带压在画面下沿,立绘一直画到屏幕底部。
//
// 图层顺序(照源工程 .ux 的 z-index):背景 -> SD 装饰 -> 立绘 -> 事件 CG -> 正文带。
// 事件 CG 是不透明整屏画面,盖住背景与立绘;正文带与浅暗帘永远在最上面。
//
// 各资源的格式与解码方式:
//   背景 / 事件 CG   JPEG 240x320 -> 直接解进画布(解码目标就是画布,无额外缓冲)
//   立绘            调色板 PNG,底部对齐、水平居中 -> 需要一个 32 KB inflate 窗口 + 一行缓冲
//   SD 装饰         调色板 PNG 240x144,固定在 (SANOBA_SD_X, SANOBA_SD_Y)
//   事件 CG 补丁    先画基准 JPEG,再按掩码把 RGB565 像素覆写上去(一行缓冲)
#pragma once

#include "sanoba_pack.h"

#include <stdbool.h>
#include <stdint.h>

// 画面区尺寸,必须与 tools/sanoba_pack.py 的 --screen-w/--screen-h 一致。
#define SANOBA_ART_W 240
#define SANOBA_ART_H 320
// 正文带的顶边与高度(与 main/sanoba_ui.h 的 SANOBA_BOX_Y/SANOBA_BOX_H 一致)。
#define SANOBA_BAND_Y 210
#define SANOBA_BAND_H (SANOBA_ART_H - SANOBA_BAND_Y)

typedef struct {
    struct _lv_obj_t *canvas;   // LVGL 画布对象(直接显示 pixels)
    uint16_t *pixels;           // 画布缓冲,由调用方提供并保证生命周期
    const sanoba_pack_t *pack;  // 资源包(只读)
    uint32_t last_bg_ms;        // 最近一次背景解码耗时,仅用于日志
    uint32_t last_sprite_ms;    // 最近一次立绘解码耗时
} sanoba_image_t;

// 建画布(需持有 LVGL 锁)。pixels 必须是 SANOBA_ART_W * SANOBA_ART_H 个像素的缓冲。
bool sanoba_image_init(sanoba_image_t *img, struct _lv_obj_t *parent, uint16_t *pixels);

// 绑定资源包(初始化后调用一次)。
void sanoba_image_set_pack(sanoba_image_t *img, const sanoba_pack_t *pack);

// 合成一帧:背景 -> SD -> 立绘 -> 事件 CG。空字符串表示该层不画;
// bg 为空字符串时画黑屏。事件 CG 是补丁条目时自动先画基准再覆写补丁。
void sanoba_image_compose(sanoba_image_t *img, const char *bg, const char *sd, const char *sprite,
                        const char *ev);

// 只画正文带与文字区浅暗帘(文字由 LVGL 叠在上面)。换页/进菜单后要重画。
void sanoba_image_draw_band(sanoba_image_t *img);

// 画布清成黑屏。
void sanoba_image_clear(sanoba_image_t *img);

// 画布首像素(供 LVGL 之外的地方直接读写,例如截图)。
uint16_t *sanoba_image_canvas(sanoba_image_t *img);
