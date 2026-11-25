#include "nine_image.h"
#include "nine_crc.h"
#include "nine_format.h"
#include "tjpgd/tjpgd.h"

#include <limits.h>
#include <string.h>

#ifdef NINE_IMAGE_HOST
#include <stdlib.h>
#else
#include "lvgl.h"
#include "esp_heap_caps.h"
#endif

// No allocation exceeds 25,600 bytes. The decoder streams MCU pixels directly
// into five independent LVGL images; neither decoding nor display needs a second
// full canvas or a transform cache. LVGL must use a partial display buffer.
#define BAND_BYTES (NINE_IMAGE_W * NINE_IMAGE_BAND_H * 2u)
#define JPEG_POOL_BYTES 4096u
#define INDEX_WIDTH_MAX 1024u
#define INDEX_ROWS_MAX 8u
static uint16_t *s_bands[NINE_IMAGE_BANDS];
static uint8_t s_jpeg_pool[JPEG_POOL_BYTES];
static uint8_t s_indices[INDEX_WIDTH_MAX * INDEX_ROWS_MAX];
static nine_image_source_t s_source;
static bool s_bound;
static bool s_busy;
// Fast-forward commonly crosses several sentences with the same visual. Keep
// only the visual id/valid flag; the decoded bands are the existing canvas.
// Every operation that can change the canvas invalidates this cache.
static uint32_t s_visual;
static bool s_visual_valid;
static const uint8_t *s_script;
static size_t s_script_size;
#ifndef NINE_IMAGE_HOST
static lv_obj_t *s_objects[NINE_IMAGE_BANDS];
static lv_image_dsc_t s_descriptors[NINE_IMAGE_BANDS];
#else
static size_t s_largest_alloc;
static unsigned s_alloc_count, s_fail_alloc;
static unsigned s_visual_decode_count, s_visual_cache_hit_count;
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
        // The art is decoded at 240x135 and displayed at 320x180 (4/3). A
        // centered 341/256 scale left 1-pixel transform edges at every band
        // boundary (black seams). Use the exact object/border geometry below:
        // 322x37 objects at x=-1/y=36*i, left/top aligned and clipped by the
        // 320x180 stage; 342/256 is the nearest equal X/Y scale. The one-row
        // overlap is covered by the next band; no source or pool size changes.
        lv_obj_set_pos(s_objects[i], -1, (int)(i * (NINE_IMAGE_BAND_H * 320 / NINE_IMAGE_W)));
        lv_obj_set_size(s_objects[i], 322, 37);
        lv_image_set_inner_align(s_objects[i], LV_IMAGE_ALIGN_STRETCH);
        lv_image_set_scale_x(s_objects[i], 342);
        lv_image_set_scale_y(s_objects[i], 342);
        lv_image_set_offset_y(s_objects[i], 1);
        // UI pages are created by gal_ui_create(); the bands sit directly above
        // its opaque gradient background and below every page/panel.
        lv_obj_move_to_index(s_objects[i], (int)i + 1);
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
    s_visual_valid = false;
    s_script = NULL;
    s_script_size = 0;
}

void nine_image_invalidate_cache(void)
{
    s_visual_valid = false;
}

void nine_image_clear(uint16_t color)
{
    s_visual_valid = false;
    if (!s_bands[0]) return;
    for (unsigned y = 0; y < NINE_IMAGE_H; ++y) {
        uint16_t *row = row_at(y);
        for (unsigned x = 0; x < NINE_IMAGE_W; ++x) row[x] = color;
    }
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

void nine_image_set_op_fast(bool fast)
{
#ifndef NINE_IMAGE_HOST
    for (unsigned i = 0; i < NINE_IMAGE_BANDS; ++i) {
        if (s_objects[i]) lv_image_set_antialias(s_objects[i], !fast);
    }
#else
    (void)fast;
#endif
}

typedef struct {
    uint32_t kind, w, h, off, len, crc;
} asset_t;

static bool asset_at_counted(const nine_image_source_t *s, uint32_t image,
                             asset_t *out, bool *used_crc)
{
    if (used_crc) *used_crc = false;
    if (!s || !s->data || s->size < 128 || image >= rd32(s->data + 20)) return false;
    uint32_t table = rd32(s->data + 24);
    uint64_t pos = (uint64_t)table + (uint64_t)image * 32u;
    if (pos > UINT32_MAX || !range_ok(s->size, (uint32_t)pos, 32)) return false;
    const uint8_t *p = s->data + pos;
    out->kind = rd32(p); out->w = rd32(p + 4); out->h = rd32(p + 8);
    out->off = rd32(p + 12); out->len = rd32(p + 16); out->crc = rd32(p + 20);
    if (!out->w || !out->h || out->w > INDEX_WIDTH_MAX || out->h > 2048 ||
        !range_ok(s->size, out->off, out->len)) return false;
    if (out->kind == NINE_IMG_JPEG) {
        // JPEG records store the CRC of the payload, so bind can check it
        // without decoding the whole image at boot.
        if (used_crc) *used_crc = true;
        if (out->crc != nine_crc32_zero(s->data + out->off, out->len, SIZE_MAX, 0))
            return false;
    } else if (out->kind != NINE_IMG_INDEXED) {
        return false;
    }
    // Indexed records store the CRC of the decoded palette indices (checked
    // while decoding) because the compressed payload is already covered by the
    // image package CRC.
    return true;
}

static bool asset_at(const nine_image_source_t *s, uint32_t image, asset_t *out)
{
    return asset_at_counted(s, image, out, NULL);
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
            blend((unsigned)dx, (unsigned)dy, px[sy * stride + sx], c->layer.opacity);
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
    uint32_t running_crc = 0xFFFFFFFFu;
    for (uint32_t block = 0; block < blocks; ++block) {
        uint32_t off = rd32(table + block * 8), len = rd32(table + block * 8 + 4);
        unsigned sy0 = block * rows;
        unsigned nrows = a->h - sy0 < rows ? a->h - sy0 : rows;
        size_t raw = (size_t)a->w * nrows;
        if (off < a->off || !range_ok(s->size, off, len) ||
            (uint64_t)off + len > (uint64_t)a->off + a->len ||
            off - a->off < table_size || !len || raw > sizeof(s_indices) ||
            nine_inflate(s_indices, raw, s->data + off, len) != raw) return false;
        for (size_t i = 0; i < raw; ++i) {
            if (s_indices[i] >= palette_count) return false;
            running_crc ^= s_indices[i];
            for (unsigned bit = 0; bit < 8; ++bit)
                running_crc = (running_crc >> 1) ^ (0xEDB88320u & (0u - (running_crc & 1u)));
        }
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
    return (~running_crc) == a->crc;
}

static bool layer_decode_asset(const nine_image_source_t *s, const asset_t *a,
                               const nine_image_layer_t *layer, bool write)
{
    if (!a || !layer->width || !layer->height || layer->width > 4096 ||
        layer->height > 4096 || layer->opacity > 255) return false;
    return a->kind == NINE_IMG_JPEG ? jpeg_decode(s, a, layer, write) :
                                     indexed_decode(s, a, layer, write);
}

static bool layer_decode(const nine_image_source_t *s, const nine_image_layer_t *layer,
                         bool write)
{
    if (!layer->width || !layer->height || layer->width > 4096 ||
        layer->height > 4096 || layer->opacity > 255) return false;
    asset_t a;
    if (!asset_at(s, layer->image, &a)) return false;
    return layer_decode_asset(s, &a, layer, write);
}

bool nine_image_bind_source(const nine_image_source_t *s)
{
    if (s_busy || !s || !s->data || s->size < 128 || s->size > UINT32_MAX ||
        memcmp(s->data, NINE_IMAGE_MAGIC, 8) || rd32(s->data + 8) != NINE_FORMAT_VERSION ||
        rd32(s->data + 12) != s->size ||
        rd32(s->data + 16) != nine_crc32_zero(s->data, s->size, 16, 4)) return false;
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
    if (ok) { s_source = *s; s_bound = true; s_visual_valid = false; }
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

static bool show_asset_layer_asset(const nine_image_layer_t *layer, const asset_t *a)
{
    if (!s_bound || s_busy || !s_bands[0] || !a) return false;
    s_busy = true;
    // A valid OP JPEG overwrites every canvas pixel; the old frame only needs
    // clearing on failure. Avoid a redundant 64.8 KB clear + invalidation in
    // the steady 5 fps path.
    bool ok = layer_decode_asset(&s_source, a, layer, true);
    if (!ok) nine_image_clear(0); // A failed/partial OP frame is never shown.
    invalidate();
    s_busy = false;
    return ok;
}

bool nine_image_show_asset_op_ex(uint32_t image, nine_image_op_result_t *result)
{
    if (result) {
        result->crc_checks = 0;
        result->jpeg_decodes = 0;
        result->ok = false;
    }
    s_visual_valid = false;
    if (!s_bound) {
        if (s_bands[0]) nine_image_clear(0);
        return false;
    }
    asset_t a;
    bool used_crc = false;
    const bool record_ok = asset_at_counted(&s_source, image, &a, &used_crc);
    if (result) result->crc_checks = used_crc ? 1u : 0u;
    if (!record_ok) {
        // A CRC/bounds/record failure must not leave the previous OP frame or
        // a partially initialized canvas visible.
        nine_image_clear(0);
        return false;
    }
    // OP is a JPEG sequence. Reject any indexed/unknown record explicitly
    // before reporting a JPEG decode attempt.
    if (a.kind != NINE_IMG_JPEG) {
        nine_image_clear(0);
        return false;
    }
    uint32_t w = NINE_IMAGE_W, h = (uint32_t)((uint64_t)a.h * w / a.w);
    if (h > NINE_IMAGE_H) { h = NINE_IMAGE_H; w = (uint32_t)((uint64_t)a.w * h / a.h); }
    if (!w) w = 1;
    if (!h) h = 1;
    nine_image_layer_t layer = { .image = image, .x = (NINE_IMAGE_W - w) / 2,
        .y = (NINE_IMAGE_H - h) / 2, .width = w, .height = h, .opacity = 255 };
    if (result) ++result->jpeg_decodes;
    const bool ok = show_asset_layer_asset(&layer, &a);
    if (result) result->ok = ok;
    return ok;
}

bool nine_image_show_asset_op(uint32_t image)
{
    return nine_image_show_asset_op_ex(image, NULL);
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

static bool show_layers(const nine_image_layer_t *layers, size_t count)
{
    if (!s_bound || s_busy || !s_bands[0] || count > NINE_IMAGE_LAYER_MAX) return false;
    s_busy = true;
    bool ok = true;
    for (size_t i = 0; ok && i < count; ++i) ok = layer_decode(&s_source, &layers[i], false);
    if (ok) {
        nine_image_clear(0);
        for (size_t i = 0; ok && i < count; ++i) ok = layer_decode(&s_source, &layers[i], true);
        if (!ok) nine_image_clear(0); // A corrupt frame never leaves mixed old/new art.
        invalidate();
    }
    s_busy = false;
    return ok;
}

bool nine_image_show_visual(uint32_t visual)
{
    if (!s_bound || !s_source.visual) return false;
    if (s_visual_valid && s_visual == visual) {
#ifdef NINE_IMAGE_HOST
        ++s_visual_cache_hit_count;
#endif
        return true;
    }
#ifdef NINE_IMAGE_HOST
    ++s_visual_decode_count;
#endif
    // A different visual (or an explicitly invalidated one) cannot use the old
    // canvas cache, even if its preflight/decoder fails below.
    s_visual_valid = false;
    nine_image_layer_t layers[NINE_IMAGE_LAYER_MAX];
    size_t count = 0;
    if (!s_source.visual(s_source.ctx, visual, layers, NINE_IMAGE_LAYER_MAX, &count) ||
        count > NINE_IMAGE_LAYER_MAX) return false;
    if (!show_layers(layers, count)) return false;
    s_visual = visual;
    s_visual_valid = true;
    return true;
}

bool nine_image_show_asset(uint32_t image)
{
    s_visual_valid = false;
    asset_t a;
    if (!s_bound || !asset_at(&s_source, image, &a)) return false;
    uint32_t w = NINE_IMAGE_W, h = (uint32_t)((uint64_t)a.h * w / a.w);
    if (h > NINE_IMAGE_H) { h = NINE_IMAGE_H; w = (uint32_t)((uint64_t)a.w * h / a.h); }
    if (!w) w = 1;
    if (!h) h = 1;
    nine_image_layer_t layer = { .image = image, .x = (NINE_IMAGE_W - w) / 2,
        .y = (NINE_IMAGE_H - h) / 2, .width = w, .height = h, .opacity = 255 };
    return show_layers(&layer, 1);
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
bool nine_image_copy_scaled(uint16_t *dst, unsigned width, unsigned height)
{
    if (!s_bands[0] || !dst || !width || !height) return false;
    for (unsigned y = 0; y < height; ++y) {
        const unsigned sy = (unsigned)((uint64_t)y * NINE_IMAGE_H / height);
        for (unsigned x = 0; x < width; ++x) {
            const unsigned sx = (unsigned)((uint64_t)x * NINE_IMAGE_W / width);
            dst[(size_t)y * width + x] = row_at(sy)[sx];
        }
    }
    return true;
}

#ifdef NINE_IMAGE_HOST
uint16_t nine_image_test_pixel(unsigned x, unsigned y)
{ return s_bands[0] && x < NINE_IMAGE_W && y < NINE_IMAGE_H ? row_at(y)[x] : 0; }
size_t nine_image_test_largest_allocation(void) { return s_largest_alloc; }
void nine_image_test_fail_allocation(unsigned call)
{ s_alloc_count = 0; s_fail_alloc = call; }
unsigned nine_image_test_visual_decode_count(void) { return s_visual_decode_count; }
unsigned nine_image_test_visual_cache_hit_count(void) { return s_visual_cache_hit_count; }
void nine_image_test_reset_visual_counts(void)
{ s_visual_decode_count = 0; s_visual_cache_hit_count = 0; }
#endif
