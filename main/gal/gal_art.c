#include "gal_art.h"
#include "esp_log.h"
#include "src/draw/lv_image_decoder_private.h"
#include "src/libs/tjpgd/tjpgd.h"
#include "src/libs/tjpgd/lv_tjpgd.h"
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_timer.h"
#else
#include <time.h>
#endif

#if JD_FORMAT != 0
#error "gal_art expects LVGL TJpgDec BGR888 MCU output"
#endif

#define SCREEN_W 320
#define SCREEN_H 240
#define THUMB_W 64
#define THUMB_H 40
#define THUMB_COUNT 8
#define THUMB_PIXELS (THUMB_W * THUMB_H)
_Static_assert(THUMB_COUNT * THUMB_PIXELS * 2u <= GAL_ART_ARENA_BYTES, "thumbnail arena overflow");

static const char *TAG = "gal_art";
static uint16_t *s_pixels;
static lv_image_decoder_t *s_decoder;
static lv_image_dsc_t s_large;
static lv_image_dsc_t s_thumbs[THUMB_COUNT];
static const void *s_large_key;
static const void *s_thumb_keys[THUMB_COUNT];
static enum { EMPTY, LARGE, THUMBS } s_mode;
static gal_art_stats_t s_stats;
static _Alignas(4) uint8_t s_jpeg_work[4096];
static JDEC s_jpeg;
static _Alignas(LV_DRAW_BUF_ALIGN) uint16_t s_row_pixels[SCREEN_W];
static lv_draw_buf_t s_row;
static bool s_busy;

typedef struct {
    const uint8_t *input;
    size_t length, position;
    uint16_t *output;
    uint32_t src_w, src_h, dst_w, dst_h;
} jpeg_context_t;
static jpeg_context_t s_context;

static uint64_t now_ms(void)
{
#ifdef ESP_PLATFORM
    return (uint64_t)esp_timer_get_time() / 1000u;
#else
    return (uint64_t)clock() * 1000u / CLOCKS_PER_SEC;
#endif
}

static size_t jpeg_input(JDEC *jd, uint8_t *buffer, size_t count)
{
    jpeg_context_t *c = jd->device;
    size_t left = c->length - c->position;
    if (count > left) count = left;
    if (buffer) memcpy(buffer, c->input + c->position, count);
    c->position += count;
    return count;
}

static int jpeg_output(JDEC *jd, void *bitmap, JRECT *r)
{
    jpeg_context_t *c = jd->device;
    const uint8_t *bgr = bitmap;
    uint32_t block_w = r->right - r->left + 1u;
    uint32_t x0 = (r->left * c->dst_w + c->src_w - 1u) / c->src_w;
    uint32_t x1 = ((r->right + 1u) * c->dst_w + c->src_w - 1u) / c->src_w;
    uint32_t y0 = (r->top * c->dst_h + c->src_h - 1u) / c->src_h;
    uint32_t y1 = ((r->bottom + 1u) * c->dst_h + c->src_h - 1u) / c->src_h;
    for (uint32_t y = y0; y < y1 && y < c->dst_h; ++y) {
        uint32_t sy = y * c->src_h / c->dst_h;
        for (uint32_t x = x0; x < x1 && x < c->dst_w; ++x) {
            uint32_t sx = x * c->src_w / c->dst_w;
            const uint8_t *p = bgr + ((sy - r->top) * block_w + sx - r->left) * 3u;
            c->output[y * c->dst_w + x] = (uint16_t)(((p[2] & 0xf8u) << 8) |
                                                       ((p[1] & 0xfcu) << 3) | (p[0] >> 3));
        }
    }
    return 1;
}

static bool decode(const lv_image_dsc_t *image, uint16_t *out, uint32_t w, uint32_t h)
{
    if (!image || !image->data || image->data_size < 10 || s_busy) return false;
    if (!image->header.w || !image->header.h || image->header.w > SCREEN_W || image->header.h > SCREEN_H) return false;
    s_busy = true;
    uint64_t start = now_ms();
    s_context = (jpeg_context_t){ .input=image->data, .length=image->data_size,
        .output=out, .src_w=image->header.w, .src_h=image->header.h, .dst_w=w, .dst_h=h };
    JRESULT result = jd_prepare(&s_jpeg, jpeg_input, s_jpeg_work, sizeof(s_jpeg_work), &s_context);
    if (result == JDR_OK && (s_jpeg.width != image->header.w || s_jpeg.height != image->header.h)) result = JDR_PAR;
    if (result == JDR_OK) result = jd_decomp(&s_jpeg, jpeg_output, 0);
    s_busy = false;
    if (result != JDR_OK) {
        ESP_LOGE(TAG, "JPEG decode failed: %d", (int)result);
        return false;
    }
    ++s_stats.jpeg_decodes;
    s_stats.last_decode_ms = (uint32_t)(now_ms() - start);
    ESP_LOGI(TAG, "decode %ux%u -> %ux%u: %u ms, total=%u", (unsigned)image->header.w,
             (unsigned)image->header.h, (unsigned)w, (unsigned)h,
             (unsigned)s_stats.last_decode_ms, (unsigned)s_stats.jpeg_decodes);
    return true;
}

static lv_result_t row_info(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc, lv_image_header_t *header)
{
    (void)decoder;
    if (dsc->src != &s_large) return LV_RESULT_INVALID;
    *header = s_large.header;
    return LV_RESULT_OK;
}

static lv_result_t row_open(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc)
{
    (void)decoder;
    if (dsc->src != &s_large || s_mode != LARGE) return LV_RESULT_INVALID;
    dsc->header = s_large.header;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.stride = SCREEN_W * 2;
    dsc->decoded = NULL;
    dsc->args.no_cache = true;
    return LV_RESULT_OK;
}

static lv_result_t row_area(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc,
                            const lv_area_t *full, lv_area_t *area)
{
    (void)decoder;
    if (s_mode != LARGE) return LV_RESULT_INVALID;
    int y = area->y1 == LV_COORD_MIN ? full->y1 : area->y1 + 1;
    if (y < 0) y = 0;
    if (y > full->y2 || y >= SCREEN_H) return LV_RESULT_INVALID;
    area->x1 = full->x1 < 0 ? 0 : full->x1;
    area->x2 = full->x2 >= SCREEN_W ? SCREEN_W-1 : full->x2;
    area->y1 = area->y2 = y;
    if (area->x2 < area->x1) return LV_RESULT_INVALID;
    uint32_t width = (uint32_t)(area->x2 - area->x1 + 1);
    const uint16_t *source = s_pixels + (uint32_t)y * GAL_ART_CACHE_H / SCREEN_H * GAL_ART_CACHE_W;
    for (uint32_t x=0; x<width; ++x) s_row_pixels[x] = source[(x + (uint32_t)area->x1) * GAL_ART_CACHE_W / SCREEN_W];
    lv_draw_buf_init(&s_row, width, 1, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO,
                     s_row_pixels, sizeof(s_row_pixels));
    dsc->decoded = &s_row;
    ++s_stats.output_rows;
    return LV_RESULT_OK;
}

static void row_close(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc)
{
    (void)decoder;
    dsc->decoded = NULL; // The row buffer is static, not owned by an open session.
}

bool gal_art_init(void)
{
    if (s_pixels) return true;
    s_pixels = malloc(GAL_ART_ARENA_BYTES);
    if (!s_pixels) { ESP_LOGE(TAG, "pixel arena allocation failed"); return false; }
    // Never send a JPEG through LVGL's decoder_info(), whose 4 KB local array
    // overflowed the real 7168-byte LVGL task. Direct JPEG decoding above uses
    // static workspace; redraws use only this bounded RGB565 row decoder.
    lv_tjpgd_deinit();
    s_decoder = lv_image_decoder_create();
    if (!s_decoder) { free(s_pixels); s_pixels=NULL; return false; }
    lv_image_decoder_set_info_cb(s_decoder, row_info);
    lv_image_decoder_set_open_cb(s_decoder, row_open);
    lv_image_decoder_set_get_area_cb(s_decoder, row_area);
    lv_image_decoder_set_close_cb(s_decoder, row_close);
    s_decoder->name = "Yakata cached RGB565";
    s_large.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_large.header.cf = LV_COLOR_FORMAT_RAW;
    s_large.header.w = SCREEN_W; s_large.header.h = SCREEN_H;
    s_large.data = (const uint8_t *)s_pixels; s_large.data_size = GAL_ART_ARENA_BYTES;
    ESP_LOGI(TAG, "pixel arena: %u bytes", (unsigned)GAL_ART_ARENA_BYTES);
    return true;
}

const lv_image_dsc_t *gal_art_large(const void *jpeg, bool *changed)
{
    if (changed) *changed = false;
    if (!jpeg || !gal_art_init()) return NULL;
    if (s_mode == LARGE && s_large_key == jpeg) { ++s_stats.cache_hits; return &s_large; }
    s_mode = EMPTY; s_large_key=NULL;
    memset(s_thumb_keys, 0, sizeof(s_thumb_keys));
    if (!decode(jpeg, s_pixels, GAL_ART_CACHE_W, GAL_ART_CACHE_H)) return NULL;
    s_large_key=jpeg; s_mode=LARGE;
    if (changed) *changed=true;
    return &s_large;
}

const lv_image_dsc_t *gal_art_thumb(int slot, const void *jpeg, bool *changed)
{
    if (changed) *changed=false;
    if (slot<0 || slot>=THUMB_COUNT || !jpeg || !gal_art_init()) return NULL;
    const lv_image_dsc_t *image=jpeg;
    if (!image->header.w || !image->header.h || image->header.w>THUMB_W || image->header.h>THUMB_H) return NULL;
    if (s_mode != THUMBS) { memset(s_thumb_keys,0,sizeof(s_thumb_keys)); s_large_key=NULL; s_mode=THUMBS; }
    if (s_thumb_keys[slot]==jpeg) { ++s_stats.cache_hits; return &s_thumbs[slot]; }
    uint16_t *pixels=s_pixels+(uint32_t)slot*THUMB_PIXELS;
    if (!decode(image,pixels,image->header.w,image->header.h)) { s_thumb_keys[slot]=NULL; return NULL; }
    s_thumbs[slot]=(lv_image_dsc_t){.header={.magic=LV_IMAGE_HEADER_MAGIC,.cf=LV_COLOR_FORMAT_RGB565,
        .w=image->header.w,.h=image->header.h,.stride=image->header.w*2},
        .data=(const uint8_t *)pixels,.data_size=image->header.w*image->header.h*2};
    s_thumb_keys[slot]=jpeg;
    if (changed) *changed=true;
    return &s_thumbs[slot];
}

gal_art_stats_t gal_art_stats(void) { return s_stats; }
