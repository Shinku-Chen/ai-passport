// main/nn_canvas.c —— 场景画布实现。
#include "nn_canvas.h"

#include "nn_image.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "lvgl.h"

#include <string.h>

static const char *TAG = "nn_canvas";

static uint16_t *s_band_px[6];      // 每带独立缓冲（真机实测一块共享会渲染成同一张图）
static lv_image_dsc_t s_band_dsc[6];
static int s_band_h;                // 80 或 60
static int s_band_count;            // 3 或 4

static size_t band_bytes(int h) { return (size_t)NN_SCREEN_W * (size_t)h * sizeof(uint16_t); }

static void free_bands(void)
{
    for (int i = 0; i < 6; ++i) {
        if (s_band_px[i]) {
            heap_caps_free(s_band_px[i]);
            s_band_px[i] = NULL;
        }
    }
    s_band_count = 0;
}

// 申请 n 块 h 行缓冲；任何一块失败就全部释放并返回 false。
static bool try_alloc(int h, int n)
{
    for (int i = 0; i < n; ++i) {
        s_band_px[i] = heap_caps_malloc(band_bytes(h),
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!s_band_px[i]) {
            free_bands();
            return false;
        }
    }
    s_band_h = h;
    s_band_count = n;
    for (int i = 0; i < n; ++i) {
        memset(s_band_px[i], 0, band_bytes(h));
        memset(&s_band_dsc[i], 0, sizeof(s_band_dsc[i]));
        s_band_dsc[i].header.magic = LV_IMAGE_HEADER_MAGIC;
        s_band_dsc[i].header.cf = LV_COLOR_FORMAT_RGB565;
        s_band_dsc[i].header.w = NN_SCREEN_W;
        s_band_dsc[i].header.h = (uint32_t)h;
        s_band_dsc[i].header.stride = NN_SCREEN_W * sizeof(uint16_t);
        s_band_dsc[i].data_size = (uint32_t)band_bytes(h);
        s_band_dsc[i].data = (const uint8_t *)s_band_px[i];
    }
    return true;
}

bool nn_canvas_init(void)
{
    if (s_band_px[0]) return true;
    const size_t free_before =
        heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    // 独立缓冲：先 320x80x3，再 320x60x4；都拿不到才退回单缓冲（并打醒目日志）。
    if (!try_alloc(NN_BAND_H, NN_BAND_COUNT) && !try_alloc(NN_BAND_H_MIN, NN_BAND_COUNT_MIN)) {
        ESP_LOGW(TAG, "独立分带缓冲申请失败，回落单缓冲（可能出现重复画面）");
        if (!try_alloc(NN_BAND_H_MIN, 1)) {
            ESP_LOGE(TAG, "条带分配失败（%u B），空闲堆 %u / 最大连续块 %u",
                     (unsigned)band_bytes(NN_BAND_H_MIN), (unsigned)free_before,
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT |
                                                                MALLOC_CAP_INTERNAL));
            return false;
        }
    }
    ESP_LOGI(TAG, "分带缓冲 %d 带 x %d 行 x %u B（独立缓冲）: %p %p %p %p, 空闲堆 %u",
             s_band_count, s_band_h, (unsigned)band_bytes(s_band_h),
             (void *)s_band_px[0], (void *)s_band_px[1], (void *)s_band_px[2],
             (void *)s_band_px[3], (unsigned)esp_get_free_heap_size());
    return true;
}

int nn_canvas_band_h(void) { return s_band_h ? s_band_h : NN_BAND_H; }
int nn_canvas_band_count(void) { return s_band_count ? s_band_count : NN_BAND_COUNT; }

uint16_t *nn_canvas_pixels(void)
{
    return s_band_px[0];
}

const void *nn_canvas_band_dsc(int band)
{
    if (band < 0 || band >= s_band_count) return NULL;
    return &s_band_dsc[band];
}

void nn_canvas_clear_band(int band, uint16_t color)
{
    if (band < 0 || band >= s_band_count || !s_band_px[band]) return;
    const int n = NN_SCREEN_W * s_band_h;
    for (int i = 0; i < n; ++i) s_band_px[band][i] = color;
}

void nn_canvas_black_band(int band)
{
    nn_canvas_clear_band(band, 0x0000);
}

const void *nn_canvas_dsc(void)
{
    return &s_band_dsc[0];
}

// 把一张图按 kind 画进指定条带。返回 false = 解码失败（调用方保留旧画面）。
bool nn_canvas_draw_image_band(const nn_pack_t *pack, uint16_t image_id, int band,
                               bool cg_full)
{
    if (band < 0 || band >= s_band_count || !s_band_px[band]) return false;
    uint16_t *buf = s_band_px[band];
    const int bh = s_band_h;
    const int tag = band * bh;                 // 本条带在虚拟 240 画布里的起始行
    if (band < 0 || band >= nn_canvas_band_count()) return false;
    const nn_image_t *img = nn_pack_image(pack, image_id);
    if (!img) return false;
    const uint8_t *data = nn_pack_image_data(pack, img);
    if (!data) return false;

    if (img->kind == NN_IMG_TITLE) {
        // 标题：等比放大铺满（cover，只放大不挤压），溢出部分居中裁掉。
        return nn_jpeg_draw_fit_band(buf, NN_SCREEN_W, bh, NN_ART_H, data,
                                     img->jpeg_len, false, tag, 0, 0);
    }
    if (img->kind == NN_IMG_CG) {
        // 游戏内：等比放大到铺满宽度（cover）并从顶部对齐裁（底部由对话框盖住）。
        // 全屏鉴赏页：contain，整幅可见，上下留黑边。
        if (cg_full) {
            return nn_jpeg_draw_fit_band(buf, NN_SCREEN_W, bh, NN_ART_H, data,
                                         img->jpeg_len, true, tag, 0, 0);
        }
        return nn_jpeg_draw_fit_band(buf, NN_SCREEN_W, bh, NN_ART_H, data,
                                     img->jpeg_len, false, tag, 0, 1);
    }
    if (img->kind == NN_IMG_BG) {
        // 背景：等比放大铺满（cover，1.9x），绝不非等比拉伸。
        return nn_jpeg_draw_fit_band(buf, NN_SCREEN_W, bh, NN_ART_H, data,
                                     img->jpeg_len, false, tag, 0, 0);
    }
    int x0, y0;
    const uint8_t *mask = NULL;
    int stride = 0;
    if (img->kind == NN_IMG_SPRITE) {
        x0 = (NN_SCREEN_W - img->w) / 2;
        // 底边贴**屏幕底**（240）：立绘必须完整显示，下半截允许被半透明对话框压住。
        // 之前贴面板顶（140）会把下半身整段裁掉，实机看着"立绘显示不全"。
        y0 = NN_ART_H - img->h;   // 立绘底边贴面板顶（面板不透明，画到后面也看不见）
        mask = data + img->jpeg_len;
        stride = (img->w + 7) / 8;
    } else {
        x0 = (NN_SCREEN_W - img->w) / 2;       // 缩略图：原尺寸居中，不放大
        y0 = (NN_ART_H - img->h) / 2;
    }
    return nn_jpeg_draw_band(buf, NN_SCREEN_W, bh, data, img->jpeg_len, x0, y0, tag,
                             mask, stride);
}
