#pragma once
#include "cafe_pack.h"
#include <stdbool.h>
#include <stdint.h>
#define CAFE_BAND_W 224
#define CAFE_BAND_H 42
#define CAFE_BAND_COUNT 3
typedef struct { uint16_t *pixels[CAFE_BAND_COUNT]; } cafe_bands_t;
bool cafe_image_init(void);
bool cafe_image_bands_init(cafe_bands_t *bands);
void cafe_image_bands_free(cafe_bands_t *bands);
bool cafe_image_render_scene(cafe_bands_t *bands, uint16_t bg_asset,
                             const cafe_sprite_t *sprites, unsigned sprite_count);
bool cafe_image_render_image(cafe_bands_t *bands, uint16_t image_id);
bool cafe_image_render_op(cafe_bands_t *bands, uint16_t image_id);
bool cafe_image_thumb(uint16_t image_id, uint16_t *dst, int dst_w, int dst_h);
int cafe_image_gallery_group(uint16_t asset);
bool cafe_image_gallery_alias(uint16_t asset, uint16_t index, uint16_t *group);
bool cafe_image_verify(uint16_t image_id);
uint32_t cafe_image_render_scene_count(void);
uint32_t cafe_image_verify_count(void);
void cafe_image_reset_counts(void);
