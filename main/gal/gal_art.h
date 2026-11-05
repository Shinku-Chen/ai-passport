#pragma once
#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

// One shared arena: 256x192 RGB565 scene, OR eight native 64x40 thumbnails.
// The row decoder expands scenes to 320x240 without an LVGL transform buffer.
#define GAL_ART_CACHE_W 256
#define GAL_ART_CACHE_H 192
#define GAL_ART_ARENA_BYTES (GAL_ART_CACHE_W * GAL_ART_CACHE_H * 2u)

typedef struct {
    uint32_t jpeg_decodes;
    uint32_t cache_hits;
    uint32_t output_rows;
    uint32_t last_decode_ms;
} gal_art_stats_t;

// All calls require the LVGL lock, including cache replacement. The arena and
// JPEG workspace live for the application lifetime and never on a task stack.
bool gal_art_init(void);
const lv_image_dsc_t *gal_art_large(const void *jpeg, bool *changed);
const lv_image_dsc_t *gal_art_thumb(int slot, const void *jpeg, bool *changed);
gal_art_stats_t gal_art_stats(void);
