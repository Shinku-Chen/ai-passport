// main/atri_image.c —— 画面区合成实现。
#include "atri_image.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "jpeg_decoder.h"
#include "lvgl.h"

#include <string.h>

static const char *TAG = "atri_img";

// esp_jpeg 在 C3 上走软件 TJpgDec(ROM 里没有解码器),JD_FORMAT=0(RGB888),
// 输出 RGB565 时由它做 888->565 转换:swap=false 写 LOBYTE 在前 = 小端,
// 正好是 LVGL 要的原生 16 位字节序。若以后改成 JD_FORMAT=1(TJpgDec 原生
// RGB565,大端输出),这里要跟着改成 swap=true。
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
static bool show_bg(atri_image_t *img, const atri_pack_t *pack, uint16_t bg)
{
    if (bg == ATRI_NONE) {
        memset(img->pixels, 0, (size_t)ATRI_ART_W * ATRI_ART_H * sizeof(uint16_t));
        return true;
    }
    atri_bg_t info;
    if (!atri_pack_bg(pack, bg, &info)) {
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

// 叠加:无损 RGB565 + 4bpp 遮罩,逐行直接从 Flash 合成进画布。
// 带遮罩的行按 alpha 做 15 级混合,不带遮罩的整行直接覆写。越界部分按画布裁剪。
static void blit_ovl(atri_image_t *img, const atri_ovl_t *ovl, int x0, int y0)
{
    const bool masked = ovl->mask_len > 0;
    const uint32_t stride = (uint32_t)((ovl->w + 1) / 2);
    const uint16_t *color = (const uint16_t *)ovl->color;

    for (uint16_t y = 0; y < ovl->h; ++y) {
        const int dst_y = y0 + y;
        if (dst_y < 0) continue;
        if (dst_y >= ATRI_ART_H) break;
        uint16_t *dst_row = img->pixels + (size_t)dst_y * ATRI_ART_W;
        const uint16_t *src_row = color + (size_t)y * ovl->w;
        const uint8_t *mask_row = masked ? ovl->mask + (size_t)y * stride : NULL;

        for (uint16_t x = 0; x < ovl->w; ++x) {
            const int dst_x = x0 + x;
            if (dst_x < 0) continue;
            if (dst_x >= ATRI_ART_W) break;

            int alpha = 15;
            if (mask_row) {
                const uint8_t packed = mask_row[x >> 1];
                alpha = (x & 1) ? (packed & 0x0Fu) : (packed >> 4);
                if (alpha == 0) continue;
            }
            const uint16_t src = src_row[x];
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
}

static bool show_ovl(atri_image_t *img, const atri_pack_t *pack, uint16_t ovl, int16_t x,
                     int16_t y)
{
    atri_ovl_t info;
    if (!atri_pack_ovl(pack, ovl, &info)) {
        ESP_LOGW(TAG, "叠加下标越界: %u", (unsigned)ovl);
        return false;
    }
    blit_ovl(img, &info, x, y);
    return true;
}

// 正文带:源工程 text_bg 的蓝底向上渐透明,这里按行线性插值直接混进画布。
// (LVGL 画不了"逐行不同的透明度",而这条带又必须让立绘透出来,所以在像素层做。)
// 绘制顺序是:背景 -> 蓝带 -> 立绘 -> 浅暗帘 -> (LVGL 文字)。
// 源工程把立绘压在带子下面,人物会被蓝带洗掉大半;这里反过来让立绘压在带子上面,
// 再只给文字所在的几行盖一层浅暗帘,兼顾"人物完整"和"文字看得清"。
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

// 文字区浅暗帘:盖在立绘之上、文字之下,只为了让白字在任何立绘上都能读。
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

bool atri_image_show(atri_image_t *img, const atri_pack_t *pack, uint16_t bg, uint16_t ovl,
                     int16_t x, int16_t y)
{
    if (!img || !img->canvas || !pack) return false;
    if (!show_bg(img, pack, bg)) return false;
    draw_text_band(img);
    if (ovl != ATRI_NONE) {
        (void)show_ovl(img, pack, ovl, x, y);   // 叠加失败也要把背景显示出来
    }
    draw_text_scrim(img);
    lv_obj_invalidate(img->canvas);
    return true;
}
