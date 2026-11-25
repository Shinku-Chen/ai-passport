// Bounded-memory image compositor. Call from the LVGL owner/with its lock held.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct _lv_obj_t;
#define NINE_IMAGE_W 256u
#define NINE_IMAGE_H 144u
#define NINE_IMAGE_BAND_H 24u
#define NINE_IMAGE_BANDS 6u
// Title/CG use two extra bands: 256x192 -> 320x240 full cover. The reading
// viewport remains 256x144 -> 320x180; the extra bands stay hidden there.
#define NINE_IMAGE_FULL_H 192u
#define NINE_IMAGE_FULL_BANDS 8u
#define NINE_IMAGE_SCREEN_W 320u
#define NINE_IMAGE_SCREEN_H 180u       // OP: full 320x180, no crop
#define NINE_IMAGE_SCREEN_FULL_H 240u  // title/pure CG: full 320x240
#define NINE_IMAGE_READ_SCREEN_H 180u  // reading: full 320x180, no crop
#define NINE_IMAGE_READ_CROP 0u        // transparent name overlays 160..179
#define NINE_IMAGE_LAYER_MAX 32u

typedef struct {
    uint32_t image;
    int32_t x, y;
    uint32_t width, height, opacity;
} nine_image_layer_t;

// The callback copies a visual snapshot, never returns pointers into a temporary
// VM text cache. Coordinates are output pixels on the 320x240 screen.
typedef bool (*nine_image_visual_fn)(void *ctx, uint32_t visual,
                                    nine_image_layer_t *layers, size_t capacity,
                                    size_t *count);
typedef struct {
    const uint8_t *data;
    size_t size;
    nine_image_visual_fn visual;
    void *ctx;
} nine_image_source_t;

bool nine_image_init(struct _lv_obj_t *parent);
void nine_image_deinit(void);
// Validate the image package (CRC, tables, payloads) before installing it.
// On failure the previous binding and displayed frame remain unchanged.
bool nine_image_bind_source(const nine_image_source_t *source);
bool nine_image_bind_bundle(const uint8_t *script, size_t script_size,
                            const uint8_t *images, size_t image_size);
bool nine_image_show_visual(uint32_t visual);
// Display a full asset with aspect preserved, centered in the reading 256x144.
// Used by OP frames; the reading visual path uses nine_image_show_visual().
bool nine_image_show_asset(uint32_t image);
// Title/CG artwork: 256x192 center-cover (crop) -> 320x240 full screen.
bool nine_image_show_asset_full(uint32_t image);
bool nine_image_validate_asset(uint32_t image);
void nine_image_clear(uint16_t rgb565);
typedef struct {
    uint32_t renders, crc_checks, jpeg_decodes, last_render_us;
} nine_image_stats_t;
bool nine_image_stats(nine_image_stats_t *out);
void nine_image_set_visible(bool visible);
size_t nine_image_canvas_bytes(void);
uint32_t nine_image_view_height(void);
bool nine_image_band_visible(unsigned index);
uint16_t nine_image_pixel(unsigned x, unsigned y);
size_t nine_image_scratch_bytes(void);
uint32_t nine_image_asset_count(void);
bool nine_image_title(unsigned index, uint32_t *image);

// Media reads the same immutable image package through bounded byte ranges.
const uint8_t *nine_image_package(size_t *size);
bool nine_image_op_frame(uint32_t index, uint32_t *image, uint32_t *timestamp,
                         uint32_t *duration);
uint32_t nine_image_op_count(void);

#ifdef NINE_IMAGE_HOST
// Production compositor/decoder pixels; no alternate host rendering algorithm.
uint16_t nine_image_test_pixel(unsigned x, unsigned y);
size_t nine_image_test_largest_allocation(void);
void nine_image_test_fail_allocation(unsigned call);
#endif
