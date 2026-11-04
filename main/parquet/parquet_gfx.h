// main/parquet/parquet_gfx.h —— 320x150 画面区合成（无整屏 RGBA、无图片缓存）。
//
// 版面：上方 320x150 RGB565 画布，下方 90px 对话框由 LVGL 不透明面板承担。
// * 背景 / CG / 标题：生产 tjpgd 逐 MCU 解码进画布；
// * 立绘：BLOCK（调色板 + alpha）身体 + PATCH（RGB565 + alpha）表情层，按源锚点定位；
// * cut-in：BLOCK 图按 78% 不透明度叠加，先整体压暗背景（对应原作的 cutin-shade）。
#pragma once

#include "lvgl.h"
#include "parquet_pack.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PQ_ART_W 320
#define PQ_ART_H 150        // 阅读页画面区高度（对话框从 150 起）
#define PQ_CANVAS_H 240     // 虚拟画布高度（双带拼成整屏）
#define PQ_BAND_H 120       // 双带的单带高度：320x120 = 76,800 B（比 153.6KB 单块好拿）
#define PQ_BOX_H 90
#define PQ_CANVAS_MIN_FREE 32768   // 扩容后至少要留 32 KB 空闲堆

bool pq_gfx_init(void);
void pq_gfx_deinit(void);

// 第 band 条带的图像描述符（双带模式 0/1，单块模式只有 0）。
const lv_image_dsc_t *pq_gfx_canvas(int band);
// 带数：2 = 双带（虚拟 320x240，两条各 320x120）；1 = 单块 320x150。
int pq_gfx_band_count(void);
// 虚拟画布高度：240（双带）或 150（单块回退）。
int pq_gfx_canvas_height(void);
// band 0 的像素指针（测试用；双带模式下整幅读取请用 pq_gfx_test_flatten）。
uint16_t *pq_gfx_canvas_data(void);
uint16_t *pq_gfx_canvas_data(void);

void pq_gfx_clear(uint16_t color);
// 全画布乘一个 0..255 的亮度系数（cut-in 背景压暗）。
void pq_gfx_dim(uint8_t factor);

// 解码 JPEG 到画布 (dx,dy)，越界部分裁剪。失败返回 false 且不改动画布。
bool pq_gfx_draw_jpeg(const pq_pack_t *pack, uint16_t image_index, int dx, int dy);
// 等比 cover：按 max(dw/sw, dh/sh) 放大/缩小到宽高都盖住 (dx,dy,dw,dh)，
// 多出的部分居中裁掉（不非等比拉伸）。最近邻采样，不需要第二块缓冲。
bool pq_gfx_draw_jpeg_cover(const pq_pack_t *pack, uint16_t image_index, int dx, int dy,
                            int dw, int dh);

// 任意 BLOCK / JPEG / PATCH 图（Logo、标题图）按左上角绘制。
bool pq_gfx_draw_image(const pq_pack_t *pack, uint16_t image_index, int x, int y);

// 立绘：以 center_x 为脸部锚点水平中心，底边贴画面区底部。ok=false 表示缺图/解码失败。
bool pq_gfx_draw_portrait(const pq_pack_t *pack, uint16_t asset_id, int center_x,
                          bool *body_ok, bool *patch_ok);
// 立绘身体宽度（用于判断是否越界）；未知返回 0。
int pq_gfx_portrait_width(const pq_pack_t *pack, uint16_t asset_id);
// cut-in（自带画布坐标）；返回是否成功。
bool pq_gfx_draw_cutin(const pq_pack_t *pack, uint16_t asset_id, uint8_t opacity);

// 诊断：画布/工作缓冲占用。
void pq_gfx_stats(size_t *canvas_bytes, size_t *scratch_bytes);
#ifdef PQ_GFX_TEST_HOOKS
// 测试用：把双带拼成的虚拟画布整幅拷进 dst（dst 至少 320*height 个像素）。
void pq_gfx_test_flatten(uint16_t *dst);
// 测试用：最近一次立绘（身体）的绘制矩形；底边应等于 PQ_ART_H（面板顶）。
void pq_gfx_test_last_portrait(int *x, int *y, int *w, int *h);
void pq_gfx_test_drop_buffers(void);
// 用任意载荷驱动 BLOCK/PATCH 解码，做坏图/长度不符的故障注入。
bool pq_gfx_test_draw_payload(uint8_t kind, const uint8_t *payload, uint32_t len, int x,
                              int y);
#endif
