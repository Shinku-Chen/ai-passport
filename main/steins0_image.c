// main/steins0_image.c —— TJpgDec 分块解码 + 1bpp 遮罩/透明度合成。
#include "steins0_image.h"

#include "esp_log.h"
#include "lvgl.h"
#include "sdkconfig.h"

#if !defined(CONFIG_JD_FORMAT_RGB565) || defined(CONFIG_JD_USE_ROM)
#error "需要 esp_jpeg 自带的 TJpgDec: CONFIG_JD_USE_ROM=n 且 CONFIG_JD_FORMAT_RGB565=y"
#endif

#include "tjpgd.h"

#include <string.h>

static const char *TAG = "steins0_img";

#define THEME_TOP 0x18E3u      // #1B1B1B-ish dark
#define THEME_BOTTOM 0x10A2u

static _Alignas(4) uint8_t s_jpeg_pool[4096];
static uint8_t s_mask_pool[7168];

typedef struct {
    const uint8_t *jpeg;
    uint32_t len;
    uint32_t pos;
    uint16_t *canvas;
    const uint8_t *mask;
    int clip_x0, clip_y0, clip_x1, clip_y1;
    int dst_x;
    int dst_y;
    uint16_t src_w;
    uint16_t src_h;
    uint16_t dst_w;
    uint16_t dst_h;
    uint16_t surface_w;
    uint16_t surface_h;
    uint16_t stride;
    uint16_t opacity;
    uint32_t covered;
    uint32_t written;
} decode_ctx_t;

static void fill_gradient(uint16_t *pixels, uint16_t width, uint16_t height,
                          uint16_t top, uint16_t bottom)
{
    for (int y = 0; y < height; ++y) {
        const uint16_t color = (y < SG0_IMG_H) ? top : bottom;
        uint16_t *row = pixels + (size_t)y * width;
        for (int x = 0; x < width; ++x) row[x] = color;
    }
}

static void fill_rect(uint16_t *pixels, uint16_t surface_w, uint16_t surface_h,
                      int x, int y, int w, int h, uint16_t color)
{
    for (int yy = y; yy < y + h; ++yy) {
        if (yy < 0 || yy >= surface_h) continue;
        uint16_t *row = pixels + (size_t)yy * surface_w;
        for (int xx = x; xx < x + w; ++xx) {
            if (xx >= 0 && xx < surface_w) row[xx] = color;
        }
    }
}

static uint16_t blend565(uint16_t dst, uint16_t src, uint16_t opacity)
{
    if (opacity >= 1000u) return src;
    if (opacity == 0u) return dst;
    const uint32_t r = (((src >> 11) & 0x1Fu) * opacity + ((dst >> 11) & 0x1Fu) * (1000u - opacity)) /
                       1000u;
    const uint32_t g = (((src >> 5) & 0x3Fu) * opacity + ((dst >> 5) & 0x3Fu) * (1000u - opacity)) /
                       1000u;
    const uint32_t b = ((src & 0x1Fu) * opacity + (dst & 0x1Fu) * (1000u - opacity)) / 1000u;
    return (uint16_t)((r << 11) | (g << 5) | b);
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
        if (cy < ctx->clip_y0) continue;
        if (cy >= ctx->clip_y1) break;
        const uint32_t sy = (ctx->dst_h == ctx->src_h)
                                ? dy
                                : (uint32_t)(((uint64_t)dy * ctx->src_h) / ctx->dst_h);
        const uint16_t *src_row = src + (size_t)(sy - top) * block_w;
        const uint8_t *mask_row = ctx->mask ? ctx->mask + (size_t)sy * stride : NULL;
        uint16_t *dst_row = ctx->canvas ? ctx->canvas + (size_t)cy * ctx->stride : NULL;
        for (uint32_t dx = dx0; dx < dx1; ++dx) {
            const int cx = ctx->dst_x + (int)dx;
            if (cx < ctx->clip_x0) continue;
            if (cx >= ctx->clip_x1) break;
            const uint32_t sx = (ctx->dst_w == ctx->src_w)
                                    ? dx
                                    : (uint32_t)(((uint64_t)dx * ctx->src_w) / ctx->dst_w);
            if (mask_row && ((mask_row[sx >> 3] >> (7u - (sx & 7u))) & 1u) == 0) continue;
            ++ctx->covered;
            if (dst_row) {
                const uint16_t value = src_row[sx - left];
                dst_row[cx] = ctx->opacity >= 1000u ? value : blend565(dst_row[cx], value,
                                                                      ctx->opacity);
                ++ctx->written;
            }
        }
    }
    return 1;
}

static bool decode_part(uint16_t *canvas, const sg0_pack_t *pack, const sg0_part_t *part,
                        int dst_x, int dst_y, int dst_w, int dst_h, uint16_t opacity,
                        int clip_x1, int clip_y1, uint16_t surface_w, uint16_t surface_h,
                        uint32_t *covered)
{
    if (!pack || !part || dst_w <= 0 || dst_h <= 0) return false;
    if (part->w == 0 || part->h == 0) return false;
    const sg0_section_t *blob = &pack->sec[SG0_SEC_BLOB];
    if (part->off > blob->size || part->len > blob->size - part->off) return false;
    decode_ctx_t ctx = {
        .jpeg = pack->data + blob->offset + part->off,
        .len = part->len,
        .pos = 0,
        .canvas = canvas,
        .mask = NULL,
        .clip_x0 = 0,
        .clip_y0 = 0,
        .clip_x1 = clip_x1,
        .clip_y1 = clip_y1,
        .dst_x = dst_x,
        .dst_y = dst_y,
        .src_w = part->w,
        .src_h = part->h,
        .dst_w = (uint16_t)dst_w,
        .dst_h = (uint16_t)dst_h,
        .surface_w = surface_w,
        .surface_h = surface_h,
        .stride = surface_w,
        .opacity = opacity,
        .covered = 0,
        .written = 0,
    };
    JDEC jd;
    const JRESULT prep = jd_prepare(&jd, decode_in, s_jpeg_pool, sizeof(s_jpeg_pool), &ctx);
    if (prep != JDR_OK) {
        ESP_LOGE(TAG, "JPEG 解析失败(%d)", (int)prep);
        return false;
    }
    if (jd.width != part->w || jd.height != part->h) {
        ESP_LOGE(TAG, "JPEG 尺寸不符: %ux%u != %ux%u", (unsigned)jd.width,
                 (unsigned)jd.height, (unsigned)part->w, (unsigned)part->h);
        return false;
    }
    const uint8_t *mask = NULL;
    if (part->mask_len) {
        const uint32_t need = ((uint32_t)part->w + 7u) / 8u * part->h;
        uint32_t mask_raw = 0;
        if (!sg0_pack_inflate(pack->data + blob->offset + part->mask_off, part->mask_len,
                              s_mask_pool, sizeof(s_mask_pool), &mask_raw)) {
            ESP_LOGE(TAG, "遮罩解压失败(%u 字节)", (unsigned)part->mask_len);
            return false;
        }
        if (mask_raw < need) {
            ESP_LOGE(TAG, "遮罩尺寸不足: %u < %u", (unsigned)mask_raw, (unsigned)need);
            return false;
        }
        mask = s_mask_pool;
    } else {
        mask = NULL;
    }
    ctx.mask = mask;
    const JRESULT res = jd_decomp(&jd, decode_out, 0);
    if (res != JDR_OK) {
        ESP_LOGE(TAG, "JPEG 解码失败(%d)", (int)res);
        return false;
    }
    if (covered) *covered = ctx.covered;
    // 完全落在裁剪区外/全透明遮罩是合法的“不可见”，不是解码失败。
    return true;
}

void steins0_image_layer_box(const sg0_layer_t *layer, int *x, int *y, int *w, int *h)
{
    if (!layer) return;
    if (x) *x = (int)((int64_t)layer->x * SG0_IMG_W / SG0_VIEW_W);
    if (y) *y = (int)((int64_t)layer->y * SG0_IMG_H / SG0_VIEW_H);
    if (w) *w = (int)((int64_t)layer->w * SG0_IMG_W / SG0_VIEW_W);
    if (h) *h = (int)((int64_t)layer->h * SG0_IMG_H / SG0_VIEW_H);
}

static bool draw_asset(uint16_t *canvas, const sg0_pack_t *pack, uint16_t asset_index,
                       int box_x, int box_y, int box_w, int box_h, uint16_t layer_opacity,
                       int clip_x1, int clip_y1, uint16_t surface_w, uint16_t surface_h,
                       bool draw)
{
    sg0_assetid_t asset;
    if (!sg0_pack_asset(pack, asset_index, &asset)) return false;
    for (uint8_t i = 0; i < asset.part_count; ++i) {
        sg0_part_t part;
        if (!sg0_pack_part(pack, (uint16_t)(asset.part_first + i), &part)) return false;
        if (part.w == 0 || part.h == 0) return false;
        int dx = box_x;
        int dy = box_y;
        int dw = box_w;
        int dh = box_h;
        if (part.ow && part.oh && (part.ax || part.ay || part.w != part.ow ||
                                   part.h != part.oh)) {
            dx = box_x + (int)(((int64_t)part.ax * box_w + part.ow / 2) / part.ow);
            dy = box_y + (int)(((int64_t)part.ay * box_h + part.oh / 2) / part.oh);
            dw = (int)(((int64_t)part.w * box_w + part.ow / 2) / part.ow);
            dh = (int)(((int64_t)part.h * box_h + part.oh / 2) / part.oh);
            if (dw <= 0) dw = 1;
            if (dh <= 0) dh = 1;
        }
        uint32_t covered = 0;
        if (!decode_part(draw ? canvas : NULL, pack, &part, dx, dy, dw, dh,
                         layer_opacity, clip_x1, clip_y1, surface_w, surface_h, &covered)) {
            return false;
        }
    }
    return true;
}

static bool walk_view(uint16_t *canvas, const sg0_pack_t *pack, uint16_t view,
                      uint16_t surface_w, uint16_t surface_h, bool draw)
{
    sg0_view_t v;
    if (!sg0_pack_view(pack, view, &v)) return false;
    if (v.layer_count == 0) return true;
    for (uint8_t i = 0; i < v.layer_count; ++i) {
        sg0_layer_t layer;
        if (!sg0_pack_layer(pack, (uint16_t)(v.layer_first + i), &layer)) return false;
        int x, y, w, h;
        steins0_image_layer_box(&layer, &x, &y, &w, &h);
        if (w <= 0) w = 1;
        if (h <= 0) h = 1;
        if (!draw_asset(canvas, pack, layer.asset, x, y, w, h, layer.opacity,
                        SG0_SCREEN_W, SG0_IMG_H, surface_w, surface_h, draw)) {
            return false;
        }
    }
    return true;
}

bool steins0_image_init_ex(steins0_image_t *image, struct _lv_obj_t *parent, uint16_t *pixels,
                           uint16_t width, uint16_t height)
{
    if (!image || !parent || !pixels || width == 0 || height == 0) return false;
    memset(image, 0, sizeof(*image));
    image->pixels = pixels;
    image->width = width;
    image->height = height;
    for (uint32_t i = 0; i < (uint32_t)width * height; ++i) pixels[i] = THEME_TOP;
    image->canvas = lv_canvas_create(parent);
    if (!image->canvas) return false;
    lv_canvas_set_buffer(image->canvas, pixels, width, height, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(image->canvas, 0, 0);
    lv_obj_set_size(image->canvas, width, height);
    lv_image_set_align(image->canvas, LV_IMAGE_ALIGN_TOP_LEFT);
    return true;
}

bool steins0_image_init(steins0_image_t *image, struct _lv_obj_t *parent, uint16_t *pixels)
{
    return steins0_image_init_ex(image, parent, pixels, SG0_SCREEN_W, SG0_SCREEN_H);
}

void steins0_image_clear(steins0_image_t *image)
{
    if (!image || !image->pixels) return;
    fill_gradient(image->pixels, image->width, image->height, THEME_TOP, THEME_BOTTOM);
    if (image->canvas) lv_obj_invalidate(image->canvas);
}

bool steins0_image_validate_asset(const sg0_pack_t *pack, uint16_t asset)
{
    sg0_assetid_t info;
    if (!sg0_pack_asset(pack, asset, &info) || info.part_count == 0) return false;
    for (uint8_t i = 0; i < info.part_count; ++i) {
        sg0_part_t part;
        if (!sg0_pack_part(pack, (uint16_t)(info.part_first + i), &part)) return false;
        uint32_t covered = 0;
        if (!decode_part(NULL, pack, &part, 0, 0, part.w ? part.w : 1, part.h ? part.h : 1,
                         1000, SG0_SCREEN_W, SG0_IMG_H, SG0_SCREEN_W, SG0_SCREEN_H,
                         &covered)) {
            return false;
        }
    }
    return true;
}

bool steins0_image_validate_view(const sg0_pack_t *pack, uint16_t view)
{
    return walk_view(NULL, pack, view, SG0_SCREEN_W, SG0_SCREEN_H, false);
}

bool steins0_image_show_scene(steins0_image_t *image, const sg0_pack_t *pack, uint16_t view,
                              bool draw)
{
    if (!image || !pack) return false;
    if (draw) {
        if (!walk_view(NULL, pack, view, image->width, image->height, false)) return false;
        fill_gradient(image->pixels, image->width, image->height, THEME_TOP, THEME_BOTTOM);
    }
    if (!walk_view(draw ? image->pixels : NULL, pack, view, image->width, image->height,
                   draw)) return false;
    if (draw && image->canvas) lv_obj_invalidate(image->canvas);
    return true;
}

bool steins0_image_show_cg(steins0_image_t *image, const sg0_pack_t *pack, uint16_t asset,
                           bool draw)
{
    if (!image || !pack) return false;
    sg0_assetid_t info;
    if (!sg0_pack_asset(pack, asset, &info) || !steins0_image_validate_asset(pack, asset)) {
        return false;
    }
    if (draw) {
        fill_rect(image->pixels, image->width, image->height, 0, 0, image->width,
                  image->height, 0x0000u);
        // 完整 fit:320x180 等比居中,不裁切构图。
        int dw = SG0_IMG_W;
        int dh = SG0_IMG_H;
        sg0_part_t part;
        if (sg0_pack_part(pack, info.part_first, &part) && part.ow && part.oh) {
            if ((int64_t)part.ow * dh > (int64_t)part.oh * dw) {
                dh = (int)((int64_t)part.oh * dw / part.ow);
            } else {
                dw = (int)((int64_t)part.ow * dh / part.oh);
            }
        }
        const int dx = (SG0_SCREEN_W - dw) / 2;
        const int dy = (SG0_SCREEN_H - dh) / 2;
        if (!draw_asset(image->pixels, pack, asset, dx, dy, dw, dh, 1000,
                        image->width, image->height, image->width, image->height, true)) {
            return false;
        }
    }
    if (draw && image->canvas) lv_obj_invalidate(image->canvas);
    return true;
}

bool steins0_image_show_title(steins0_image_t *image, const sg0_pack_t *pack, uint16_t asset,
                              bool draw)
{
    if (!image || !pack) return false;
    if (!steins0_image_validate_asset(pack, asset)) return false;
    if (draw) {
        fill_gradient(image->pixels, image->width, image->height, THEME_TOP, THEME_BOTTOM);
        if (!draw_asset(image->pixels, pack, asset, 0, 0, SG0_IMG_W, SG0_IMG_H, 1000,
                        image->width, image->height, image->width, image->height, true)) {
            return false;
        }
    }
    if (draw && image->canvas) lv_obj_invalidate(image->canvas);
    return true;
}

bool steins0_image_show_card(steins0_image_t *image, const sg0_pack_t *pack, uint16_t asset,
                             bool draw)
{
    if (!image || !pack) return false;
    if (!steins0_image_validate_asset(pack, asset)) return false;
    if (draw) {
        fill_gradient(image->pixels, image->width, image->height, THEME_TOP, THEME_BOTTOM);
        if (!draw_asset(image->pixels, pack, asset, 0, 0, SG0_IMG_W, SG0_IMG_H, 1000,
                        image->width, image->height, image->width, image->height, true)) {
            return false;
        }
    }
    if (draw && image->canvas) lv_obj_invalidate(image->canvas);
    return true;
}

bool steins0_image_show_stamp(steins0_image_t *image, const sg0_pack_t *pack, uint16_t asset,
                              bool draw)
{
    if (!image || !pack) return false;
    sg0_assetid_t info;
    if (!sg0_pack_asset(pack, asset, &info) || !steins0_image_validate_asset(pack, asset)) {
        return false;
    }
    if (!draw) return true;
    const int w = image->width ? image->width : 96;
    const int h = image->height ? image->height : 96;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) image->pixels[y * w + x] = 0x0000u;
    }
    sg0_part_t part;
    int pw = w;
    int ph = h;
    if (sg0_pack_part(pack, info.part_first, &part) && part.ow && part.oh) {
        if ((int64_t)part.ow * h > (int64_t)part.oh * w) {
            ph = (int)((int64_t)part.oh * w / part.ow);
        } else {
            pw = (int)((int64_t)part.ow * h / part.oh);
        }
    }
    const int dx = (w - pw) / 2;
    const int dy = (h - ph) / 2;
    if (!draw_asset(image->pixels, pack, asset, dx, dy, pw, ph, 1000, w, h, w, h, true)) {
        return false;
    }
    if (image->canvas) lv_obj_invalidate(image->canvas);
    return true;
}
