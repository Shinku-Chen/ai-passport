// main/nn_canvas.h —— 场景画布（320x150 RGB565，只覆盖画面区）。
//
// 阅读时屏幕上只有一块“画”：背景或 CG 解进去，立绘再按遮罩叠上去。画布由
// nn_canvas 统一管理并交给一个 lv_image 显示，因此:
//   * 背景/CG/立绘都不需要各自的解码缓冲（nn_image 边解边写）；
//   * 换图只重画这块缓冲并 invalidate，不重建 LVGL 对象树；
//   * 面板、文字、菜单都是画布之上的 LVGL 对象，半透明叠色即可。
#pragma once

#include "nn_pack.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NN_SCREEN_W 240
#define NN_SCREEN_H 320
// 分带画布：真机最大连续块只有 ~49–55 KB，单块 153,600/96,000 都不可靠。
// 用 320x80 = 51,200 B 一条带，三条带画满 0..239（band0 0..79 / band1 80..159 /
// band2 160..239），每带画完就 invalidate+flush。
// 拿不到 51,200 时回退 320x60 = 38,400（四条带）。
#define NN_PANEL_TOP 224      // 面板顶（= 320 - 面板高 96）；画面只画 0..223（面板不透明）
// 真机实测：开机后空闲堆 159 KB，但**最大连续块只有 ~30 KB**，
// 240x80=38,400 一条都拿不到（实机日志："独立分带缓冲申请失败，回落单缓冲"）。
// 所以带高按 56 → 240x56=26,880 B/带，4 带正好铺满 224 行画面区。
#define NN_ART_H NN_SCREEN_H   // 画面铺满整屏（面板半透明，艺术要透上来）
#define NN_BAND_H 56
#define NN_BAND_COUNT 6        // 6 x 56 = 336 >= 320，整屏都画
#define NN_BAND_H_MIN 40
#define NN_BAND_COUNT_MIN 8

// 当前分带：带高（80 或 60）与带数（3 或 4）。
int nn_canvas_band_h(void);
int nn_canvas_band_count(void);

// 分配画布（内部 RAM）。失败返回 false —— 没有画布就没法阅读。
bool nn_canvas_init(void);

// 条带像素（RGB565，行优先，stride = 320，高度 = nn_canvas_band_h()）。
uint16_t *nn_canvas_pixels(void);

// 把指定条带铺纯色。
void nn_canvas_clear_band(int band, uint16_t color);

// 把 pack 里的图像画到**指定条带**上（虚拟 320x240 画布坐标）：
//   kind 0/2（背景/缩略图）: 居中绘制，不裁不拉伸；
//   kind 3（CG）与 kind 4（标题）: 等比 contain 到 320x240；
//   kind 1（立绘）: 底部居中（贴面板上沿），带 1bpp 遮罩。
// 调用方按带循环：band=0..count-1，每带之后自己 invalidate+flush。
// cg_full: 本次是**全屏 CG 鉴赏页**（CGVIEW）——CG 用 contain 整幅可见（上下黑边可接受）；
//   传 false = 游戏内对话画面，CG 用 cover + 顶部对齐（底部裁切区被 224..320 对话框盖住）。
//   由调用方显式传入，不由页面名硬猜。
bool nn_canvas_draw_image_band(const nn_pack_t *pack, uint16_t image_id, int band,
                               bool cg_full);

// 把指定条带刷成黑（黑场用）。
void nn_canvas_black_band(int band);

// 每条带各自的 LVGL 图像描述符（独立缓冲，绑一次不再改）。
const void *nn_canvas_dsc(void);
const void *nn_canvas_band_dsc(int band);

#ifdef __cplusplus
}
#endif
