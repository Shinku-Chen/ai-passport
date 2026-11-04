// main/parquet/parquet_gfx.c —— 画布合成 + tjpgd(JPEG) + BLOCK/PATCH 解码。
//
// 所有图片都在绘制时解码到画布，不保留解码后的整图：C3 无 PSRAM，320x150 画布
// 96,000 B 之外只有 2 KB 级的工作缓冲。
#include "parquet_gfx.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "parquet_inflate.h"
#include "tjpgd.h"

#include <string.h>

static const char *TAG = "pq_gfx";

// 画布高度运行期决定（240 或 150），行跨度始终 PQ_ART_W。
#define CANVAS_BYTES_AT(h) (PQ_ART_W * (h) * 2)
#define TJPGD_POOL 4096
// 调色板 BLOCK 单行最大宽度：立绘身体缩放后 ≤151、cut-in ≤119、Logo ≤120。
#define BLOCK_W_MAX 168
// 表情补丁上限：320x240 屏上脸部差分最大约 60x40；给 4096 px 上限。
#define PATCH_PX_MAX 4096
#define PATCH_W_MAX 128
#define PATCH_H_MAX 128

// 画布用“条带”拼：双带模式两条 320x120（共 153,600 B，但拆成两块 76,800 B，
// 内部堆更容易给出连续块），单块回退模式一条 320x150。
static uint16_t *s_band[2];
static int s_band_count;   // 2 = 双带(虚拟 240)，1 = 单块 320x150
static int s_canvas_h;     // 虚拟画布高度：240 或 150
static lv_image_dsc_t s_dsc[2];
static uint8_t s_jpeg_pool[TJPGD_POOL];

static uint8_t s_block_rows[8 * (BLOCK_W_MAX + 1)];
static uint8_t s_block_prev[BLOCK_W_MAX];
static uint8_t s_block_row[BLOCK_W_MAX];

#ifdef PQ_GFX_TEST_HOOKS
static int s_last_px, s_last_py, s_last_pw, s_last_ph;   // 最近一次立绘绘制矩形
#endif

static uint8_t s_patch_alpha[PATCH_PX_MAX];
static uint8_t s_patch_rows[8 * PATCH_W_MAX * 2];

static size_t inflate_into(void *dst, size_t dst_len, const uint8_t *src, size_t src_len)
{
    return pq_inflate(dst, dst_len, src, src_len);
}

#ifdef PQ_GFX_TEST_HOOKS
static bool draw_block_at(const uint8_t *payload, uint32_t len, int x0, int y0, uint8_t opa);
static bool draw_patch_at(const uint8_t *payload, uint32_t len, int x0, int y0);
#endif

static void make_dsc(lv_image_dsc_t *dsc, const uint16_t *px, int h)
{
    memset(dsc, 0, sizeof(*dsc));
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.w = PQ_ART_W;
    dsc->header.h = (uint32_t)h;
    dsc->header.stride = PQ_ART_W * 2;
    dsc->data = (const uint8_t *)px;
    dsc->data_size = (uint32_t)PQ_ART_W * (uint32_t)h * 2u;
}

bool pq_gfx_init(void)
{
    if (s_band[0]) {
        return true;
    }
    const size_t free_before =
        heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    const size_t band_bytes = (size_t)PQ_ART_W * PQ_BAND_H * 2u;      // 76,800
    const size_t single_bytes = (size_t)PQ_ART_W * PQ_ART_H * 2u;     // 96,000

    // 首选双带：两块 76,800。真机上单块 153,600 拿不到，但 76,800 可以。
    s_band[0] = heap_caps_malloc(band_bytes, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (s_band[0]) {
        s_band[1] = heap_caps_malloc(band_bytes, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
        if (!s_band[1]) {
            ESP_LOGW(TAG, "第二条带申请失败（%u B），回退单块 320x150",
                     (unsigned)band_bytes);
            heap_caps_free(s_band[0]);
            s_band[0] = NULL;
        } else {
            s_band_count = 2;
            s_canvas_h = PQ_CANVAS_H;
        }
    }
    if (!s_band[0]) {
        s_band[0] = heap_caps_malloc(single_bytes, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
        if (!s_band[0]) {
            ESP_LOGE(TAG, "画布申请失败（%u B），可用堆 %u B，最大连续块 %u B",
                     (unsigned)band_bytes, (unsigned)free_before,
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT |
                                                                MALLOC_CAP_INTERNAL));
            return false;
        }
        s_band_count = 1;
        s_canvas_h = PQ_ART_H;
    }
    if (s_band_count == 2) {
        memset(s_band[0], 0, band_bytes);
        memset(s_band[1], 0, band_bytes);
        make_dsc(&s_dsc[0], s_band[0], PQ_BAND_H);
        make_dsc(&s_dsc[1], s_band[1], PQ_BAND_H);
    } else {
        memset(s_band[0], 0, single_bytes);
        make_dsc(&s_dsc[0], s_band[0], PQ_ART_H);
    }
    ESP_LOGI(TAG, "画布 %d 带 x %u B（320x%d 虚拟高 %d），剩余堆 %u B（申请前 %u B）",
             s_band_count, (unsigned)(s_band_count == 2 ? band_bytes : single_bytes),
             s_band_count == 2 ? PQ_BAND_H : PQ_ART_H, s_canvas_h,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
             (unsigned)free_before);
    return true;
}

void pq_gfx_deinit(void)
{
    for (int i = 0; i < 2; ++i) {
        if (s_band[i]) {
            heap_caps_free(s_band[i]);
            s_band[i] = NULL;
        }
    }
    s_band_count = 0;
    s_canvas_h = 0;
}

int pq_gfx_canvas_height(void) { return s_canvas_h ? s_canvas_h : PQ_ART_H; }
int pq_gfx_band_count(void) { return s_band_count; }

// 虚拟画布的行首指针：双带模式按 y/(y-120) 落到两条带。
static inline uint16_t *row_at(int y)
{
    if (s_band_count == 2) {
        return s_band[y >= PQ_BAND_H ? 1 : 0] + (size_t)(y % PQ_BAND_H) * PQ_ART_W;
    }
    return s_band[0] + (size_t)y * PQ_ART_W;
}

#ifdef PQ_GFX_TEST_HOOKS
bool pq_gfx_test_draw_payload(uint8_t kind, const uint8_t *payload, uint32_t len, int x,
                              int y)
{
    if (!payload) {
        return false;
    }
    if (kind == PQ_IMG_BLOCK) {
        return draw_block_at(payload, len, x, y, 255);
    }
    if (kind == PQ_IMG_PATCH) {
        return draw_patch_at(payload, len, x, y);
    }
    return false;
}

void pq_gfx_test_drop_buffers(void)
{
    // 只用于 host 测试复现"画布未初始化"路径；工作缓冲是静态区，无需释放。
    pq_gfx_deinit();
}

// 测试用：最近一次立绘（身体）的绘制矩形。底边必须等于 PQ_ART_H（面板顶）。
void pq_gfx_test_last_portrait(int *x, int *y, int *w, int *h)
{
    if (x) *x = s_last_px;
    if (y) *y = s_last_py;
    if (w) *w = s_last_pw;
    if (h) *h = s_last_ph;
}

// 测试用：把虚拟画布（双带拼起来的 320xN）整幅拷进 dst。
void pq_gfx_test_flatten(uint16_t *dst)
{
    if (!dst || !s_band[0]) return;
    const int h = pq_gfx_canvas_height();
    for (int y = 0; y < h; y++) {
        const uint16_t *row = row_at(y);
        for (int x = 0; x < PQ_ART_W; x++) {
            dst[y * PQ_ART_W + x] = row[x];
        }
    }
}
#endif

const lv_image_dsc_t *pq_gfx_canvas(int band)
{
    if (band < 0 || band >= s_band_count) {
        return NULL;
    }
    return &s_dsc[band];
}

uint16_t *pq_gfx_canvas_data(void) { return s_band[0]; }

void pq_gfx_clear(uint16_t color)
{
    if (!s_band[0]) {
        return;
    }
    for (int y = 0; y < pq_gfx_canvas_height(); y++) {
        uint16_t *row = row_at(y);
        for (int x = 0; x < PQ_ART_W; x++) {
            row[x] = color;
        }
    }
}

void pq_gfx_dim(uint8_t factor)
{
    if (!s_band[0]) {
        return;
    }
    for (int y = 0; y < pq_gfx_canvas_height(); y++) {
        uint16_t *row = row_at(y);
        for (int x = 0; x < PQ_ART_W; x++) {
            const uint16_t c = row[x];
            const int r = ((c >> 11) & 0x1F) * factor / 255;
            const int g = ((c >> 5) & 0x3F) * factor / 255;
            const int b = (c & 0x1F) * factor / 255;
            row[x] = (uint16_t)((r << 11) | (g << 5) | b);
        }
    }
}

static inline void put_px(int x, int y, uint16_t color, uint8_t a)
{
    if (x < 0 || x >= PQ_ART_W || y < 0 || y >= pq_gfx_canvas_height() || a == 0) {
        return;
    }
    uint16_t *dst = &row_at(y)[x];
    if (a >= 250) {
        *dst = color;
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

// ---------------------------------------------------------------- JPEG
typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
    int dst_x;
    int dst_y;
    // cover 模式：把 (0,0,src_w,src_h) 等比映射进 (dst_x,dst_y,dst_w,dst_h)。
    bool cover;
    int dst_w;
    int dst_h;
    int src_w;
    int src_h;
    int scale_q16;   // 缩放系数 << 16
    int off_x;       // 放大后画面的目标左上角（相对画布）
    int off_y;
} jpeg_src_t;

static size_t jpeg_in(JDEC *jd, uint8_t *buf, size_t n)
{
    jpeg_src_t *src = (jpeg_src_t *)jd->device;
    if (src->pos >= src->len) {
        return 0;
    }
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

// 本版 tjpgd 的 JD_FORMAT==1 按 B,G,R 顺序装 565，这里转回画布用的 RGB565。
static inline uint16_t bgr_to_rgb565(uint16_t v)
{
    uint16_t r5 = v & 0x1F;
    uint16_t g6 = (v >> 5) & 0x3F;
    uint16_t b5 = (v >> 11) & 0x1F;
    return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

// cover：目标像素 -> 源像素（最近邻）。逐目标行扫描，MCU 行只处理落在本次 rect 的。
static void jpeg_out_cover(jpeg_src_t *src, const uint16_t *src_px, const JRECT *rect)
{
    const int sw = rect->right - rect->left + 1;
    for (int y = 0; y < src->dst_h; y++) {
        const int dy = src->dst_y + y;
        if (dy < 0 || dy >= pq_gfx_canvas_height()) continue;
        // 目标行 y 对应的源行（四舍五入，落在 [0,src_h)）
        const int sy = (int)(((long)(y - src->off_y) * 65536) / src->scale_q16);
        if (sy < rect->top || sy > rect->bottom) continue;
        const uint16_t *srow = src_px + (sy - rect->top) * sw;
        uint16_t *drow = row_at(dy);
        for (int x = 0; x < src->dst_w; x++) {
            const int dx = src->dst_x + x;
            if (dx < 0 || dx >= PQ_ART_W) continue;
            const int sx = (int)(((long)(x - src->off_x) * 65536) / src->scale_q16);
            if (sx < rect->left || sx > rect->right) continue;
            drow[dx] = bgr_to_rgb565(srow[sx - rect->left]);
        }
    }
}

static int jpeg_out(JDEC *jd, void *bitmap, JRECT *rect)
{
    jpeg_src_t *src = (jpeg_src_t *)jd->device;
    const uint16_t *src_px = (const uint16_t *)bitmap;
    int w = rect->right - rect->left + 1;
    if (!s_band[0]) {
        return 0;
    }
    if (src->cover) {
        jpeg_out_cover(src, src_px, rect);
        return 1;
    }
    for (int y = rect->top; y <= rect->bottom; y++) {
        int dy = y + src->dst_y;
        if (dy < 0 || dy >= pq_gfx_canvas_height()) {
            src_px += w;
            continue;
        }
        int dx = rect->left + src->dst_x;
        uint16_t *row = row_at(dy);
        for (int x = 0; x < w; x++) {
            int px = dx + x;
            if (px >= 0 && px < PQ_ART_W) {
                row[px] = bgr_to_rgb565(src_px[x]);
            }
        }
        src_px += w;
    }
    return 1;
}

static bool jpeg_decode_ex(const uint8_t *data, size_t len, int dx, int dy, bool cover,
                           int dst_w, int dst_h)
{
    JDEC jd;
    jpeg_src_t src = {data, len, 0, dx, dy, cover, dst_w, dst_h, 0, 0, 65536, 0, 0};
    JRESULT rc = jd_prepare(&jd, jpeg_in, s_jpeg_pool, sizeof(s_jpeg_pool), &src);
    if (rc != JDR_OK) {
        ESP_LOGW(TAG, "jd_prepare 失败: %d", rc);
        return false;
    }
    if ((int)jd.width > PQ_ART_W * 2 || (int)jd.height > PQ_CANVAS_H * 2) {
        ESP_LOGW(TAG, "JPEG 尺寸异常 %ux%u", (unsigned)jd.width, (unsigned)jd.height);
        return false;
    }
    if (cover) {
        const int sw = (int)jd.width;
        const int sh = (int)jd.height;
        if (sw <= 0 || sh <= 0 || dst_w <= 0 || dst_h <= 0) return false;
        // scale = max(dst_w/sw, dst_h/sh)，定点 16.16
        const int sx_q = (dst_w << 16) / sw;
        const int sy_q = (dst_h << 16) / sh;
        src.scale_q16 = sx_q > sy_q ? sx_q : sy_q;
        src.src_w = sw;
        src.src_h = sh;
        const int scaled_w = (int)(((long)sw * src.scale_q16) >> 16);
        const int scaled_h = (int)(((long)sh * src.scale_q16) >> 16);
        src.off_x = dx + (dst_w - scaled_w) / 2;
        src.off_y = dy + (dst_h - scaled_h) / 2;
    }
    rc = jd_decomp(&jd, jpeg_out, 0);
    if (rc != JDR_OK) {
        ESP_LOGW(TAG, "jd_decomp 失败: %d (%ux%u)", rc, (unsigned)jd.width,
                 (unsigned)jd.height);
        return false;
    }
    return true;
}

static bool jpeg_decode(const uint8_t *data, size_t len, int dx, int dy)
{
    return jpeg_decode_ex(data, len, dx, dy, false, 0, 0);
}

bool pq_gfx_draw_jpeg(const pq_pack_t *pack, uint16_t image_index, int dx, int dy)
{
    const pq_img_entry_t *e = pq_img_entry(pack, image_index);
    if (!e || !s_band[0]) {
        return false;
    }
    if (e->kind != PQ_IMG_JPEG) {
        return false;
    }
    const uint8_t *p = pq_img_data(pack, e);
    if (!p) {
        return false;
    }
    return jpeg_decode(p, e->data_len, dx, dy);
}

bool pq_gfx_draw_jpeg_cover(const pq_pack_t *pack, uint16_t image_index, int dx, int dy,
                            int dw, int dh)
{
    const pq_img_entry_t *e = pq_img_entry(pack, image_index);
    if (!e || !s_band[0] || e->kind != PQ_IMG_JPEG) {
        return false;
    }
    const uint8_t *p = pq_img_data(pack, e);
    if (!p) {
        return false;
    }
    return jpeg_decode_ex(p, e->data_len, dx, dy, true, dw, dh);
}

// ---------------------------------------------------------------- BLOCK
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

static void unfilter_row(uint8_t filter, const uint8_t *src, uint8_t *prev, uint8_t *out,
                         int w)
{
    for (int i = 0; i < w; i++) {
        int a = i ? out[i - 1] : 0;
        int b = prev[i];
        int c = i ? prev[i - 1] : 0;
        int v = src[i];
        switch (filter) {
        case 0:
            break;
        case 1:
            v = (v + a) & 0xFF;
            break;
        case 2:
            v = (v + b) & 0xFF;
            break;
        case 3:
            v = (v + ((a + b) >> 1)) & 0xFF;
            break;
        case 4:
            v = (v + paeth(a, b, c)) & 0xFF;
            break;
        default:
            break;
        }
        out[i] = (uint8_t)v;
    }
}

// 解一块 BLOCK 到 (x0,y0)，可乘不透明度（0..255，255=原样）。
static bool draw_block_at(const uint8_t *payload, uint32_t len, int x0, int y0, uint8_t opa)
{
    if (!s_band[0] || len < 16) {
        return false;
    }
    uint16_t w, h, pal_count, rows, nblocks, unused;
    uint32_t row_bytes;
    memcpy(&w, payload, 2);
    memcpy(&h, payload + 2, 2);
    memcpy(&pal_count, payload + 4, 2);
    memcpy(&rows, payload + 6, 2);
    memcpy(&nblocks, payload + 8, 2);
    memcpy(&unused, payload + 10, 2);
    memcpy(&row_bytes, payload + 12, 4);
    (void)unused;
    if (w == 0 || h == 0 || w > BLOCK_W_MAX || pal_count == 0 || pal_count > 256 ||
        rows != 8 || nblocks != (h + 7) / 8 || row_bytes != rows * (w + 1)) {
        ESP_LOGW(TAG, "BLOCK 头非法 w=%u h=%u pal=%u", w, h, pal_count);
        return false;
    }
    uint32_t off = 16;
    if (off + 2u * pal_count + pal_count + 4u * nblocks > len) {
        return false;
    }
    uint16_t palette[256];
    uint8_t alphas[256];
    memcpy(palette, payload + off, 2u * pal_count);
    off += 2u * pal_count;
    memcpy(alphas, payload + off, pal_count);
    off += pal_count;
    const uint8_t *lens = payload + off;
    off += 4u * nblocks;
    memset(s_block_prev, 0, w);
    for (uint16_t b = 0; b < nblocks; b++) {
        uint32_t comp_len;
        memcpy(&comp_len, lens + 4u * b, 4);
        if (off + comp_len > len || comp_len == 0) {
            ESP_LOGW(TAG, "BLOCK 块越界 b=%u off=%u len=%u comp=%u", b, (unsigned)off,
                     (unsigned)len, (unsigned)comp_len);
            return false;
        }
        int remaining = (int)h - (int)b * 8;
        int block_rows = remaining < 8 ? remaining : 8;
        size_t expect = (size_t)block_rows * (w + 1);
        size_t produced = inflate_into(s_block_rows, expect, payload + off, comp_len);
        if (produced != expect) {
            ESP_LOGW(TAG, "BLOCK 解压失败 b=%u expect=%u got=%u w=%u h=%u", b,
                     (unsigned)expect, (unsigned)produced, w, h);
            return false;
        }
        off += comp_len;
        int y = b * 8;
        for (int r = 0; r < block_rows && y + r < h; r++) {
            const uint8_t *rowp = s_block_rows + r * (w + 1);
            uint8_t filter = rowp[0];
            unfilter_row(filter, rowp + 1, s_block_prev, s_block_row, w);
            for (int x = 0; x < w; x++) {
                uint8_t i = s_block_row[x];
                uint8_t a = alphas[i];
                if (opa != 255) {
                    a = (uint8_t)((a * opa + 127) / 255);
                }
                put_px(x0 + x, y0 + y + r, palette[i], a);
            }
            memcpy(s_block_prev, s_block_row, w);
        }
    }
    return true;
}

// ---------------------------------------------------------------- PATCH
static bool draw_patch_at(const uint8_t *payload, uint32_t len, int x0, int y0)
{
    if (!s_band[0] || len < 12) {
        return false;
    }
    uint16_t w, h, rows, nblocks;
    uint32_t alpha_len;
    memcpy(&w, payload, 2);
    memcpy(&h, payload + 2, 2);
    memcpy(&rows, payload + 4, 2);
    memcpy(&nblocks, payload + 6, 2);
    memcpy(&alpha_len, payload + 8, 4);
    if (w == 0 || h == 0 || w > PATCH_W_MAX || h > PATCH_H_MAX || w * h > PATCH_PX_MAX ||
        rows != 8 || nblocks != (h + 7) / 8) {
        ESP_LOGW(TAG, "PATCH 头非法 w=%u h=%u", w, h);
        return false;
    }
    uint32_t off = 12;
    if (off + alpha_len + 4u * nblocks > len) {
        return false;
    }
    size_t produced = inflate_into(s_patch_alpha, (size_t)w * h, payload + off, alpha_len);
    if (produced != (size_t)w * h) {
        return false;
    }
    off += alpha_len;
    const uint8_t *lens = payload + off;
    off += 4u * nblocks;
    for (uint16_t b = 0; b < nblocks; b++) {
        uint32_t comp_len;
        memcpy(&comp_len, lens + 4u * b, 4);
        int block_rows = h - b * 8 < 8 ? h - b * 8 : 8;
        uint32_t raw = (uint32_t)block_rows * w * 2;
        if (off + comp_len > len || comp_len == 0) {
            return false;
        }
        produced = inflate_into(s_patch_rows, raw, payload + off, comp_len);
        if (produced != raw) {
            return false;
        }
        off += comp_len;
        for (int r = 0; r < block_rows; r++) {
            const uint8_t *row = s_patch_rows + r * w * 2;
            for (int x = 0; x < w; x++) {
                uint16_t c;
                memcpy(&c, row + 2 * x, 2);
                put_px(x0 + x, y0 + b * 8 + r, c, s_patch_alpha[(b * 8 + r) * w + x]);
            }
        }
    }
    return true;
}

bool pq_gfx_draw_image(const pq_pack_t *pack, uint16_t image_index, int x, int y)
{
    const pq_img_entry_t *e = pq_img_entry(pack, image_index);
    if (!e) {
        return false;
    }
    const uint8_t *p = pq_img_data(pack, e);
    if (!p) {
        return false;
    }
    switch (e->kind) {
    case PQ_IMG_JPEG:
        return jpeg_decode(p, e->data_len, x, y);
    case PQ_IMG_BLOCK:
        return draw_block_at(p, e->data_len, x, y, 255);
    case PQ_IMG_PATCH:
        return draw_patch_at(p, e->data_len, x, y);
    default:
        return false;
    }
}

int pq_gfx_portrait_width(const pq_pack_t *pack, uint16_t asset_id)
{
    const pq_sprite_t *s = pq_asset_sprite(pack, asset_id);
    if (!s) {
        return 0;
    }
    const pq_img_entry_t *body = pq_img_entry(pack, s->body);
    if (!body) {
        return 0;
    }
    return body->w;
}

#ifdef PQ_GFX_TEST_HOOKS
#endif

bool pq_gfx_draw_portrait(const pq_pack_t *pack, uint16_t asset_id, int center_x,
                          bool *body_ok, bool *patch_ok)
{
    bool ok_body = false, ok_patch = true;
    const pq_sprite_t *s = pq_asset_sprite(pack, asset_id);
    if (!s_band[0] || !s) {
        if (body_ok) {
            *body_ok = false;
        }
        if (patch_ok) {
            *patch_ok = false;
        }
        return false;
    }
    const int left = center_x - s->anchor_x;
    const pq_img_entry_t *body = pq_img_entry(pack, s->body);
    if (!body) {
        ok_body = false;
        ok_patch = false;
    } else {
        // 底边必须正好落在对话框顶（PQ_ART_H=150）：用**解码后的实际高度**回推，
        // 不再依赖 pack 里的 body_dy（小立绘曾因此悬空）。
        const int body_y = PQ_ART_H - (int)body->h;
        const int dy_fix = body_y - (int)s->body_dy;   // 数据一致时为 0
        const uint8_t *p = pq_img_data(pack, body);
        ok_body = p && body->kind == PQ_IMG_BLOCK &&
                  draw_block_at(p, body->data_len, left + s->body_dx, body_y, 255);
#ifdef PQ_GFX_TEST_HOOKS
        s_last_px = left + s->body_dx;
        s_last_py = body_y;
        s_last_pw = (int)body->w;
        s_last_ph = (int)body->h;
#endif
        if (ok_body && s->patch != PQ_NO_IMAGE) {
            const pq_img_entry_t *patch = pq_img_entry(pack, s->patch);
            const uint8_t *pp = patch ? pq_img_data(pack, patch) : NULL;
            // 表情层跟着身体一起平移，保持脸与身体对齐。
            ok_patch = pp && patch->kind == PQ_IMG_PATCH &&
                       draw_patch_at(pp, patch->data_len, left + s->patch_dx,
                                     (int)s->patch_dy + dy_fix);
        }
    }
    if (body_ok) {
        *body_ok = ok_body;
    }
    if (patch_ok) {
        *patch_ok = ok_patch;
    }
    return ok_body && ok_patch;
}

bool pq_gfx_draw_cutin(const pq_pack_t *pack, uint16_t asset_id, uint8_t opacity)
{
    const pq_cutin_t *c = pq_asset_cutin(pack, asset_id);
    if (!c || !s_band[0]) {
        return false;
    }
    const pq_img_entry_t *e = pq_img_entry(pack, c->image);
    const uint8_t *p = e ? pq_img_data(pack, e) : NULL;
    if (!p || e->kind != PQ_IMG_BLOCK) {
        return false;
    }
    return draw_block_at(p, e->data_len, c->x, c->y, opacity);
}

void pq_gfx_stats(size_t *canvas_bytes, size_t *scratch_bytes)
{
    if (canvas_bytes) {
        *canvas_bytes = s_band[0]
                            ? (size_t)PQ_ART_W * (size_t)pq_gfx_canvas_height() * 2u
                            : 0;
    }
    if (scratch_bytes) {
        *scratch_bytes = sizeof(s_jpeg_pool) + sizeof(s_block_rows) + sizeof(s_block_prev) +
                         sizeof(s_block_row) + sizeof(s_patch_alpha) + sizeof(s_patch_rows) +
                         pq_inflate_state_bytes();
    }
}
