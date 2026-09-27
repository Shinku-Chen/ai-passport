// main/tsxx_image.c —— 美术层合成实现。
//
// 解码走 esp_jpeg 组件自带的 TJpgDec 回调接口(tjpgd):每解出一块 MCU 就在回调里
// 直接合成进画布,所以不再需要"整张解码输出"的暂存区 —— 立绘现在按 240x320 存,
// 一张就要 150 KB,板上没有 PSRAM 放不下。
// 背景与事件图在包里按更小的尺寸存(META 的 bg= / event=,默认 180x240),回调里
// 按最近邻放大到画布;立绘是 1:1,不参与缩放。
#include "tsxx_image.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "sdkconfig.h"

// 分块解码需要组件自带的那份 TJpgDec,而且要它直接输出 RGB565:
//   CONFIG_JD_USE_ROM=n          —— ROM(esp32c3)里那份是旧版 tjpgd,只输出 RGB888,
//                                   头文件与函数签名也不一样;
//   CONFIG_JD_FORMAT_RGB565=y    —— 让 tjpgd 直接给 16 位像素,省掉每像素的 888->565。
// 两个都在 sdkconfig.defaults 里。本地留着旧 sdkconfig 的话这里会直接编译失败,
// 而不是悄悄退回 RGB888。
#if !defined(CONFIG_JD_FORMAT_RGB565) || defined(CONFIG_JD_USE_ROM)
#error "tsxx 的分块 JPEG 解码需要 esp_jpeg 组件自带的 TJpgDec:请在 sdkconfig.defaults 里保留 CONFIG_JD_USE_ROM=n 与 CONFIG_JD_FORMAT_RGB565=y,并删掉本地 sdkconfig 后重新配置。"
#endif

#include "tjpgd.h"

#include <string.h>

static const char *TAG = "tsxx_img";

// 主题底色 #12172B 的 RGB565 形式(没有背景的页、画布清屏都用它)。
#define TSXX_KEY_COLOR_RGB565 0x10A5u

// tjpgd 的工作池:JD_SZBUF(512 字节输入缓冲)、哈夫曼/量化表、IDCT 与 MCU 缓冲都从
// 这里分。4096 是留了余量的:本包里的 JPEG(Pillow optimize,表很小)实测最多用
// 2824 字节,而带标准满哈夫曼表的 JPEG 要 3476 字节 —— 池子不够时 jd_prepare 会
// 返回 JDR_MEM1,整张图就画不出来。
// 所有解码都在 LVGL 锁里串行执行,所以一份共享工作池就够。
#define TSXX_JPEG_POOL_BYTES 4096u
// alloc_pool 把每块分到 4 字节边界,池子底也必须是 4 字节对齐,回调里的
// (uint16_t *)bitmap 才不会在 RISC-V 上触发未对齐访问。
static _Alignas(4) uint8_t s_jpeg_pool[TSXX_JPEG_POOL_BYTES];

// 一次分块解码的状态。回调拿到的 bitmap 是"这一块"的像素(源图坐标下的矩形),
// 目标位置由 dst_* 决定:src == dst 时 1:1 覆盖,否则最近邻放大。
typedef struct {
    const uint8_t *jpeg;
    uint32_t len;
    uint32_t pos;
    uint16_t *canvas;
    const uint8_t *mask;    // 1bpp 遮罩(NULL = 整块不透明),stride = (src_w + 7) / 8
    int dst_x;              // 目标左上角(画布坐标)
    int dst_y;
    uint16_t src_w;         // 源(JPEG/存储)尺寸
    uint16_t src_h;
    uint16_t dst_w;         // 目标矩形尺寸
    uint16_t dst_h;
    uint32_t written;       // 实际写入的像素数,仅用于日志
} tsxx_decode_t;

static void fill_canvas(tsxx_art_t *img, uint16_t color)
{
    for (uint32_t i = 0; i < (uint32_t)TSXX_ART_W * TSXX_ART_H; ++i) {
        img->pixels[i] = color;
    }
}

// 源区间 [s0, s1) 映射到目标像素区间 [*d0, *d1)。
// 与下面的解码回调同一条公式:目标像素 d 取源像素 floor(d * src / dst)。
static void dst_span(uint32_t s0, uint32_t s1, uint32_t src, uint32_t dst, uint32_t *d0,
                     uint32_t *d1)
{
    *d0 = (s0 * dst + src - 1u) / src;   // ceil:第一个落在 s0 及之后的目标像素
    *d1 = (s1 * dst + src - 1u) / src;
}

// 把源坐标 (x, y, w, h)(源尺寸 src_w x src_h)换算到画布上的目标矩形。
// 补丁用它换算落点,和整帧放大走同一条映射,接缝不会错半个像素。
static void scale_rect(int x, int y, int w, int h, int src_w, int src_h,
                       int *dst_x, int *dst_y, int *dst_w, int *dst_h)
{
    const int x0 = (x * TSXX_ART_W + src_w - 1) / src_w;
    const int x1 = ((x + w) * TSXX_ART_W + src_w - 1) / src_w;
    const int y0 = (y * TSXX_ART_H + src_h - 1) / src_h;
    const int y1 = ((y + h) * TSXX_ART_H + src_h - 1) / src_h;
    *dst_x = x0;
    *dst_y = y0;
    *dst_w = x1 - x0;
    *dst_h = y1 - y0;
}

// tjpgd 的输入回调:从内存里的 JPEG 取数据,越界就少给(解码器会当流结束)。
static size_t decode_in(JDEC *jd, uint8_t *buff, size_t nbyte)
{
    tsxx_decode_t *ctx = (tsxx_decode_t *)jd->device;
    const size_t left = (ctx->pos < ctx->len) ? (size_t)(ctx->len - ctx->pos) : 0;
    const size_t take = (nbyte < left) ? nbyte : left;
    if (buff != NULL && take != 0) {
        memcpy(buff, ctx->jpeg + ctx->pos, take);
    }
    ctx->pos += (uint32_t)take;
    return take;
}

// tjpgd 的输出回调:一块 MCU(源图坐标下的 rect)合成进画布。
// rect 已经被 tjpgd 裁到图像范围内(JPEG 块是 8/16 的倍数),这里再兜一次底:
// 缩放目标不可能越界写。
static int decode_out(JDEC *jd, void *bitmap, JRECT *rect)
{
    tsxx_decode_t *ctx = (tsxx_decode_t *)jd->device;
    const uint16_t *src = (const uint16_t *)bitmap;   // JD_FORMAT=1:RGB565,行距 = 块宽
    uint32_t left = rect->left;
    uint32_t right = rect->right;
    uint32_t top = rect->top;
    uint32_t bottom = rect->bottom;
    if (left >= ctx->src_w || top >= ctx->src_h) {
        return 1;   // 这一块整块在有效区域之外
    }
    if (right >= ctx->src_w) right = (uint32_t)ctx->src_w - 1u;
    if (bottom >= ctx->src_h) bottom = (uint32_t)ctx->src_h - 1u;

    const uint32_t block_w = right - left + 1u;
    const uint32_t stride = ((uint32_t)ctx->src_w + 7u) / 8u;

    uint32_t dx0 = 0;
    uint32_t dx1 = 0;
    uint32_t dy0 = 0;
    uint32_t dy1 = 0;
    dst_span(left, right + 1u, ctx->src_w, ctx->dst_w, &dx0, &dx1);
    dst_span(top, bottom + 1u, ctx->src_h, ctx->dst_h, &dy0, &dy1);

    for (uint32_t dy = dy0; dy < dy1; ++dy) {
        const int canvas_y = ctx->dst_y + (int)dy;
        if (canvas_y < 0) continue;
        if (canvas_y >= TSXX_ART_H) break;
        // 目标像素 dy 取源行 floor(dy * src_h / dst_h);1:1 时就是 dy 自己。
        const uint32_t sy = (ctx->dst_h == ctx->src_h)
                                ? dy
                                : (uint32_t)(((uint64_t)dy * ctx->src_h) / ctx->dst_h);
        const uint16_t *src_row = src + (size_t)(sy - top) * block_w;
        const uint8_t *mask_row = (ctx->mask != NULL) ? ctx->mask + (size_t)sy * stride : NULL;
        uint16_t *dst_row = ctx->canvas + (size_t)canvas_y * TSXX_ART_W;
        for (uint32_t dx = dx0; dx < dx1; ++dx) {
            const int canvas_x = ctx->dst_x + (int)dx;
            if (canvas_x < 0) continue;
            if (canvas_x >= TSXX_ART_W) break;
            const uint32_t sx = (ctx->dst_w == ctx->src_w)
                                    ? dx
                                    : (uint32_t)(((uint64_t)dx * ctx->src_w) / ctx->dst_w);
            if (mask_row != NULL &&
                ((mask_row[sx >> 3] >> (7u - (sx & 7u))) & 1u) == 0) {
                continue;   // 位 0 = 透明,露出下层
            }
            dst_row[canvas_x] = src_row[sx - left];
            ++ctx->written;
        }
    }
    return 1;
}

// 解码一张 JPEG 到画布的 (dst_x, dst_y) 起的 dst_w x dst_h 矩形。
// mask 非空时按 1bpp 遮罩合成(遮罩在源图坐标系里),否则整块不透明覆盖。
static bool decode_image(tsxx_art_t *img, const uint8_t *jpeg, uint32_t len, uint16_t src_w,
                         uint16_t src_h, const uint8_t *mask, int dst_x, int dst_y, int dst_w,
                         int dst_h, uint32_t *written_out)
{
    if (img == NULL || jpeg == NULL || len == 0 || src_w == 0 || src_h == 0 || dst_w <= 0 ||
        dst_h <= 0) {
        ESP_LOGE(TAG, "解码参数非法: %ux%u -> %dx%d", (unsigned)src_w, (unsigned)src_h, dst_w,
                 dst_h);
        return false;
    }
    tsxx_decode_t ctx = {
        .jpeg = jpeg,
        .len = len,
        .pos = 0,
        .canvas = img->pixels,
        .mask = mask,
        .dst_x = dst_x,
        .dst_y = dst_y,
        .src_w = src_w,
        .src_h = src_h,
        .dst_w = (uint16_t)dst_w,
        .dst_h = (uint16_t)dst_h,
        .written = 0,
    };
    JDEC jd;
    const JRESULT prep = jd_prepare(&jd, decode_in, s_jpeg_pool, sizeof(s_jpeg_pool), &ctx);
    if (prep != JDR_OK) {
        ESP_LOGE(TAG, "JPEG 解析失败(%d)", (int)prep);
        return false;
    }
    // 输出尺寸不符时按失败处理:宁可不画,也不要把画面画歪。
    if (jd.width != src_w || jd.height != src_h) {
        ESP_LOGE(TAG, "JPEG 尺寸不符: %ux%u != %ux%u", (unsigned)jd.width, (unsigned)jd.height,
                 (unsigned)src_w, (unsigned)src_h);
        return false;
    }
    const JRESULT res = jd_decomp(&jd, decode_out, 0);
    if (res != JDR_OK) {
        ESP_LOGE(TAG, "JPEG 解码失败(%d)", (int)res);
        return false;
    }
    if (written_out != NULL) *written_out = ctx.written;
    return true;
}

bool tsxx_art_init(tsxx_art_t *img, struct _lv_obj_t *parent, uint16_t *pixels)
{
    if (!img || !parent || !pixels) return false;
    memset(img, 0, sizeof(*img));
    img->pixels = pixels;
    fill_canvas(img, TSXX_KEY_COLOR_RGB565);

    img->canvas = lv_canvas_create(parent);
    if (!img->canvas) {
        ESP_LOGE(TAG, "画布创建失败");
        return false;
    }
    lv_canvas_set_buffer(img->canvas, pixels, TSXX_ART_W, TSXX_ART_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(img->canvas, 0, 0);
    // 画布与屏幕同尺寸:不缩放、不旋转,也就不需要 pivot / 抗锯齿 / lv_image_set_scale()。
    return true;
}

void tsxx_art_clear(tsxx_art_t *img)
{
    if (!img || !img->canvas) return;
    fill_canvas(img, TSXX_KEY_COLOR_RGB565);
    lv_obj_invalidate(img->canvas);
}

// 事件图:整帧直接盖满画布(按 META 的 event= 放大);补丁先画基准帧再把补丁贴上去,
// 补丁矩形的坐标是事件图自己的坐标系,按同一条比例换算到画布。
static bool show_cg(tsxx_art_t *img, const tsxx_pack_t *pack, const tsxx_cg_t *cg)
{
    if (cg->kind == TSXX_CG_PATCH) {
        tsxx_cg_t base;
        tsxx_image_t base_img;
        if (!tsxx_pack_cg(pack, cg->base, &base) || base.kind != TSXX_CG_FRAME ||
            !tsxx_pack_cg_image(pack, &base, &base_img)) {
            ESP_LOGW(TAG, "事件图基准帧不可用: base=%u", (unsigned)cg->base);
            return false;
        }
        if (base_img.w != pack->ev_w || base_img.h != pack->ev_h) {
            ESP_LOGE(TAG, "事件整帧尺寸 %ux%u 与 META event=%ux%u 不符",
                     (unsigned)base_img.w, (unsigned)base_img.h, pack->ev_w, pack->ev_h);
            return false;
        }
        if (!decode_image(img, base_img.jpeg, base_img.jpeg_len, base_img.w, base_img.h, NULL, 0,
                          0, TSXX_ART_W, TSXX_ART_H, NULL)) {
            return false;
        }
    }

    tsxx_image_t view;
    if (!tsxx_pack_cg_image(pack, cg, &view)) {
        ESP_LOGW(TAG, "事件图下标越界: img=%u", (unsigned)cg->img);
        return false;
    }
    if (cg->kind == TSXX_CG_FRAME) {
        if (view.w != pack->ev_w || view.h != pack->ev_h) {
            ESP_LOGE(TAG, "事件整帧尺寸 %ux%u 与 META event=%ux%u 不符", (unsigned)view.w,
                     (unsigned)view.h, pack->ev_w, pack->ev_h);
            return false;
        }
        return decode_image(img, view.jpeg, view.jpeg_len, view.w, view.h, NULL, 0, 0,
                            TSXX_ART_W, TSXX_ART_H, NULL);
    }
    if (view.w != cg->w || view.h != cg->h) {
        ESP_LOGW(TAG, "补丁尺寸与配方不符: %ux%u != %ux%u", (unsigned)view.w, (unsigned)view.h,
                 (unsigned)cg->w, (unsigned)cg->h);
        return false;
    }
    int dst_x = 0;
    int dst_y = 0;
    int dst_w = 0;
    int dst_h = 0;
    scale_rect(cg->x, cg->y, cg->w, cg->h, pack->ev_w, pack->ev_h, &dst_x, &dst_y, &dst_w,
               &dst_h);
    return decode_image(img, view.jpeg, view.jpeg_len, view.w, view.h, NULL, dst_x, dst_y, dst_w,
                        dst_h, NULL);
}

static bool show_sprite(tsxx_art_t *img, const tsxx_pack_t *pack, uint8_t id)
{
    tsxx_sprite_t sprite;
    if (!tsxx_pack_sprite(pack, id, &sprite)) {
        ESP_LOGW(TAG, "立绘下标越界: %u", (unsigned)id);
        return false;
    }
    uint32_t written = 0;
    // 立绘就是画布坐标、1:1 合成,不经过缩放。
    if (!decode_image(img, sprite.jpeg, sprite.jpeg_len, sprite.w, sprite.h, sprite.mask,
                      sprite.x, sprite.y, sprite.w, sprite.h, &written)) {
        return false;
    }
    ESP_LOGI(TAG, "立绘 #%u %ux%u @(%u,%u) -> 合成 %u 像素", (unsigned)id, (unsigned)sprite.w,
             (unsigned)sprite.h, (unsigned)sprite.x, (unsigned)sprite.y, (unsigned)written);
    return true;
}

bool tsxx_art_show(tsxx_art_t *img, const tsxx_pack_t *pack, uint8_t bg, const tsxx_cg_t *cg,
                   uint8_t sprite)
{
    if (!img || !img->canvas || !pack) return false;
    bool ok = true;
    const int64_t start = esp_timer_get_time();

    if (bg == TSXX_NONE8) {
        fill_canvas(img, TSXX_KEY_COLOR_RGB565);
    } else {
        tsxx_image_t view;
        if (!tsxx_pack_bg(pack, bg, &view)) {
            ESP_LOGE(TAG, "背景下标越界: %u", (unsigned)bg);
            ok = false;
        } else if (view.w != pack->bg_w || view.h != pack->bg_h) {
            ESP_LOGE(TAG, "背景尺寸 %ux%u 与 META bg=%ux%u 不符", (unsigned)view.w,
                     (unsigned)view.h, pack->bg_w, pack->bg_h);
            ok = false;
        } else if (!decode_image(img, view.jpeg, view.jpeg_len, view.w, view.h, NULL, 0, 0,
                                 TSXX_ART_W, TSXX_ART_H, NULL)) {
            ok = false;
        }
    }
    if (cg && !show_cg(img, pack, cg)) {
        ok = false;
    }
    if (sprite != TSXX_NONE8 && !show_sprite(img, pack, sprite)) {
        ok = false;
    }
    img->last_decode_ms = (uint32_t)((esp_timer_get_time() - start) / 1000);
    ESP_LOGD(TAG, "一屏合成 %u ms(背景 #%u,立绘 #%u)", (unsigned)img->last_decode_ms,
             (unsigned)bg, (unsigned)sprite);
    lv_obj_invalidate(img->canvas);
    return ok;
}
