#include "nine_image.h"
#include "nine_format.h"
#include "tjpgd/tjpgd.h"

#include <limits.h>
#include <string.h>

#if !defined(NINE_IMAGE_HOST) && !defined(NINE_MEDIA_HOST)
#include "esp_timer.h"
#endif

#ifdef NINE_IMAGE_HOST
#include <stdlib.h>
#else
#include "lvgl.h"
#include "esp_heap_caps.h"
#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif
#endif

// No allocation exceeds 25,600 bytes. The decoder streams MCU pixels directly
// into six independent LVGL images; neither decoding nor display needs a second
// full canvas or a transform cache. LVGL must use a partial display buffer.
#define BAND_BYTES (NINE_IMAGE_W * NINE_IMAGE_BAND_H * 2u)
#define JPEG_POOL_BYTES 4096u
#define INDEX_WIDTH_MAX 320u
#define INDEX_ROWS_MAX 8u
static uint16_t *s_bands[NINE_IMAGE_BANDS];
static uint8_t s_jpeg_pool[JPEG_POOL_BYTES];
static uint8_t s_indices[INDEX_WIDTH_MAX * INDEX_ROWS_MAX];
static nine_image_source_t s_source;
static bool s_bound;
static bool s_busy;
static const uint8_t *s_script;
static size_t s_script_size;
static bool s_perf_active;
static nine_image_perf_t s_perf;
#ifndef NINE_IMAGE_HOST
static lv_obj_t *s_objects[NINE_IMAGE_BANDS];
static lv_image_dsc_t s_descriptors[NINE_IMAGE_BANDS];
#else
static size_t s_largest_alloc;
static unsigned s_alloc_count, s_fail_alloc;
static int s_fail_show;
static int s_skip_decode;
#endif

// Shared non-reentrant inflater owned by nine_pack.c. All image/VM work runs in
// the same LVGL caller; the audio task never calls this decoder.
extern size_t nine_inflate(void *dst, size_t capacity, const uint8_t *src, size_t len);

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static bool range_ok(size_t size, uint32_t off, uint32_t len)
{
    return off <= size && len <= size - off;
}

// Table-driven CRC32 with the source polynomial. This is the same value as the
// original bitwise loop, but boot no longer blocks the idle task for seconds
// while validating the whole pack and every asset.
static const uint32_t s_crc32_table[256] = {
    0x00000000u, 0x77073096u, 0xee0e612cu, 0x990951bau, 0x076dc419u, 0x706af48fu, 0xe963a535u, 0x9e6495a3u,
    0x0edb8832u, 0x79dcb8a4u, 0xe0d5e91eu, 0x97d2d988u, 0x09b64c2bu, 0x7eb17cbdu, 0xe7b82d07u, 0x90bf1d91u,
    0x1db71064u, 0x6ab020f2u, 0xf3b97148u, 0x84be41deu, 0x1adad47du, 0x6ddde4ebu, 0xf4d4b551u, 0x83d385c7u,
    0x136c9856u, 0x646ba8c0u, 0xfd62f97au, 0x8a65c9ecu, 0x14015c4fu, 0x63066cd9u, 0xfa0f3d63u, 0x8d080df5u,
    0x3b6e20c8u, 0x4c69105eu, 0xd56041e4u, 0xa2677172u, 0x3c03e4d1u, 0x4b04d447u, 0xd20d85fdu, 0xa50ab56bu,
    0x35b5a8fau, 0x42b2986cu, 0xdbbbc9d6u, 0xacbcf940u, 0x32d86ce3u, 0x45df5c75u, 0xdcd60dcfu, 0xabd13d59u,
    0x26d930acu, 0x51de003au, 0xc8d75180u, 0xbfd06116u, 0x21b4f4b5u, 0x56b3c423u, 0xcfba9599u, 0xb8bda50fu,
    0x2802b89eu, 0x5f058808u, 0xc60cd9b2u, 0xb10be924u, 0x2f6f7c87u, 0x58684c11u, 0xc1611dabu, 0xb6662d3du,
    0x76dc4190u, 0x01db7106u, 0x98d220bcu, 0xefd5102au, 0x71b18589u, 0x06b6b51fu, 0x9fbfe4a5u, 0xe8b8d433u,
    0x7807c9a2u, 0x0f00f934u, 0x9609a88eu, 0xe10e9818u, 0x7f6a0dbbu, 0x086d3d2du, 0x91646c97u, 0xe6635c01u,
    0x6b6b51f4u, 0x1c6c6162u, 0x856530d8u, 0xf262004eu, 0x6c0695edu, 0x1b01a57bu, 0x8208f4c1u, 0xf50fc457u,
    0x65b0d9c6u, 0x12b7e950u, 0x8bbeb8eau, 0xfcb9887cu, 0x62dd1ddfu, 0x15da2d49u, 0x8cd37cf3u, 0xfbd44c65u,
    0x4db26158u, 0x3ab551ceu, 0xa3bc0074u, 0xd4bb30e2u, 0x4adfa541u, 0x3dd895d7u, 0xa4d1c46du, 0xd3d6f4fbu,
    0x4369e96au, 0x346ed9fcu, 0xad678846u, 0xda60b8d0u, 0x44042d73u, 0x33031de5u, 0xaa0a4c5fu, 0xdd0d7cc9u,
    0x5005713cu, 0x270241aau, 0xbe0b1010u, 0xc90c2086u, 0x5768b525u, 0x206f85b3u, 0xb966d409u, 0xce61e49fu,
    0x5edef90eu, 0x29d9c998u, 0xb0d09822u, 0xc7d7a8b4u, 0x59b33d17u, 0x2eb40d81u, 0xb7bd5c3bu, 0xc0ba6cadu,
    0xedb88320u, 0x9abfb3b6u, 0x03b6e20cu, 0x74b1d29au, 0xead54739u, 0x9dd277afu, 0x04db2615u, 0x73dc1683u,
    0xe3630b12u, 0x94643b84u, 0x0d6d6a3eu, 0x7a6a5aa8u, 0xe40ecf0bu, 0x9309ff9du, 0x0a00ae27u, 0x7d079eb1u,
    0xf00f9344u, 0x8708a3d2u, 0x1e01f268u, 0x6906c2feu, 0xf762575du, 0x806567cbu, 0x196c3671u, 0x6e6b06e7u,
    0xfed41b76u, 0x89d32be0u, 0x10da7a5au, 0x67dd4accu, 0xf9b9df6fu, 0x8ebeeff9u, 0x17b7be43u, 0x60b08ed5u,
    0xd6d6a3e8u, 0xa1d1937eu, 0x38d8c2c4u, 0x4fdff252u, 0xd1bb67f1u, 0xa6bc5767u, 0x3fb506ddu, 0x48b2364bu,
    0xd80d2bdau, 0xaf0a1b4cu, 0x36034af6u, 0x41047a60u, 0xdf60efc3u, 0xa867df55u, 0x316e8eefu, 0x4669be79u,
    0xcb61b38cu, 0xbc66831au, 0x256fd2a0u, 0x5268e236u, 0xcc0c7795u, 0xbb0b4703u, 0x220216b9u, 0x5505262fu,
    0xc5ba3bbeu, 0xb2bd0b28u, 0x2bb45a92u, 0x5cb36a04u, 0xc2d7ffa7u, 0xb5d0cf31u, 0x2cd99e8bu, 0x5bdeae1du,
    0x9b64c2b0u, 0xec63f226u, 0x756aa39cu, 0x026d930au, 0x9c0906a9u, 0xeb0e363fu, 0x72076785u, 0x05005713u,
    0x95bf4a82u, 0xe2b87a14u, 0x7bb12baeu, 0x0cb61b38u, 0x92d28e9bu, 0xe5d5be0du, 0x7cdcefb7u, 0x0bdbdf21u,
    0x86d3d2d4u, 0xf1d4e242u, 0x68ddb3f8u, 0x1fda836eu, 0x81be16cdu, 0xf6b9265bu, 0x6fb077e1u, 0x18b74777u,
    0x88085ae6u, 0xff0f6a70u, 0x66063bcau, 0x11010b5cu, 0x8f659effu, 0xf862ae69u, 0x616bffd3u, 0x166ccf45u,
    0xa00ae278u, 0xd70dd2eeu, 0x4e048354u, 0x3903b3c2u, 0xa7672661u, 0xd06016f7u, 0x4969474du, 0x3e6e77dbu,
    0xaed16a4au, 0xd9d65adcu, 0x40df0b66u, 0x37d83bf0u, 0xa9bcae53u, 0xdebb9ec5u, 0x47b2cf7fu, 0x30b5ffe9u,
    0xbdbdf21cu, 0xcabac28au, 0x53b39330u, 0x24b4a3a6u, 0xbad03605u, 0xcdd70693u, 0x54de5729u, 0x23d967bfu,
    0xb3667a2eu, 0xc4614ab8u, 0x5d681b02u, 0x2a6f2b94u, 0xb40bbe37u, 0xc30c8ea1u, 0x5a05df1bu, 0x2d02ef8du,
};

static uint32_t crc32_zero(const uint8_t *p, size_t len, size_t zero_off, size_t zeros)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < len; ++i) {
        uint8_t b = i >= zero_off && i - zero_off < zeros ? 0 : p[i];
        crc = (crc >> 8) ^ s_crc32_table[(crc ^ b) & 0xffu];
    }
    return ~crc;
}

static void *pixel_alloc(void)
{
#ifdef NINE_IMAGE_HOST
    ++s_alloc_count;
    if (s_alloc_count == s_fail_alloc) return NULL;
    if (s_largest_alloc < BAND_BYTES) s_largest_alloc = BAND_BYTES;
    return malloc(BAND_BYTES);
#else
    return heap_caps_malloc(BAND_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
}

static void pixel_free(void *p)
{
#ifdef NINE_IMAGE_HOST
    free(p);
#else
    heap_caps_free(p);
#endif
}

static uint16_t *row_at(unsigned y)
{
    return s_bands[y / NINE_IMAGE_BAND_H] +
           (size_t)(y % NINE_IMAGE_BAND_H) * NINE_IMAGE_W;
}

static void invalidate(void)
{
#ifndef NINE_IMAGE_HOST
    for (unsigned i = 0; i < NINE_IMAGE_BANDS; ++i)
        if (s_objects[i]) lv_obj_invalidate(s_objects[i]);
#endif
}

bool nine_image_init(struct _lv_obj_t *parent)
{
    if (s_bands[0]) return true;
#ifndef NINE_IMAGE_HOST
    if (!parent) return false;
#else
    (void)parent;
#endif
    for (unsigned i = 0; i < NINE_IMAGE_BANDS; ++i) {
        s_bands[i] = pixel_alloc();
        if (!s_bands[i]) { nine_image_deinit(); return false; }
        memset(s_bands[i], 0, BAND_BYTES);
#ifndef NINE_IMAGE_HOST
        lv_image_dsc_t *d = &s_descriptors[i];
        memset(d, 0, sizeof(*d));
        d->header.magic = LV_IMAGE_HEADER_MAGIC;
        d->header.cf = LV_COLOR_FORMAT_RGB565;
        d->header.w = NINE_IMAGE_W;
        d->header.h = NINE_IMAGE_BAND_H;
        d->header.stride = NINE_IMAGE_W * 2;
        d->data_size = BAND_BYTES;
        d->data = (const uint8_t *)s_bands[i];
        s_objects[i] = lv_image_create(parent);
        if (!s_objects[i]) { nine_image_deinit(); return false; }
        lv_image_set_src(s_objects[i], d);
        lv_obj_set_pos(s_objects[i], 0, i * NINE_IMAGE_BAND_H);
        lv_obj_set_size(s_objects[i], NINE_IMAGE_W, NINE_IMAGE_BAND_H);
        lv_obj_remove_flag(s_objects[i], LV_OBJ_FLAG_SCROLLABLE);
#endif
    }
    return true;
}

void nine_image_deinit(void)
{
    for (unsigned i = 0; i < NINE_IMAGE_BANDS; ++i) {
#ifndef NINE_IMAGE_HOST
        if (s_objects[i]) lv_obj_delete(s_objects[i]);
        s_objects[i] = NULL;
        memset(&s_descriptors[i], 0, sizeof(s_descriptors[i]));
#endif
        if (s_bands[i]) pixel_free(s_bands[i]);
        s_bands[i] = NULL;
    }
    memset(&s_source, 0, sizeof(s_source));
    s_bound = false;
    s_script = NULL;
    s_script_size = 0;
}

static uint32_t now_us(void)
{
#if defined(NINE_IMAGE_HOST) || defined(NINE_MEDIA_HOST)
    return 0;
#else
    int64_t t = esp_timer_get_time();
    return t <= 0 ? 0 : (uint32_t)t;
#endif
}

static void perf_take(uint32_t *max_us, uint32_t *total_us, uint32_t elapsed)
{
    if (!s_perf_active) return;
    if (elapsed > *max_us) *max_us = elapsed;
    *total_us += elapsed;
}

void nine_image_perf_reset(void)
{
    memset(&s_perf, 0, sizeof(s_perf));
    s_perf_active = true;
}

void nine_image_perf_get(nine_image_perf_t *out)
{
    if (out) *out = s_perf;
}

static void clear_pixels(uint16_t color)
{
    for (unsigned y = 0; y < NINE_IMAGE_H; ++y) {
        uint16_t *row = row_at(y);
        for (unsigned x = 0; x < NINE_IMAGE_W; ++x) row[x] = color;
    }
}

void nine_image_clear(uint16_t color)
{
    if (!s_bands[0]) return;
    clear_pixels(color);
    invalidate();
}

void nine_image_set_visible(bool visible)
{
#ifndef NINE_IMAGE_HOST
    for (unsigned i = 0; i < NINE_IMAGE_BANDS; ++i) {
        if (!s_objects[i]) continue;
        if (visible) lv_obj_remove_flag(s_objects[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_objects[i], LV_OBJ_FLAG_HIDDEN);
    }
#else
    (void)visible;
#endif
}

typedef struct {
    uint32_t kind, w, h, off, len;
} asset_t;

static bool asset_at(const nine_image_source_t *s, uint32_t image, asset_t *out)
{
    if (!s || !s->data || s->size < 128 || image >= rd32(s->data + 20)) return false;
    uint32_t table = rd32(s->data + 24);
    uint64_t pos = (uint64_t)table + (uint64_t)image * 32u;
    if (pos > UINT32_MAX || !range_ok(s->size, (uint32_t)pos, 32)) return false;
    const uint8_t *p = s->data + pos;
    out->kind = rd32(p); out->w = rd32(p + 4); out->h = rd32(p + 8);
    out->off = rd32(p + 12); out->len = rd32(p + 16);
    if (!out->w || !out->h || out->w > INDEX_WIDTH_MAX || out->h > 2048 ||
        !range_ok(s->size, out->off, out->len)) return false;
    const uint32_t start = now_us();
    const uint32_t crc = crc32_zero(s->data + out->off, out->len, SIZE_MAX, 0);
    perf_take(&s_perf.crc_max_us, &s_perf.crc_total_us, now_us() - start);
    if (rd32(p + 20) != crc) return false;
    return out->kind == NINE_IMG_JPEG || out->kind == NINE_IMG_INDEXED;
}

static void blend(unsigned x, unsigned y, uint16_t src, unsigned opacity)
{
    uint16_t *dst = &row_at(y)[x];
    if (opacity == 255) { *dst = src; return; }
    if (!opacity) return;
    uint16_t d = *dst;
    unsigned inv = 255 - opacity;
    unsigned r = (((src >> 11) & 31) * opacity + ((d >> 11) & 31) * inv + 127) / 255;
    unsigned g = (((src >> 5) & 63) * opacity + ((d >> 5) & 63) * inv + 127) / 255;
    unsigned b = ((src & 31) * opacity + (d & 31) * inv + 127) / 255;
    *dst = (uint16_t)((r << 11) | (g << 5) | b);
}

// MCU/source rectangles map to destination ranges using ceil, so downscaling
// drops pixels intentionally and upscaling fills every output pixel exactly once.
typedef struct {
    const uint8_t *data;
    size_t size, pos;
    asset_t asset;
    nine_image_layer_t layer;
    bool write;
} jpeg_ctx_t;

static size_t jpeg_input(JDEC *jd, uint8_t *buffer, size_t n)
{
    jpeg_ctx_t *c = jd->device;
    if (n > c->size - c->pos) n = c->size - c->pos;
    if (buffer && n) memcpy(buffer, c->data + c->pos, n);
    c->pos += n;
    return n;
}

static uint16_t bgr565_to_rgb565(uint16_t v)
{
    // TJpgDec's RGB565 conversion consumes its BGR byte order, so the packed
    // word is BGR565. LVGL and the reference compositor expect RGB565.
    return (uint16_t)(((v & 0x001fu) << 11) | (v & 0x07e0u) | ((v & 0xf800u) >> 11));
}

static int jpeg_output(JDEC *jd, void *bitmap, JRECT *rect)
{
    jpeg_ctx_t *c = jd->device;
    if (!c->write) return 1;
    const uint16_t *px = bitmap;
    const uint32_t sw = c->asset.w, sh = c->asset.h;
    const uint32_t dw = c->layer.width, dh = c->layer.height;
    uint32_t x0 = ((uint32_t)rect->left * dw + sw - 1) / sw;
    uint32_t x1 = (((uint32_t)rect->right + 1) * dw + sw - 1) / sw;
    uint32_t y0 = ((uint32_t)rect->top * dh + sh - 1) / sh;
    uint32_t y1 = (((uint32_t)rect->bottom + 1) * dh + sh - 1) / sh;
    unsigned stride = rect->right - rect->left + 1u;
    for (uint32_t y = y0; y < y1; ++y) {
        int64_t dy = (int64_t)c->layer.y + y;
        if (dy < 0 || dy >= NINE_IMAGE_H) continue;
        unsigned sy = y * sh / dh - rect->top;
        for (uint32_t x = x0; x < x1; ++x) {
            int64_t dx = (int64_t)c->layer.x + x;
            if (dx < 0 || dx >= NINE_IMAGE_W) continue;
            unsigned sx = x * sw / dw - rect->left;
            blend((unsigned)dx, (unsigned)dy, bgr565_to_rgb565(px[sy * stride + sx]),
                  c->layer.opacity);
        }
    }
    return 1;
}

static bool jpeg_decode(const nine_image_source_t *s, const asset_t *a,
                        const nine_image_layer_t *layer, bool write)
{
    if (a->len < 4 || s->data[a->off] != 0xff || s->data[a->off + 1] != 0xd8 ||
        s->data[a->off + a->len - 2] != 0xff || s->data[a->off + a->len - 1] != 0xd9)
        return false;
    jpeg_ctx_t c = { .data = s->data + a->off, .size = a->len, .asset = *a,
                     .layer = *layer, .write = write };
    JDEC decoder;
    if (jd_prepare(&decoder, jpeg_input, s_jpeg_pool, sizeof(s_jpeg_pool), &c) != JDR_OK ||
        decoder.width != a->w || decoder.height != a->h) return false;
    return jd_decomp(&decoder, jpeg_output, 0) == JDR_OK;
}

static bool indexed_decode(const nine_image_source_t *s, const asset_t *a,
                           const nine_image_layer_t *layer, bool write)
{
    if (a->len < 12) return false;
    const uint8_t *p = s->data + a->off;
    uint32_t palette_count = rd32(p), rows = rd32(p + 4), blocks = rd32(p + 8);
    if (!palette_count || palette_count > 256 || !rows || rows > INDEX_ROWS_MAX ||
        blocks != (a->h + rows - 1) / rows) return false;
    uint64_t table_size = 12u + (uint64_t)palette_count * 4u + (uint64_t)blocks * 8u;
    if (table_size > a->len) return false;
    const uint8_t *palette = p + 12;
    const uint8_t *table = palette + palette_count * 4u;
    for (uint32_t block = 0; block < blocks; ++block) {
        uint32_t off = rd32(table + block * 8), len = rd32(table + block * 8 + 4);
        unsigned sy0 = block * rows;
        unsigned nrows = a->h - sy0 < rows ? a->h - sy0 : rows;
        size_t raw = (size_t)a->w * nrows;
        if (off < a->off || !range_ok(s->size, off, len) ||
            (uint64_t)off + len > (uint64_t)a->off + a->len ||
            off - a->off < table_size || !len || raw > sizeof(s_indices) ||
            nine_inflate(s_indices, raw, s->data + off, len) != raw) return false;
        for (size_t i = 0; i < raw; ++i) if (s_indices[i] >= palette_count) return false;
        if (!write) continue;
        uint32_t y0 = (sy0 * layer->height + a->h - 1) / a->h;
        uint32_t y1 = ((sy0 + nrows) * layer->height + a->h - 1) / a->h;
        for (uint32_t y = y0; y < y1; ++y) {
            int64_t dy = (int64_t)layer->y + y;
            if (dy < 0 || dy >= NINE_IMAGE_H) continue;
            unsigned sy = y * a->h / layer->height - sy0;
            for (uint32_t x = 0; x < layer->width; ++x) {
                int64_t dx = (int64_t)layer->x + x;
                if (dx < 0 || dx >= NINE_IMAGE_W) continue;
                unsigned sx = x * a->w / layer->width;
                const uint8_t *rgba = palette + (size_t)s_indices[sy * a->w + sx] * 4;
                unsigned opa = (rgba[3] * layer->opacity + 127) / 255;
                uint16_t rgb = (uint16_t)(((rgba[0] >> 3) << 11) |
                                         ((rgba[1] >> 2) << 5) | (rgba[2] >> 3));
                blend((unsigned)dx, (unsigned)dy, rgb, opa);
            }
        }
    }
    return true;
}

static bool layer_decode(const nine_image_source_t *s, const nine_image_layer_t *layer,
                         bool write)
{
    if (!layer->width || !layer->height || layer->width > 4096 ||
        layer->height > 4096 || layer->opacity > 255) return false;
    asset_t a;
    if (!asset_at(s, layer->image, &a)) return false;
    return a.kind == NINE_IMG_JPEG ? jpeg_decode(s, &a, layer, write) :
                                   indexed_decode(s, &a, layer, write);
}

bool nine_image_bind_source(const nine_image_source_t *s)
{
    if (s_busy || !s || !s->data || s->size < 128 || s->size > UINT32_MAX ||
        memcmp(s->data, NINE_IMAGE_MAGIC, 8) || rd32(s->data + 8) != NINE_FORMAT_VERSION ||
        rd32(s->data + 12) != s->size ||
        rd32(s->data + 16) != crc32_zero(s->data, s->size, 16, 4)) return false;
    const unsigned counts[] = {20, 28, 36};
    const unsigned strides[] = {32, 12, 32};
    for (unsigned i = 0; i < 3; ++i) {
        uint32_t n = rd32(s->data + counts[i]), off = rd32(s->data + counts[i] + 4);
        if (n > UINT32_MAX / strides[i] || !range_ok(s->size, off, n * strides[i]) ||
            (n && off < 128)) return false;
    }
    s_busy = true;
    bool ok = true;
    for (uint32_t i = 0; ok && i < rd32(s->data + 20); ++i) {
        asset_t a;
        ok = asset_at(s, i, &a);
#if defined(ESP_PLATFORM) && !defined(NINE_IMAGE_HOST)
        // Yield to the idle/task watchdog during the 1,057-asset CRC scan.
        // This is a real scheduler yield, not a WDT feed or a skipped check.
        if ((i & 63u) == 0) vTaskDelay(1);
#endif
        // Boot validates structure and payload CRC. JPEG/zlib decode validation
        // is performed on demand before any displayed pixels are overwritten.
    }
    for (unsigned i = 0; ok && i < 4; ++i) {
        uint32_t id = rd32(s->data + 44 + i * 4);
        if (id != NINE_NONE && id >= rd32(s->data + 20)) ok = false;
    }
    uint32_t opn = rd32(s->data + 28), opo = rd32(s->data + 32);
    uint64_t last = 0;
    for (uint32_t i = 0; ok && i < opn; ++i) {
        const uint8_t *p = s->data + opo + i * 12;
        uint32_t image = rd32(p), t = rd32(p + 4), duration = rd32(p + 8);
        if (image >= rd32(s->data + 20) || !duration || t < last ||
            (uint64_t)t + duration > UINT32_MAX) ok = false;
        last = (uint64_t)t + duration;
    }
    if (ok) { s_source = *s; s_bound = true; }
    s_busy = false;
    return ok;
}

static bool script_visual(void *ctx, uint32_t visual, nine_image_layer_t *layers,
                          size_t capacity, size_t *count)
{
    (void)ctx;
    if (!s_script || !count || visual >= rd32(s_script + 60)) return false;
    const uint8_t *v = s_script + rd32(s_script + 64) + (size_t)visual * 8;
    uint32_t first = rd32(v), n = rd32(v + 4), total = rd32(s_script + 68);
    if (n > capacity || first > total || n > total - first) return false;
    uint32_t table = rd32(s_script + 72);
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t *p = s_script + table + (size_t)(first + i) * 24;
        layers[i].image = rd32(p);
        layers[i].x = (int32_t)rd32(p + 4);
        layers[i].y = (int32_t)rd32(p + 8);
        layers[i].width = rd32(p + 12);
        layers[i].height = rd32(p + 16);
        layers[i].opacity = rd32(p + 20);
    }
    *count = n;
    return true;
}

bool nine_image_bind_bundle(const uint8_t *script, size_t script_size,
                            const uint8_t *images, size_t image_size)
{
    if (!script || script_size < 256 || script_size > UINT32_MAX ||
        memcmp(script, NINE_SCRIPT_MAGIC, 8) || rd32(script + 8) != NINE_FORMAT_VERSION ||
        rd32(script + 12) != script_size ||
        rd32(script + 60) > UINT32_MAX / 8 || rd32(script + 68) > UINT32_MAX / 24 ||
        !range_ok(script_size, rd32(script + 64), rd32(script + 60) * 8) ||
        !range_ok(script_size, rd32(script + 72), rd32(script + 68) * 24)) return false;
    nine_image_source_t source = { .data = images, .size = image_size,
                                   .visual = script_visual };
    if (!nine_image_bind_source(&source)) return false;
    s_script = script;
    s_script_size = script_size;
    return true;
}

bool nine_image_validate_asset(uint32_t image)
{
    if (!s_bound || s_busy) return false;
    s_busy = true;
    asset_t a;
    bool ok = asset_at(&s_source, image, &a);
    if (ok) {
        nine_image_layer_t layer = { .image = image, .width = a.w, .height = a.h,
                                     .opacity = 255 };
        ok = layer_decode(&s_source, &layer, false);
    }
    s_busy = false;
    return ok;
}

// Single-resource fast path for title/OP/CG frames. asset_at performs the one
// strict CRC/bounds check; the one decode writes directly into the bands. No
// invalidation occurs until the decode has completed, and a failed decode is
// cleared before the single invalidation, so a half-frame can never reach LVGL.
static bool show_single_asset(const asset_t *a, const nine_image_layer_t *layer)
{
    if (!s_bound || !a || !layer || s_busy || !s_bands[0]) return false;
#ifdef NINE_IMAGE_HOST
    if (s_fail_show) {
        clear_pixels(0);
        invalidate();
        return false;
    }
    if (s_skip_decode) return true;
#endif
    s_busy = true;
    uint32_t t0 = now_us();
    clear_pixels(0);
    perf_take(&s_perf.clear_max_us, &s_perf.clear_total_us, now_us() - t0);
    t0 = now_us();
    bool ok = a->kind == NINE_IMG_JPEG
                  ? jpeg_decode(&s_source, a, layer, true)
                  : indexed_decode(&s_source, a, layer, true);
    perf_take(&s_perf.decode_max_us, &s_perf.decode_total_us, now_us() - t0);
    if (!ok) clear_pixels(0);
    t0 = now_us();
    invalidate();
    perf_take(&s_perf.invalidate_max_us, &s_perf.invalidate_total_us, now_us() - t0);
    ++s_perf.frames;
    s_busy = false;
    return ok;
}

static bool show_layers(const nine_image_layer_t *layers, size_t count)
{
    if (!s_bound || s_busy || !s_bands[0] || count > NINE_IMAGE_LAYER_MAX) return false;
#ifdef NINE_IMAGE_HOST
    if (s_fail_show) {
        nine_image_clear(0);
        return false;
    }
    if (s_skip_decode) return true;
#endif
    s_busy = true;
    bool ok = true;
    for (size_t i = 0; ok && i < count; ++i) ok = layer_decode(&s_source, &layers[i], false);
    if (ok) {
        nine_image_clear(0);
        for (size_t i = 0; ok && i < count; ++i) ok = layer_decode(&s_source, &layers[i], true);
    }
    // A failed frame is cleared: never leave stale art that contradicts the
    // current text, and never publish a half-composited frame.
    if (!ok) nine_image_clear(0);
    invalidate();
    s_busy = false;
    return ok;
}

bool nine_image_show_visual(uint32_t visual)
{
    if (!s_bound || !s_source.visual) return false;
    nine_image_layer_t layers[NINE_IMAGE_LAYER_MAX];
    size_t count = 0;
    if (!s_source.visual(s_source.ctx, visual, layers, NINE_IMAGE_LAYER_MAX, &count) ||
        count > NINE_IMAGE_LAYER_MAX) return false;
    return show_layers(layers, count);
}

bool nine_image_show_asset(uint32_t image)
{
    asset_t a;
    if (!s_bands[0]) return false;
    if (!s_bound || s_busy) return false;
    if (!asset_at(&s_source, image, &a)) {
        clear_pixels(0);
        invalidate();
        return false;
    }
    uint32_t w = NINE_IMAGE_W, h = (uint32_t)((uint64_t)a.h * w / a.w);
    if (h > NINE_IMAGE_H) { h = NINE_IMAGE_H; w = (uint32_t)((uint64_t)a.w * h / a.h); }
    if (!w) w = 1;
    if (!h) h = 1;
    // The art area is the top rows; keep the full 16:9 composition and leave
    // the lower strip for the dialogue panel/caption.
    nine_image_layer_t layer = { .image = image, .x = (NINE_IMAGE_W - w) / 2,
        .y = 0, .width = w, .height = h, .opacity = 255 };
    return show_single_asset(&a, &layer);
}

bool nine_image_show_asset_top(uint32_t image)
{
    asset_t a;
    if (!s_bands[0]) return false;
    if (!s_bound || s_busy) return false;
    if (!asset_at(&s_source, image, &a)) {
        clear_pixels(0);
        invalidate();
        return false;
    }
    uint32_t w = NINE_IMAGE_W, h = (uint32_t)((uint64_t)a.h * w / a.w);
    if (h > NINE_IMAGE_H) { h = NINE_IMAGE_H; w = (uint32_t)((uint64_t)a.w * h / a.h); }
    if (!w) w = 1;
    if (!h) h = 1;
    nine_image_layer_t layer = { .image = image, .x = (NINE_IMAGE_W - w) / 2,
        .y = 0, .width = w, .height = h, .opacity = 255 };
    return show_single_asset(&a, &layer);
}

size_t nine_image_canvas_bytes(void) { return s_bands[0] ? BAND_BYTES * NINE_IMAGE_BANDS : 0; }
size_t nine_image_scratch_bytes(void) { return sizeof(s_indices) + sizeof(s_jpeg_pool); }
uint32_t nine_image_asset_count(void) { return s_bound ? rd32(s_source.data + 20) : 0; }
bool nine_image_title(unsigned index, uint32_t *image)
{
    if (!s_bound || !image || index >= 4) return false;
    *image = rd32(s_source.data + 44 + index * 4);
    return *image != NINE_NONE;
}
const uint8_t *nine_image_package(size_t *size)
{
    if (size) *size = s_bound ? s_source.size : 0;
    return s_bound ? s_source.data : NULL;
}
uint32_t nine_image_op_count(void) { return s_bound ? rd32(s_source.data + 28) : 0; }
bool nine_image_op_frame(uint32_t i, uint32_t *image, uint32_t *timestamp, uint32_t *duration)
{
    if (!s_bound || i >= nine_image_op_count()) return false;
    const uint8_t *p = s_source.data + rd32(s_source.data + 32) + i * 12;
    if (image) *image = rd32(p);
    if (timestamp) *timestamp = rd32(p + 4);
    if (duration) *duration = rd32(p + 8);
    return true;
}
#ifdef NINE_IMAGE_HOST
uint16_t nine_image_test_pixel(unsigned x, unsigned y)
{ return s_bands[0] && x < NINE_IMAGE_W && y < NINE_IMAGE_H ? row_at(y)[x] : 0; }
size_t nine_image_test_largest_allocation(void) { return s_largest_alloc; }
void nine_image_test_fail_allocation(unsigned call)
{ s_alloc_count = 0; s_fail_alloc = call; }
void nine_image_test_fail_show(int fail) { s_fail_show = fail; }
void nine_image_test_disable_decode(int disable) { s_skip_decode = disable; }
#endif
