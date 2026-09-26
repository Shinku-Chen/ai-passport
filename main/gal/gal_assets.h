/*
 * Read-only access to the packed galgame assets.
 *
 * The `assets` data partition is memory-mapped, so every packed image is turned
 * into an lv_image_dsc_t pointing straight into flash. LVGL renders indexed
 * images one scan line at a time (LV_BIN_DECODER_RAM_LOAD is off), so a
 * full-screen background costs roughly one scan line of RAM rather than the
 * 150 KB a frame buffer would need -- this board has no PSRAM.
 *
 * Mapping the whole partition costs about 72 of the chip's 128 flash-MMU pages.
 * The application's own code and read-only data occupy roughly 28, which leaves
 * headroom; gal_assets_init() reports a failure rather than degrading silently.
 */
#ifndef GAL_ASSETS_H
#define GAL_ASSETS_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#include "gal_script.h"

/* Upper bound on packed images; the pack reports its real count at startup. */
#define GAL_MAX_IMAGES 160

typedef struct {
    const uint8_t *pack;
    uint32_t pack_size;
    gal_pack_t script;
    lv_image_dsc_t descriptors[GAL_MAX_IMAGES];
    uint16_t count;
    bool ready;
} gal_assets_t;

/* Locate, validate and map the assets partition. */
esp_err_t gal_assets_init(gal_assets_t *assets);
void gal_assets_deinit(gal_assets_t *assets);
bool gal_assets_ready(const gal_assets_t *assets);

/* NULL when the index is out of range or the entry is a solid colour. */
const lv_image_dsc_t *gal_assets_image(const gal_assets_t *assets, uint16_t index);
/* NULL when absent. `solid_color` receives -1, or the RGB565 fill of a solid image. */
const lv_image_dsc_t *gal_assets_find(const gal_assets_t *assets, const char *name,
                                      int32_t *solid_color);
/*
 * RGB565 fill for a solid image, or -1 when the entry is not solid.
 *
 * A plain 0xFFFF sentinel would collide with white (RGB565 0xFFFF), which the
 * scripts use as a real background.
 */
int32_t gal_assets_solid_color(const gal_assets_t *assets, uint16_t index);
/* A short human-readable description of the last init failure. */
const char *gal_assets_failure(void);

#endif /* GAL_ASSETS_H */
