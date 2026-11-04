// main/steins_image.c —— 分块 JPEG 解码 + 半分辨率遮罩 + 合成。
#include "steins_image.h"

#include "esp_log.h"
#include "lvgl.h"
#include "sdkconfig.h"

#if !defined(CONFIG_JD_FORMAT_RGB565) || defined(CONFIG_JD_USE_ROM)
#error "Steins 阅读器需要 esp_jpeg 自带的 TJpgDec:CONFIG_JD_USE_ROM=n 且 CONFIG_JD_FORMAT_RGB565=y"
#endif

#include "tjpgd.h"

#include <string.h>

static const char *TAG = "steins_img";

static _Alignas(4) uint8_t s_jpeg_pool[4096];

typedef struct {
    const uint8_t *jpeg;
    uint32_t len;
    uint32_t pos;
    uint16_t *canvas;
    const uint8_t *mask;
    uint16_t mw;
    uint16_t mh;
    int dst_x;
    int dst_y;
    int dst_w;
    int dst_h;
    uint16_t src_w;
    uint16_t src_h;
    uint16_t opacity;    // /1000
    uint32_t written;    // 真正写进画布的像素(带透明度的层可能一个都没写)
    uint32_t produced;   // 解码器吐出的像素,用于判断"这张图能解"
} decode_ctx_t;

static void fill_rect(uint16_t *pixels, int x, int y, int w, int h, uint16_t color)
{
    for (int yy = y; yy < y + h; ++yy) {
        if (yy < 0 || yy >= STEINS_IMAGE_H) continue;
        uint16_t *row = pixels + (size_t)yy * STEINS_IMAGE_W;
        for (int xx = x; xx < x + w; ++xx) {
            if (xx >= 0 && xx < STEINS_IMAGE_W) row[xx] = color;
        }
    }
}

void steins_image_clear(steins_image_t *image, uint16_t color)
{
    if (!image || !image->pixels) return;
    for (int i = 0; i < STEINS_IMAGE_PIXELS; ++i) image->pixels[i] = color;
    if (image->canvas) lv_obj_invalidate(image->canvas);
}

static inline uint16_t rgb565_blend(uint16_t dst, uint16_t src, unsigned alpha)
{
    if (alpha == 0u) return dst;
    if (alpha >= 255u) return src;
    const unsigned inv = 255u - alpha;
    const unsigned dr = (dst >> 11) & 0x1Fu, dg = (dst >> 5) & 0x3Fu, db = dst & 0x1Fu;
    const unsigned sr = (src >> 11) & 0x1Fu, sg = (src >> 5) & 0x3Fu, sb = src & 0x1Fu;
    const unsigned r = (dr * inv + sr * alpha) / 255u;
    const unsigned g = (dg * inv + sg * alpha) / 255u;
    const unsigned b = (db * inv + sb * alpha) / 255u;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

// 半分辨率 1bpp 遮罩：**二值最近邻**采样。
// 原来做过 4 抽头插值，会得到 0/63/127/191/255 五级 alpha —— 立绘内部与边缘都半透明，
// 叠在背景上就是“发灰/发虚”。现在 bit=1 → 255（全不透明），bit=0 → 0（跳过），
// 内部实心、边缘为硬边。
static inline unsigned mask_sample(const decode_ctx_t *ctx, uint32_t sx, uint32_t sy)
{
    if (!ctx->mask || ctx->mw == 0u || ctx->mh == 0u) return 255u;
    const uint32_t bx = sx >> 1;
    const uint32_t by = sy >> 1;
    if (bx >= ctx->mw || by >= ctx->mh) return 0u;
    const uint32_t stride = (ctx->mw + 7u) / 8u;
    const uint8_t *row = ctx->mask + (size_t)by * stride;
    const unsigned bit = (row[bx >> 3] >> (7u - (bx & 7u))) & 1u;
    return bit ? 255u : 0u;
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

static void dst_span(uint32_t s0, uint32_t s1, uint32_t src, uint32_t dst,
                     uint32_t *d0, uint32_t *d1)
{
    *d0 = (s0 * (uint32_t)dst + src - 1u) / src;
    *d1 = (s1 * (uint32_t)dst + src - 1u) / src;
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
    ctx->produced += block_w * (bottom - top + 1u);

    uint32_t dx0, dx1, dy0, dy1;
    dst_span(left, right + 1u, ctx->src_w, (uint32_t)ctx->dst_w, &dx0, &dx1);
    dst_span(top, bottom + 1u, ctx->src_h, (uint32_t)ctx->dst_h, &dy0, &dy1);

    for (uint32_t dy = dy0; dy < dy1; ++dy) {
        const int cy = ctx->dst_y + (int)dy;
        if (cy < 0) continue;
        if (cy >= STEINS_IMAGE_H) break;
        const uint32_t sy = ((uint64_t)dy * ctx->src_h) / (uint32_t)ctx->dst_h;
        if (sy < top || sy > bottom) continue;
        const uint16_t *src_row = src + (size_t)(sy - top) * block_w;
        uint16_t *dst_row = ctx->canvas ? ctx->canvas + (size_t)cy * STEINS_IMAGE_W : NULL;
        for (uint32_t dx = dx0; dx < dx1; ++dx) {
            const int cx = ctx->dst_x + (int)dx;
            if (cx < 0) continue;
            if (cx >= STEINS_IMAGE_W) break;
            const uint32_t sx = ((uint64_t)dx * ctx->src_w) / (uint32_t)ctx->dst_w;
            if (sx < left || sx > right) continue;
            unsigned alpha = mask_sample(ctx, sx, sy);
            if (alpha == 0u) continue;
            if (ctx->opacity < 1000u) alpha = alpha * ctx->opacity / 1000u;
            const uint16_t value = src_row[sx - left];
            if (dst_row) {
                dst_row[cx] = (alpha >= 255u) ? value
                                              : rgb565_blend(dst_row[cx], value, alpha);
            }
            ++ctx->written;
        }
    }
    return 1;
}

static void layer_rect(const steins_asset_t *asset, int x, int y, int w, int h,
                       int *dx, int *dy, int *dw, int *dh)
{
    // 包里的图可能只保存了原图的一块(裁剪框 cx,cy,cw,ch)。目标矩形按同一分数
    // 收缩,所以构图与人物比例不变,只是不再为永远看不见的像素付 Flash。
    const int ox = asset->ow ? asset->ow : 1;
    const int oy = asset->oh ? asset->oh : 1;
    const int x0 = x + (w * (int)asset->cx) / ox;
    const int x1 = x + (w * (int)(asset->cx + asset->cw)) / ox;
    const int y0 = y + (h * (int)asset->cy) / oy;
    const int y1 = y + (h * (int)(asset->cy + asset->ch)) / oy;
    *dx = x0;
    *dy = y0;
    *dw = x1 > x0 ? x1 - x0 : 1;
    *dh = y1 > y0 ? y1 - y0 : 1;
}

static bool decode_asset_to(uint16_t *canvas, const steins_pack_t *pack, uint16_t index,
                            int x, int y, int w, int h, uint16_t opacity,
                            steins_image_error_t *err)
{
    if (err) {
        err->asset = index;
        err->layer = 0;
        err->reason = STEINS_IMG_OK;
    }
    steins_asset_t asset;
    if (!pack || !steins_pack_asset(pack, index, &asset)) {
        if (err) err->reason = STEINS_IMG_ERR_PACK;
        ESP_LOGE(TAG, "资源下标无效: %u", (unsigned)index);
        return false;
    }
    if (w <= 0 || h <= 0) {
        if (err) err->reason = STEINS_IMG_ERR_PACK;
        return false;
    }
    int dx, dy, dw, dh;
    layer_rect(&asset, x, y, w, h, &dx, &dy, &dw, &dh);

    const uint8_t *blob = pack->data + pack->sec_off[STEINS_SEC_BLOB];
    decode_ctx_t ctx = {
        .jpeg = blob + asset.off,
        .len = asset.len,
        .pos = 0,
        .canvas = canvas,
        .mask = asset.mask_len ? blob + asset.mask_off : NULL,
        .mw = asset.mw,
        .mh = asset.mh,
        .dst_x = dx,
        .dst_y = dy,
        .dst_w = dw,
        .dst_h = dh,
        .src_w = asset.w,
        .src_h = asset.h,
        .opacity = opacity ? opacity : 1000u,
        .written = 0,
        .produced = 0,
    };
    JDEC jd;
    const JRESULT prep = jd_prepare(&jd, decode_in, s_jpeg_pool, sizeof(s_jpeg_pool), &ctx);
    if (prep != JDR_OK) {
        if (err) err->reason = STEINS_IMG_ERR_PREPARE;
        ESP_LOGE(TAG, "JPEG 解析失败(%d) asset=%u", (int)prep, (unsigned)index);
        return false;
    }
    if (jd.width != asset.w || jd.height != asset.h) {
        if (err) err->reason = STEINS_IMG_ERR_SIZE;
        ESP_LOGE(TAG, "JPEG 尺寸不符: %ux%u != %ux%u", (unsigned)jd.width,
                 (unsigned)jd.height, (unsigned)asset.w, (unsigned)asset.h);
        return false;
    }
    const JRESULT res = jd_decomp(&jd, decode_out, 0);
    if (res != JDR_OK) {
        if (err) err->reason = STEINS_IMG_ERR_DECOMP;
        ESP_LOGE(TAG, "JPEG 解码失败(%d) asset=%u", (int)res, (unsigned)index);
        return false;
    }
    if (ctx.produced == 0u) {
        if (err) err->reason = STEINS_IMG_ERR_EMPTY;
        return false;
    }
    return true;
}

bool steins_image_draw_asset(steins_image_t *image, const steins_pack_t *pack,
                             uint16_t asset_index, int x, int y, int w, int h,
                             uint16_t opacity, steins_image_error_t *err_out)
{
    if (!image || !image->pixels) return false;
    return decode_asset_to(image->pixels, pack, asset_index, x, y, w, h, opacity, err_out);
}

bool steins_image_validate(const steins_pack_t *pack, uint16_t asset_index,
                           steins_image_error_t *err_out)
{
    return decode_asset_to(NULL, pack, asset_index, 0, 0, 1, 1, 1000u, err_out);
}

// 失败画面:清黑 + 刷屏。这是确定的视觉结果,不会留半帧。
static void show_failure_frame(steins_image_t *image)
{
    steins_image_clear(image, 0x0000u);
    if (image && image->canvas) lv_obj_invalidate(image->canvas);
}

bool steins_image_show_view(steins_image_t *image, const steins_pack_t *pack,
                            uint16_t view_index, steins_image_error_t *err_out)
{
    if (!image || !image->pixels || !pack) return false;
    steins_view_t view;
    if (!steins_pack_view(pack, view_index, &view)) {
        if (err_out) {
            err_out->reason = STEINS_IMG_ERR_PACK;
            err_out->asset = STEINS_NONE;
            err_out->layer = 0;
        }
        show_failure_frame(image);
        return false;
    }
    steins_image_clear(image, 0x0000u);
    steins_image_error_t err;
    for (uint8_t i = 0; i < view.count; ++i) {
        steins_layer_t layer;
        if (!steins_pack_layer(pack, (uint16_t)(view.layer_first + i), &layer)) {
            if (err_out) {
                err_out->reason = STEINS_IMG_ERR_PACK;
                err_out->asset = STEINS_NONE;
                err_out->layer = i;
            }
            show_failure_frame(image);
            return false;
        }
        // 立绘（带遮罩的层）默认完全不做“非说话人变暗”：pack 里的 layer.opacity
        // 是原作用来压暗其他角色的系数，直接乘进 alpha 会让立绘整体发灰。
        // 需要原作那种变暗时把 image->dim_speakers 打开（默认关）。
        steins_asset_t layer_asset;
        const bool is_sprite = steins_pack_asset(pack, layer.asset, &layer_asset) &&
                               layer_asset.mask_len > 0;
        const uint16_t opacity = (is_sprite && !image->dim_speakers) ? 1000u : layer.opacity;
        if (!decode_asset_to(image->pixels, pack, layer.asset, layer.x, layer.y,
                             layer.w, layer.h, opacity, &err)) {
            err.layer = i;
            if (err_out) *err_out = err;
            // 任一层失败:立刻丢弃这半帧,给出确定的失败画面。
            // 不在这里做“无限重试”——重试由调用方在下一次正常渲染时进行。
            show_failure_frame(image);
            return false;
        }
    }
    if (image->canvas) lv_obj_invalidate(image->canvas);
    return true;
}

bool steins_image_show_asset(steins_image_t *image, const steins_pack_t *pack,
                             uint16_t asset_index, bool fit, steins_image_error_t *err_out)
{
    if (!image || !image->pixels || !pack) return false;
    steins_asset_t asset;
    if (!steins_pack_asset(pack, asset_index, &asset)) {
        if (err_out) err_out->reason = STEINS_IMG_ERR_PACK;
        show_failure_frame(image);
        return false;
    }
    steins_image_clear(image, 0x0000u);
    int dw = STEINS_IMAGE_W;
    int dh = STEINS_IMAGE_H;
    int dx = 0;
    int dy = 0;
    if (fit) {
        // 用裁剪后的原始宽高比等比装进画布,不裁切、不拉伸。
        const int cw = asset.cw ? asset.cw : asset.w;
        const int ch = asset.ch ? asset.ch : asset.h;
        int num = STEINS_IMAGE_W, den = cw;
        if ((long)STEINS_IMAGE_H * cw < (long)STEINS_IMAGE_W * ch) {
            num = STEINS_IMAGE_H;
            den = ch;
        }
        dw = cw * num / den;
        dh = ch * num / den;
        if (dw <= 0 || dh <= 0) {
            if (err_out) err_out->reason = STEINS_IMG_ERR_PACK;
            show_failure_frame(image);
            return false;
        }
        dx = (STEINS_IMAGE_W - dw) / 2;
        dy = (STEINS_IMAGE_H - dh) / 2;
    }
    // layer_rect 会把裁剪框映射进目标矩形,所以这里传"整幅图的目标矩形"。
    const bool ok = decode_asset_to(image->pixels, pack, asset_index, dx, dy, dw, dh,
                                    1000u, err_out);
    if (!ok) {
        show_failure_frame(image);
        return false;
    }
    if (image->canvas) lv_obj_invalidate(image->canvas);
    return true;
}

void steins_image_set_dim_speakers(steins_image_t *image, bool on)
{
    if (image) image->dim_speakers = on;
}

bool steins_image_init(steins_image_t *image, struct _lv_obj_t *parent, uint16_t *pixels)
{
    if (!image || !parent || !pixels) return false;
    memset(image, 0, sizeof(*image));
    image->pixels = pixels;
    fill_rect(pixels, 0, 0, STEINS_IMAGE_W, STEINS_IMAGE_H, 0x0000u);
    image->canvas = lv_canvas_create(parent);
    if (!image->canvas) return false;
    lv_canvas_set_buffer(image->canvas, pixels, STEINS_IMAGE_W, STEINS_IMAGE_H,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(image->canvas, 0, 0);
    lv_obj_set_size(image->canvas, STEINS_IMAGE_W, STEINS_IMAGE_H);
    lv_image_set_align(image->canvas, LV_IMAGE_ALIGN_TOP_LEFT);
    return true;
}

void steins_image_place(steins_image_t *image, int y)
{
    if (!image || !image->canvas) return;
    lv_obj_set_pos(image->canvas, 0, y);
}
