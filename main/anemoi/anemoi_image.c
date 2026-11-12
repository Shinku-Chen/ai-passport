// main/anemoi/anemoi_image.c —— 画面解码实现（TJpgDec + 调色板 BLOCK 流式合成）。
#include "anemoi_image.h"

#include "miniz.h"
#include "tjpgd.h"

#include <string.h>

// tinfl 状态放静态区：解压窗口不压在任务栈上。
static tinfl_decompressor s_inflate;

#define ANEM_BLOCK_BUF_MAX (32 * (256 + 1))
static uint8_t s_block_buf[ANEM_BLOCK_BUF_MAX];
static uint16_t s_palette[256];
static uint8_t s_alpha[256];
static uint8_t s_prev_row[256];
static uint8_t s_row[256];

typedef struct {
    const uint8_t *data;
    uint32_t len;
    uint32_t pos;
    anem_canvas_t *canvas;
    int dx;
    int dy;
} jpeg_src_t;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

void anem_image_init(anem_image_ctx_t *ctx)
{
    if (ctx != NULL) {
        memset(ctx, 0, sizeof(*ctx));
    }
}

static size_t inflate_into(void *dst, size_t dst_len, const uint8_t *src, size_t src_len)
{
    tinfl_init(&s_inflate);
    size_t in_bytes = src_len;
    size_t out_bytes = dst_len;
    const tinfl_status status =
        tinfl_decompress(&s_inflate, (const mz_uint8 *)src, &in_bytes, (mz_uint8 *)dst,
                         (mz_uint8 *)dst, &out_bytes,
                         TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    return status == TINFL_STATUS_DONE ? out_bytes : TINFL_DECOMPRESS_MEM_TO_MEM_FAILED;
}

// ---------------------------------------------------------------- JPEG

static size_t jpeg_in(JDEC *jd, uint8_t *buf, size_t n)
{
    jpeg_src_t *src = (jpeg_src_t *)jd->device;
    const uint32_t left = src->len - src->pos;
    if (n > left) {
        n = left;
    }
    if (buf != NULL) {
        memcpy(buf, src->data + src->pos, n);
    }
    src->pos += (uint32_t)n;
    return n;
}

// 本仓库内置的 TJpgDec（R0.03 配置 JD_FORMAT=1）实际按 B,G,R 顺序打包 565
// （红色 = 0x001F / 蓝色 = 0xF800），而 LVGL 要 R 在高 5 位；这里逐像素换 R/B。
// 夹具 tests/anemoi/fixtures/jpeg_split.jpg 会把这条检查住。
static uint16_t s_swap_row[ANEM_CANVAS_W];

static uint16_t rgb565_swap_rb(uint16_t v)
{
    return (uint16_t)((v & 0x07E0u) | ((v & 0x001Fu) << 11) | ((v & 0xF800u) >> 11));
}

static int jpeg_out(JDEC *jd, void *bitmap, JRECT *rect)
{
    jpeg_src_t *src = (jpeg_src_t *)jd->device;
    const uint16_t *pixels = (const uint16_t *)bitmap;
    const int w = rect->right - rect->left + 1;
    for (int y = rect->top; y <= rect->bottom; y++) {
        const int dy = y + src->dy;
        if (dy < 0 || dy >= ANEM_CANVAS_H || w > ANEM_CANVAS_W) {
            pixels += w;
            continue;
        }
        for (int x = 0; x < w; x++) {
            s_swap_row[x] = rgb565_swap_rb(pixels[x]);
        }
        anem_canvas_blit(src->canvas, s_swap_row, w, w, 1, rect->left + src->dx, dy);
        pixels += w;
    }
    return 1;
}

bool anem_image_probe_jpeg(const uint8_t *data, uint32_t len, uint16_t *w, uint16_t *h)
{
    if (data == NULL || len < 4 || data[0] != 0xFF || data[1] != 0xD8) {
        return false;
    }
    (void)w;
    (void)h;
    return true;
}

static bool jpeg_decode(anem_image_ctx_t *ctx, anem_canvas_t *canvas, const uint8_t *data,
                        uint32_t len)
{
    JDEC jd;
    jpeg_src_t src = { .data = data, .len = len, .pos = 0, .canvas = canvas, .dx = 0, .dy = 0 };
    if (jd_prepare(&jd, jpeg_in, ctx->jpeg_pool, sizeof(ctx->jpeg_pool), &src) != JDR_OK) {
        return false;
    }
    // Use the dimensions decoded from the JPEG itself. The pack row's w/h is
    // still validated by tests, but centering must not depend on a stale row.
    src.dx = (ANEM_CANVAS_W - (int)jd.width) / 2;
    src.dy = (ANEM_CANVAS_H - (int)jd.height) / 2;
    return jd_decomp(&jd, jpeg_out, 0) == JDR_OK;
}

bool anem_image_check(const anem_pack_t *pack, uint16_t image_id)
{
    anem_image_t image;
    if (!anem_pack_image(pack, image_id, &image)) {
        return false;
    }
    const uint8_t *data = anem_pack_blob(pack, image.off, image.len);
    if (data == NULL) {
        return false;
    }
    if (image.kind == ANEM_IMG_JPEG) {
        return image.len >= 4 && data[0] == 0xFF && data[1] == 0xD8;
    }
    if (image.kind == ANEM_IMG_BLOCK) {
        return anem_image_probe_block(data, image.len);
    }
    return image.kind == ANEM_IMG_SOLID && image.len == 2;
}

bool anem_image_draw_scene(anem_image_ctx_t *ctx, anem_canvas_t *canvas, const anem_pack_t *pack,
                           uint16_t image_id)
{
    anem_image_t image;
    if (ctx == NULL || canvas == NULL || !anem_pack_image(pack, image_id, &image)) {
        return false;
    }
    const uint8_t *data = anem_pack_blob(pack, image.off, image.len);
    if (data == NULL) {
        return false;
    }
    if (image.kind == ANEM_IMG_SOLID) {
        return anem_image_draw_solid(canvas, pack, image_id);
    }
    if (image.kind != ANEM_IMG_JPEG || !anem_image_probe_jpeg(data, image.len, NULL, NULL)) {
        return false;
    }
    // Source .art uses #fcfaf0 and object-fit:contain. Fill the letterbox
    // with paper, then centre the decoded JPEG; no edge-column smear.
    anem_canvas_fill(canvas, 0xFFDE);
    return jpeg_decode(ctx, canvas, data, image.len);
}

bool anem_image_draw_solid(anem_canvas_t *canvas, const anem_pack_t *pack, uint16_t image_id)
{
    anem_image_t image;
    if (canvas == NULL || !anem_pack_image(pack, image_id, &image) ||
        image.kind != ANEM_IMG_SOLID) {
        return false;
    }
    const uint8_t *data = anem_pack_blob(pack, image.off, image.len);
    if (data == NULL || image.len != 2) {
        return false;
    }
    anem_canvas_fill(canvas, rd16(data));
    return true;
}

// ---------------------------------------------------------------- BLOCK

bool anem_image_probe_block(const uint8_t *data, uint32_t len)
{
    if (data == NULL || len < 16) {
        return false;
    }
    const uint16_t w = rd16(data);
    const uint16_t h = rd16(data + 2);
    const uint16_t pal = rd16(data + 4);
    const uint16_t rows = rd16(data + 6);
    const uint16_t blocks = rd16(data + 8);
    const uint32_t raw = rd32(data + 12);
    if (w == 0 || h == 0 || pal == 0 || pal > 256 || rows == 0 || rows > 32 || w > 255) {
        return false;
    }
    if (blocks == 0 || (uint32_t)blocks * rows < h) {
        return false;
    }
    if (raw < (uint32_t)(w + 1) * rows || raw > ANEM_BLOCK_BUF_MAX) {
        return false;
    }
    const uint32_t header = 16u + pal * 2u + pal + blocks * 4u;
    return header <= len;
}

static int paeth(int a, int b, int c)
{
    const int p = a + b - c;
    const int pa = p > a ? p - a : a - p;
    const int pb = p > b ? p - b : b - p;
    const int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) {
        return a;
    }
    return pb <= pc ? b : c;
}

static void unfilter(uint8_t filter, const uint8_t *src, const uint8_t *prev, uint8_t *out,
                     int w)
{
    for (int i = 0; i < w; i++) {
        const int a = i ? out[i - 1] : 0;
        const int b = prev[i];
        const int c = i ? prev[i - 1] : 0;
        int v = src[i];
        switch (filter) {
        case 1: v += a; break;
        case 2: v += b; break;
        case 3: v += (a + b) >> 1; break;
        case 4: v += paeth(a, b, c); break;
        default: break;
        }
        out[i] = (uint8_t)v;
    }
}

bool anem_image_draw_sprite(anem_image_ctx_t *ctx, anem_canvas_t *canvas, const anem_pack_t *pack,
                            uint16_t image_id, int x, int y)
{
    (void)ctx;
    anem_image_t image;
    if (canvas == NULL || !anem_pack_image(pack, image_id, &image) ||
        image.kind != ANEM_IMG_BLOCK) {
        return false;
    }
    const uint8_t *data = anem_pack_blob(pack, image.off, image.len);
    if (data == NULL || !anem_image_probe_block(data, image.len)) {
        return false;
    }
    const uint16_t w = rd16(data);
    const uint16_t h = rd16(data + 2);
    const uint16_t pal = rd16(data + 4);
    const uint16_t rows = rd16(data + 6);
    const uint16_t blocks = rd16(data + 8);
    const uint32_t raw = rd32(data + 12);
    const uint8_t *palette = data + 16;
    const uint8_t *alpha = palette + pal * 2;
    const uint8_t *lengths = alpha + pal;
    const uint8_t *streams = lengths + blocks * 4;
    const uint8_t *end = data + image.len;
    if (w > sizeof(s_row) || rows > 32) {
        return false;
    }
    for (uint16_t i = 0; i < pal; i++) {
        s_palette[i] = rd16(palette + i * 2);
        s_alpha[i] = alpha[i];
    }
    memset(s_prev_row, 0, w);
    for (uint16_t b = 0; b < blocks; b++) {
        const uint32_t clen = rd32(lengths + b * 4);
        if (streams + clen > end) {
            return false;
        }
        const size_t produced = inflate_into(s_block_buf, raw, streams, clen);
        if (produced == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) {
            return false;
        }
        streams += clen;
        uint16_t block_rows = rows;
        if ((uint32_t)b * rows + block_rows > h) {
            block_rows = (uint16_t)(h - b * rows);
        }
        const uint8_t *row_ptr = s_block_buf;
        for (uint16_t r = 0; r < block_rows; r++) {
            if ((size_t)(row_ptr - s_block_buf) + w + 1 > raw) {
                return false;
            }
            unfilter(row_ptr[0], row_ptr + 1, s_prev_row, s_row, w);
            memcpy(s_prev_row, s_row, w);
            const int dy = y + b * rows + r;
            for (int col = 0; col < w; col++) {
                const uint8_t idx = s_row[col];
                if (idx >= pal) {
                    continue;
                }
                anem_canvas_blend_pixel(canvas, x + col, dy, s_palette[idx], s_alpha[idx]);
            }
            row_ptr += w + 1;
        }
    }
    return true;
}
