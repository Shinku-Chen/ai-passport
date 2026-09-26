#include "gal_assets.h"

#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"

static const char *TAG = "gal_assets";

/* The packer writes this subtype for the read-only resource partition. */
#define GAL_PARTITION_SUBTYPE 0x40
#define GAL_PARTITION_LABEL "assets"

static const char *s_failure = "not initialised";

const char *gal_assets_failure(void)
{
    return s_failure;
}

/* Build an LVGL descriptor view over one packed image. */
static bool make_descriptor(gal_assets_t *assets, uint16_t index)
{
    const gal_image_entry_t *image = gal_pack_image(&assets->script, index);
    if (image == NULL || image->kind == GAL_IMG_KIND_SOLID) {
        return false;
    }
    const uint8_t *payload = gal_pack_image_payload(&assets->script, image);
    if (payload == NULL) {
        return false;
    }

    lv_image_dsc_t *dsc = &assets->descriptors[index];
    memset(dsc, 0, sizeof(*dsc));
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    /* One byte per pixel with a 256-entry palette; alpha comes from the palette. */
    dsc->header.cf = LV_COLOR_FORMAT_I8;
    dsc->header.flags = 0;
    dsc->header.w = image->width;
    dsc->header.h = image->height;
    dsc->header.stride = image->width;
    dsc->data_size = image->data_len;
    /* LVGL's bin decoder reads the palette from the start of `data` for indexed
     * formats, which is exactly how the packer laid the payload out. */
    dsc->data = payload;
    return true;
}

esp_err_t gal_assets_init(gal_assets_t *assets)
{
    if (assets == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(assets, 0, sizeof(*assets));
    s_failure = "not initialised";

    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)GAL_PARTITION_SUBTYPE,
        GAL_PARTITION_LABEL);
    if (partition == NULL) {
        s_failure = "assets partition missing";
        ESP_LOGE(TAG, "%s: no '%s' data partition found", s_failure, GAL_PARTITION_LABEL);
        return ESP_ERR_NOT_FOUND;
    }

    /* Read the header through the partition API first. A blank partition reads as
     * 0xFF, and mapping several megabytes only to reject it would waste MMU pages. */
    gal_pack_header_t header;
    esp_err_t err = esp_partition_read(partition, 0, &header, sizeof(header));
    if (err != ESP_OK) {
        s_failure = "cannot read assets header";
        ESP_LOGE(TAG, "%s: %s", s_failure, esp_err_to_name(err));
        return err;
    }
    if (header.magic != GAL_PACK_MAGIC || header.version != GAL_PACK_VERSION) {
        s_failure = "assets partition not flashed";
        ESP_LOGE(TAG, "%s (magic 0x%08" PRIX32 ")", s_failure, header.magic);
        return ESP_ERR_INVALID_STATE;
    }

    const void *mapped = NULL;
    esp_partition_mmap_handle_t handle;
    err = esp_partition_mmap(partition, 0, partition->size, ESP_PARTITION_MMAP_DATA,
                             &mapped, &handle);
    if (err != ESP_OK) {
        /* Most likely cause is running out of flash-MMU pages, not a corrupt pack. */
        s_failure = "cannot map assets partition";
        ESP_LOGE(TAG, "%s: %s", s_failure, esp_err_to_name(err));
        return err;
    }

    if (!gal_pack_open(&assets->script, (const uint8_t *)mapped, partition->size) ||
        !gal_pack_usable(&assets->script)) {
        s_failure = "assets pack is not usable";
        ESP_LOGE(TAG, "%s", s_failure);
        return ESP_ERR_INVALID_STATE;
    }

    assets->pack = (const uint8_t *)mapped;
    assets->pack_size = partition->size;
    assets->count = gal_pack_image_count(&assets->script);
    if (assets->count > GAL_MAX_IMAGES) {
        s_failure = "assets pack has too many images";
        ESP_LOGE(TAG, "%s (%u > %u)", s_failure, assets->count, GAL_MAX_IMAGES);
        return ESP_ERR_INVALID_SIZE;
    }

    for (uint16_t index = 0; index < assets->count; index++) {
        make_descriptor(assets, index);
    }

    assets->ready = true;
    s_failure = NULL;
    ESP_LOGI(TAG, "assets ready: %u images, %u chapters, %" PRIu32 " KiB mapped",
             assets->count, gal_pack_chapter_count(&assets->script),
             assets->pack_size / 1024);
    return ESP_OK;
}

void gal_assets_deinit(gal_assets_t *assets)
{
    if (assets != NULL) {
        assets->ready = false;
    }
}

bool gal_assets_ready(const gal_assets_t *assets)
{
    return assets != NULL && assets->ready;
}

const lv_image_dsc_t *gal_assets_image(const gal_assets_t *assets, uint16_t index)
{
    if (assets == NULL || !assets->ready || index >= assets->count) {
        return NULL;
    }
    lv_image_dsc_t *dsc = (lv_image_dsc_t *)&assets->descriptors[index];
    return dsc->data != NULL ? dsc : NULL;
}

int32_t gal_assets_solid_color(const gal_assets_t *assets, uint16_t index)
{
    if (assets == NULL || !assets->ready || index >= assets->count) {
        return -1;
    }
    const gal_image_entry_t *image = gal_pack_image(&assets->script, index);
    if (image == NULL || image->kind != GAL_IMG_KIND_SOLID) {
        return -1;
    }
    return (int32_t)gal_pack_image_color(&assets->script, image);
}

const lv_image_dsc_t *gal_assets_find(const gal_assets_t *assets, const char *name,
                                      int32_t *solid_color)
{
    if (solid_color != NULL) {
        *solid_color = -1;
    }
    if (assets == NULL || !assets->ready || name == NULL) {
        return NULL;
    }
    int32_t index = gal_pack_find_image(&assets->script, name);
    if (index < 0) {
        ESP_LOGW(TAG, "image '%s' is not in the pack", name);
        return NULL;
    }
    const lv_image_dsc_t *image = gal_assets_image(assets, (uint16_t)index);
    if (image == NULL && solid_color != NULL) {
        *solid_color = gal_assets_solid_color(assets, (uint16_t)index);
    }
    return image;
}
