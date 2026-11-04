// main/paper/paper_gfx.h —— 320x150 画面区合成层（无整屏 RGBA，无图片缓存）。
//
// 版面：320x150 RGB565 画面区画布（对话框由 LVGL 不透明面板承担）；背景/事件 CG 用内置 tjpgd 逐 MCU 解码进画布；
// 立绘用调色板 BLOCK 载荷逐 8 行解压后直接合成到画布（不需要立绘解码缓冲）；
// 表情差分按 PATCH（基准图 + 掩码 + RGB565 补丁）叠加。画布以 lv_image 形式交给 LVGL，
// 文本与面板是 LVGL 对象，叠在画布上。
#pragma once

#include "lvgl.h"
#include "paper_pack.h"

#include <stdbool.h>
#include <stdint.h>

#define PAPER_SCR_W 320
// 画布按 320 宽整屏高；真机最大连续块有限，实际行数由 paper_gfx_init 动态回退。
#define PAPER_SCR_H 232
#define PAPER_BOX_H 90

bool paper_gfx_init(void);
void paper_gfx_deinit(void);

// 画布（320x150 RGB565）作为 LVGL 图源。
const lv_image_dsc_t *paper_gfx_canvas(void);
uint16_t *paper_gfx_canvas_data(void);

void paper_gfx_clear(uint16_t color);
// 解码一张场景图到画布（JPEG）。失败返回 false 并保持原内容。
bool paper_gfx_draw_frame(const paper_pack_t *pack, uint16_t image_index);
// 在画布上叠加立绘（BLOCK / PATCH），返回绘制区域高度（用于定位）。
bool paper_gfx_draw_sprite(const paper_pack_t *pack, uint16_t image_index, int x, int y);
// 立绘天然尺寸（用于水平居中）；未知返回 0。
bool paper_gfx_image_size(const paper_pack_t *pack, uint16_t image_index, int *w, int *h);

#ifdef PAPER_PACK_USE_HOST_BLOBS
void paper_gfx_test_release_patch(void);   // 仅 host 测试：释放补丁缓存以复现分配失败
#endif

// 诊断：当前画布/工作缓冲占用（字节）。
void paper_gfx_stats(size_t *canvas_bytes, size_t *scratch_bytes);

#ifdef PAPER_UI_TEST_HOOKS
// 测试钩子：验证 tjpgd 的 BGR565 -> 画布 RGB565 换序（红蓝不能互换）。
uint16_t paper_gfx_test_swap(uint16_t v);
#endif
