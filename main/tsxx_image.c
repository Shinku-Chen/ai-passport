// main/tsxx_image.c —— 美术层合成实现。
#include "tsxx_image.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "jpeg_decoder.h"
#include "lvgl.h"

#include <string.h>

static const char *TAG = "tsxx_img";

// 主题底色 #12172B 的 RGB565 形式(没有背景的页、画布清屏都用它)。
#define TSXX_KEY_COLOR_RGB565 0x10A5u

// esp_jpeg 在 C3 上走软件 TJpgDec,JD_FORMAT=0(RGB888 -> 由它转 565):
// swap=false 写出低字节在前 = 小端,正好是 LVGL 要的原生 16 位字节序。
#define TSXX_JPEG_SWAP_BYTES 0

static void fill_canvas(tsxx_art_t *img, uint16_t color)
{
    for (uint32_t i = 0; i < (uint32_t)TSXX_ART_W * TSXX_ART_H; ++i) {
        img->pixels[i] = color;
    }
}

// 解码一张 JPEG 到给定的 RGB565 缓冲(尺寸必须与资源包记录的一致)。
// 输出尺寸不符时按失败处理:宁可不画,也不要把画面画歪。
static bool decode_rgb565(uint16_t *dst, uint32_t dst_pixels, const uint8_t *jpeg, uint32_t len,
                          uint16_t w, uint16_t h, uint32_t *ms_out)
{
    if (!dst || !jpeg || len == 0 || w == 0 || h == 0 ||
        (uint32_t)w * h > dst_pixels) {
        ESP_LOGE(TAG, "解码参数非法: %ux%u,缓冲 %u 像素", (unsigned)w, (unsigned)h,
                 (unsigned)dst_pixels);
        return false;
    }
    esp_jpeg_image_cfg_t cfg = {
        .indata = (uint8_t *)jpeg,
        .indata_size = len,
        .outbuf = (uint8_t *)dst,
        .outbuf_size = dst_pixels * 2u,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
        .flags = { .swap_color_bytes = TSXX_JPEG_SWAP_BYTES },
    };
    esp_jpeg_image_output_t out = { 0 };
    const int64_t start = esp_timer_get_time();
    const esp_err_t err = esp_jpeg_decode(&cfg, &out);
    if (ms_out) *ms_out = (uint32_t)((esp_timer_get_time() - start) / 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "JPEG 解码失败: %s", esp_err_to_name(err));
        return false;
    }
    if (out.width != w || out.height != h) {
        ESP_LOGE(TAG, "JPEG 尺寸不符: %ux%u != %ux%u", (unsigned)out.width, (unsigned)out.height,
                 (unsigned)w, (unsigned)h);
        return false;
    }
    return true;
}

// 把一块 src(w x h)贴到画布的 (x0, y0),越界部分按画布裁剪。用于事件补丁。
static void blit_opaque(tsxx_art_t *img, const uint16_t *src, int w, int h, int x0, int y0)
{
    for (int y = 0; y < h; ++y) {
        const int dst_y = y0 + y;
        if (dst_y < 0) continue;
        if (dst_y >= TSXX_ART_H) break;
        const int span_x = x0 < 0 ? -x0 : 0;
        const int count = (w - span_x) < (TSXX_ART_W - (x0 + span_x)) ? (w - span_x)
                                                                     : (TSXX_ART_W - (x0 + span_x));
        if (count <= 0) continue;
        memcpy(img->pixels + (size_t)dst_y * TSXX_ART_W + (x0 + span_x),
               src + (size_t)y * (size_t)w + span_x, (size_t)count * sizeof(uint16_t));
    }
}

// 把一块 1bpp 遮罩的立绘合成到画布:位 1 = 不透明,位 0 = 露出下层。
// 遮罩在打包时就对齐了立绘边缘(并做过颜色膨胀),所以这里不需要混色。
static uint32_t blit_masked(tsxx_art_t *img, const uint16_t *src, const uint8_t *mask,
                            int w, int h, int x0, int y0)
{
    const int stride = (w + 7) / 8;
    uint32_t written = 0;
    for (int y = 0; y < h; ++y) {
        const int dst_y = y0 + y;
        if (dst_y < 0) continue;
        if (dst_y >= TSXX_ART_H) break;
        const uint16_t *src_row = src + (size_t)y * (size_t)w;
        uint16_t *dst_row = img->pixels + (size_t)dst_y * TSXX_ART_W;
        const uint8_t *mask_row = mask + (size_t)y * (size_t)stride;
        for (int x = 0; x < w; ++x) {
            if (((mask_row[x >> 3] >> (7 - (x & 7))) & 1u) == 0) continue;
            const int dst_x = x0 + x;
            if (dst_x < 0 || dst_x >= TSXX_ART_W) continue;
            dst_row[dst_x] = src_row[x];
            ++written;
        }
    }
    return written;
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
    // 唯一一处"把美术层变成整屏"的地方:围绕左上角放大 4/3。
    // LVGL 会按变换后的区域失效,因此画布内容变化时整块 240x320 都会重绘。
    // 关掉抗锯齿:放大是整数比 4/3 的最近邻,抗锯齿只会更糊,还更费 CPU。
    lv_image_set_pivot(img->canvas, 0, 0);
    lv_image_set_antialias(img->canvas, false);
    // 美术层是 180x240,放大 4/3 铺满 240x320。
    lv_image_set_scale(img->canvas, TSXX_ART_SCALE);
    return true;
}

bool tsxx_art_prepare(tsxx_art_t *img, const tsxx_pack_t *pack)
{
    if (!img || !pack) return false;
    uint32_t needed = 0;
    uint32_t sprite_max = 0;
    uint32_t patch_max = 0;

    // 立绘:每张都要一整块 w*h*2 的解码输出。
    for (uint8_t i = 0; i < tsxx_pack_sprite_count(pack); ++i) {
        tsxx_sprite_t sprite;
        if (!tsxx_pack_sprite(pack, i, &sprite)) continue;
        const uint32_t bytes = (uint32_t)sprite.w * sprite.h * 2u;
        if (bytes > sprite_max) sprite_max = bytes;
    }
    // 事件补丁:和立绘共用同一块暂存区。
    for (uint16_t i = 0; i < tsxx_pack_cg_count(pack); ++i) {
        tsxx_cg_t cg;
        if (!tsxx_pack_cg(pack, i, &cg)) continue;
        if (cg.kind != TSXX_CG_PATCH) continue;
        const uint32_t bytes = (uint32_t)cg.w * cg.h * 2u;
        if (bytes > patch_max) patch_max = bytes;
    }
    needed = sprite_max > patch_max ? sprite_max : patch_max;
    if (needed == 0) {
        ESP_LOGW(TAG, "资源包里没有立绘/补丁,跳过暂存区分配");
        return false;
    }
    if (img->sprite && img->sprite_size >= needed) return true;

    // 换更大的暂存区时先放旧的:堆上没有 PSRAM,不能两份同时存在。
    if (img->sprite) {
        heap_caps_free(img->sprite);
        img->sprite = NULL;
        img->sprite_size = 0;
    }
    img->sprite = heap_caps_malloc(needed, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!img->sprite) {
        ESP_LOGE(TAG, "立绘暂存区分配失败(%u 字节),本次运行不画立绘", (unsigned)needed);
        return false;
    }
    img->sprite_size = needed;
    ESP_LOGI(TAG, "立绘暂存区 %u 字节(立绘最大 %u,补丁最大 %u)", (unsigned)needed,
             (unsigned)sprite_max, (unsigned)patch_max);
    return true;
}

void tsxx_art_clear(tsxx_art_t *img)
{
    if (!img || !img->canvas) return;
    fill_canvas(img, TSXX_KEY_COLOR_RGB565);
    lv_obj_invalidate(img->canvas);
}

// 事件图:整帧直接盖满画布;补丁先画基准帧再把补丁贴上去。
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
        if (!decode_rgb565(img->pixels, (uint32_t)TSXX_ART_W * TSXX_ART_H, base_img.jpeg,
                           base_img.jpeg_len, base_img.w, base_img.h, &img->last_decode_ms)) {
            return false;
        }
    }

    tsxx_image_t view;
    if (!tsxx_pack_cg_image(pack, cg, &view)) {
        ESP_LOGW(TAG, "事件图下标越界: img=%u", (unsigned)cg->img);
        return false;
    }
    if (cg->kind == TSXX_CG_FRAME) {
        if (!decode_rgb565(img->pixels, (uint32_t)TSXX_ART_W * TSXX_ART_H, view.jpeg,
                           view.jpeg_len, view.w, view.h, &img->last_decode_ms)) {
            return false;
        }
        return true;
    }
    if (view.w != cg->w || view.h != cg->h) {
        ESP_LOGW(TAG, "补丁尺寸与配方不符: %ux%u != %ux%u", (unsigned)view.w, (unsigned)view.h,
                 (unsigned)cg->w, (unsigned)cg->h);
        return false;
    }
    if (!img->sprite || img->sprite_size < (uint32_t)cg->w * cg->h * 2u) {
        ESP_LOGW(TAG, "补丁暂存区不足(%u 字节),只画基准帧", (unsigned)img->sprite_size);
        return true;
    }
    uint16_t *patch = (uint16_t *)(void *)img->sprite;
    if (!decode_rgb565(patch, img->sprite_size / 2u, view.jpeg, view.jpeg_len, view.w, view.h,
                       NULL)) {
        return false;
    }
    blit_opaque(img, patch, cg->w, cg->h, cg->x, cg->y);
    return true;
}

static bool show_sprite(tsxx_art_t *img, const tsxx_pack_t *pack, uint8_t id)
{
    tsxx_sprite_t sprite;
    if (!tsxx_pack_sprite(pack, id, &sprite)) {
        ESP_LOGW(TAG, "立绘下标越界: %u", (unsigned)id);
        return false;
    }
    if (!img->sprite || img->sprite_size < (uint32_t)sprite.w * sprite.h * 2u) {
        ESP_LOGW(TAG, "立绘暂存区不足(%u 字节),跳过立绘 %u", (unsigned)img->sprite_size,
                 (unsigned)id);
        return false;
    }
    uint16_t *scratch = (uint16_t *)(void *)img->sprite;
    if (!decode_rgb565(scratch, img->sprite_size / 2u, sprite.jpeg, sprite.jpeg_len, sprite.w,
                       sprite.h, NULL)) {
        return false;
    }
    const uint32_t written =
        blit_masked(img, scratch, sprite.mask, sprite.w, sprite.h, sprite.x, sprite.y);
    ESP_LOGI(TAG, "立绘 #%u %ux%u @(%u,%u) -> 合成 %u 像素", (unsigned)id, (unsigned)sprite.w,
             (unsigned)sprite.h, (unsigned)sprite.x, (unsigned)sprite.y, (unsigned)written);
    return true;
}

bool tsxx_art_show(tsxx_art_t *img, const tsxx_pack_t *pack, uint8_t bg,
                     const tsxx_cg_t *cg, uint8_t sprite)
{
    if (!img || !img->canvas || !pack) return false;
    bool ok = true;

    if (bg == TSXX_NONE8) {
        fill_canvas(img, TSXX_KEY_COLOR_RGB565);
    } else {
        tsxx_image_t view;
        if (!tsxx_pack_bg(pack, bg, &view)) {
            ESP_LOGE(TAG, "背景下标越界: %u", (unsigned)bg);
            ok = false;
        } else if (view.w != TSXX_ART_W || view.h != TSXX_ART_H) {
            ESP_LOGE(TAG, "背景尺寸与画布不符: %ux%u", (unsigned)view.w, (unsigned)view.h);
            ok = false;
        } else if (!decode_rgb565(img->pixels, (uint32_t)TSXX_ART_W * TSXX_ART_H, view.jpeg,
                                  view.jpeg_len, view.w, view.h, &img->last_decode_ms)) {
            ok = false;
        }
    }
    if (ok) {
        ESP_LOGD(TAG, "背景 #%u 解码 %u ms", (unsigned)bg, (unsigned)img->last_decode_ms);
    }
    if (cg && !show_cg(img, pack, cg)) {
        ok = false;
    }
    if (sprite != TSXX_NONE8 && !show_sprite(img, pack, sprite)) {
        ok = false;
    }
    lv_obj_invalidate(img->canvas);
    return ok;
}
