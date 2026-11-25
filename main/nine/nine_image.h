// Bounded-memory image compositor. Call from the LVGL owner/with its lock held.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct _lv_obj_t;
#define NINE_IMAGE_W 256u
#define NINE_IMAGE_H 144u
#define NINE_IMAGE_BAND_H 36u
#define NINE_IMAGE_BANDS 4u
#define NINE_IMAGE_DISPLAY_W 320u
#define NINE_IMAGE_DISPLAY_H 180u
#define NINE_IMAGE_DISPLAY_BAND_H 45u
// LVGL image scale units: 256 = 1x, so 320 = 1.25x for 256->320 and 36->45.
#define NINE_IMAGE_DISPLAY_SCALE 320
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

// Production requires a live LVGL parent (the framework screen); NULL is
// rejected instead of silently skipping object creation. The host image test
// passes a non-NULL sentinel because it has no LVGL object graph.
bool nine_image_init(struct _lv_obj_t *parent);
unsigned nine_image_band_count(void);
// Read-only access to one decoded band (320 x 30 RGB565). Used by the
// production integration regressions; the compositor never exposes native
// pointers through the UI.
bool nine_image_band_info(unsigned index, const uint16_t **pixels,
                          unsigned *width, unsigned *height);
void nine_image_deinit(void);
// Validate the image package (CRC, tables, payloads) before installing it.
// On failure the previous binding and displayed frame remain unchanged.
bool nine_image_bind_source(const nine_image_source_t *source);
bool nine_image_bind_bundle(const uint8_t *script, size_t script_size,
                            const uint8_t *images, size_t image_size);
bool nine_image_show_visual(uint32_t visual);
// Display a full asset with aspect preserved, centered in 320x240.
bool nine_image_show_asset(uint32_t image);
bool nine_image_validate_asset(uint32_t image);
// Pack-embedded raw RGB565 asset (title art / gallery thumbnail); flash-backed.
bool nine_image_raw_asset(uint32_t image, uint32_t *w, uint32_t *h, const uint16_t **pixels);
void nine_image_clear(uint16_t rgb565);
// Move the six band objects to root indices index..index+5, just above the
// framework background and below the UI pages. Call under the LVGL lock.
void nine_image_set_base_layer(int index);
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
#endif
