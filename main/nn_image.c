// main/nn_image.c —— TJpgDec 直写画布实现。
//
// 内存:JDEC 工作区固定 6 KB(.bss),解码过程不再 malloc;输出回调把每个 MCU
// 直接换算成 RGB565 写进画布,所以整条链路只有“画布”这一块大内存。
#include "nn_image.h"

#include "tjpgd.h"

#include <string.h>

// 4:2:0 基线 JPEG 在 JD_FASTDECODE=1 下约需 4.6 KB;留一点余量。
#define NN_JPEG_POOL_SIZE 6144

static uint8_t s_pool[NN_JPEG_POOL_SIZE];

// TJpgDec 只给一个 device 指针,输入回调和输出回调共用它,所以两者放在同一个
// 上下文里——分成两个结构体会在输出回调里把输入游标当画布指针用。
typedef struct {
    // 输入侧
    const uint8_t *data;
    uint32_t size;
    uint32_t pos;
    // 输出侧
    uint16_t *canvas;
    int cw;
    int ch;
    int x0;
    int y0;
    const uint8_t *mask;
    int mask_stride;
    // 等比缩放（只有标题图用）：目标矩形 (0,0,cw,ch)，源尺寸 sw x sh。
    bool scaled;
    int band_top;    // 本条带在虚拟画布里的起始行（0 = 整幅模式）
    int band_h;      // 本条带高度
    int vh;          // 虚拟画布高度（缩放模式用）
    int sw;
    int sh;
    int scale_q16;
    int off_x;
    int off_y;
} jpeg_ctx_t;

// TJpgDec 的输入回调:直接从 flash 指针取数据(包已经映射在 .rodata)。
static size_t in_func(JDEC *jd, uint8_t *buff, size_t nbyte)
{
    jpeg_ctx_t *src = (jpeg_ctx_t *)jd->device;
    const uint32_t left = src->size - src->pos;
    if (nbyte > left) nbyte = left;
    if (buff) {
        if (nbyte) memcpy(buff, src->data + src->pos, nbyte);
    } else {
        // 跳过 nbyte 字节（TJpgDec 用 buff==NULL 表示 seek）
    }
    src->pos += (uint32_t)nbyte;
    return nbyte;
}

// TJpgDec 的输出回调:一个 MCU 块(RGB888) -> 画布上的 RGB565(带可选遮罩)。
static int out_func(JDEC *jd, void *bitmap, JRECT *rect)
{
    jpeg_ctx_t *dst = (jpeg_ctx_t *)jd->device;
    const uint8_t *src = (const uint8_t *)bitmap;
    const int w = rect->right - rect->left + 1;
    const int h = rect->bottom - rect->top + 1;

    if (dst->scaled) {
        // 目标像素 -> 源像素（最近邻）。逐目标行扫描，只处理本 MCU 覆盖的源行。
        for (int cy = 0; cy < dst->ch; ++cy) {
            const int vy = dst->band_top + cy;           // 虚拟行
            const int sy = (int)(((long)(vy - dst->off_y) * 65536) / dst->scale_q16);
            if (sy < rect->top || sy > rect->bottom) continue;
            const uint8_t *sp_row = src + (size_t)(sy - rect->top) * w * 3;
            uint16_t *row = dst->canvas + (size_t)cy * dst->cw;
            for (int cx = 0; cx < dst->cw; ++cx) {
                const int sx = (int)(((long)(cx - dst->off_x) * 65536) / dst->scale_q16);
                if (sx < rect->left || sx > rect->right) continue;
                const uint8_t *sp = sp_row + (size_t)(sx - rect->left) * 3;
                // tjpgd 输出是 **B,G,R** 顺序（见 nn_jpeg/tjpgd.c: *pix++ = B; G; R），
                // 之前按 RGB 拼 565 → 红蓝互换，皮肤/头发发青。这里按 BGR 取。
                row[cx] = (uint16_t)(((sp[2] & 0xF8u) << 8) | ((sp[1] & 0xFCu) << 3) |
                                     (sp[0] >> 3));
            }
        }
        return 1;
    }

    for (int y = 0; y < h; ++y) {
        const int vy = dst->y0 + rect->top + y;          // 虚拟画布行
        const int cy = vy - dst->band_top;               // 条带内的行
        if (cy < 0 || cy >= dst->ch) continue;
        uint16_t *row = dst->canvas + (size_t)cy * dst->cw;
        const uint8_t *sp = src + (size_t)y * w * 3;
        const uint8_t *mask_row = NULL;
        if (dst->mask) {
            mask_row = dst->mask + (size_t)(rect->top + y) * dst->mask_stride;
        }
        for (int x = 0; x < w; ++x) {
            const int cx = dst->x0 + rect->left + x;
            if (cx < 0 || cx >= dst->cw) { sp += 3; continue; }
            if (mask_row) {
                const int mx = rect->left + x;
                const uint8_t bit = (uint8_t)(0x80u >> (mx & 7));
                if ((mask_row[mx >> 3] & bit) == 0) { sp += 3; continue; }
            }
            // 同上：tjpgd 是 B,G,R，R 在 sp[2]。
            const uint16_t px = (uint16_t)(((sp[2] & 0xF8u) << 8) |
                                           ((sp[1] & 0xFCu) << 3) |
                                           (sp[0] >> 3));
            row[cx] = px;
            sp += 3;
        }
    }
    return 1;   // 继续解码
}

bool nn_jpeg_draw(uint16_t *canvas, int cw, int ch,
                  const uint8_t *jpg, uint32_t jpg_len, int x0, int y0,
                  const uint8_t *mask, int mask_stride)
{
    if (!canvas || !jpg || jpg_len == 0) return false;

    jpeg_ctx_t ctx = { .data = jpg, .size = jpg_len, .pos = 0,
                       .canvas = canvas, .cw = cw, .ch = ch, .x0 = x0, .y0 = y0,
                       .mask = mask, .mask_stride = mask_stride, .scaled = false,
                       .band_top = 0, .band_h = ch, .vh = ch };
    JDEC jd;
    memset(&jd, 0, sizeof(jd));

    JRESULT rc = jd_prepare(&jd, in_func, s_pool, sizeof(s_pool), &ctx);
    if (rc != JDR_OK) return false;
    if (ctx.mask && mask_stride != (int)((jd.width + 7) / 8)) return false;
    rc = jd_decomp(&jd, out_func, 0);
    return rc == JDR_OK;
}

// ---------------------------------------------------------------- 等比缩放
bool nn_jpeg_draw_fit(uint16_t *canvas, int cw, int ch, const uint8_t *jpg, uint32_t jpg_len,
                      bool contain)
{
    if (!canvas || !jpg || jpg_len == 0 || cw <= 0 || ch <= 0) return false;

    jpeg_ctx_t ctx = { .data = jpg, .size = jpg_len, .pos = 0, .canvas = canvas,
                       .cw = cw, .ch = ch, .x0 = 0, .y0 = 0, .mask = NULL,
                       .mask_stride = 0, .scaled = true, .band_top = 0, .band_h = ch,
                       .vh = ch };
    JDEC jd;
    memset(&jd, 0, sizeof(jd));
    if (jd_prepare(&jd, in_func, s_pool, sizeof(s_pool), &ctx) != JDR_OK) return false;
    if (jd.width == 0 || jd.height == 0) return false;

    const int sw = (int)jd.width;
    const int sh = (int)jd.height;
    const int vh = ctx.vh > 0 ? ctx.vh : ch;
    const int x_q = (cw << 16) / sw;
    const int y_q = (vh << 16) / sh;
    ctx.sw = sw;
    ctx.sh = sh;
    ctx.scale_q16 = contain ? (x_q < y_q ? x_q : y_q) : (x_q > y_q ? x_q : y_q);
    const int scaled_w = (int)(((long)sw * ctx.scale_q16) >> 16);
    const int scaled_h = (int)(((long)sh * ctx.scale_q16) >> 16);
    ctx.off_x = (cw - scaled_w) / 2;
    ctx.off_y = (vh - scaled_h) / 2;
    return jd_decomp(&jd, out_func, 0) == JDR_OK;
}

// ---------------------------------------------------------------- 分带
bool nn_jpeg_draw_band(uint16_t *band, int cw, int band_h, const uint8_t *jpg, uint32_t jpg_len,
                       int x0, int vy0, int band_top, const uint8_t *mask, int mask_stride)
{
    if (!band || !jpg || jpg_len == 0 || cw <= 0 || band_h <= 0) return false;
    jpeg_ctx_t ctx = { .data = jpg, .size = jpg_len, .pos = 0, .canvas = band, .cw = cw,
                       .ch = band_h, .x0 = x0, .y0 = vy0, .mask = mask,
                       .mask_stride = mask_stride, .scaled = false, .band_top = band_top,
                       .band_h = band_h, .vh = 0 };
    JDEC jd;
    memset(&jd, 0, sizeof(jd));
    if (jd_prepare(&jd, in_func, s_pool, sizeof(s_pool), &ctx) != JDR_OK) return false;
    if (mask && mask_stride != (int)((jd.width + 7) / 8)) return false;
    return jd_decomp(&jd, out_func, 0) == JDR_OK;
}

bool nn_jpeg_draw_fit_band(uint16_t *band, int cw, int band_h, int vh, const uint8_t *jpg,
                           uint32_t jpg_len, bool contain, int band_top, int anchor_x,
                           int anchor_y)
{
    if (!band || !jpg || jpg_len == 0 || cw <= 0 || band_h <= 0 || vh <= 0) return false;
    jpeg_ctx_t ctx = { .data = jpg, .size = jpg_len, .pos = 0, .canvas = band, .cw = cw,
                       .ch = band_h, .x0 = 0, .y0 = 0, .mask = NULL, .mask_stride = 0,
                       .scaled = true, .band_top = band_top, .band_h = band_h, .vh = vh };
    JDEC jd;
    memset(&jd, 0, sizeof(jd));
    if (jd_prepare(&jd, in_func, s_pool, sizeof(s_pool), &ctx) != JDR_OK) return false;
    if (jd.width == 0 || jd.height == 0) return false;
    const int sw = (int)jd.width;
    const int sh = (int)jd.height;
    const int x_q = (cw << 16) / sw;
    const int y_q = (vh << 16) / sh;
    ctx.sw = sw;
    ctx.sh = sh;
    ctx.scale_q16 = contain ? (x_q < y_q ? x_q : y_q) : (x_q > y_q ? x_q : y_q);
    const int scaled_w = (int)(((long)sw * ctx.scale_q16) >> 16);
    const int scaled_h = (int)(((long)sh * ctx.scale_q16) >> 16);
    // anchor_x=1：右对齐（裁左侧，保留右侧 logo）；否则居中裁。
    ctx.off_x = anchor_x ? (cw - scaled_w) : (cw - scaled_w) / 2;
    // anchor_y=1：顶部对齐（放大后裁**底部**）——CG 人物通常在画面上半部，
    // 居中裁会把头裁掉，底部被对话框挡住反而不影响。
    ctx.off_y = anchor_y ? 0 : (vh - scaled_h) / 2;
    return jd_decomp(&jd, out_func, 0) == JDR_OK;
}
