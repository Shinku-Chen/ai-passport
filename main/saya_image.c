// main/saya_image.c —— 画面区合成实现。
#include "saya_image.h"

#include "jpeg_decoder.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

static const char *TAG = "saya_img";

// esp_jpeg 在 C3 上走软件 TJpgDec(ROM 里没有解码器),JD_FORMAT=0(RGB888),
// 输出 RGB565 时由它做 888->565 转换:swap=false 写 LOBYTE 在前 = 小端,
// 正好是 LVGL 要的原生 16 位字节序。若以后改成 JD_FORMAT=1(TJpgDec 原生
// RGB565,大端输出),这里要跟着改成 swap=true。
#define SAYA_JPEG_SWAP_BYTES 0

static bool decode_jpeg(const uint8_t *data, uint32_t len, uint8_t *out, uint32_t out_size,
                        uint16_t expect_w, uint16_t expect_h, uint32_t *elapsed_ms)
{
    esp_jpeg_image_cfg_t cfg = {
        .indata = (uint8_t *)data,
        .indata_size = len,
        .outbuf = out,
        .outbuf_size = out_size,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
        .flags = { .swap_color_bytes = SAYA_JPEG_SWAP_BYTES },
    };
    esp_jpeg_image_output_t info = { 0 };
    const int64_t start = esp_timer_get_time();
    const esp_err_t err = esp_jpeg_decode(&cfg, &info);
    if (elapsed_ms) *elapsed_ms = (uint32_t)((esp_timer_get_time() - start) / 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "JPEG 解码失败: %s", esp_err_to_name(err));
        return false;
    }
    if (expect_w && info.width != expect_w) {
        ESP_LOGE(TAG, "JPEG 宽度不符: %u != %u", (unsigned)info.width, (unsigned)expect_w);
        return false;
    }
    if (expect_h && info.height != expect_h) {
        ESP_LOGE(TAG, "JPEG 高度不符: %u != %u", (unsigned)info.height, (unsigned)expect_h);
        return false;
    }
    return true;
}

bool saya_image_init(saya_image_t *img, struct _lv_obj_t *parent, uint16_t *pixels,
                     uint8_t *sprite_scratch, uint32_t sprite_scratch_size)
{
    if (!img || !parent || !pixels || !sprite_scratch) return false;
    if (sprite_scratch_size < (uint32_t)SAYA_SPRITE_MAX_W * SAYA_ART_H * 2u) return false;

    img->pixels = pixels;
    img->sprite_scratch = sprite_scratch;
    img->sprite_scratch_size = sprite_scratch_size;
    // 先清成黑色,避免第一帧显示未初始化内存。
    for (int i = 0; i < SAYA_ART_W * SAYA_ART_H; ++i) pixels[i] = 0;

    img->canvas = lv_canvas_create(parent);
    if (!img->canvas) {
        ESP_LOGE(TAG, "画布创建失败");
        return false;
    }
    lv_canvas_set_buffer(img->canvas, pixels, SAYA_ART_W, SAYA_ART_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(img->canvas, 0, 0);
    return true;
}

bool saya_image_show_bg(saya_image_t *img, const saya_pack_t *pack, uint16_t bg_id)
{
    if (!img || !img->canvas || !pack) return false;

    // 没有背景(数据缺失)时保持黑屏,不算错误。
    if (bg_id == SAYA_NONE) {
        for (int i = 0; i < SAYA_ART_W * SAYA_ART_H; ++i) img->pixels[i] = 0;
        lv_obj_invalidate(img->canvas);
        return true;
    }

    saya_bg_t bg;
    if (!saya_pack_bg(pack, bg_id, &bg)) {
        ESP_LOGE(TAG, "背景下标越界: %u", (unsigned)bg_id);
        return false;
    }
    uint32_t elapsed = 0;
    const uint32_t need = (uint32_t)SAYA_ART_W * SAYA_ART_H * 2u;
    if (bg.w != SAYA_ART_W || bg.h != SAYA_ART_H) {
        ESP_LOGE(TAG, "背景尺寸与画布不符: %ux%u", (unsigned)bg.w, (unsigned)bg.h);
        return false;
    }
    if (!decode_jpeg(bg.jpeg, bg.jpeg_len, (uint8_t *)img->pixels, need, SAYA_ART_W,
                     SAYA_ART_H, &elapsed)) {
        return false;
    }
    img->last_decode_ms = elapsed;
    ESP_LOGI(TAG, "背景 %u 解码 %u ms", (unsigned)bg_id, (unsigned)elapsed);
    lv_obj_invalidate(img->canvas);
    return true;
}

bool saya_image_show(saya_image_t *img, const saya_pack_t *pack, uint16_t bg_id, uint16_t fg_id)
{
    if (!saya_image_show_bg(img, pack, bg_id)) return false;
    if (fg_id == SAYA_NONE || fg_id == SAYA_FG_KEEP) return true;

    saya_fg_t fg;
    if (!saya_pack_fg(pack, fg_id, &fg)) {
        ESP_LOGW(TAG, "立绘下标越界: %u", (unsigned)fg_id);
        return true;   // 没有立绘也要把背景显示出来
    }
    if (fg.w > SAYA_SPRITE_MAX_W || fg.h > SAYA_ART_H) {
        // 打包时已限宽限高,这里只防御损坏的包。
        ESP_LOGW(TAG, "立绘尺寸超缓冲: %ux%u", (unsigned)fg.w, (unsigned)fg.h);
        return true;
    }
    const uint32_t need = (uint32_t)fg.w * fg.h * 2u;
    if (need > img->sprite_scratch_size) {
        ESP_LOGW(TAG, "立绘解码缓冲不足: 需要 %u", (unsigned)need);
        return true;
    }

    uint32_t elapsed = 0;
    if (!decode_jpeg(fg.jpeg, fg.jpeg_len, img->sprite_scratch, img->sprite_scratch_size, fg.w,
                     fg.h, &elapsed)) {
        return true;
    }
    img->last_decode_ms += elapsed;
    ESP_LOGI(TAG, "立绘 %u(%ux%u) 解码 + 合成 %u ms", (unsigned)fg_id, (unsigned)fg.w,
             (unsigned)fg.h, (unsigned)elapsed);

    // 按 1bpp 遮罩把立绘拷进画布:遮罩位为 1 才覆盖,MSB 在左。
    const uint16_t *src = (const uint16_t *)img->sprite_scratch;
    const int x_off = (SAYA_ART_W - (int)fg.w) / 2;
    const uint32_t row_bytes = ((uint32_t)fg.w + 7u) / 8u;
    for (uint16_t y = 0; y < fg.h; ++y) {
        uint16_t *dst_row = img->pixels + (size_t)y * SAYA_ART_W + (size_t)x_off;
        const uint8_t *mask_row = fg.mask + (size_t)y * row_bytes;
        const uint16_t *src_row = src + (size_t)y * fg.w;
        for (uint16_t x = 0; x < fg.w; ++x) {
            if (mask_row[x >> 3] & (uint8_t)(0x80u >> (x & 7u))) {
                dst_row[x] = src_row[x];
            }
        }
    }
    lv_obj_invalidate(img->canvas);
    return true;
}
