// main/nn_image.h —— JPEG 直写画布（TJpgDec,边解边写,不建整幅解码缓冲）。
//
// 背景/CG 与立绘都走同一条链路:
//   pack 里的 JPEG(直接从 flash 读) -> TJpgDec -> 逐 MCU 转 RGB565 -> 写进 320x240 画布
// 立绘额外带一张 1bpp 遮罩(按行字节对齐),只有遮罩位为 1 的像素才写入,
// 因此既不需要立绘解码缓冲,也不需要第二层 LVGL 图像。
#pragma once

#include <stdbool.h>
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif

// 把一张 JPEG 画到 RGB565 画布的 (x0, y0)。
// mask 非空时按 1bpp 行对齐遮罩做“只写不透明像素”的合成。
// 返回 false 表示 JPEG 数据损坏或画布越界。
bool nn_jpeg_draw(uint16_t *canvas, int cw, int ch,
                  const uint8_t *jpg, uint32_t jpg_len, int x0, int y0,
                  const uint8_t *mask, int mask_stride);

// 等比缩放绘制（最近邻）：把整幅 JPEG 按 contain(min) / cover(max) 缩放并居中
// 画进 (0,0,cw,ch)。contain 保证整幅可见（标题图 320x240 -> 200x150），
// cover 则是铺满后居中裁。解码仍然逐 MCU 直写，不需要整图缓冲。
bool nn_jpeg_draw_fit(uint16_t *canvas, int cw, int ch, const uint8_t *jpg, uint32_t jpg_len,
                      bool contain);

// 分带版本：画布只是一条带（cw x band_h），band_top 是这条带在“虚拟 320xvh 画布”里
// 的起始行。vy0 是图像在虚拟画布里的顶边 y。超出本条带的像素直接跳过。
bool nn_jpeg_draw_band(uint16_t *band, int cw, int band_h, const uint8_t *jpg, uint32_t jpg_len,
                       int x0, int vy0, int band_top, const uint8_t *mask, int mask_stride);
// anchor_x: 0 = 水平居中裁，1 = 右对齐裁（保留右侧内容）。
bool nn_jpeg_draw_fit_band(uint16_t *band, int cw, int band_h, int vh, const uint8_t *jpg,
                           uint32_t jpg_len, bool contain, int band_top, int anchor_x,
                           int anchor_y);

#ifdef __cplusplus
}
#endif
