// main/paper/paper_gfx.c —— 画布合成 + tjpgd(JPEG) + 调色板 BLOCK / PATCH 解码。
#include "paper_gfx.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "miniz.h"

// 与 paper_pack.c 相同的理由：把 tinfl 状态放静态区，不压在任务栈上。
static tinfl_decompressor s_inflate;
static bool s_inflate_busy;

static size_t inflate_into(void *dst, size_t dst_len, const uint8_t *src, size_t src_len)
{
    if (s_inflate_busy) {
        return TINFL_DECOMPRESS_MEM_TO_MEM_FAILED;
    }
    s_inflate_busy = true;
    tinfl_init(&s_inflate);
    size_t in_bytes = src_len;
    size_t out_bytes = dst_len;
    tinfl_status st = tinfl_decompress(&s_inflate, (const mz_uint8 *)src, &in_bytes,
                                       (mz_uint8 *)dst, (mz_uint8 *)dst, &out_bytes,
                                       TINFL_FLAG_PARSE_ZLIB_HEADER |
                                           TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    size_t produced = (st == TINFL_STATUS_DONE) ? out_bytes : TINFL_DECOMPRESS_MEM_TO_MEM_FAILED;
    s_inflate_busy = false;
    return produced;
}
#include "tjpgd.h"

#include <string.h>

static const char *TAG = "paper_gfx";

#define CANVAS_PX (PAPER_SCR_W * PAPER_SCR_H)

// 实际拿到的画布高度：堆碎片导致 232 行不一定能一次拿到，逐级回退。
static int s_cv_scr_h = PAPER_SCR_H;
#define CANVAS_BYTES (CANVAS_PX * 2)

// 画布是 240x180，而立绘是按 320 宽屏幕烤出来的 → 统一按 3/4 缩小绘制，
// 屏幕侧 LVGL 再放大 4/3，等效原始尺寸（只是不放大，不会糊）。
#define SPR_NUM 1
#define SPR_DEN 1
#define TJPGD_POOL 4096

static uint16_t *s_canvas;
static lv_image_dsc_t s_dsc;
static uint8_t s_jpeg_pool[TJPGD_POOL];

// BLOCK 解码工作区：一块最多 8 行 × (w+1) 字节；w ≤ 190
#define BLOCK_ROW_MAX 200
static uint8_t s_block_buf[8 * (BLOCK_ROW_MAX + 1)];
static uint8_t s_block_prev[BLOCK_ROW_MAX];
static uint8_t s_block_row[BLOCK_ROW_MAX];

// 补丁缓冲按需分配（≤ 4096 px → 8 KB）
static uint8_t *s_patch_px;
static uint16_t s_patch_pal[256];

bool paper_gfx_init(void)
{
    if (s_canvas) {
        return true;
    }
    // 从最高画布逐级回退：真机堆总量够（151,760 B）但**拿不到 148,480 B 连续块**，
    // 所以不能写死一个尺寸，否则直接黑屏。
    s_cv_scr_h = PAPER_SCR_H;
    while (s_cv_scr_h >= 160) {
        s_canvas = heap_caps_malloc((size_t)PAPER_SCR_W * s_cv_scr_h * 2u,
                                    MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
        if (s_canvas) {
            break;
        }
        s_cv_scr_h -= 8;
    }
    if (!s_canvas) {
        ESP_LOGE(TAG, "画布申请失败（最低 %d 行），可用堆 %u B", 160,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
        return false;
    }
    const uint32_t canvas_bytes = (uint32_t)PAPER_SCR_W * (uint32_t)s_cv_scr_h * 2u;
    ESP_LOGI(TAG, "画布 %d 行 = %u B（最大块 %u B）", s_cv_scr_h, (unsigned)canvas_bytes,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
    memset(s_canvas, 0, canvas_bytes);
    memset(&s_dsc, 0, sizeof(s_dsc));
    s_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_dsc.header.w = PAPER_SCR_W;
    s_dsc.header.h = s_cv_scr_h;
    s_dsc.header.stride = PAPER_SCR_W * 2;
    s_dsc.data = (const uint8_t *)s_canvas;
    s_dsc.data_size = canvas_bytes;
    return true;
}

#ifdef PAPER_PACK_USE_HOST_BLOBS
void paper_gfx_test_release_patch(void)
{
    if (s_patch_px) {
        heap_caps_free(s_patch_px);
        s_patch_px = NULL;
    }
}
#endif

void paper_gfx_deinit(void)
{
    if (s_patch_px) {
        heap_caps_free(s_patch_px);
        s_patch_px = NULL;
    }
    if (s_canvas) {
        heap_caps_free(s_canvas);
        s_canvas = NULL;
    }
}

const lv_image_dsc_t *paper_gfx_canvas(void) { return &s_dsc; }
uint16_t *paper_gfx_canvas_data(void) { return s_canvas; }

void paper_gfx_clear(uint16_t color)
{
    if (!s_canvas) {
        return;
    }
    const int n = PAPER_SCR_W * s_cv_scr_h;   // 用实际行数：写满 232 行会越过缓冲 → Store access fault
    for (int i = 0; i < n; i++) {
        s_canvas[i] = color;
    }
}

// ---------------------------------------------------------------- JPEG
typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
    int dst_x;
    int dst_y;
} jpeg_src_t;

static size_t jpeg_in(JDEC *jd, uint8_t *buf, size_t n)
{
    jpeg_src_t *src = (jpeg_src_t *)jd->device;
    size_t left = src->len - src->pos;
    if (n > left) {
        n = left;
    }
    if (buf) {
        memcpy(buf, src->data + src->pos, n);
    }
    src->pos += n;
    return n;
}

// 本版 tjpgd 的 JD_FORMAT==1 按 B,G,R 顺序装 565（首个字节是 B），同一份代码
// 在 parquet 上也必须这样换回来，否则红蓝互换——红多的画面就发紫。
static inline uint16_t bgr_to_rgb565(uint16_t v)
{
    const uint16_t r5 = v & 0x1F;
    const uint16_t g6 = (v >> 5) & 0x3F;
    const uint16_t b5 = (v >> 11) & 0x1F;
    return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

#ifdef PAPER_UI_TEST_HOOKS
uint16_t paper_gfx_test_swap(uint16_t v) { return bgr_to_rgb565(v); }
#endif

// 场景/CG 的 cover 映射：源图 (sw,sh) 等比放大到 (S_CV_W,S_CV_H) 再居中裁切。
// 以前的实现是"原尺寸居中 + 四周清黑"，因为包里的 JPEG 已按 fit 缩放（267x150），
// 所以左右天然各留 26px 黑边——实机就是"CG 没有全屏"。这里改成整数最近邻放大，
// 每源像素映射到目标像素区间，保证目标 320x150 被填满、不留黑。
static int s_cv_w = PAPER_SCR_W;   // 目标宽
static int s_cv_h = PAPER_SCR_H;   // 目标高
static int s_cv_y0 = 0;            // 目标纵向偏移（cover 时可能为负 = 裁上下）
static bool s_cv_scale = false;    // false = 原尺寸 1:1（带 dst_x/dst_y 偏移）

static int jpeg_out(JDEC *jd, void *bitmap, JRECT *rect)
{
    jpeg_src_t *src = (jpeg_src_t *)jd->device;
    const uint16_t *src_px = (const uint16_t *)bitmap;
    const int w = rect->right - rect->left + 1;
    const int sw = (int)jd->width;
    const int sh = (int)jd->height;
    if (!s_cv_scale || sw <= 0 || sh <= 0) {
        for (int y = rect->top; y <= rect->bottom; y++) {
            const int dy = y + src->dst_y;
            if (dy < 0 || dy >= s_cv_scr_h) {
                continue;
            }
            const int dx = rect->left + src->dst_x;
            uint16_t *row = s_canvas + dy * PAPER_SCR_W;
            for (int x = 0; x < w; x++) {
                const int px = dx + x;
                if (px >= 0 && px < PAPER_SCR_W) {
                    row[px] = bgr_to_rgb565(src_px[x]);
                }
            }
            src_px += w;
        }
        return 1;
    }
    for (int y = rect->top; y <= rect->bottom; y++) {
        const int dy0 = s_cv_y0 + y * s_cv_h / sh;
        const int dy1 = s_cv_y0 + (y + 1) * s_cv_h / sh;
        for (int x = 0; x < w; x++) {
            const int sx = rect->left + x;
            const uint16_t color = bgr_to_rgb565(src_px[x]);
            const int dx0 = sx * s_cv_w / sw;
            const int dx1 = (sx + 1) * s_cv_w / sw;
            for (int dy = dy0; dy < dy1; dy++) {
                if (dy < 0 || dy >= s_cv_scr_h) {
                    continue;
                }
                uint16_t *row = s_canvas + dy * PAPER_SCR_W;
                const int x_end = (dx1 > PAPER_SCR_W) ? PAPER_SCR_W : dx1;
                for (int dx = (dx0 < 0 ? 0 : dx0); dx < x_end; dx++) {
                    row[dx] = color;
                }
            }
        }
        src_px += w;
    }
    return 1;
}

static bool jpeg_decode(const uint8_t *data, size_t len, int dst_x, int dst_y)
{
    JDEC jd;
    jpeg_src_t src = {data, len, 0, dst_x, dst_y};
    JRESULT rc = jd_prepare(&jd, jpeg_in, s_jpeg_pool, sizeof(s_jpeg_pool), &src);
    if (rc != JDR_OK) {
        ESP_LOGW(TAG, "jd_prepare 失败: %d", rc);
        return false;
    }
    rc = jd_decomp(&jd, jpeg_out, 0);
    if (rc != JDR_OK) {
        ESP_LOGW(TAG, "jd_decomp 失败: %d (%ux%u)", rc, (unsigned)jd.width,
                 (unsigned)jd.height);
        return false;
    }
    return true;
}

bool paper_gfx_draw_frame(const paper_pack_t *pack, uint16_t image_index)
{
    const paper_img_entry_t *e = paper_image(pack, image_index);
    if (!e || !s_canvas) {
        return false;
    }
    const uint8_t *p = paper_image_data(pack, e);
    switch (e->kind) {
    case PAPER_IMG_JPEG: {
        // 场景/CG 一律 **cover 铺满**：等比放大到宽度铺满，超出部分居中裁掉。
        // （包里的 JPEG 是按 fit 缩放的 267x150，直接 1:1 居中就会留左右黑边。）
        const int sw = (int)e->w;
        const int sh = (int)e->h;
        if (sw > 0 && sh > 0) {
            // 缩放系数取 max(320/sw, 150/sh)：先按宽铺满，再看高度是否够，取大的那个。
            const int by_w_h = (PAPER_SCR_W * sh + sw - 1) / sw;   // 按宽铺满后的高
            if (by_w_h >= s_cv_scr_h) {
                s_cv_w = PAPER_SCR_W;
                s_cv_h = by_w_h;
            } else {
                s_cv_w = (s_cv_scr_h * sw + sh - 1) / sh;
                s_cv_h = s_cv_scr_h;
            }
            s_cv_y0 = (s_cv_scr_h - s_cv_h) / 2;
            s_cv_scale = true;
        }
        paper_gfx_clear(0x0000);
        const bool ok = jpeg_decode(p, e->data_len, 0, 0);
        s_cv_scale = false;
        return ok;
    }
    case PAPER_IMG_BLOCK:
    case PAPER_IMG_PATCH:
        // 兜底：把立绘当全屏图时（例如鉴赏占位）直接画在黑底上
        paper_gfx_clear(0x0000);
        return paper_gfx_draw_sprite(pack, image_index,
                                     (PAPER_SCR_W - (int)e->w * SPR_NUM / SPR_DEN) / 2,
                                     s_cv_scr_h - (int)e->h * SPR_NUM / SPR_DEN);
    default:
        return false;
    }
}

// ---------------------------------------------------------------- BLOCK
static inline void put_px(int x, int y, uint16_t color, uint8_t a)
{
    if (x < 0 || x >= PAPER_SCR_W || y < 0 || y >= s_cv_scr_h) {
        return;
    }
    uint16_t *dst = &s_canvas[y * PAPER_SCR_W + x];
    if (a >= 250) {
        *dst = color;
        return;
    }
    if (a == 0) {
        return;
    }
    uint16_t d = *dst;
    int dr = (d >> 11) & 0x1F, dg = (d >> 5) & 0x3F, db = d & 0x1F;
    int sr = (color >> 11) & 0x1F, sg = (color >> 5) & 0x3F, sb = color & 0x1F;
    int r = (sr * a + dr * (255 - a)) / 255;
    int g = (sg * a + dg * (255 - a)) / 255;
    int b = (sb * a + db * (255 - a)) / 255;
    *dst = (uint16_t)((r << 11) | (g << 5) | b);
}

static int paeth(int a, int b, int c)
{
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) {
        return a;
    }
    return pb <= pc ? b : c;
}

// 逐行反滤波：src = [filter][w bytes]，prev 为上一行（跨块保留）
static void unfilter_row(uint8_t filter, const uint8_t *src, uint8_t *prev, uint8_t *out, int w)
{
    for (int i = 0; i < w; i++) {
        int a = i ? out[i - 1] : 0;
        int b = prev[i];
        int c = i ? prev[i - 1] : 0;
        int v = src[i];
        switch (filter) {
        case 1:
            v += a;
            break;
        case 2:
            v += b;
            break;
        case 3:
            v += (a + b) >> 1;
            break;
        case 4:
            v += paeth(a, b, c);
            break;
        default:
            break;
        }
        out[i] = (uint8_t)v;
    }
}

static bool blit_block(const paper_img_entry_t *e, const uint8_t *payload, int ox, int oy)
{
    const uint8_t *p = payload;
    uint16_t w = (uint16_t)(p[0] | (p[1] << 8));
    uint16_t h = (uint16_t)(p[2] | (p[3] << 8));
    uint16_t pal_count = (uint16_t)(p[4] | (p[5] << 8));
    uint16_t block_rows = (uint16_t)(p[6] | (p[7] << 8));
    uint16_t block_count = (uint16_t)(p[8] | (p[9] << 8));
    uint32_t raw_block = (uint32_t)p[12] | ((uint32_t)p[13] << 8) | ((uint32_t)p[14] << 16) |
                         ((uint32_t)p[15] << 24);
    (void)e;
    if (w == 0 || h == 0 || pal_count == 0 || block_rows == 0 || w > BLOCK_ROW_MAX ||
        raw_block > sizeof(s_block_buf)) {
        ESP_LOGW(TAG, "BLOCK 头异常 w=%u h=%u pal=%u rows=%u raw=%u", w, h, pal_count,
                 block_rows, (unsigned)raw_block);
        return false;
    }
    const uint8_t *pal = p + 16;
    const uint8_t *alpha = pal + pal_count * 2;
    const uint8_t *lens = alpha + pal_count;
    const uint8_t *streams = lens + block_count * 4;

    for (uint16_t i = 0; i < pal_count; i++) {
        s_patch_pal[i] = (uint16_t)(pal[i * 2] | (pal[i * 2 + 1] << 8));
    }
    memset(s_block_prev, 0, w);
    for (uint16_t b = 0; b < block_count; b++) {
        uint32_t clen = (uint32_t)lens[b * 4] | ((uint32_t)lens[b * 4 + 1] << 8) |
                        ((uint32_t)lens[b * 4 + 2] << 16) | ((uint32_t)lens[b * 4 + 3] << 24);
        size_t out = inflate_into(s_block_buf, sizeof(s_block_buf), streams, clen);
        if (out == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) {
            ESP_LOGW(TAG, "BLOCK 块 %u 解压失败 (%u B)", b, (unsigned)clen);
            return false;
        }
        streams += clen;
        int rows = block_rows;
        if (b * block_rows + rows > h) {
            rows = h - b * block_rows;
        }
        const uint8_t *rp = s_block_buf;
        for (int r = 0; r < rows; r++) {
            uint8_t filter = rp[0];
            unfilter_row(filter, rp + 1, s_block_prev, s_block_row, w);
            memcpy(s_block_prev, s_block_row, w);
            const int y = oy + ((b * block_rows + r) * SPR_NUM) / SPR_DEN;
            if (y >= 0 && y < s_cv_scr_h) {
                for (int x = 0; x < w; x++) {
                    uint8_t idx = s_block_row[x];
                    if (idx >= pal_count) {
                        continue;
                    }
                    put_px(ox + (x * SPR_NUM) / SPR_DEN, y, s_patch_pal[idx], alpha[idx]);
                }
            }
            rp += w + 1;
        }
    }
    return true;
}

#define PATCH_PX_MAX 4096
static uint8_t s_mask_buf[190 * 240 / 8 + 32];

static int popcount8(uint8_t v)
{
    int n = 0;
    while (v) {
        n += v & 1;
        v >>= 1;
    }
    return n;
}

static bool apply_patch(const paper_pack_t *pack, const paper_img_entry_t *e, int ox, int oy)
{
    const paper_img_patch_t pr = paper_image_patch(pack, e);
    const uint8_t *pay = paper_image_payload(pack, e);
    if (pr.w == 0 || pr.h == 0 || (int)pr.w * pr.h > PATCH_PX_MAX) {
        return false;
    }
    uint32_t mask_len = (uint32_t)pay[0] | ((uint32_t)pay[1] << 8) | ((uint32_t)pay[2] << 16) |
                        ((uint32_t)pay[3] << 24);
    uint32_t px_len = (uint32_t)pay[4] | ((uint32_t)pay[5] << 8) | ((uint32_t)pay[6] << 16) |
                      ((uint32_t)pay[7] << 24);
    uint32_t mask_bytes = (uint32_t)((pr.w + 7) / 8) * pr.h;
    if (mask_bytes > sizeof(s_mask_buf) || 8u + mask_len + px_len > e->data_len - e->extra_len) {
        ESP_LOGW(TAG, "PATCH 长度异常 mask=%u px=%u extra=%u len=%u", (unsigned)mask_len,
                 (unsigned)px_len, e->extra_len, (unsigned)e->data_len);
        return false;
    }
    size_t out = inflate_into(s_mask_buf, sizeof(s_mask_buf), pay + 8, mask_len);
    if (out != mask_bytes) {
        ESP_LOGW(TAG, "PATCH 掩码解压不符 %u != %u", (unsigned)out, (unsigned)mask_bytes);
        return false;
    }
    int count = 0;
    for (uint32_t i = 0; i < mask_bytes; i++) {
        count += popcount8(s_mask_buf[i]);
    }
    if (count <= 0 || count > PATCH_PX_MAX) {
        return false;
    }
    if (!s_patch_px) {
        s_patch_px = heap_caps_malloc(PATCH_PX_MAX * 2, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
        if (!s_patch_px) {
            ESP_LOGW(TAG, "补丁缓冲申请失败");
            return false;
        }
    }
    out = inflate_into(s_patch_px, PATCH_PX_MAX * 2, pay + 8 + mask_len, px_len);
    if (out != (size_t)count * 2) {
        ESP_LOGW(TAG, "PATCH 像素流长度不符 %u != %d*2", (unsigned)out, count);
        return false;
    }
    int k = 0;
    for (int y = 0; y < (int)pr.h; y++) {
        for (int x = 0; x < (int)pr.w; x++) {
            int bit = y * pr.w + x;
            if (!(s_mask_buf[bit >> 3] & (0x80 >> (bit & 7)))) {
                continue;
            }
            uint16_t color = (uint16_t)(s_patch_px[k * 2] | (s_patch_px[k * 2 + 1] << 8));
            k++;
            // 补丁只覆盖完全不透明的变更点（打包器保证），因此整像素替换
            put_px(ox + pr.x + x, oy + pr.y + y, color, 255);
        }
    }
    return true;
}

bool paper_gfx_draw_sprite(const paper_pack_t *pack, uint16_t image_index, int x, int y)
{
    const paper_img_entry_t *e = paper_image(pack, image_index);
    if (!e || !s_canvas) {
        return false;
    }
    if (e->kind == PAPER_IMG_BLOCK) {
        return blit_block(e, paper_image_data(pack, e), x, y);
    }
    if (e->kind == PAPER_IMG_PATCH) {
        const paper_img_patch_t pr = paper_image_patch(pack, e);
        const paper_img_entry_t *base = paper_image(pack, pr.base);
        if (!base || base->kind != PAPER_IMG_BLOCK) {
            return false;
        }
        if (!blit_block(base, paper_image_data(pack, base), x, y)) {
            return false;
        }
        return apply_patch(pack, e, x, y);
    }
    return false;
}

bool paper_gfx_image_size(const paper_pack_t *pack, uint16_t image_index, int *w, int *h)
{
    const paper_img_entry_t *e = paper_image(pack, image_index);
    if (!e) {
        return false;
    }
    if (w) {
        *w = e->w;
    }
    if (h) {
        *h = e->h;
    }
    return true;
}

void paper_gfx_stats(size_t *canvas_bytes, size_t *scratch_bytes)
{
    if (canvas_bytes) {
        *canvas_bytes = s_canvas ? (size_t)PAPER_SCR_W * (size_t)s_cv_scr_h * 2u : 0;
    }
    if (scratch_bytes) {
        *scratch_bytes = sizeof(s_block_buf) + sizeof(s_block_prev) + sizeof(s_block_row) +
                         sizeof(s_jpeg_pool) + (s_patch_px ? PATCH_PX_MAX * 2 : 0);
    }
}
