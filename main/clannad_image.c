// main/clannad_image.c —— TJpgDec 分块解码 + 1bpp 遮罩合成。 
#include "clannad_image.h"

#include "esp_log.h"
#include "lvgl.h"
#include "sdkconfig.h"

#if !defined(CONFIG_JD_FORMAT_RGB565) || defined(CONFIG_JD_USE_ROM)
#error "CLANNAD reader needs esp_jpeg's TJpgDec: CONFIG_JD_USE_ROM=n and CONFIG_JD_FORMAT_RGB565=y"
#endif

#include "tjpgd.h"

#include <math.h>
#include <string.h>

static const char *TAG = "clannad_img";

#define THEME_TOP 0x0964u      // #0E2A22-ish RGB565
#define THEME_BOTTOM 0x124Eu   // #17443A-ish RGB565

typedef struct {
    const uint8_t *jpeg;
    uint32_t len;
    uint32_t pos;
    uint16_t *canvas;          // NULL = validation only
    const uint8_t *mask;
    uint32_t mask_len;
    uint16_t src_w;
    uint16_t src_h;
    uint16_t dst_w;
    uint16_t dst_h;
    int dst_x;
    int dst_y;
    uint32_t written;
} decode_ctx_t;

static _Alignas(4) uint8_t s_jpeg_pool[4096];

static size_t decode_in(JDEC *jd, uint8_t *buff, size_t nbyte)
{
    decode_ctx_t *ctx = (decode_ctx_t *)jd->device;
    const size_t left = ctx->pos < ctx->len ? (size_t)(ctx->len - ctx->pos) : 0;
    const size_t take = nbyte < left ? nbyte : left;
    if (buff && take) memcpy(buff, ctx->jpeg + ctx->pos, take);
    ctx->pos += (uint32_t)take;
    return take;
}

static void dst_span(uint32_t s0, uint32_t s1, uint32_t src, uint32_t dst,
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
    dst_span(left, right + 1u, ctx->src_w, ctx->dst_w, &dx0, &dx1);
    dst_span(top, bottom + 1u, ctx->src_h, ctx->dst_h, &dy0, &dy1);
    if (dx1 <= dx0 || dy1 <= dy0) return 1;

    for (uint32_t dy = dy0; dy < dy1; ++dy) {
        const int cy = ctx->dst_y + (int)dy;
        if (cy < 0) continue;
        if (cy >= CLAN_IMG_H) break;
        const uint32_t sy = (ctx->dst_h == ctx->src_h)
                                ? dy
                                : (uint32_t)(((uint64_t)dy * ctx->src_h) / ctx->dst_h);
        const uint16_t *src_row = src + (size_t)(sy - top) * block_w;
        const uint8_t *mask_row = ctx->mask ? ctx->mask + (size_t)sy * stride : NULL;
        uint16_t *dst_row = ctx->canvas ? ctx->canvas + (size_t)cy * CLAN_IMG_W : NULL;
        for (uint32_t dx = dx0; dx < dx1; ++dx) {
            const int cx = ctx->dst_x + (int)dx;
            if (cx < 0) continue;
            if (cx >= CLAN_IMG_W) break;
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

static bool decode_asset_to(clannad_image_t *img, const clannad_pack_t *pack,
                            uint16_t index, int dst_x, int dst_y, int dst_w, int dst_h,
                            bool use_mask, bool validate_only)
{
    clannad_asset_t asset;
    if (!pack || !clannad_pack_asset(pack, index, &asset)) {
        ESP_LOGE(TAG, "asset %u missing", index);
        return false;
    }
    if (dst_w <= 0 || dst_h <= 0) return false;
    if (asset.w == 0 || asset.h == 0 || asset.w > 640 || asset.h > 480) return false;
    if (asset.jpeg_off + asset.jpeg_len > pack->blob_size) return false;
    if (use_mask) {
        if (asset.mask_len == 0) return false;
        const uint32_t stride = ((uint32_t)asset.w + 7u) / 8u;
        if (asset.mask_len != stride * asset.h) return false;
    }
    decode_ctx_t ctx = {
        .jpeg = pack->blob + asset.jpeg_off,
        .len = asset.jpeg_len,
        .pos = 0,
        .canvas = validate_only ? NULL : (img ? img->pixels : NULL),
        .mask = use_mask ? pack->blob + asset.mask_off : NULL,
        .mask_len = use_mask ? asset.mask_len : 0,
        .src_w = asset.w,
        .src_h = asset.h,
        .dst_w = (uint16_t)dst_w,
        .dst_h = (uint16_t)dst_h,
        .dst_x = dst_x,
        .dst_y = dst_y,
        .written = 0,
    };
    JDEC jd;
    JRESULT rc = jd_prepare(&jd, decode_in, s_jpeg_pool, sizeof(s_jpeg_pool), &ctx);
    if (rc != JDR_OK) {
        ESP_LOGW(TAG, "asset %u jd_prepare %d", index, (int)rc);
        return false;
    }
    if (jd.width != asset.w || jd.height != asset.h) {
        ESP_LOGW(TAG, "asset %u size mismatch: jpeg %ux%u declared %ux%u", index,
                 (unsigned)jd.width, (unsigned)jd.height, asset.w, asset.h);
        return false;
    }
    rc = jd_decomp(&jd, decode_out, 0);
    if (rc != JDR_OK) {
        ESP_LOGW(TAG, "asset %u jd_decomp %d", index, (int)rc);
        return false;
    }
    if (ctx.written == 0) {
        ESP_LOGW(TAG, "asset %u decoded no visible pixels", index);
        return false;
    }
    return true;
}

bool clannad_image_init(clannad_image_t *img, struct _lv_obj_t *parent,
                        uint16_t *pixels)
{
    if (!img || !parent || !pixels) return false;
    img->pixels = pixels;
    img->canvas = lv_canvas_create(parent);
    if (!img->canvas) return false;
    lv_canvas_set_buffer(img->canvas, pixels, CLAN_IMG_W, CLAN_IMG_H,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(img->canvas, 0, 0);
    lv_obj_set_size(img->canvas, CLAN_IMG_W, CLAN_IMG_H);
    lv_obj_remove_flag(img->canvas, LV_OBJ_FLAG_SCROLLABLE);
    return true;
}

void clannad_image_clear(clannad_image_t *img)
{
    if (!img || !img->pixels) return;
    for (int y = 0; y < CLAN_IMG_H; ++y) {
        const uint16_t color = (y < CLAN_ART_H) ? THEME_TOP : THEME_BOTTOM;
        uint16_t *row = img->pixels + (size_t)y * CLAN_IMG_W;
        for (int x = 0; x < CLAN_IMG_W; ++x) row[x] = color;
    }
    lv_obj_invalidate(img->canvas);
}

void clannad_image_invalidate(clannad_image_t *img)
{
    if (img && img->canvas) lv_obj_invalidate(img->canvas);
}

bool clannad_image_portrait_box(uint8_t count, uint8_t index,
                                const clannad_asset_t *asset, int *x, int *y,
                                int *w, int *h)
{
    if (!asset || count == 0 || count > 3 || index >= count) return false;
    if (asset->ow == 0 || asset->oh == 0) return false;
    const int cell = count >= 3 ? 106 : (count == 2 ? 160 : CLAN_IMG_W);
    const int max_w = count == 1 ? 248 : cell;
    const float sw = (float)max_w / (float)asset->ow;
    const float sh = (float)CLAN_SPRITE_H / (float)asset->oh;
    float scale = sw < sh ? sw : sh;
    if (scale > 2.0f) scale = 2.0f;
    const int full_w = (int)(asset->ow * scale + 0.5f);
    const int box_x = (count >= 3 ? 1 : 0) + cell * index + (cell - full_w) / 2;
    const int origin_x = box_x + (int)(asset->ax * scale + 0.5f);
    *w = (int)(asset->w * scale + 0.5f);
    *h = (int)(asset->h * scale + 0.5f);
    // 立绘基线：用“实际绘制高度”回推，使脚正好落在对话框上沿。
    // （pack 里的图是裁过黑边的，h < full_h，用 full_h 会抬高 (full_h-h) 像素→悬空。）
    const int origin_y = CLAN_PANEL_Y + (int)(asset->ay * scale + 0.5f) - *h;
    if (*w <= 0 || *h <= 0) return false;
    *x = origin_x;
    *y = origin_y;
    return true;
}

bool clannad_image_show_scene(clannad_image_t *img, const clannad_pack_t *pack,
                              uint16_t background, const uint16_t bodies[3],
                              uint8_t body_count)
{
    if (!img || !pack) return false;
    clannad_image_clear(img);
    // 背景铺满可视画面区：按 1:1 宽度缩放到 320，纵向居中裁到 168（等比，不拉伸）。
    bool ok = decode_asset_to(img, pack, background, 0, CLAN_TITLE_Y, CLAN_SCREEN_W,
                              CLAN_TITLE_H, false, false);
    if (!ok) goto fail;
    if (body_count > 3) goto fail;
    for (uint8_t i = 0; i < body_count; ++i) {
        clannad_asset_t asset;
        if (!clannad_pack_asset(pack, bodies[i], &asset)) goto fail;
        int x, y, w, h;
        if (!clannad_image_portrait_box(body_count, i, &asset, &x, &y, &w, &h)) {
            goto fail;
        }
        if (!decode_asset_to(img, pack, bodies[i], x, y, w, h, true, false)) {
            goto fail;
        }
    }
    lv_obj_invalidate(img->canvas);
    return true;

fail:
    // The canvas is written in place, so a later component can fail after an
    // earlier one has already reached the display. Never leave that half frame
    // behind: clear the whole canvas and invalidate before reporting failure.
    clannad_image_clear(img);
    return false;
}

bool clannad_image_show_cg(clannad_image_t *img, const clannad_pack_t *pack,
                           uint16_t asset)
{
    if (!img || !pack) return false;
    clannad_image_clear(img);
    const bool ok = decode_asset_to(img, pack, asset, 0, CLAN_TITLE_Y, CLAN_SCREEN_W,
                                    CLAN_TITLE_H, false, false);
    if (!ok) {
        clannad_image_clear(img);
        return false;
    }
    lv_obj_invalidate(img->canvas);
    return true;
}

bool clannad_image_show_asset(clannad_image_t *img, const clannad_pack_t *pack,
                              uint16_t asset, int x, int y, int w, int h,
                              bool use_mask)
{
    if (!img || !pack) return false;
    const bool ok = decode_asset_to(img, pack, asset, x, y, w, h, use_mask, false);
    if (!ok) {
        // Title art is composed from several show_asset calls. A failed later
        // component must not leave the already-drawn earlier components on
        // screen, so the component failure clears the complete composition.
        clannad_image_clear(img);
        return false;
    }
    lv_obj_invalidate(img->canvas);
    return true;
}

bool clannad_image_validate(const clannad_pack_t *pack, uint16_t asset)
{
    clannad_asset_t info;
    if (!pack || !clannad_pack_asset(pack, asset, &info)) return false;
    return decode_asset_to(NULL, pack, asset, 0, 0, info.w, info.h,
                           info.mask_len > 0, true);
}
