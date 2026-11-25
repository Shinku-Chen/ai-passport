// main/gal/maitetsu_media.c —— 媒体包 + LVGL 图像解码器适配。
//
// 这里没有任何跨调用的临时对象：图像描述符是静态数组，按图描述来自生成的
// Flash 常量表（maitetsu_pack_image 直接返回它的元素），所以 info / area
// 回调看到的宽高、条带位置始终是这张图自己的值。
#include "maitetsu_media.h"

#include "gal_content.h"
#include "maitetsu_pack.h"

#include "esp_log.h"

#include "lvgl_private.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "maitetsu_media";

// 每个包内图像一个描述符：LVGL 在绘制期间持有它的指针。
static lv_image_dsc_t s_descriptors[MAITETSU_MAX_IMAGES];
static uint16_t s_descriptor_count;
static bool s_ready;
static char s_error[64];
static volatile bool s_fault;
static char s_fault_text[64];

typedef struct {
    lv_draw_buf_t draw;
    uint16_t image;
} media_ctx_t;

static void set_fault(const char *text)
{
    if (s_fault) return;
    snprintf(s_fault_text, sizeof(s_fault_text), "%s", text ? text : "媒体解码失败");
    s_fault = true;
    ESP_LOGE(TAG, "%s", s_fault_text);
}

bool maitetsu_media_faulted(void)
{
    return s_fault;
}

const char *maitetsu_media_fault_text(void)
{
    return s_fault_text[0] ? s_fault_text : "媒体解码失败";
}

// 描述符指针 → 图像下标；不是我们的描述符就返回 -1（绝不让别的解码器
// 把描述符本身当成像素数据）。
static int descriptor_index(const void *src)
{
    const uintptr_t base = (uintptr_t)&s_descriptors[0];
    const uintptr_t addr = (uintptr_t)src;
    if (addr < base || addr >= base + sizeof(s_descriptors)) return -1;
    if ((addr - base) % sizeof(lv_image_dsc_t) != 0) return -1;
    const uint16_t index = (uint16_t)((addr - base) / sizeof(lv_image_dsc_t));
    if (index >= s_descriptor_count) return -1;
    return (int)index;
}

static lv_result_t decoder_info(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc,
                                lv_image_header_t *header)
{
    LV_UNUSED(decoder);
    if (dsc->src_type != LV_IMAGE_SRC_VARIABLE || !dsc->src) return LV_RESULT_INVALID;
    const int index = descriptor_index(dsc->src);
    if (index < 0) return LV_RESULT_INVALID;
    const maitetsu_image_t *info = maitetsu_pack_image((uint16_t)index);
    if (!info) {
        set_fault("媒体图像描述无效");
        return LV_RESULT_INVALID;
    }
    const lv_image_dsc_t *src = (const lv_image_dsc_t *)dsc->src;
    *header = src->header;
    header->cf = LV_COLOR_FORMAT_RGB565;
    header->stride = (uint16_t)(info->width * 2u);
    return LV_RESULT_OK;
}

static lv_result_t decoder_open(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc)
{
    if (decoder_info(decoder, dsc, &dsc->header) != LV_RESULT_OK) return LV_RESULT_INVALID;
    const int index = descriptor_index(dsc->src);
    if (index < 0) return LV_RESULT_INVALID;
    media_ctx_t *ctx = lv_malloc(sizeof(*ctx));
    if (!ctx) return LV_RESULT_INVALID;
    memset(ctx, 0, sizeof(*ctx));
    ctx->image = (uint16_t)index;
    dsc->user_data = ctx;
    dsc->decoded = NULL;   // 不向 LVGL/缓存发布整幅图像
    dsc->args.no_cache = true;
    return LV_RESULT_OK;
}

static lv_result_t decoder_area(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc,
                                const lv_area_t *full, lv_area_t *area)
{
    LV_UNUSED(decoder);
    media_ctx_t *ctx = (media_ctx_t *)dsc->user_data;
    if (!ctx) return LV_RESULT_INVALID;
    const maitetsu_image_t *info = maitetsu_pack_image(ctx->image);
    if (!info) {
        set_fault("媒体图像描述无效");
        return LV_RESULT_INVALID;
    }

    // LVGL 把上一次返回的区域写回 area，用它的下沿推进。
    int32_t y = (area->y2 == LV_COORD_MIN) ? (full->y1 > 0 ? full->y1 : 0) : area->y2 + 1;
    if (y < 0) y = 0;
    if (y >= info->height || y > full->y2) return LV_RESULT_INVALID;

    const uint8_t band = (uint8_t)(y / MAITETSU_BAND_ROWS);
    const uint16_t skip = (uint16_t)(y % MAITETSU_BAND_ROWS);
    maitetsu_band_t view;
    if (!maitetsu_pack_band(ctx->image, band, &view)) {
        set_fault("媒体条带校验或解码失败");
        return LV_RESULT_INVALID;
    }
    if (view.width != info->width || view.rows <= skip) {
        set_fault("媒体条带尺寸异常");
        return LV_RESULT_INVALID;
    }

    int32_t rows = (int32_t)view.rows - skip;
    if (rows > full->y2 - y + 1) rows = full->y2 - y + 1;
    if (rows > (int32_t)info->height - y) rows = (int32_t)info->height - y;
    if (rows <= 0) return LV_RESULT_INVALID;

    const uint8_t *data = (const uint8_t *)view.pixels +
                          (size_t)skip * info->width * sizeof(uint16_t);
    const uint32_t stride = (uint32_t)info->width * sizeof(uint16_t);
    if (lv_draw_buf_init(&ctx->draw, info->width, (uint32_t)rows, LV_COLOR_FORMAT_RGB565,
                         stride, (void *)data, stride * (uint32_t)rows) != LV_RESULT_OK) {
        return LV_RESULT_INVALID;
    }
    dsc->decoded = &ctx->draw;
    area->x1 = 0;
    area->y1 = y;
    area->x2 = (int32_t)info->width - 1;
    area->y2 = y + rows - 1;
    return LV_RESULT_OK;
}

static void decoder_close(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc)
{
    LV_UNUSED(decoder);
    lv_free(dsc->user_data);
    dsc->user_data = NULL;
    dsc->decoded = NULL;
}

bool maitetsu_media_init_from(const uint8_t *data, size_t size)
{
    if (s_ready) return true;
    s_fault = false;
    s_fault_text[0] = '\0';
    if (!maitetsu_pack_init(data, size, s_error, sizeof(s_error))) {
        ESP_LOGE(TAG, "媒体包不可用: %s", s_error);
        return false;
    }
    s_descriptor_count = maitetsu_pack_image_count();
    for (uint16_t i = 0; i < s_descriptor_count; ++i) {
        const maitetsu_image_t *info = maitetsu_pack_image(i);
        if (!info) {
            snprintf(s_error, sizeof(s_error), "图像 %u 描述非法", (unsigned)i);
            ESP_LOGE(TAG, "%s", s_error);
            return false;
        }
        lv_image_dsc_t *desc = &s_descriptors[i];
        memset(desc, 0, sizeof(*desc));
        // 描述符本身不含像素：cf/stride/data_size 留空，只有我们的 info
        // 回调会给出 RGB565 视图。任何兜底解码器都会因格式未知而拒绝。
        desc->header.magic = LV_IMAGE_HEADER_MAGIC;
        desc->header.cf = LV_COLOR_FORMAT_UNKNOWN;
        desc->header.w = info->width;
        desc->header.h = info->height;
        desc->header.stride = 0;
        desc->data_size = 0;
        desc->data = (const uint8_t *)desc;   // 仅作身份标识
    }

    lv_image_decoder_t *decoder = lv_image_decoder_create();
    if (!decoder) {
        snprintf(s_error, sizeof(s_error), "图像解码器注册失败");
        return false;
    }
    lv_image_decoder_set_info_cb(decoder, decoder_info);
    lv_image_decoder_set_open_cb(decoder, decoder_open);
    lv_image_decoder_set_get_area_cb(decoder, decoder_area);
    lv_image_decoder_set_close_cb(decoder, decoder_close);
    s_ready = true;
    ESP_LOGI(TAG, "媒体解码器就绪：%u 张图像", (unsigned)s_descriptor_count);
    return true;
}

bool maitetsu_media_ready(void)
{
    return s_ready;
}

const char *maitetsu_media_error(void)
{
    return s_error[0] ? s_error : "媒体包不可用";
}

uint16_t maitetsu_media_image_count(void)
{
    return s_descriptor_count;
}

uint16_t maitetsu_media_scene_count(void)
{
    return (uint16_t)maitetsu_scene_count;
}

const lv_image_dsc_t *maitetsu_media_image(uint16_t index)
{
    if (!s_ready || index >= s_descriptor_count) return NULL;
    return &s_descriptors[index];
}

const maitetsu_scene_t *maitetsu_media_scene(uint16_t scene)
{
    if (!s_ready || scene >= maitetsu_scene_count) return NULL;
    const maitetsu_scene_t *row = &maitetsu_scenes[scene];
    if ((uint32_t)row->first_patch + row->patch_count > (uint32_t)maitetsu_patch_count) return NULL;
    if (row->patch_count > 2) return NULL;
    return row;
}

const lv_image_dsc_t *maitetsu_media_scene_bg(uint16_t scene)
{
    const maitetsu_scene_t *row = maitetsu_media_scene(scene);
    if (!row || row->bg == MAITETSU_SCENE_NONE) return NULL;
    return maitetsu_media_image(row->bg);
}

const lv_image_dsc_t *maitetsu_media_scene_patch(uint16_t scene, uint8_t patch)
{
    const maitetsu_scene_t *row = maitetsu_media_scene(scene);
    if (!row || patch >= row->patch_count) return NULL;
    return maitetsu_media_image(maitetsu_patches[row->first_patch + patch].image);
}

const lv_image_dsc_t *maitetsu_media_cg(uint16_t cg)
{
    if (!s_ready || cg >= (uint16_t)maitetsu_cg_count) return NULL;
    return maitetsu_media_image(maitetsu_cg_images[cg]);
}

const lv_image_dsc_t *maitetsu_media_cg_thumb(uint16_t cg)
{
    if (!s_ready || cg >= (uint16_t)maitetsu_cg_count) return NULL;
    return maitetsu_media_image(maitetsu_cg_thumbs[cg]);
}

const lv_image_dsc_t *maitetsu_media_title(void)
{
    return maitetsu_media_image(maitetsu_title_media);
}
