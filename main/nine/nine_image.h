// Bounded-memory image compositor. Call from the LVGL owner/with its lock held.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct _lv_obj_t;
#define NINE_IMAGE_W 240u
#define NINE_IMAGE_H 135u
#define NINE_IMAGE_BAND_H 27u
#define NINE_IMAGE_BANDS 5u
#define NINE_IMAGE_LAYER_MAX 32u

typedef struct {
    uint32_t image;
    int32_t x, y;
    uint32_t width, height, opacity;
} nine_image_layer_t;

// The callback copies a visual snapshot, never returns pointers into a temporary
// VM text cache. Coordinates are pixels on the 240x135 art canvas; LVGL scales
// each 240x27 band to 320x36, so the full art is displayed at 320x180.
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
// Explicitly drop the same-visual fast-forward cache. Normally show_asset() and
// nine_image_clear() do this; corruption/source-mutation tests use this hook.
void nine_image_invalidate_cache(void);
// Display a full asset with aspect preserved, filling the 240x135 canvas.
bool nine_image_show_asset(uint32_t image);
// OP-only fast path: CRC/boundary precheck + one JPEG decode into the existing
// bands. It never allocates a full-screen buffer and clears all bands on any
// CRC/bounds/decode failure so the caller can expose a visible error.
typedef struct {
    uint32_t crc_checks;
    uint32_t jpeg_decodes;
    bool ok;
} nine_image_op_result_t;
bool nine_image_show_asset_op_ex(uint32_t image, nine_image_op_result_t *result);
bool nine_image_show_asset_op(uint32_t image);
// OP-only render hint: nearest-neighbour scaling avoids the per-pixel
// anti-aliased transform cost while OP is moving. Restored on return/fault.
void nine_image_set_op_fast(bool fast);
bool nine_image_validate_asset(uint32_t image);
void nine_image_clear(uint16_t rgb565);
void nine_image_set_visible(bool visible);
size_t nine_image_canvas_bytes(void);
size_t nine_image_scratch_bytes(void);
uint32_t nine_image_asset_count(void);
bool nine_image_title(unsigned index, uint32_t *image);
// Copy the current 320x240 canvas into a caller-owned RGB565 thumbnail. Used by
// the paginated CG browser after nine_image_show_asset() has decoded the group
// cover through the same production decoder. Returns false if not bound.
bool nine_image_copy_scaled(uint16_t *dst, unsigned width, unsigned height);

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
unsigned nine_image_test_visual_decode_count(void);
unsigned nine_image_test_visual_cache_hit_count(void);
void nine_image_test_reset_visual_counts(void);
#endif
