// main/summer_image.h —— 320x180 画面合成：背景/CG（JPEG）+ 立绘（JPEG+RLE 遮罩）。
//
// 解码走 TJpgDec 回调：每解出一块 MCU 就直接写进画布，不需要整图输出缓冲，
// 也不存在第二块全屏画布。画面区只占 320x180；下方 60px 是正文带。
#pragma once

#include "summer_pack.h"

#include <stdbool.h>
#include <stdint.h>

struct _lv_obj_t;

#define SUMMER_SCREEN_W 320
#define SUMMER_SCREEN_H 240
#define SUMMER_ART_W 320
#define SUMMER_ART_H 180
#define SUMMER_PANEL_H (SUMMER_SCREEN_H - SUMMER_ART_H)

typedef struct {
    struct _lv_obj_t *canvas;
    uint16_t *pixels;     // SUMMER_ART_W * SUMMER_ART_H，调用方提供（static）
} summer_canvas_t;

// 设备端：建 LVGL 画布（需持有 LVGL 锁）。
bool summer_image_init(summer_canvas_t *img, struct _lv_obj_t *parent, uint16_t *pixels);
// 宿主机测试：只绑定像素缓冲，不建 LVGL 对象。
bool summer_image_bind(summer_canvas_t *img, uint16_t *pixels);

void summer_image_clear(summer_canvas_t *img);
void summer_image_invalidate(summer_canvas_t *img);

// 背景 + 立绘；任何一张失败返回 false（不视为成功，不解锁，下次可重试）。
bool summer_image_scene(summer_canvas_t *img, const summer_pack_t *pack, uint16_t bg,
                        uint16_t body);
// CG 全屏：完整 fit 到 320x180。
bool summer_image_cg(summer_canvas_t *img, const summer_pack_t *pack, uint16_t source_id);
bool summer_image_cg_index(summer_canvas_t *img, const summer_pack_t *pack, uint32_t image_index);
// 标题 logo：按 image index 叠加到指定矩形（带遮罩）。
bool summer_image_logo(summer_canvas_t *img, const summer_pack_t *pack, uint16_t image_index,
                       int x, int y, int w, int h);

// 只校验 JPEG 头 + 遮罩结构，不写画布；用于读档前拒绝坏图。
bool summer_image_probe(const summer_pack_t *pack, uint16_t source_id);
bool summer_image_probe_index(const summer_pack_t *pack, uint32_t image_index);
bool summer_image_probe_scene(const summer_pack_t *pack, uint16_t bg, uint16_t body);
// 遮罩展开（测试用）：把 RLE 遮罩展开成 1bpp；返回是否填满 w*h。
bool summer_mask_expand(const summer_pack_t *pack, uint32_t image_index,
                        uint8_t *out, size_t cap, int *out_w, int *out_h);
