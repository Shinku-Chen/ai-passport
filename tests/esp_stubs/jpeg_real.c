// Real TJpgDec wrapper for host image regression (same decoder source as the
// device component; JD_FORMAT=1 -> RGB565).  The input/output callbacks share
// one context because TJpgDec keeps a single `device` pointer.
#include "jpeg_decoder.h"
#include "tjpgd.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
} in_ctx_t;

typedef struct {
    uint8_t *out;
    uint32_t cap;
    size_t out_len;
    uint16_t width;
    uint16_t height;
    int ok;
} out_ctx_t;

typedef struct {
    in_ctx_t in;
    out_ctx_t out;
} jpeg_ctx_t;

static size_t in_cb(JDEC *jd, uint8_t *buf, size_t n)
{
    jpeg_ctx_t *c = (jpeg_ctx_t *)jd->device;
    in_ctx_t *ctx = &c->in;
    const size_t left = ctx->len - ctx->pos;
    if (n > left) n = left;
    if (buf) memcpy(buf, ctx->data + ctx->pos, n);
    ctx->pos += n;
    return n;
}

static int out_cb(JDEC *jd, void *bitmap, JRECT *rect)
{
    jpeg_ctx_t *c = (jpeg_ctx_t *)jd->device;
    out_ctx_t *ctx = &c->out;
    if (!ctx->ok) return 0;
    const int bw = rect->right - rect->left + 1;
    const int bh = rect->bottom - rect->top + 1;
    const size_t need = (size_t)jd->width * jd->height * 2;
    if (need > ctx->cap) { ctx->ok = 0; return 0; }
    const uint16_t *src = (const uint16_t *)bitmap;
    uint16_t *dst = (uint16_t *)ctx->out;
    for (int y = 0; y < bh; ++y) {
        memcpy(dst + (size_t)(rect->top + y) * jd->width + rect->left,
               src + (size_t)y * bw, (size_t)bw * 2);
    }
    ctx->width = jd->width;
    ctx->height = jd->height;
    ctx->out_len = need;
    return 1;
}

esp_err_t esp_jpeg_decode(esp_jpeg_image_cfg_t *cfg, esp_jpeg_image_output_t *img)
{
    if (!cfg || !img || !cfg->indata || !cfg->indata_size) return ESP_ERR_INVALID_ARG;
    uint8_t *work = cfg->advanced.working_buffer;
    size_t work_size = cfg->advanced.working_buffer_size;
    uint8_t *fallback = NULL;
    if (!work || work_size < 4096) {
        work_size = 8192;
        fallback = malloc(work_size);
        if (!fallback) return ESP_ERR_NO_MEM;
        work = fallback;
    }
    static JDEC jd;
    jpeg_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.in.data = cfg->indata;
    ctx.in.len = cfg->indata_size;
    ctx.out.out = cfg->outbuf;
    ctx.out.cap = cfg->outbuf_size;
    ctx.out.ok = 1;
    JRESULT res = jd_prepare(&jd, in_cb, work, work_size, &ctx);
    if (res != JDR_OK) { free(fallback); return ESP_FAIL; }
    if (cfg->out_format != JPEG_IMAGE_FORMAT_RGB565 || cfg->out_scale != JPEG_IMAGE_SCALE_0) {
        free(fallback);
        return ESP_ERR_INVALID_ARG;
    }
    res = jd_decomp(&jd, out_cb, 0);
    free(fallback);
    if (res != JDR_OK || !ctx.out.ok) return ESP_FAIL;
    img->width = ctx.out.width;
    img->height = ctx.out.height;
    img->output_len = ctx.out.out_len;
    return ESP_OK;
}

void jpeg_stub_fail_next(int times) { (void)times; }
int jpeg_stub_decode_count(void) { return 0; }
