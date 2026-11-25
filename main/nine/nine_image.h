// Bounded-memory image compositor. Call from the LVGL owner/with its lock held.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct _lv_obj_t;
#define NINE_IMAGE_W 320u
#define NINE_IMAGE_H 180u
#define NINE_IMAGE_BAND_H 36u
#define NINE_IMAGE_BANDS 5u
#define NINE_IMAGE_LAYER_MAX 32u

typedef struct {
    uint32_t image;
    int32_t x, y;
    uint32_t width, height, opacity;
} nine_image_layer_t;

typedef struct {
    uint32_t crc_max_us, crc_total_us;
    uint32_t decode_max_us, decode_total_us;
    uint32_t clear_max_us, clear_total_us;
    uint32_t invalidate_max_us, invalidate_total_us;
    uint32_t frames;
} nine_image_perf_t;

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
// Display a full asset with aspect preserved, centered in 320x240.
bool nine_image_show_asset(uint32_t image);
// Display an asset at the top of the screen (title art / opening frames).
bool nine_image_show_asset_top(uint32_t image);
bool nine_image_validate_asset(uint32_t image);
// Bounded diagnostics for the single-resource OP/CG path.
void nine_image_perf_reset(void);
void nine_image_perf_get(nine_image_perf_t *out);
void nine_image_clear(uint16_t rgb565);
void nine_image_set_visible(bool visible);
size_t nine_image_canvas_bytes(void);
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
void nine_image_test_fail_show(int fail);
void nine_image_test_disable_decode(int disable);
#endif
