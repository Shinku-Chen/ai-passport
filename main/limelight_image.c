// main/limelight_image.c —— 画面区实现(LVGL 画布 + esp_jpeg + 立绘遮罩合成)。
#include "limelight_image.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "jpeg_decoder.h"

#include <string.h>

static const char *TAG = "lime_img";

// esp_jpeg 在 C3 上走 ROM 里的 TJpgDec;输出 RGB565 时 swap=false = 低字节在前,
// 正好是 LVGL 要的 16 位字节序。
#define LIME_JPEG_SWAP_BYTES 0

uint32_t lime_image_required_sprite_bytes(const lime_assets_t *assets)
{
    if (!assets) return 0;
    uint32_t worst = 0;
    for (uint16_t i = 0; i < lime_assets_count(assets); i++) {
        lime_asset_t entry;
        if (!lime_assets_get(assets, i, &entry)) continue;
        if (entry.kind != LIME_ASSET_KIND_SPRITE) continue;
        const uint32_t bytes = (uint32_t)entry.w * entry.h * 2u;
        if (bytes > worst) worst = bytes;
    }
    return worst;
}

uint32_t lime_image_required_mask_bytes(const lime_assets_t *assets)
{
    if (!assets) return 0;
    uint32_t worst = 0;
    for (uint16_t i = 0; i < lime_assets_count(assets); i++) {
        lime_asset_t entry;
        if (!lime_assets_get(assets, i, &entry)) continue;
        if (entry.kind != LIME_ASSET_KIND_SPRITE) continue;
        const uint32_t bytes = lime_mask_raw_len(entry.w, entry.h);
        if (bytes > worst) worst = bytes;
    }
    return worst;
}

static void invalidate(lime_image_t *img)
{
    if (img->canvas) lv_obj_invalidate(img->canvas);
}

bool lime_image_init(lime_image_t *img, struct _lv_obj_t *parent, uint16_t *pixels,
                     uint8_t *sprite_buffer, uint32_t sprite_capacity,
                     uint8_t *mask_buffer, uint32_t mask_capacity)
{
    if (!img || !parent || !pixels || !sprite_buffer || !mask_buffer) return false;
    memset(img, 0, sizeof(*img));
    img->pixels = pixels;
    img->sprite_buffer = sprite_buffer;
    img->sprite_capacity = sprite_capacity;
    img->mask_buffer = mask_buffer;
    img->mask_capacity = mask_capacity;
    memset(pixels, 0, (size_t)LIME_ART_W * LIME_ART_H * sizeof(uint16_t));

    img->canvas = lv_canvas_create(parent);
    if (!img->canvas) {
        ESP_LOGE(TAG, "画布创建失败");
        return false;
    }
    lv_canvas_set_buffer(img->canvas, pixels, LIME_ART_W, LIME_ART_H,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(img->canvas, 0, 0);

    // 立绘下摆的宿主对象(数据由 show_sprite 指到解码缓冲里)。
    img->sprite_lower = lv_image_create(parent);
    if (!img->sprite_lower) {
        ESP_LOGE(TAG, "立绘下摆对象创建失败");
        return false;
    }
    lv_obj_add_flag(img->sprite_lower, LV_OBJ_FLAG_HIDDEN);
    return true;
}

void lime_image_clear(lime_image_t *img)
{
    if (!img || !img->pixels) return;
    memset(img->pixels, 0, (size_t)LIME_ART_W * LIME_ART_H * sizeof(uint16_t));
    invalidate(img);
}

// 把 JPEG 解到 dst(紧密 w*h*2 排列)。dst 容量必须 ≥ w*h*2。
static bool decode_jpeg_into(uint16_t *dst, uint32_t capacity, const uint8_t *jpeg,
                             uint32_t jpeg_len, int expect_w, int expect_h, uint32_t *out_ms)
{
    esp_jpeg_image_cfg_t cfg = {
        .indata = (uint8_t *)jpeg,
        .indata_size = jpeg_len,
        .outbuf = (uint8_t *)dst,
        .outbuf_size = capacity,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
        .flags = { .swap_color_bytes = LIME_JPEG_SWAP_BYTES },
    };
    esp_jpeg_image_output_t out = { 0 };
    const int64_t start = esp_timer_get_time();
    const esp_err_t err = esp_jpeg_decode(&cfg, &out);
    if (out_ms) *out_ms = (uint32_t)((esp_timer_get_time() - start) / 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "JPEG 解码失败: %s", esp_err_to_name(err));
        return false;
    }
    if (out.width != (uint16_t)expect_w || out.height != (uint16_t)expect_h) {
        ESP_LOGE(TAG, "JPEG 尺寸不符: %ux%u,期望 %dx%d", (unsigned)out.width,
                 (unsigned)out.height, expect_w, expect_h);
        return false;
    }
    return true;
}

// 背景/CG:解进画布(资源包里就是 240x214,紧密排列正好对上画布,不需要临时缓冲)。
static bool show_bg(lime_image_t *img, const lime_assets_t *assets, int bg_index)
{
    if (img->sprite_lower) lv_obj_add_flag(img->sprite_lower, LV_OBJ_FLAG_HIDDEN);
    if (bg_index < 0) {
        memset(img->pixels, 0, (size_t)LIME_ART_W * LIME_ART_H * sizeof(uint16_t));
        return true;
    }
    lime_asset_t entry;
    if (!lime_assets_get(assets, (uint16_t)bg_index, &entry)) {
        ESP_LOGE(TAG, "背景下标越界: %d", bg_index);
        return false;
    }
    if (entry.kind != LIME_ASSET_KIND_IMAGE) {
        ESP_LOGE(TAG, "不是图片条目: %d", bg_index);
        return false;
    }
    if (entry.w > LIME_ART_W || entry.h > LIME_ART_H) {
        ESP_LOGE(TAG, "图片比画布还大: %ux%u", (unsigned)entry.w, (unsigned)entry.h);
        return false;
    }
    uint32_t jpeg_len = 0;
    const uint8_t *jpeg = lime_assets_jpeg(assets, &entry, &jpeg_len);
    if (!jpeg) return false;
    // 先解到画布左上角(紧密排列),再把整幅图挪到居中位置。
    if (!decode_jpeg_into(img->pixels, (uint32_t)entry.w * entry.h * 2u, jpeg, jpeg_len,
                          entry.w, entry.h, &img->last_decode_ms)) {
        return false;
    }
    if (entry.w == LIME_ART_W && entry.h == LIME_ART_H) return true;

    // 比画布小(标题图 240x135 就是这种):把图像从左上角挪到居中位置,留黑边。
    // 自下而上搬:目标行在源行下方,先搬下面才不会覆盖还没读的行。
    const int x0 = (LIME_ART_W - entry.w) / 2;
    const int y0 = (LIME_ART_H - entry.h) / 2;
    for (int y = entry.h - 1; y >= 0; y--) {
        uint16_t *dst = img->pixels + (size_t)(y0 + y) * LIME_ART_W;
        const uint16_t *src = img->pixels + (size_t)y * LIME_ART_W;
        memmove(dst + x0, src, (size_t)entry.w * sizeof(uint16_t));
    }
    // 搬完之后再清留边:上下两条整行 + 目标行左右的窄边。
    if (y0 > 0) {
        memset(img->pixels, 0, (size_t)y0 * LIME_ART_W * sizeof(uint16_t));
    }
    if (y0 + entry.h < LIME_ART_H) {
        memset(img->pixels + (size_t)(y0 + entry.h) * LIME_ART_W, 0,
               (size_t)(LIME_ART_H - y0 - entry.h) * LIME_ART_W * sizeof(uint16_t));
    }
    if (x0 > 0) {
        for (int y = 0; y < entry.h; y++) {
            uint16_t *row = img->pixels + (size_t)(y0 + y) * LIME_ART_W;
            memset(row, 0, (size_t)x0 * sizeof(uint16_t));
            memset(row + x0 + entry.w, 0,
                   (size_t)(LIME_ART_W - x0 - entry.w) * sizeof(uint16_t));
        }
    }
    return true;
}

// 立绘:解到临时缓冲 -> 解遮罩 -> 合成进画布(人物下半身落在正文带后面)。
static bool show_sprite(lime_image_t *img, const lime_assets_t *assets, int sprite_index)
{
    lime_asset_t entry;
    if (!lime_assets_get(assets, (uint16_t)sprite_index, &entry)) {
        ESP_LOGW(TAG, "立绘下标越界: %d", sprite_index);
        return false;
    }
    if (entry.kind != LIME_ASSET_KIND_SPRITE) {
        ESP_LOGW(TAG, "不是立绘条目: %d", sprite_index);
        return false;
    }
    uint32_t jpeg_len = 0;
    const uint8_t *jpeg = lime_assets_jpeg(assets, &entry, &jpeg_len);
    uint32_t mask_len = 0;
    const uint8_t *mask = lime_assets_mask(assets, &entry, &mask_len);
    if (!jpeg || !mask) return false;

    const uint32_t pixels_bytes = (uint32_t)entry.w * entry.h * 2u;
    const uint32_t mask_bytes = lime_mask_raw_len(entry.w, entry.h);
    if (pixels_bytes > img->sprite_capacity || mask_bytes > img->mask_capacity) {
        ESP_LOGE(TAG, "立绘缓冲不足: 需要 %u/%u,实际 %u/%u", (unsigned)pixels_bytes,
                 (unsigned)mask_bytes, (unsigned)img->sprite_capacity,
                 (unsigned)img->mask_capacity);
        return false;
    }
    if (!decode_jpeg_into((uint16_t *)img->sprite_buffer, pixels_bytes, jpeg, jpeg_len,
                          entry.w, entry.h, NULL)) {
        return false;
    }
    if (!lime_mask_decode(mask, mask_len, entry.w, entry.h, img->mask_buffer, mask_bytes)) {
        ESP_LOGE(TAG, "遮罩解码失败");
        return false;
    }
    const int x = lime_sprite_origin_x(entry.w);
    const int y = lime_sprite_origin_y(entry.h);
    const uint32_t written = lime_sprite_blit(img->pixels, LIME_ART_W, LIME_ART_H,
                                              img->sprite_buffer, entry.w, entry.h,
                                              img->mask_buffer, x, y);

    // 下摆:画布放不下的那几行,指到解码缓冲里对应位置(自定义 stride),画在对话框下面。
    const int lower_rows = lime_sprite_rows_below_canvas(entry.h);
    if (lower_rows > 0 && img->sprite_lower) {
        const int first_row = entry.h - lower_rows;
        img->sprite_lower_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        img->sprite_lower_dsc.header.w = (uint32_t)entry.w;
        img->sprite_lower_dsc.header.h = (uint32_t)lower_rows;
        img->sprite_lower_dsc.header.stride = (uint32_t)entry.w * 2u;
        img->sprite_lower_dsc.data_size = (uint32_t)entry.w * lower_rows * 2u;
        img->sprite_lower_dsc.data = img->sprite_buffer + (size_t)first_row * entry.w * 2u;
        lv_image_set_src(img->sprite_lower, &img->sprite_lower_dsc);
        lv_obj_set_pos(img->sprite_lower, x, LIME_ART_H);
        lv_obj_remove_flag(img->sprite_lower, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGI(TAG, "立绘下摆 %d 行 @(%d,%d)", lower_rows, x, LIME_ART_H);
    } else if (img->sprite_lower) {
        lv_obj_add_flag(img->sprite_lower, LV_OBJ_FLAG_HIDDEN);
    }
    ESP_LOGI(TAG, "立绘 #%d %ux%u @(%d,%d) -> 合成 %u 像素", sprite_index, (unsigned)entry.w,
             (unsigned)entry.h, x, y, (unsigned)written);
    (void)written;      // 日志等级关掉时这个变量会"未使用",显式声明一次
    return true;
}

bool lime_image_show(lime_image_t *img, const lime_assets_t *assets, int bg_index,
                     int sprite_index)
{
    if (!img || !img->canvas || !assets) return false;
    if (!show_bg(img, assets, bg_index)) return false;
    // 立绘画在背景之上、正文带之下:人物下半身被带子盖住(与源移植版一致)。
    if (sprite_index >= 0) {
        (void)show_sprite(img, assets, sprite_index);
    }
    invalidate(img);
    return true;
}

bool lime_image_show_backdrop(lime_image_t *img, const lime_assets_t *assets, int bg_index)
{
    if (!img || !img->canvas || !assets) return false;
    if (!show_bg(img, assets, bg_index)) return false;
    invalidate(img);
    return true;
}
