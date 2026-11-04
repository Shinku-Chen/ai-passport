// main/anemoi/anemoi_canvas.h —— 画面区画布（320x180 RGB565，静态缓冲）。
//
// 背景 / CG / 立绘都直接合成为画布像素，不保留每张图的解码缓冲，也不做整帧
// RGBA/PNG 缓存。画布是静态数组：它的 115,200 B 在 MAP 的 .bss 里可核，不靠
// 运行期“能 malloc 到就算过”。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANEM_CANVAS_W 320
#define ANEM_CANVAS_H 180

typedef struct {
    uint16_t *pixels;
    const void *image_dsc;
    bool ready;
} anem_canvas_t;

// 初始化：绑定静态像素缓冲。失败只可能是参数错误。
bool anem_canvas_init(anem_canvas_t *canvas);

// 铺纯色（RGB565）。
void anem_canvas_fill(anem_canvas_t *canvas, uint16_t color);

// 直接写一块 RGB565 行（x/y 允许出界，按画布裁剪）。
void anem_canvas_blit(anem_canvas_t *canvas, const uint16_t *src, int src_stride, int src_w,
                      int src_h, int x, int y);

// 单像素 alpha 混合（a = 0..255）。出界忽略。
void anem_canvas_blend_pixel(anem_canvas_t *canvas, int x, int y, uint16_t color, uint8_t alpha);

// LVGL 图像描述符（给 lv_image_set_src 用）。
const void *anem_canvas_dsc(const anem_canvas_t *canvas);
uint16_t *anem_canvas_pixels(anem_canvas_t *canvas);

#ifdef __cplusplus
}
#endif
