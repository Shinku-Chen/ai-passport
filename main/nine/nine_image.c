#include "nine_image.h"
#include "nine_crc32.h"
#include "nine_format.h"
#include "tjpgd/tjpgd.h"

#include <limits.h>
#include <string.h>
#ifdef NINE_TEST_LOCK_AUDIT
extern void nine_test_lock_assert(const char *what);
#define LOCK_AUDIT(w) nine_test_lock_assert(w)
#else
#define LOCK_AUDIT(w) ((void)0)
#endif

#ifdef NINE_IMAGE_HOST
#include <stdlib.h>
#else
#include "lvgl.h"
#include "esp_heap_caps.h"
#endif

// No allocation exceeds 12,288 bytes. The decoder streams pixels directly into
// six independent 256x24 LVGL images (upscaled to 320x30 on screen); neither
// decoding nor display needs a second canvas or a transform cache.
#define BAND_BYTES (NINE_IMAGE_W * NINE_IMAGE_BAND_H * 2u)
#define JPEG_POOL_BYTES 4096u
#define INDEX_WIDTH_MAX 320u
#define ASSET_WIDTH_MAX 1024u
#define INDEX_ROWS_MAX 8u
static uint16_t *s_bands[NINE_IMAGE_FULL_BANDS];
static uint8_t s_jpeg_pool[JPEG_POOL_BYTES];
static uint8_t s_indices[INDEX_WIDTH_MAX * INDEX_ROWS_MAX];
static nine_image_source_t s_source;
static bool s_bound;
static bool s_busy;
static nine_image_stats_t s_stats;
static uint32_t s_view_h = NINE_IMAGE_H;             // decoded source rows
static uint32_t s_screen_h = NINE_IMAGE_READ_SCREEN_H; // 180 reading/OP, 240 title/CG
static uint32_t s_crop = NINE_IMAGE_READ_CROP;       // 0: full reading art
static const uint8_t *s_script;
static size_t s_script_size;
#ifndef NINE_IMAGE_HOST
static lv_obj_t *s_viewport_obj;   // 320 x visible-height clip container
static lv_obj_t *s_objects[NINE_IMAGE_FULL_BANDS];
static lv_image_dsc_t s_descriptors[NINE_IMAGE_FULL_BANDS];
#else
static size_t s_largest_alloc;
static unsigned s_alloc_count, s_fail_alloc;
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
    for (unsigned i = 0; i < NINE_IMAGE_FULL_BANDS; ++i)
        if (s_objects[i]) lv_obj_invalidate(s_objects[i]);
#endif
}

// Source bands are decoded to 256x{144,192}; the screen container is 320px
// wide and shows 180 (reading and OP) or 240 (title/pure CG). Reading no longer
// crops: the transparent name row overlays the existing 320x180 art.
static void viewport(uint32_t source_h, uint32_t screen_h, uint32_t crop)
{
    s_view_h = source_h;
    s_screen_h = screen_h;
    s_crop = crop;
#ifndef NINE_IMAGE_HOST
    if (s_viewport_obj) lv_obj_set_height(s_viewport_obj, (int32_t)screen_h);
    for (unsigned i = 0; i < NINE_IMAGE_FULL_BANDS; ++i) {
        if (!s_objects[i]) continue;
        lv_obj_set_pos(s_objects[i], 0, (int32_t)(i * 30) - (int32_t)crop);
        if (i * NINE_IMAGE_BAND_H < source_h)
            lv_obj_remove_flag(s_objects[i], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(s_objects[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (s_viewport_obj) lv_obj_invalidate(s_viewport_obj);
#else
    (void)screen_h;
    (void)crop;
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
#ifndef NINE_IMAGE_HOST
    s_viewport_obj = lv_obj_create(parent);
    if (!s_viewport_obj) { nine_image_deinit(); return false; }
    lv_obj_remove_style_all(s_viewport_obj);
    lv_obj_set_pos(s_viewport_obj, 0, 0);
    lv_obj_set_size(s_viewport_obj, (int32_t)NINE_IMAGE_SCREEN_W, (int32_t)NINE_IMAGE_READ_SCREEN_H);
    lv_obj_set_style_bg_opa(s_viewport_obj, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_viewport_obj, 0, LV_PART_MAIN);
    lv_obj_remove_flag(s_viewport_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_viewport_obj, LV_OBJ_FLAG_CLICKABLE);
#endif
    for (unsigned i = 0; i < NINE_IMAGE_FULL_BANDS; ++i) {
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
        s_objects[i] = lv_image_create(s_viewport_obj);
        if (!s_objects[i]) { nine_image_deinit(); return false; }
        lv_image_set_src(s_objects[i], d);
        lv_obj_set_pos(s_objects[i], 0, (int32_t)(i * 30) - (int32_t)NINE_IMAGE_READ_CROP);
        // The source descriptor is 256x24; the object occupies the upscaled
        // 320x30 area so LVGL does not clip the transformed image.
        lv_obj_set_size(s_objects[i], 320, 30);
        // The descriptor is 256x24 and the object is the upscaled 320x30.
        // LVGL defaults inner alignment to CENTER, which insets the native
        // source by (32,3) and leaves black columns/rows. Anchor it top-left.
        lv_image_set_inner_align(s_objects[i], LV_IMAGE_ALIGN_TOP_LEFT);
        lv_image_set_pivot(s_objects[i], 0, 0);
        lv_image_set_scale(s_objects[i], 320);   // 256x24 -> 320x30
        lv_obj_remove_flag(s_objects[i], LV_OBJ_FLAG_SCROLLABLE);
        if (i * NINE_IMAGE_BAND_H >= NINE_IMAGE_H)
            lv_obj_add_flag(s_objects[i], LV_OBJ_FLAG_HIDDEN);
#endif
    }
    s_view_h = NINE_IMAGE_H;
    s_screen_h = NINE_IMAGE_READ_SCREEN_H;
    s_crop = NINE_IMAGE_READ_CROP;
    return true;
}

void nine_image_deinit(void)
{
#ifndef NINE_IMAGE_HOST
    if (s_viewport_obj) lv_obj_delete(s_viewport_obj);
    s_viewport_obj = NULL;
    for (unsigned i = 0; i < NINE_IMAGE_FULL_BANDS; ++i) {
        s_objects[i] = NULL;
        memset(&s_descriptors[i], 0, sizeof(s_descriptors[i]));
    }
#endif
    for (unsigned i = 0; i < NINE_IMAGE_FULL_BANDS; ++i) {
        if (s_bands[i]) pixel_free(s_bands[i]);
        s_bands[i] = NULL;
    }
    s_view_h = NINE_IMAGE_H;
    s_screen_h = NINE_IMAGE_READ_SCREEN_H;
    s_crop = NINE_IMAGE_READ_CROP;
    memset(&s_source, 0, sizeof(s_source));
    s_bound = false;
    s_script = NULL;
    s_script_size = 0;
}

void nine_image_clear(uint16_t color)
{
    LOCK_AUDIT("nine_image_clear");
    if (!s_bands[0]) return;
    for (unsigned y = 0; y < s_view_h; ++y) {
        uint16_t *row = row_at(y);
        for (unsigned x = 0; x < NINE_IMAGE_W; ++x) row[x] = color;
    }
    invalidate();
}

void nine_image_set_visible(bool visible)
{
    LOCK_AUDIT("nine_image_set_visible");
#ifndef NINE_IMAGE_HOST
    for (unsigned i = 0; i < NINE_IMAGE_FULL_BANDS; ++i) {
        if (!s_objects[i]) continue;
        const bool in_view = i * NINE_IMAGE_BAND_H < s_view_h;
        if (visible && in_view) lv_obj_remove_flag(s_objects[i], LV_OBJ_FLAG_HIDDEN);
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
    if (!out->w || !out->h || out->w > ASSET_WIDTH_MAX || out->h > 2048 ||
        !range_ok(s->size, out->off, out->len)) return false;
    if (rd32(p + 20) != nine_crc32_zero(s->data + out->off, out->len, SIZE_MAX, 0))
        return false;
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
        if (dy < 0 || (uint32_t)dy >= s_view_h) continue;
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
            if (dy < 0 || (uint32_t)dy >= s_view_h) continue;
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
    if (a.kind == NINE_IMG_JPEG) ++s_stats.jpeg_decodes;
    return a.kind == NINE_IMG_JPEG ? jpeg_decode(s, &a, layer, write) :
                                   indexed_decode(s, &a, layer, write);
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
        // Pack geometry is the 320x180 stage; the panel canvas is 256x144.
        // Round half up so the reference compositor can reproduce it exactly.
        const int32_t x = (int32_t)rd32(p + 4), y = (int32_t)rd32(p + 8);
        const uint32_t w = rd32(p + 12), h = rd32(p + 16);
        layers[i].x = (x * 4 + (x >= 0 ? 2 : -2)) / 5;
        layers[i].y = (y * 4 + (y >= 0 ? 2 : -2)) / 5;
        layers[i].width = (w * 4 + 2) / 5;
        layers[i].height = (h * 4 + 2) / 5;
        if (!layers[i].width) layers[i].width = 1;
        if (!layers[i].height) layers[i].height = 1;
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
    ++s_stats.renders;
    if (!s_bound || !s_source.visual) return false;
    viewport(NINE_IMAGE_H, NINE_IMAGE_SCREEN_H, 0);
    nine_image_layer_t layers[NINE_IMAGE_LAYER_MAX];
    size_t count = 0;
    if (!s_source.visual(s_source.ctx, visual, layers, NINE_IMAGE_LAYER_MAX, &count) ||
        count > NINE_IMAGE_LAYER_MAX) return false;
    return show_layers(layers, count);
}

static bool show_asset_cover(uint32_t image, bool full)
{
    ++s_stats.renders;
    asset_t a;
    if (!s_bound || !asset_at(&s_source, image, &a)) return false;
    const uint32_t vh = full ? NINE_IMAGE_FULL_H : NINE_IMAGE_H;
    const uint32_t sh = full ? NINE_IMAGE_SCREEN_FULL_H : NINE_IMAGE_SCREEN_H;
    uint32_t w = NINE_IMAGE_W, h = (uint32_t)((uint64_t)a.h * w / a.w);
    if (h < vh) {                       // center cover: fill the viewport height
        h = vh;
        w = (uint32_t)((uint64_t)a.w * h / a.h);
    }
    if (!w) w = 1;
    if (!h) h = 1;
    // Signed subtraction BEFORE division: 256-341 wraps in unsigned and gives
    // a zero-visible-column layer (the black-CG device regression).
    const int32_t x = ((int32_t)NINE_IMAGE_W - (int32_t)w) / 2;
    const int32_t y = ((int32_t)vh - (int32_t)h) / 2;
    nine_image_layer_t layer = { .image = image, .x = x, .y = y,
        .width = w, .height = h, .opacity = 255 };
    viewport(vh, sh, 0);
    return show_layers(&layer, 1);
}

bool nine_image_show_asset(uint32_t image) { return show_asset_cover(image, false); }
bool nine_image_show_asset_full(uint32_t image) { return show_asset_cover(image, true); }

bool nine_image_stats(nine_image_stats_t *out)
{
    if (!out) return false;
    *out = s_stats;
    return true;
}
size_t nine_image_canvas_bytes(void) { return s_bands[0] ? BAND_BYTES * NINE_IMAGE_FULL_BANDS : 0; }
uint32_t nine_image_view_height(void) { return s_screen_h; }
bool nine_image_band_visible(unsigned index) { return index < NINE_IMAGE_FULL_BANDS && index * NINE_IMAGE_BAND_H < s_view_h; }
uint16_t nine_image_pixel(unsigned x, unsigned y) { return s_bands[0] && x < NINE_IMAGE_W && y < s_view_h ? row_at(y)[x] : 0; }
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
uint16_t nine_image_test_pixel(unsigned x, unsigned y) { return nine_image_pixel(x, y); }
size_t nine_image_test_largest_allocation(void) { return s_largest_alloc; }
void nine_image_test_fail_allocation(unsigned call)
{ s_alloc_count = 0; s_fail_alloc = call; }
#endif
