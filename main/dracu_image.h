// main/dracu_image.h —— 画面区(240x320 RGB565 画布)与背景 / 事件 CG / 立绘合成。
//
// 三套解码路径,共同点是"没有中间大缓冲、不用 32 KB inflate 字典":
//   1. 背景 / 事件 CG:JPEG 240x320,esp_jpeg 直接解进画布(输出缓冲就是画布);
//   2. 立绘 / SD:分块调色板格式(不是 PNG),每块解压到 6 KB 静态行缓冲,
//      逐行反滤波 -> 查调色板 -> 混进画布;
//   3. 事件 CG 补丁:先解基准 JPEG,再按 1bpp 掩码把 RGB565 像素逐行覆写上去。
//
// 立绘是"身体 + 表情"两层拼出来的(源工程就是分层立绘):身体画在
//   ((240 - body.w) / 2, DRACU_SPRITE_HEAD_Y - sprite.facey)
// 表情画在身体左上角 + (sprite.facex + face.dx, sprite.facey + face.dy)。
// 这个换算与 tools/dracu_pack.py 一致 —— 打包时按同一条规则裁掉了屏幕外的下半身。
#pragma once

#include "dracu_pack.h"

#include <stdbool.h>
#include <stdint.h>

// 画面区尺寸,必须与 tools/dracu_pack.py 的 screen_w/screen_h 一致。
#define DRACU_ART_W 240
#define DRACU_ART_H 320
// 正文带的顶边与高度(与 main/dracu_ui.h 的 DRACU_BOX_Y/DRACU_BOX_H 一致)。
#define DRACU_BAND_Y 210
#define DRACU_BAND_H (DRACU_ART_H - DRACU_BAND_Y)
// 立绘"脸区顶部"在屏幕上的 y,必须与 tools/dracu_source.py 的 SPRITE_HEAD_Y 一致。
#define DRACU_SPRITE_HEAD_Y 87

// 一屏要画的四个图层。四层统一用 DRACU_NO_ENTRY 表示"没有"(页记录里的 u8 字段
// 0xFF 由应用层换算过来);bg = DRACU_NO_ENTRY 就是黑屏。
typedef struct {
    uint16_t bg;      // 背景 id
    uint16_t cg;      // 事件 CG id
    uint16_t sd;      // SD 小人 id
    uint16_t sprite;  // 立绘 id
} dracu_layers_t;

typedef struct {
    struct _lv_obj_t *canvas;   // 画布对象(直接显示 pixels)
    uint16_t *pixels;           // 画布缓冲,由调用方提供并保证生命周期
    const dracu_pack_t *pack;
    uint32_t last_bg_ms;        // 最近一次背景/CG 解码耗时,仅用于日志
    uint32_t last_sprite_ms;    // 最近一次立绘合成耗时
    bool valid;                 // 已经画过一次(否则第一次必定整屏重画)
    dracu_layers_t shown;       // 画布上当前是哪四个图层
} dracu_image_t;

// 建画布(需持有 LVGL 锁)。pixels 必须是 DRACU_ART_W * DRACU_ART_H 个像素的缓冲。
bool dracu_image_init(dracu_image_t *img, struct _lv_obj_t *parent, uint16_t *pixels,
                      const dracu_pack_t *pack);

// 画一整屏:背景 -> (事件 CG 或 立绘/SD) -> 正文带。四层都没变时只重画正文带,
// 所以翻页不会反复解 JPEG。返回 false 表示某一层画不出来(已记日志)。
bool dracu_image_compose(dracu_image_t *img, const dracu_layers_t *layers);

// 只画正文带(半透明蓝底 + 文字区浅暗帘)。
void dracu_image_draw_band(dracu_image_t *img);

// 黑屏(章节过场、结局页背景等)。
void dracu_image_clear(dracu_image_t *img);

// 调试:直接取画布缓冲(串口截图用)。
uint16_t *dracu_image_pixels(dracu_image_t *img);
