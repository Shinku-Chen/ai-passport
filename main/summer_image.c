// main/summer_image.c —— TJpgDec 回调直接合成到 320x180 画布。
#include "summer_image.h"

#include "tjpgd.h"

#include <stdio.h>
#include <string.h>

#ifndef SUMMER_IMAGE_HOST
#include "esp_log.h"
#include "lvgl.h"
static const char *TAG = "summer_img";
#define IMG_LOGE(...) ESP_LOGE(TAG, __VA_ARGS__)
#else
#define IMG_LOGE(...) do { } while (0)
#endif

// TJpgDec 工作区。默认优化等级下 4 KB 足够；放静态区，不占任务栈。
static _Alignas(4) uint8_t s_jpeg_pool[4096];
static uint8_t s_mask_bits[4096];

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

static bool rd_uvar(const uint8_t **pp, const uint8_t *end, uint32_t *out)
{
    const uint8_t *p = *pp;
    uint32_t v = 0;
    uint32_t shift = 0;
    for (int i = 0; i < 5; ++i) {
        if (p >= end) return false;
        const uint8_t b = *p++;
        v |= (uint32_t)(b & 0x7Fu) << shift;
        if (!(b & 0x80u)) {
            *pp = p;
            *out = v;
            return true;
        }
        shift += 7;
    }
    return false;
}

bool summer_mask_expand(const summer_pack_t *pack, uint32_t image_index,
                        uint8_t *out, size_t cap, int *out_w, int *out_h)
{
    if (!pack || !out) return false;
    const summer_image_t *entry = summer_image_at(pack, image_index);
    if (!entry || entry->kind != SUMMER_IMG_PORTRAIT || entry->mask_len == 0) return false;
    const int w = entry->width;
    const int h = entry->height;
    const size_t need = ((size_t)w * (size_t)h + 7u) / 8u;
    if (need == 0 || need > cap) return false;
    memset(out, 0, need);
    const uint8_t *p = pack->imgdata + entry->mask_off;
    const uint8_t *end = p + entry->mask_len;
    for (int y = 0; y < h; ++y) {
        uint32_t runs = 0;
        if (!rd_uvar(&p, end, &runs) || runs > (uint32_t)w + 1u) return false;
        int x = 0;
        bool opaque = false;
        for (uint32_t r = 0; r < runs; ++r) {
            uint32_t run = 0;
            if (!rd_uvar(&p, end, &run)) return false;
            if (run > (uint32_t)w || x + (int)run > w) return false;
            if (opaque) {
                for (uint32_t i = 0; i < run; ++i) {
                    const int bx = x + (int)i;
                    out[(size_t)y * (size_t)w / 8u + (size_t)bx / 8u] |=
                        (uint8_t)(1u << (7u - (bx & 7)));
                }
            }
            x += (int)run;
            opaque = !opaque;
        }
        if (x != w) return false;
    }
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return true;
}

static size_t decode_in(JDEC *jd, uint8_t *buff, size_t nbyte)
{
    decode_ctx_t *ctx = (decode_ctx_t *)jd->device;
    const size_t left = ctx->pos < ctx->len ? (size_t)(ctx->len - ctx->pos) : 0;
    const size_t take = nbyte < left ? nbyte : left;
    if (buff && take) memcpy(buff, ctx->jpeg + ctx->pos, take);
    ctx->pos += (uint32_t)take;
    return take;
}

static void span(uint32_t s0, uint32_t s1, uint32_t src, uint32_t dst,
                 uint32_t *d0, uint32_t *d1)
{
    *d0 = (s0 * dst + src - 1u) / src;
    *d1 = (s1 * dst + src - 1u) / src;
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
    span(left, right + 1u, ctx->src_w, ctx->dst_w, &dx0, &dx1);
    span(top, bottom + 1u, ctx->src_h, ctx->dst_h, &dy0, &dy1);
    for (uint32_t dy = dy0; dy < dy1; ++dy) {
        const int cy = ctx->dst_y + (int)dy;
        if (cy < 0 || cy >= SUMMER_ART_H) continue;
        const uint32_t sy = (uint32_t)(((uint64_t)dy * ctx->src_h) / ctx->dst_h);
        const uint16_t *src_row = src + (size_t)(sy - top) * block_w;
        const uint8_t *mask_row = ctx->mask ? ctx->mask + (size_t)sy * stride : NULL;
        uint16_t *dst_row = ctx->canvas + (size_t)cy * SUMMER_ART_W;
        for (uint32_t dx = dx0; dx < dx1; ++dx) {
            const int cx = ctx->dst_x + (int)dx;
            if (cx < 0 || cx >= SUMMER_ART_W) continue;
            const uint32_t sx = (uint32_t)(((uint64_t)dx * ctx->src_w) / ctx->dst_w);
            if (mask_row) {
                const uint8_t bit = (uint8_t)(0x80u >> (sx & 7u));
                if (!(mask_row[sx >> 3] & bit)) continue;
            }
            dst_row[cx] = src_row[sx - left];
            ctx->written++;
        }
    }
    return 1;
}

static void fill_rect(uint16_t *canvas, int x, int y, int w, int h, uint16_t color)
{
    for (int yy = y; yy < y + h; ++yy) {
        if (yy < 0 || yy >= SUMMER_ART_H) continue;
        uint16_t *row = canvas + (size_t)yy * SUMMER_ART_W;
        for (int xx = x; xx < x + w; ++xx) {
            if (xx >= 0 && xx < SUMMER_ART_W) row[xx] = color;
        }
    }
}

static bool decode_index(summer_canvas_t *img, const summer_pack_t *pack, uint32_t index,
                         int dst_x, int dst_y, int dst_w, int dst_h, bool use_mask)
{
    if (!img || !img->pixels || !pack) return false;
    const summer_image_t *entry = summer_image_at(pack, index);
    if (!entry) return false;
    if (dst_w <= 0 || dst_h <= 0) return false;
    if (entry->kind == SUMMER_IMG_SOLID) {
        fill_rect(img->pixels, dst_x, dst_y, dst_w, dst_h, 0);
        return true;
    }
    const uint8_t *mask = NULL;
    if (use_mask && entry->kind == SUMMER_IMG_PORTRAIT) {
        int mw = 0, mh = 0;
        if (!summer_mask_expand(pack, index, s_mask_bits, sizeof(s_mask_bits), &mw, &mh)) {
            IMG_LOGE("遮罩展开失败 index=%u", (unsigned)index);
            return false;
        }
        if (mw != entry->width || mh != entry->height) return false;
        mask = s_mask_bits;
    }
    decode_ctx_t ctx = {
        .jpeg = pack->imgdata + entry->off,
        .len = entry->len,
        .pos = 0,
        .canvas = img->pixels,
        .mask = mask,
        .dst_x = dst_x,
        .dst_y = dst_y,
        .src_w = entry->width,
        .src_h = entry->height,
        .dst_w = (uint16_t)dst_w,
        .dst_h = (uint16_t)dst_h,
        .written = 0,
    };
    JDEC jd;
    const JRESULT prep = jd_prepare(&jd, decode_in, s_jpeg_pool, sizeof(s_jpeg_pool), &ctx);
    if (prep != JDR_OK) {
        IMG_LOGE("JPEG 解析失败 %d index=%u", (int)prep, (unsigned)index);
        return false;
    }
    if (jd.width != entry->width || jd.height != entry->height) {
        IMG_LOGE("JPEG 尺寸不符 %ux%u != %ux%u", (unsigned)jd.width, (unsigned)jd.height,
                 (unsigned)entry->width, (unsigned)entry->height);
        return false;
    }
    const JRESULT res = jd_decomp(&jd, decode_out, 0);
    if (res != JDR_OK) {
        IMG_LOGE("JPEG 解码失败 %d index=%u", (int)res, (unsigned)index);
        return false;
    }
    return ctx.written > 0;
}

bool summer_image_bind(summer_canvas_t *img, uint16_t *pixels)
{
    if (!img || !pixels) return false;
    img->canvas = NULL;
    img->pixels = pixels;
    return true;
}

#ifndef SUMMER_IMAGE_HOST
bool summer_image_init(summer_canvas_t *img, struct _lv_obj_t *parent, uint16_t *pixels)
{
    if (!img || !parent || !pixels) return false;
    if (!summer_image_bind(img, pixels)) return false;
    img->canvas = lv_canvas_create(parent);
    if (!img->canvas) return false;
    lv_canvas_set_buffer(img->canvas, pixels, SUMMER_ART_W, SUMMER_ART_H,
                         LV_COLOR_FORMAT_RGB565);
    lv_image_set_align(img->canvas, LV_IMAGE_ALIGN_TOP_LEFT);
    lv_obj_set_pos(img->canvas, 0, 0);
    lv_obj_set_size(img->canvas, SUMMER_ART_W, SUMMER_ART_H);
    lv_obj_remove_flag(img->canvas, LV_OBJ_FLAG_SCROLLABLE);
    return true;
}
#endif

void summer_image_clear(summer_canvas_t *img)
{
    if (!img || !img->pixels) return;
    memset(img->pixels, 0, (size_t)SUMMER_ART_W * SUMMER_ART_H * 2u);
    summer_image_invalidate(img);
}

void summer_image_invalidate(summer_canvas_t *img)
{
#ifndef SUMMER_IMAGE_HOST
    if (img && img->canvas) lv_obj_invalidate(img->canvas);
#else
    (void)img;
#endif
}

// Draw the background alone.  On failure the canvas is a defined black
// placeholder; a later body failure re-runs this to roll back a partially
// decoded portrait instead of leaving a half frame on screen.
static bool draw_background(summer_canvas_t *img, const summer_pack_t *pack, uint16_t bg)
{
    summer_image_clear(img);
    if (bg == 0) return true;
    const summer_image_t *entry = summer_image(pack, bg);
    if (!entry) return false;
    const uint32_t index = (uint32_t)(entry - pack->images);
    if (!decode_index(img, pack, index, 0, 0, SUMMER_ART_W, SUMMER_ART_H, false)) {
        fill_rect(img->pixels, 0, 0, SUMMER_ART_W, SUMMER_ART_H, 0);
        summer_image_invalidate(img);
        return false;
    }
    return true;
}

bool summer_image_scene(summer_canvas_t *img, const summer_pack_t *pack, uint16_t bg,
                        uint16_t body)
{
    if (!img || !pack) return false;
    if (!draw_background(img, pack, bg)) {
        summer_image_invalidate(img);
        return false;
    }
    if (body != 0) {
        const summer_image_t *entry = summer_image(pack, body);
        if (!entry) {
            // Restore the complete background before reporting the failure.
            (void)draw_background(img, pack, bg);
            summer_image_invalidate(img);
            return false;
        }
        const uint32_t index = (uint32_t)(entry - pack->images);
        bool ok;
        if (entry->kind == SUMMER_IMG_PORTRAIT) {
            int dst_h = SUMMER_ART_H;
            int dst_w = (int)((uint32_t)entry->width * (uint32_t)dst_h / entry->height);
            if (dst_w <= 0 || dst_w > SUMMER_ART_W) dst_w = SUMMER_ART_W;
            const int x = (SUMMER_ART_W - dst_w) / 2;
            ok = decode_index(img, pack, index, x, 0, dst_w, dst_h, true);
        } else {
            ok = decode_index(img, pack, index, 0, 0, SUMMER_ART_W, SUMMER_ART_H, false);
        }
        if (!ok) {
            // A TJpgDec failure can happen after some MCU blocks were written.
            // Re-draw the complete background (or the black placeholder) so the
            // failed frame never shows a partial portrait.
            (void)draw_background(img, pack, bg);
            summer_image_invalidate(img);
            return false;
        }
    }
    summer_image_invalidate(img);
    return true;
}

bool summer_image_cg(summer_canvas_t *img, const summer_pack_t *pack, uint16_t source_id)
{
    if (!img || !pack) return false;
    summer_image_clear(img);
    if (source_id == 0) return true;
    const summer_image_t *entry = summer_image(pack, source_id);
    if (!entry) return false;
    const uint32_t index = (uint32_t)(entry - pack->images);
    if (!decode_index(img, pack, index, 0, 0, SUMMER_ART_W, SUMMER_ART_H, false)) {
        fill_rect(img->pixels, 0, 0, SUMMER_ART_W, SUMMER_ART_H, 0);
        summer_image_invalidate(img);
        return false;
    }
    summer_image_invalidate(img);
    return true;
}

bool summer_image_cg_index(summer_canvas_t *img, const summer_pack_t *pack, uint32_t image_index)
{
    if (!img || !pack) return false;
    summer_image_clear(img);
    if (!decode_index(img, pack, image_index, 0, 0, SUMMER_ART_W, SUMMER_ART_H, false)) {
        fill_rect(img->pixels, 0, 0, SUMMER_ART_W, SUMMER_ART_H, 0);
        summer_image_invalidate(img);
        return false;
    }
    summer_image_invalidate(img);
    return true;
}

bool summer_image_logo(summer_canvas_t *img, const summer_pack_t *pack, uint16_t image_index,
                       int x, int y, int w, int h)
{
    if (!img || !pack || image_index >= pack->image_count) return false;
    const bool ok = decode_index(img, pack, image_index, x, y, w, h, true);
    summer_image_invalidate(img);
    return ok;
}

bool summer_image_probe(const summer_pack_t *pack, uint16_t source_id)
{
    if (!pack) return false;
    if (source_id == 0) return true;
    const summer_image_t *entry = summer_image(pack, source_id);
    if (!entry) return false;
    if (entry->kind == SUMMER_IMG_SOLID) return true;
    if (entry->kind == SUMMER_IMG_PORTRAIT) {
        int w = 0, h = 0;
        if (!summer_mask_expand(pack, (uint32_t)(entry - pack->images), s_mask_bits,
                                sizeof(s_mask_bits), &w, &h)) {
            return false;
        }
    }
    if (entry->len < 4) return false;
    decode_ctx_t ctx = {
        .jpeg = pack->imgdata + entry->off,
        .len = entry->len,
        .pos = 0,
    };
    JDEC jd;
    const JRESULT prep = jd_prepare(&jd, decode_in, s_jpeg_pool, sizeof(s_jpeg_pool), &ctx);
    if (prep != JDR_OK) return false;
    return jd.width == entry->width && jd.height == entry->height;
}

bool summer_image_probe_index(const summer_pack_t *pack, uint32_t image_index)
{
    const summer_image_t *entry = summer_image_at(pack, image_index);
    if (!entry) return false;
    if (entry->kind == SUMMER_IMG_SOLID) return true;
    if (entry->kind == SUMMER_IMG_PORTRAIT) {
        int w = 0, h = 0;
        if (!summer_mask_expand(pack, image_index, s_mask_bits, sizeof(s_mask_bits), &w, &h)) {
            return false;
        }
    }
    decode_ctx_t ctx = {
        .jpeg = pack->imgdata + entry->off,
        .len = entry->len,
        .pos = 0,
    };
    JDEC jd;
    if (jd_prepare(&jd, decode_in, s_jpeg_pool, sizeof(s_jpeg_pool), &ctx) != JDR_OK) return false;
    return jd.width == entry->width && jd.height == entry->height;
}

bool summer_image_probe_scene(const summer_pack_t *pack, uint16_t bg, uint16_t body)
{
    return summer_image_probe(pack, bg) && summer_image_probe(pack, body);
}
