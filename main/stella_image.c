// main/stella_image.c —— 分块 JPEG 解码 + 1bpp 遮罩合成。
#include "stella_image.h"

#include "esp_log.h"
#include "lvgl.h"
#include "sdkconfig.h"

#if !defined(CONFIG_JD_FORMAT_RGB565) || defined(CONFIG_JD_USE_ROM)
#error "Stella 阅读器需要 esp_jpeg 组件自带的 TJpgDec：CONFIG_JD_USE_ROM=n 且 CONFIG_JD_FORMAT_RGB565=y"
#endif

#include "tjpgd.h"

#include <string.h>

static const char *TAG = "stella_img";

#define THEME_TOP STELLA_IMAGE_THEME_TOP      // #0A1A17 in RGB565
#define THEME_BOTTOM STELLA_IMAGE_THEME_BOTTOM   // #123A30 in RGB565
#define CORNER 0x0000u

static _Alignas(4) uint8_t s_jpeg_pool[4096];

typedef struct {
    const uint8_t *jpeg;
    uint32_t len;
    uint32_t pos;
    uint16_t *canvas;
    const uint8_t *mask;
    int dst_x;
    int dst_y;
    uint16_t src_w;
    uint16_t src_h;
    uint16_t dst_w;
    uint16_t dst_h;
    uint32_t written;
} decode_ctx_t;

static void fill_gradient(uint16_t *pixels, uint16_t top, uint16_t bottom)
{
    for (int y = 0; y < STELLA_SCREEN_H; ++y) {
        const uint16_t color = (y < STELLA_BG_H) ? top : bottom;
        uint16_t *row = pixels + (size_t)y * STELLA_SCREEN_W;
        for (int x = 0; x < STELLA_SCREEN_W; ++x) row[x] = color;
    }
}

static void fill_rect(uint16_t *pixels, int x, int y, int w, int h, uint16_t color)
{
    for (int yy = y; yy < y + h; ++yy) {
        if (yy < 0 || yy >= STELLA_SCREEN_H) continue;
        uint16_t *row = pixels + (size_t)yy * STELLA_SCREEN_W;
        for (int xx = x; xx < x + w; ++xx) {
            if (xx >= 0 && xx < STELLA_SCREEN_W) row[xx] = color;
        }
    }
}

static void dst_span(uint32_t s0, uint32_t s1, uint32_t src, uint32_t dst,
                     uint32_t *d0, uint32_t *d1)
{
    *d0 = (s0 * dst + src - 1u) / src;
    *d1 = (s1 * dst + src - 1u) / src;
}

static size_t decode_in(JDEC *jd, uint8_t *buff, size_t nbyte)
{
    decode_ctx_t *ctx = (decode_ctx_t *)jd->device;
    const size_t left = (ctx->pos < ctx->len) ? (size_t)(ctx->len - ctx->pos) : 0;
    const size_t take = (nbyte < left) ? nbyte : left;
    if (buff && take) memcpy(buff, ctx->jpeg + ctx->pos, take);
    ctx->pos += (uint32_t)take;
    return take;
}

static int decode_out(JDEC *jd, void *bitmap, JRECT *rect)
{
    decode_ctx_t *ctx = (decode_ctx_t *)jd->device;
    const uint16_t *src = (const uint16_t *)bitmap;
    uint32_t left = rect->left, right = rect->right;
    uint32_t top = rect->top, bottom = rect->bottom;
    if (left >= ctx->src_w || top >= ctx->src_h) return 1;
    if (right >= ctx->src_w) right = ctx->src_w - 1u;
    if (bottom >= ctx->src_h) bottom = ctx->src_h - 1u;
    const uint32_t block_w = right - left + 1u;
    const uint32_t stride = ((uint32_t)ctx->src_w + 7u) / 8u;

    uint32_t dx0, dx1, dy0, dy1;
    dst_span(left, right + 1u, ctx->src_w, ctx->dst_w, &dx0, &dx1);
    dst_span(top, bottom + 1u, ctx->src_h, ctx->dst_h, &dy0, &dy1);

    for (uint32_t dy = dy0; dy < dy1; ++dy) {
        const int cy = ctx->dst_y + (int)dy;
        if (cy < 0) continue;
        if (cy >= STELLA_SCREEN_H) break;
        const uint32_t sy = (ctx->dst_h == ctx->src_h)
                                ? dy
                                : (uint32_t)(((uint64_t)dy * ctx->src_h) / ctx->dst_h);
        const uint16_t *src_row = src + (size_t)(sy - top) * block_w;
        const uint8_t *mask_row = ctx->mask ? ctx->mask + (size_t)sy * stride : NULL;
        uint16_t *dst_row = ctx->canvas ? ctx->canvas + (size_t)cy * STELLA_SCREEN_W : NULL;
        for (uint32_t dx = dx0; dx < dx1; ++dx) {
            const int cx = ctx->dst_x + (int)dx;
            if (cx < 0) continue;
            if (cx >= STELLA_SCREEN_W) break;
            const uint32_t sx = (ctx->dst_w == ctx->src_w)
                                    ? dx
                                    : (uint32_t)(((uint64_t)dx * ctx->src_w) / ctx->dst_w);
            if (mask_row && ((mask_row[sx >> 3] >> (7u - (sx & 7u))) & 1u) == 0) continue;
            if (dst_row) dst_row[cx] = src_row[sx - left];
            ++ctx->written;
        }
    }
    return 1;
}

static bool decode_asset_to(uint16_t *canvas, const stella_pack_t *pack, uint16_t index,
                            int dst_x, int dst_y, int dst_w, int dst_h)
{
    stella_asset_t asset;
    if (!pack || !stella_pack_asset(pack, index, &asset)) {
        ESP_LOGE(TAG, "资源下标无效: %u", (unsigned)index);
        return false;
    }
    if (dst_w <= 0 || dst_h <= 0) return false;
    decode_ctx_t ctx = {
        .jpeg = asset.jpeg,
        .len = asset.jpeg_len,
        .pos = 0,
        .canvas = canvas,
        .mask = asset.mask,
        .dst_x = dst_x,
        .dst_y = dst_y,
        .src_w = asset.w,
        .src_h = asset.h,
        .dst_w = (uint16_t)dst_w,
        .dst_h = (uint16_t)dst_h,
        .written = 0,
    };
    JDEC jd;
    const JRESULT prep = jd_prepare(&jd, decode_in, s_jpeg_pool, sizeof(s_jpeg_pool), &ctx);
    if (prep != JDR_OK) {
        ESP_LOGE(TAG, "JPEG 解析失败(%d) asset=%u", (int)prep, (unsigned)index);
        return false;
    }
    if (jd.width != asset.w || jd.height != asset.h) {
        ESP_LOGE(TAG, "JPEG 尺寸不符: %ux%u != %ux%u", (unsigned)jd.width, (unsigned)jd.height,
                 (unsigned)asset.w, (unsigned)asset.h);
        return false;
    }
    const JRESULT res = jd_decomp(&jd, decode_out, 0);
    if (res != JDR_OK) {
        ESP_LOGE(TAG, "JPEG 解码失败(%d) asset=%u", (int)res, (unsigned)index);
        return false;
    }
    return ctx.written > 0;
}

static bool decode_asset(stella_image_t *img, const stella_pack_t *pack, uint16_t index,
                         int dst_x, int dst_y, int dst_w, int dst_h)
{
    return img && decode_asset_to(img->pixels, pack, index, dst_x, dst_y, dst_w, dst_h);
}

bool stella_image_validate_asset(const stella_pack_t *pack, uint16_t asset)
{
    stella_asset_t a;
    if (!pack || !stella_pack_asset(pack, asset, &a)) return false;
    const int dx = (a.kind == STELLA_ASSET_SPRITE) ? 0 : 0;
    const int dy = (a.kind == STELLA_ASSET_SPRITE) ? 0 : 0;
    return decode_asset_to(NULL, pack, asset, dx, dy, a.w ? a.w : 1, a.h ? a.h : 1);
}

bool stella_image_validate_scene(const stella_pack_t *pack, uint16_t bg,
                                 const uint16_t bodies[3], uint8_t body_count)
{
    if (!pack || body_count > 3) return false;
    if (bg != STELLA_NONE && !stella_image_validate_asset(pack, bg)) return false;
    for (uint8_t i = 0; i < body_count; ++i) {
        if (bodies[i] != STELLA_NONE && !stella_image_validate_asset(pack, bodies[i])) return false;
    }
    return true;
}

bool stella_image_init(stella_image_t *img, struct _lv_obj_t *parent, uint16_t *pixels)
{
    if (!img || !parent || !pixels) return false;
    memset(img, 0, sizeof(*img));
    img->pixels = pixels;
    fill_gradient(pixels, THEME_TOP, THEME_BOTTOM);
    img->canvas = lv_canvas_create(parent);
    if (!img->canvas) return false;
    lv_canvas_set_buffer(img->canvas, pixels, STELLA_SCREEN_W, STELLA_SCREEN_H,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(img->canvas, 0, 0);
    lv_obj_set_size(img->canvas, STELLA_SCREEN_W, STELLA_SCREEN_H);
    lv_image_set_align(img->canvas, LV_IMAGE_ALIGN_TOP_LEFT);
    return true;
}

void stella_image_clear(stella_image_t *img, uint16_t top, uint16_t bottom)
{
    if (!img || !img->pixels) return;
    fill_gradient(img->pixels, top, bottom);
    if (img->canvas) lv_obj_invalidate(img->canvas);
}

void stella_image_portrait_box(uint8_t count, uint8_t index, const stella_asset_t *asset,
                               int *dx, int *dy, int *dw, int *dh)
{
    int max_w = 260, max_h = 220;
    if (count == 2) { max_w = 150; max_h = 210; }
    else if (count >= 3) { max_w = 100; max_h = 200; }
    const int full_w = asset->ow ? asset->ow : asset->w;
    const int full_h = asset->oh ? asset->oh : asset->h;
    int scale_num = max_w, scale_den = full_w;
    if (max_h * full_w < max_w * full_h) { scale_num = max_h; scale_den = full_h; }
    if (scale_den <= 0) { *dx = 0; *dy = 0; *dw = 0; *dh = 0; return; }
    const int cell = (count <= 1) ? STELLA_SCREEN_W : (count == 2 ? 160 : 106);
    const int full_dw = full_w * scale_num / scale_den;
    const int full_dh = full_h * scale_num / scale_den;
    const int base_x = index * cell + (cell - full_dw) / 2;
    const int base_y = STELLA_SCREEN_H - full_dh;
    *dx = base_x + asset->ax * scale_num / scale_den;
    *dy = base_y + asset->ay * scale_num / scale_den;
    *dw = asset->w * scale_num / scale_den;
    *dh = asset->h * scale_num / scale_den;
}

bool stella_image_show_scene(stella_image_t *img, const stella_pack_t *pack,
                             uint16_t bg, const uint16_t bodies[3], uint8_t body_count)
{
    if (!img || !pack) return false;
    fill_gradient(img->pixels, THEME_TOP, THEME_BOTTOM);
    bool ok = true;
    if (bg != STELLA_NONE) {
        ok = decode_asset(img, pack, bg, 0, 0, STELLA_BG_W, STELLA_BG_H) && ok;
    }
    for (uint8_t i = 0; i < body_count && i < 3; ++i) {
        if (bodies[i] == STELLA_NONE) continue;
        stella_asset_t asset;
        if (!stella_pack_asset(pack, bodies[i], &asset)) { ok = false; continue; }
        int dx, dy, dw, dh;
        stella_image_portrait_box(body_count, i, &asset, &dx, &dy, &dw, &dh);
        if (dw > 0 && dh > 0) {
            ok = decode_asset(img, pack, bodies[i], dx, dy, dw, dh) && ok;
        }
    }
    if (img->canvas) lv_obj_invalidate(img->canvas);
    return ok;
}

bool stella_image_show_title(stella_image_t *img, const stella_pack_t *pack, uint16_t asset)
{
    if (!img || !pack) return false;
    fill_gradient(img->pixels, THEME_TOP, THEME_BOTTOM);
    const bool ok = decode_asset(img, pack, asset, 0, 0, STELLA_SCREEN_W, STELLA_SCREEN_H);
    if (img->canvas) lv_obj_invalidate(img->canvas);
    return ok;
}

bool stella_image_show_cg(stella_image_t *img, const stella_pack_t *pack, uint16_t asset)
{
    if (!img || !pack) return false;
    fill_rect(img->pixels, 0, 0, STELLA_SCREEN_W, STELLA_SCREEN_H, CORNER);
    const bool ok = decode_asset(img, pack, asset, 0, (STELLA_SCREEN_H - STELLA_BG_H) / 2,
                                 STELLA_BG_W, STELLA_BG_H);
    if (img->canvas) lv_obj_invalidate(img->canvas);
    return ok;
}
