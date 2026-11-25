// main/cafe/cafe_image.c —— 条带式 JPEG/alpha 合成。
#include "cafe_image.h"

#include "tjpgd.h"

#include "esp_log.h"

#include <stdlib.h>
#include <string.h>
#include "cafe_compat.h"

static const char *TAG = "cafe_image";

static uint32_t s_render_scene_calls;
static uint32_t s_verify_calls;

#define MASK_MAX 5200
#define PATCH_MAX_W 106
#define PATCH_MAX_H 44
#define THUMB_SRC_MAX (56 * 32)

static uint16_t s_stage[CAFE_BAND_W * CAFE_BAND_H];
static uint16_t s_patch[PATCH_MAX_W * PATCH_MAX_H];
static uint8_t s_patch_mask[PATCH_MAX_W * PATCH_MAX_H * 2 / 8 + 8];
static uint8_t s_mask[MASK_MAX];
static uint16_t s_thumb_src[THUMB_SRC_MAX];
static uint8_t s_jd_pool[4096];

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
    bool segmented;
    cafe_jpeg_t jpeg;
    const uint8_t *seg_data;
    uint32_t seg_len;
    uint32_t seg_pos;
} jpeg_src_t;

typedef struct {
    jpeg_src_t src;
    struct jpeg_out *out;
} jpeg_dev_t;

typedef struct jpeg_out {
    int band_y0;
    int band_y1;
    int stage_x;
    int stage_y;
    int img_w;
    int img_h;
    const uint8_t *mask;
    int mask_stride;
    int mask_bits;
    uint8_t opacity;
    bool is_patch;
    int crop_x, crop_y, crop_w, crop_h;
    uint16_t *thumb;
    int thumb_w, thumb_h;
    bool is_thumb;
    bool is_scaled;
    int dst_w, dst_h;
} jpeg_out_t;

static size_t in_func(JDEC *jd, uint8_t *buf, size_t n)
{
    jpeg_dev_t *dev = (jpeg_dev_t *)jd->device;
    jpeg_src_t *src = dev ? &dev->src : NULL;
    if (!src) return 0;
    if (src->segmented) {
        size_t done = 0;
        while (done < n) {
            if (src->seg_pos >= src->seg_len) {
                if (!cafe_jpeg_next(&src->jpeg, &src->seg_data, &src->seg_len))
                    break;
                src->seg_pos = 0;
                if (src->seg_len == 0) continue;
            }
            size_t take = src->seg_len - src->seg_pos;
            if (take > n - done) take = n - done;
            if (buf) memcpy(buf + done, src->seg_data + src->seg_pos, take);
            src->seg_pos += (uint32_t)take;
            done += take;
        }
        return done;
    }
    if (src->pos >= src->len) return 0;
    const size_t avail = src->len - src->pos;
    if (n > avail) n = avail;
    if (buf) memcpy(buf, src->data + src->pos, n);
    src->pos += n;
    return n;
}

static inline uint16_t blend565(uint16_t dst, uint16_t src, uint8_t alpha)
{
    if (alpha == 0) return dst;
    if (alpha == 255) return src;
    const uint32_t inv = 255u - alpha;
    const uint32_t r = (((src >> 11) & 0x1F) * alpha + ((dst >> 11) & 0x1F) * inv + 127u) / 255u;
    const uint32_t g = (((src >> 5) & 0x3F) * alpha + ((dst >> 5) & 0x3F) * inv + 127u) / 255u;
    const uint32_t b = ((src & 0x1F) * alpha + (dst & 0x1F) * inv + 127u) / 255u;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static inline uint8_t mask_alpha(const uint8_t *mask, int stride, int bits, int x, int y)
{
    if (!mask) return 255;
    const int bitpos = x * bits;
    const int shift = 8 - bits - (bitpos & 7);
    const unsigned v = (mask[(size_t)y * stride + (bitpos >> 3)] >> shift) & ((1u << bits) - 1u);
    return bits == 1 ? (v ? 255 : 0) : (uint8_t)(v * 85);
}

static inline uint8_t patch_alpha(const uint8_t *mask, int bits, size_t index)
{
    const size_t bitpos = index * bits;
    const int shift = 8 - bits - (int)(bitpos & 7);
    const unsigned v = (mask[bitpos >> 3] >> shift) & ((1u << bits) - 1u);
    return (uint8_t)(v * 85);
}

static int out_func(JDEC *jd, void *bitmap, JRECT *rect)
{
    jpeg_dev_t *dev = (jpeg_dev_t *)jd->device;
    jpeg_out_t *ctx = dev ? dev->out : NULL;
    if (!ctx) return 0;
    const uint16_t *src = (const uint16_t *)bitmap;
    const int rw = rect->right - rect->left + 1;
    const int rh = rect->bottom - rect->top + 1;

    if (ctx->is_thumb) {
        if (ctx->thumb) {
            for (int y = 0; y < rh; ++y) {
                const int dy = rect->top + y;
                if (dy < 0 || dy >= ctx->thumb_h) continue;
                for (int x = 0; x < rw; ++x) {
                    const int dx = rect->left + x;
                    if (dx < 0 || dx >= ctx->thumb_w) continue;
                    ctx->thumb[dy * ctx->thumb_w + dx] = src[y * rw + x];
                }
            }
        }
        return 1;
    }

    if (ctx->is_patch) {
        for (int y = 0; y < rh; ++y) {
            const int sy = rect->top + y;
            if (sy < ctx->crop_y || sy >= ctx->crop_y + ctx->crop_h) continue;
            for (int x = 0; x < rw; ++x) {
                const int sx = rect->left + x;
                if (sx < ctx->crop_x || sx >= ctx->crop_x + ctx->crop_w) continue;
                const int px = sx - ctx->crop_x;
                const int py = sy - ctx->crop_y;
                s_patch[py * ctx->crop_w + px] = src[y * rw + x];
                const uint8_t a = mask_alpha(ctx->mask, ctx->mask_stride,
                                             ctx->mask_bits, sx, sy);
                if (a) {
                    const size_t bit = (size_t)py * ctx->crop_w + px;
                    const int level = a / 85;
                    const size_t bpos = bit * 2;
                    const int shift = 6 - (int)(bpos & 7);
                    s_patch_mask[bpos >> 3] |= (uint8_t)(level << shift);
                }
            }
        }
        if (rect->top >= ctx->crop_y + ctx->crop_h) return 0;
        return 1;
    }

    if (ctx->is_scaled) {
        // Map every source pixel to a destination span. This fills the full
        // 224x126 stage without a separate full-frame RGB565 allocation.
        for (int y = 0; y < rh; ++y) {
            int dy0 = ((rect->top + y) * ctx->dst_h) / ctx->img_h;
            int dy1 = ((rect->top + y + 1) * ctx->dst_h) / ctx->img_h;
            if (dy1 <= dy0) dy1 = dy0 + 1;
            if (dy1 <= ctx->band_y0 || dy0 >= ctx->band_y1) continue;
            if (dy0 < ctx->band_y0) dy0 = ctx->band_y0;
            if (dy1 > ctx->band_y1) dy1 = ctx->band_y1;
            for (int x = 0; x < rw; ++x) {
                int dx0 = ((rect->left + x) * ctx->dst_w) / ctx->img_w;
                int dx1 = ((rect->left + x + 1) * ctx->dst_w) / ctx->img_w;
                if (dx1 <= dx0) dx1 = dx0 + 1;
                if (dx1 <= 0 || dx0 >= CAFE_BAND_W) continue;
                if (dx0 < 0) dx0 = 0;
                if (dx1 > CAFE_BAND_W) dx1 = CAFE_BAND_W;
                for (int yy = dy0; yy < dy1; ++yy) {
                    for (int xx = dx0; xx < dx1; ++xx) {
                        s_stage[(yy - ctx->band_y0) * CAFE_BAND_W + xx] =
                            src[y * rw + x];
                    }
                }
            }
        }
        return 1;
    }

    for (int y = 0; y < rh; ++y) {
        const int sy = ctx->stage_y + rect->top + y;
        if (sy < ctx->band_y0 || sy >= ctx->band_y1) continue;
        const int dy = sy - ctx->band_y0;
        for (int x = 0; x < rw; ++x) {
            const int sx = ctx->stage_x + rect->left + x;
            if (sx < 0 || sx >= CAFE_BAND_W) continue;
            const uint16_t color = src[y * rw + x];
            uint16_t *dst = &s_stage[dy * CAFE_BAND_W + sx];
            if (ctx->mask) {
                const int mx = rect->left + x;
                const int my = rect->top + y;
                uint8_t a = mask_alpha(ctx->mask, ctx->mask_stride, ctx->mask_bits, mx, my);
                if (!a) continue;
                a = (uint8_t)((a * ctx->opacity + 127u) / 255u);
                *dst = blend565(*dst, color, a);
            } else {
                *dst = color;
            }
        }
    }
    if (rect->top + ctx->stage_y >= ctx->band_y1) return 0;
    return 1;
}

static JRESULT decode_jpeg(const cafe_image_t *img, jpeg_out_t *ctx, uint8_t scale)
{
    jpeg_dev_t dev;
    memset(&dev, 0, sizeof(dev));
    dev.out = ctx;
    if (!cafe_image_jpeg(img, &dev.src.jpeg)) {
        uint32_t len = 0;
        const uint8_t *data = cafe_image_data(img, &len);
        if (!data || len == 0) return JDR_INP;
        dev.src.data = data;
        dev.src.len = len;
        dev.src.pos = 0;
        dev.src.segmented = false;
    } else {
        dev.src.segmented = true;
    }
    JDEC jd;
    JRESULT rc = jd_prepare(&jd, in_func, s_jd_pool, sizeof(s_jd_pool), &dev);
    if (rc != JDR_OK) return rc;
    ctx->img_w = jd.width;
    ctx->img_h = jd.height;
    rc = jd_decomp(&jd, out_func, scale);
    if (rc == JDR_INTR) rc = JDR_OK;
    return rc;
}

static bool load_mask(const cafe_image_t *img, uint8_t *out, size_t cap, size_t *used)
{
    if (img->kind == 0) {
        if (used) *used = 0;
        return true;
    }
    if (img->kind != 1 && img->kind != 2) return false;
    uint32_t comp_len = 0;
    const uint8_t *comp = cafe_image_mask_data(img, &comp_len);
    if (!comp || img->mask_raw > cap) return false;
    size_t dest = 0;
    if (!cafe_inflate(out, cap, &dest, comp, comp_len) || dest != img->mask_raw) {
        ESP_LOGE(TAG, "mask 解压失败 id=%u", (unsigned)img->id);
        return false;
    }
    if (used) *used = dest;
    return true;
}

bool cafe_image_verify(uint16_t image_id)
{
    s_verify_calls++;
    cafe_image_t img;
    if (!cafe_image_get(image_id, &img)) return false;
    uint32_t crc = 0;
    cafe_jpeg_t stream;
    if (cafe_image_jpeg(&img, &stream)) {
        const uint8_t *part = NULL;
        uint32_t part_len = 0;
        while (cafe_jpeg_next(&stream, &part, &part_len))
            crc = (uint32_t)cafe_crc32(crc, part, part_len);
    } else {
        uint32_t len = 0;
        const uint8_t *data = cafe_image_data(&img, &len);
        if (!data) return false;
        crc = (uint32_t)cafe_crc32(0, data, len);
    }
    if (img.kind != 0) {
        uint32_t mlen = 0;
        const uint8_t *mask = cafe_image_mask_data(&img, &mlen);
        if (!mask) return false;
        crc = (uint32_t)cafe_crc32(crc, mask, mlen);
    }
    if (crc != img.crc) {
        ESP_LOGE(TAG, "图片 %u CRC 错误", (unsigned)image_id);
        return false;
    }
    return true;
}

bool cafe_image_init(void)
{
    return true;
}

bool cafe_image_bands_init(cafe_bands_t *bands)
{
    if (!bands) return false;
    memset(bands, 0, sizeof(*bands));
    for (int i = 0; i < CAFE_BAND_COUNT; ++i) {
        bands->pixels[i] = (uint16_t *)malloc(CAFE_BAND_W * CAFE_BAND_H * sizeof(uint16_t));
        if (!bands->pixels[i]) {
            cafe_image_bands_free(bands);
            return false;
        }
        memset(bands->pixels[i], 0, CAFE_BAND_W * CAFE_BAND_H * sizeof(uint16_t));
    }
    return true;
}

void cafe_image_bands_free(cafe_bands_t *bands)
{
    if (!bands) return;
    for (int i = 0; i < CAFE_BAND_COUNT; ++i) {
        free(bands->pixels[i]);
        bands->pixels[i] = NULL;
    }
}

static void clear_bands(cafe_bands_t *bands)
{
    if (!bands) return;
    for (int i = 0; i < CAFE_BAND_COUNT; ++i) {
        if (bands->pixels[i]) {
            memset(bands->pixels[i], 0, CAFE_BAND_W * CAFE_BAND_H * sizeof(uint16_t));
        }
    }
}

static bool render_background_band(cafe_bands_t *bands, uint16_t image_id, int band)
{
    cafe_image_t img;
    if (!cafe_image_get(image_id, &img)) return false;
    const int stage_x = (CAFE_STAGE_W - (int)img.w) / 2;
    const int stage_y = (CAFE_STAGE_H - (int)img.h) / 2;
    jpeg_out_t ctx = {
        .band_y0 = band * CAFE_BAND_H,
        .band_y1 = (band + 1) * CAFE_BAND_H,
        .stage_x = stage_x,
        .stage_y = stage_y,
    };
    const JRESULT rc = decode_jpeg(&img, &ctx, 0);
    if (rc != JDR_OK) {
        ESP_LOGE(TAG, "背景 JPEG 解码失败 id=%u rc=%d", (unsigned)image_id, (int)rc);
        return false;
    }
    if (bands->pixels[band]) {
        memcpy(bands->pixels[band], s_stage, sizeof(s_stage));
    }
    return true;
}

static bool render_op_band(cafe_bands_t *bands, uint16_t image_id, int band)
{
    cafe_image_t img;
    if (!cafe_image_get(image_id, &img)) return false;
    jpeg_out_t ctx = {
        .band_y0 = band * CAFE_BAND_H,
        .band_y1 = (band + 1) * CAFE_BAND_H,
        .stage_x = 0,
        .stage_y = 0,
        .is_scaled = true,
        .dst_w = CAFE_STAGE_W,
        .dst_h = CAFE_STAGE_H,
    };
    const JRESULT rc = decode_jpeg(&img, &ctx, 0);
    if (rc != JDR_OK) {
        ESP_LOGE(TAG, "OP JPEG 解码失败 id=%u rc=%d", (unsigned)image_id, (int)rc);
        return false;
    }
    if (bands->pixels[band]) memcpy(bands->pixels[band], s_stage, sizeof(s_stage));
    return true;
}

bool cafe_image_render_op(cafe_bands_t *bands, uint16_t image_id)
{
    if (!bands || !cafe_image_verify(image_id)) {
        clear_bands(bands);
        return false;
    }
    for (int b = 0; b < CAFE_BAND_COUNT; ++b) {
        memset(s_stage, 0, sizeof(s_stage));
        if (!render_op_band(bands, image_id, b)) {
            clear_bands(bands);
            return false;
        }
    }
    return true;
}

bool cafe_image_render_image(cafe_bands_t *bands, uint16_t image_id)
{
    if (!bands || !cafe_image_verify(image_id)) {
        clear_bands(bands);
        return false;
    }
    for (int b = 0; b < CAFE_BAND_COUNT; ++b) {
        memset(s_stage, 0, sizeof(s_stage));
        if (!render_background_band(bands, image_id, b)) {
            clear_bands(bands);
            return false;
        }
    }
    return true;
}

static bool render_sprite_band(cafe_bands_t *bands, const cafe_sprite_t *sp, int band)
{
    (void)bands;
    cafe_asset_t asset;
    if (!cafe_asset_get(sp->asset, &asset) || asset.kind != 1) return false;
    cafe_image_t img;
    if (!cafe_image_get(asset.image_id, &img)) return false;
    size_t mask_len = 0;
    if (img.kind != 0) {
        if (!load_mask(&img, s_mask, sizeof(s_mask), &mask_len)) return false;
    }
    jpeg_out_t ctx = {
        .band_y0 = band * CAFE_BAND_H,
        .band_y1 = (band + 1) * CAFE_BAND_H,
        .stage_x = sp->left,
        .stage_y = sp->top,
        .mask = img.kind != 0 ? s_mask : NULL,
        .mask_stride = (img.w * (img.kind == 2 ? 2 : 1) + 7) / 8,
        .mask_bits = img.kind == 2 ? 2 : 1,
        .opacity = sp->opacity,
    };
    const JRESULT rc = decode_jpeg(&img, &ctx, 0);
    if (rc != JDR_OK) {
        ESP_LOGE(TAG, "立绘解码失败 asset=%u rc=%d", (unsigned)sp->asset, (int)rc);
        return false;
    }
    if (asset.patch_image_id == CAFE_IMAGE_NONE) return true;

    cafe_image_t atlas;
    if (!cafe_image_get(asset.patch_image_id, &atlas) || atlas.kind == 0) return false;
    size_t atlas_mask_len = 0;
    if (!load_mask(&atlas, s_mask, sizeof(s_mask), &atlas_mask_len)) return false;
    if (asset.atlas_w > PATCH_MAX_W || asset.atlas_h > PATCH_MAX_H || asset.atlas_w == 0 ||
        asset.atlas_h == 0) {
        return false;
    }
    memset(s_patch, 0, sizeof(s_patch));
    memset(s_patch_mask, 0, sizeof(s_patch_mask));
    jpeg_out_t actx = {
        .is_patch = true,
        .crop_x = asset.atlas_x,
        .crop_y = asset.atlas_y,
        .crop_w = asset.atlas_w,
        .crop_h = asset.atlas_h,
        .mask = s_mask,
        .mask_stride = (atlas.w * (atlas.kind == 2 ? 2 : 1) + 7) / 8,
        .mask_bits = atlas.kind == 2 ? 2 : 1,
    };
    const JRESULT arc = decode_jpeg(&atlas, &actx, 0);
    if (arc != JDR_OK) {
        ESP_LOGE(TAG, "表情图集解码失败 image=%u rc=%d", (unsigned)atlas.id, (int)arc);
        return false;
    }
    const int dest_x = sp->left + asset.patch_x;
    const int dest_y = sp->top + asset.patch_y;
    const int dest_w = asset.patch_w;
    const int dest_h = asset.patch_h;
    if (dest_w <= 0 || dest_h <= 0) return false;
    for (int band_row = 0; band_row < CAFE_BAND_H; ++band_row) {
        const int sy = band * CAFE_BAND_H + band_row;
        const int dy = sy - dest_y;
        if (dy < 0 || dy >= dest_h) continue;
        const int py = dy * asset.atlas_h / dest_h;
        for (int dx = 0; dx < dest_w; ++dx) {
            const int sx = dest_x + dx;
            if (sx < 0 || sx >= CAFE_BAND_W) continue;
            const int px = dx * asset.atlas_w / dest_w;
            const size_t pbit = (size_t)py * asset.atlas_w + px;
            uint8_t a = patch_alpha(s_patch_mask, 2, pbit);
            if (!a) continue;
            a = (uint8_t)((a * sp->opacity + 127u) / 255u);
            const uint16_t color = s_patch[py * asset.atlas_w + px];
            uint16_t *dst = &s_stage[band_row * CAFE_BAND_W + sx];
            *dst = blend565(*dst, color, a);
        }
    }
    return true;
}

bool cafe_image_render_scene(cafe_bands_t *bands, uint16_t bg_asset,
                             const cafe_sprite_t *sprites, unsigned sprite_count)
{
    s_render_scene_calls++;
    if (!bands || sprite_count > 5) {
        clear_bands(bands);
        return false;
    }
    cafe_asset_t bg;
    if (!cafe_asset_get(bg_asset, &bg) || bg.kind != 0 || !cafe_image_verify(bg.image_id)) {
        clear_bands(bands);
        return false;
    }
    for (unsigned i = 0; i < sprite_count; ++i) {
        cafe_asset_t a;
        if (!sprites || !cafe_asset_get(sprites[i].asset, &a) || a.kind != 1 ||
            !cafe_image_verify(a.image_id)) {
            clear_bands(bands);
            return false;
        }
        if (a.patch_image_id != CAFE_IMAGE_NONE && !cafe_image_verify(a.patch_image_id)) {
            clear_bands(bands);
            return false;
        }
    }
    for (int b = 0; b < CAFE_BAND_COUNT; ++b) {
        memset(s_stage, 0, sizeof(s_stage));
        if (!render_background_band(bands, bg.image_id, b)) {
            clear_bands(bands);
            return false;
        }
        for (unsigned i = 0; i < sprite_count; ++i) {
            if (!render_sprite_band(bands, &sprites[i], b)) {
                clear_bands(bands);
                return false;
            }
        }
        if (bands->pixels[b]) memcpy(bands->pixels[b], s_stage, sizeof(s_stage));
    }
    return true;
}

bool cafe_image_thumb(uint16_t image_id, uint16_t *dst, int dst_w, int dst_h)
{
    cafe_image_t img;
    if (!dst || dst_w <= 0 || dst_h <= 0 || !cafe_image_get(image_id, &img) ||
        !cafe_image_verify(image_id)) {
        return false;
    }
    const int src_w = (img.w + 3) / 4;
    const int src_h = (img.h + 3) / 4;
    if (src_w * src_h > THUMB_SRC_MAX) return false;
    memset(s_thumb_src, 0, sizeof(s_thumb_src));
    jpeg_out_t ctx = {
        .is_thumb = true,
        .thumb = s_thumb_src,
        .thumb_w = src_w,
        .thumb_h = src_h,
    };
    const JRESULT rc = decode_jpeg(&img, &ctx, 2);
    if (rc != JDR_OK) return false;
    for (int y = 0; y < dst_h; ++y) {
        const int sy = y * src_h / dst_h;
        for (int x = 0; x < dst_w; ++x) {
            const int sx = x * src_w / dst_w;
            dst[y * dst_w + x] = s_thumb_src[sy * src_w + sx];
        }
    }
    return true;
}

int cafe_image_gallery_group(uint16_t asset)
{
    cafe_asset_t a;
    cafe_image_t img;
    if (!cafe_asset_get(asset, &a) || !cafe_image_get(a.image_id, &img)) return -1;
    return img.gallery == 0xFFFF ? -1 : (int)img.gallery;
}

bool cafe_image_gallery_alias(uint16_t asset, uint16_t index, uint16_t *group)
{
    cafe_asset_t a;
    if (!group || !cafe_asset_get(asset, &a)) return false;
    return cafe_image_alias_get(a.image_id, index, group);
}

// Bounded counters used by the FF/cache host tests; no per-frame logging.
uint32_t cafe_image_render_scene_count(void) { return s_render_scene_calls; }
uint32_t cafe_image_verify_count(void) { return s_verify_calls; }
void cafe_image_reset_counts(void) { s_render_scene_calls = 0; s_verify_calls = 0; }
