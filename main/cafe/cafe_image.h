// main/cafe/cafe_image.h —— 224x126 舞台合成（JPEG + 1/2bpp alpha mask）。
//
// 为什么不是整幅 RGB565：ESP32-C3 无 PSRAM，单块 320x180 RGB565 是 115,200B。
// 这里固定三条 224x42 条带（每条 18,816B，另加 42 行 staging），由 LVGL 放大到
// 320x180 显示；立绘/表情在条带内合成，因此不需要整幅画布。
#pragma once

#include "cafe_pack.h"

#include <stdbool.h>
#include <stdint.h>

#define CAFE_BAND_W 224
#define CAFE_BAND_H 42
#define CAFE_BAND_COUNT 3
#define CAFE_STAGE_W 224
#define CAFE_STAGE_H 126

typedef struct {
    uint16_t *pixels[CAFE_BAND_COUNT];   // 每条 CAFE_BAND_W * CAFE_BAND_H
} cafe_bands_t;

bool cafe_image_init(void);
bool cafe_image_bands_init(cafe_bands_t *bands);
void cafe_image_bands_free(cafe_bands_t *bands);

// 画一整帧：背景资产 + 0..5 个立绘资产（坐标已由打包器映射到 224x126 舞台）。
// 失败时会把三条带清黑并返回 false，调用方必须显示可见错误，不能沿用旧画面。
bool cafe_image_render_scene(cafe_bands_t *bands, uint16_t bg_asset,
                             const cafe_sprite_t *sprites, unsigned sprite_count);

// 只画一张背景/CG/标题图（image id，非 asset id）。
bool cafe_image_render_image(cafe_bands_t *bands, uint16_t image_id);

// OP 帧：源生 JPEG（224x126，q15）等比映射到完整 224x126 舞台，
// 直接写现有三条带；不分配整幅 RGB565，不改变 LVGL 缓存。
bool cafe_image_render_op(cafe_bands_t *bands, uint16_t image_id);

// 64x36 缩略图（背景图），dst 为 RGB565。失败返回 false。
bool cafe_image_thumb(uint16_t image_id, uint16_t *dst, int dst_w, int dst_h);

// 背景资产对应的鉴赏组；-1 = 不属于任何组。
int cafe_image_gallery_group(uint16_t asset);
bool cafe_image_gallery_alias(uint16_t asset, uint16_t index, uint16_t *group);

// 校验一张图片的 JPEG+mask CRC（渲染前调用；失败不碰目标条带）。
bool cafe_image_verify(uint16_t image_id);

// Host-test counters: bounded counts, not per-frame diagnostics.
uint32_t cafe_image_render_scene_count(void);
uint32_t cafe_image_verify_count(void);
void cafe_image_reset_counts(void);
