// main/atri_image.c —— 画面区合成实现(从 ATRI 阅读器移植,资源来源接到 starry 包)。
#include "atri_image.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "jpeg_decoder.h"
#include "lvgl.h"

#include <string.h>

static const char *TAG = "atri_img";

// esp_jpeg 在 C3 上走 ROM 里的 TJpgDec,输出 RGB565 时由它做 888->565 转换:
// swap=false 写 LOBYTE 在前 = 小端,正好是 LVGL 要的原生 16 位字节序。
#define ATRI_JPEG_SWAP_BYTES 0

static void clear_canvas(atri_image_t *img)
{
    memset(img->pixels, 0, (size_t)ATRI_ART_W * ATRI_ART_H * sizeof(uint16_t));
    lv_obj_invalidate(img->canvas);
}

bool atri_image_init(atri_image_t *img, struct _lv_obj_t *parent, uint16_t *pixels)
{
    if (!img || !parent || !pixels) return false;
    memset(img, 0, sizeof(*img));
    img->pixels = pixels;
    clear_canvas(img);
    img->canvas = lv_canvas_create(parent);
    if (!img->canvas) {
        ESP_LOGE(TAG, "画布创建失败");
        return false;
    }
    lv_canvas_set_buffer(img->canvas, pixels, ATRI_ART_W, ATRI_ART_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(img->canvas, 0, 0);
    return true;
}

void atri_image_clear(atri_image_t *img)
{
    if (!img || !img->canvas) return;
    clear_canvas(img);
}

// 背景:整张 JPEG 解码进画布。画布本身就是输出缓冲,不需要额外 RAM。
static bool show_bg(atri_image_t *img, const starry_pack_t *pack, uint16_t bg)
{
    if (bg == STARRY_NONE) {
        memset(img->pixels, 0, (size_t)ATRI_ART_W * ATRI_ART_H * sizeof(uint16_t));
        return true;
    }
    starry_bg_t info;
    if (!starry_pack_bg(pack, bg, &info)) {
        ESP_LOGE(TAG, "背景下标越界: %u", (unsigned)bg);
        return false;
    }
    if (info.w != ATRI_ART_W || info.h != ATRI_ART_H) {
        ESP_LOGE(TAG, "背景尺寸与画布不符: %ux%u", (unsigned)info.w, (unsigned)info.h);
        return false;
    }
    esp_jpeg_image_cfg_t cfg = {
        .indata = (uint8_t *)info.jpeg,
        .indata_size = info.jpeg_len,
        .outbuf = (uint8_t *)img->pixels,
        .outbuf_size = (uint32_t)ATRI_ART_W * ATRI_ART_H * 2u,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
        .flags = { .swap_color_bytes = ATRI_JPEG_SWAP_BYTES },
    };
    esp_jpeg_image_output_t out = { 0 };
    const int64_t start = esp_timer_get_time();
    const esp_err_t err = esp_jpeg_decode(&cfg, &out);
    img->last_decode_ms = (uint32_t)((esp_timer_get_time() - start) / 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "背景 %u 解码失败: %s", (unsigned)bg, esp_err_to_name(err));
        return false;
    }
    if (out.width != ATRI_ART_W || out.height != ATRI_ART_H) {
        ESP_LOGE(TAG, "背景解码尺寸不符: %ux%u", (unsigned)out.width, (unsigned)out.height);
        return false;
    }
    return true;
}

// 立绘:无损 RGB565 + 4bpp 遮罩,逐行直接从 Flash 合成进画布。
// 带遮罩的像素按 alpha 做 15 级混合,alpha=0 跳过;越界部分按画布裁剪。
// 颜色按字节读取(而不是直接按 uint16_t 读):SEC_FG 数据区不保证 2 字节对齐,
// RISC-V 的不对齐 16 位读取会直接抛异常。
static uint32_t blit_sprite(atri_image_t *img, const starry_fg_t *fg)
{
    uint32_t written = 0;
    const uint32_t stride = (uint32_t)((fg->w + 1u) / 2u);
    const int x0 = fg->x;
    const int y0 = fg->y;

    for (uint16_t y = 0; y < fg->h; ++y) {
        const int dst_y = y0 + (int)y;
        if (dst_y < 0) continue;
        if (dst_y >= ATRI_ART_H) break;
        uint16_t *dst_row = img->pixels + (size_t)dst_y * ATRI_ART_W;
        const uint8_t *src_row = fg->color + (size_t)y * fg->w * 2u;
        const uint8_t *mask_row = fg->mask + (size_t)y * stride;

        for (uint16_t x = 0; x < fg->w; ++x) {
            const int dst_x = x0 + (int)x;
            if (dst_x < 0) continue;
            if (dst_x >= ATRI_ART_W) break;

            const uint8_t packed = mask_row[x >> 1];
            const int alpha = (x & 1) ? (packed & 0x0F) : (packed >> 4);
            if (alpha == 0) continue;
            const uint16_t src = (uint16_t)(src_row[x * 2] | ((uint16_t)src_row[x * 2 + 1] << 8));
            ++written;
            if (alpha == 15) {
                dst_row[dst_x] = src;
                continue;
            }
            const uint16_t dst = dst_row[dst_x];
            const unsigned sa = (unsigned)alpha;
            const unsigned ia = 15u - sa;
            const unsigned r = (((src >> 11) & 0x1Fu) * sa + ((dst >> 11) & 0x1Fu) * ia) / 15u;
            const unsigned g = (((src >> 5) & 0x3Fu) * sa + ((dst >> 5) & 0x3Fu) * ia) / 15u;
            const unsigned b = ((src & 0x1Fu) * sa + (dst & 0x1Fu) * ia) / 15u;
            dst_row[dst_x] = (uint16_t)((r << 11) | (g << 5) | b);
        }
    }
    return written;
}

// 角色立绘:位置来自立绘记录(缩放 + alpha 包围盒裁剪后的偏移)。
// STARRY_FG_KEEP 表示沿用当前这张;STARRY_NONE 表示不画。
static bool show_char(atri_image_t *img, const starry_pack_t *pack, uint16_t chr)
{
    if (chr == STARRY_FG_KEEP || chr == STARRY_NONE) return true;
    starry_fg_t info;
    if (!starry_pack_fg(pack, chr, &info)) {
        ESP_LOGW(TAG, "立绘下标越界: %u", (unsigned)chr);
        return false;
    }
    const uint32_t written = blit_sprite(img, &info);
    ESP_LOGI(TAG, "立绘 #%u %ux%u @(%u,%u) -> 合成 %u 像素", (unsigned)chr, (unsigned)info.w,
             (unsigned)info.h, (unsigned)info.x, (unsigned)info.y, (unsigned)written);
    return true;
}

// 正文带:源工程 text_bg 的蓝底向上渐透明,这里按行线性插值直接混进画布。
// (LVGL 画不了"逐行不同的透明度",而这条带又必须让立绘透出来,所以在像素层做。)
// 绘制顺序是:背景 -> 立绘 -> 蓝带 -> 浅暗帘 -> (LVGL 文字)。
// 立绘压在带子下面(与源工程一致):人物下半身被正文带盖住,带子外面的人物完全不变。
static void draw_text_band(atri_image_t *img)
{
    // text_bg.png 采样值:RGB 约 (73,138,217),alpha 从 65/255 渐到 219/255。
    // 这里把两端的 alpha 各抬高一点(110..232):16px 白字比源工程的 28px 字细,
    // 顶上那几行要压住明亮天空才看得清;整体观感仍然是"上淡下深"的蓝带。
    const unsigned band_r = 73, band_g = 138, band_b = 217;
    const unsigned alpha_top = 110, alpha_bottom = 232;
    for (int y = ATRI_BAND_Y; y < ATRI_ART_H; ++y) {
        const unsigned span = (unsigned)(ATRI_ART_H - ATRI_BAND_Y - 1);
        const unsigned t = span ? (unsigned)(y - ATRI_BAND_Y) * 255u / span : 255u;
        const unsigned a = alpha_top + (alpha_bottom - alpha_top) * t / 255u;
        const unsigned ia = 255u - a;
        uint16_t *row = img->pixels + (size_t)y * ATRI_ART_W;
        for (int x = 0; x < ATRI_ART_W; ++x) {
            const uint16_t dst = row[x];
            const unsigned dr = ((dst >> 11) & 0x1Fu) << 3;
            const unsigned dg = ((dst >> 5) & 0x3Fu) << 2;
            const unsigned db = (dst & 0x1Fu) << 3;
            const unsigned r = (band_r * a + dr * ia) / 255u;
            const unsigned g = (band_g * a + dg * ia) / 255u;
            const unsigned b = (band_b * a + db * ia) / 255u;
            row[x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }
}

// 文字区浅暗帘:盖在正文带之上、文字之下,只为了让白字在任何画面上都能读。
static void draw_text_scrim(atri_image_t *img)
{
    const unsigned scrim_r = 6, scrim_g = 16, scrim_b = 25;
    const unsigned alpha_top = 30, alpha_bottom = 110;
    const unsigned span = (unsigned)(ATRI_ART_H - ATRI_BAND_Y - 1);
    for (int y = ATRI_BAND_Y; y < ATRI_ART_H; ++y) {
        const unsigned t = span ? (unsigned)(y - ATRI_BAND_Y) * 255u / span : 255u;
        const unsigned a = alpha_top + (alpha_bottom - alpha_top) * t / 255u;
        const unsigned ia = 255u - a;
        uint16_t *row = img->pixels + (size_t)y * ATRI_ART_W;
        for (int x = 0; x < ATRI_ART_W; ++x) {
            const uint16_t dst = row[x];
            const unsigned dr = ((dst >> 11) & 0x1Fu) << 3;
            const unsigned dg = ((dst >> 5) & 0x3Fu) << 2;
            const unsigned db = (dst & 0x1Fu) << 3;
            const unsigned r = (scrim_r * a + dr * ia) / 255u;
            const unsigned g = (scrim_g * a + dg * ia) / 255u;
            const unsigned b = (scrim_b * a + db * ia) / 255u;
            row[x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }
}

bool atri_image_show(atri_image_t *img, const starry_pack_t *pack, uint16_t bg, uint16_t chr)
{
    if (!img || !img->canvas || !pack) return false;
    if (!show_bg(img, pack, bg)) return false;
    ESP_LOGI(TAG, "背景 #%u 解码 %u ms,立绘 #%u", (unsigned)bg,
             (unsigned)img->last_decode_ms, (unsigned)chr);
    // 立绘先画、正文带后画 —— 人物下半身被正文带盖住(与源工程一致)。
    (void)show_char(img, pack, chr);
    draw_text_band(img);
    draw_text_scrim(img);
    lv_obj_invalidate(img->canvas);
    return true;
}
