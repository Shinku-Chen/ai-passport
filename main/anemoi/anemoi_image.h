// main/anemoi/anemoi_image.h —— 画面解码：JPEG（TJpgDec 逐 MCU 写画布）+ 调色板 BLOCK。
//
// 失败语义：
//   * 绘制前先校验包内几何（图像尺寸/块表长度/调色板下标），坏包不会碰画布；
//   * JPEG 头预检失败或解码中断：返回 false，调用方保持“当前画面无效”状态，
//     不更新解锁位、不提示成功；同一张图下次可以重试；
//   * BLOCK 的每块都会校验解压长度，越界索引被跳过。
#pragma once

#include "anemoi_canvas.h"
#include "anemoi_pack.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t jpeg_pool[4096];
    bool busy;
} anem_image_ctx_t;

void anem_image_init(anem_image_ctx_t *ctx);

// 把一张背景/CG 画到画布：整幅 fit 到画布内并水平居中（不裁切）。
// 返回 false 表示解码失败（画布内容不保证有效）。
bool anem_image_draw_scene(anem_image_ctx_t *ctx, anem_canvas_t *canvas,
                           const anem_pack_t *pack, uint16_t image_id);

// 纯色填充（ANEM_IMG_SOLID）。
bool anem_image_draw_solid(anem_canvas_t *canvas, const anem_pack_t *pack, uint16_t image_id);

// 立绘/表情：BLOCK 载荷按 alpha 混合到 (x, y)。
bool anem_image_draw_sprite(anem_image_ctx_t *ctx, anem_canvas_t *canvas,
                            const anem_pack_t *pack, uint16_t image_id, int x, int y);

// 只解析 JPEG 头并用包内声明尺寸核对（测试与绘制前预检共用）。
bool anem_image_probe_jpeg(const uint8_t *data, uint32_t len, uint16_t *w, uint16_t *h);

// BLOCK 头部自检（不解压像素）：尺寸/调色板/块表是否自洽。
bool anem_image_probe_block(const uint8_t *data, uint32_t len);

// 检查一张图像条目是否可用（type 与载荷长度）。
bool anem_image_check(const anem_pack_t *pack, uint16_t image_id);

#ifdef __cplusplus
}
#endif
