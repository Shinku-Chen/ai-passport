// main/saya_image.c —— 画面区合成实现。
#include "saya_image.h"

#include "jpeg_decoder.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

static const char *TAG = "saya_img";

// 下半块画面的像素缓冲(57.6 KB):整幅画面的第 150..239 行,1:1 显示在文本框背后。
// 同时兼作立绘上半段的解码暂存(上半段最大 180x150x2 = 54 KB < 57.6 KB),这样
// 就不必再为它单独留一块和画面区一样大的缓冲。
static uint16_t s_strip_pixels[SAYA_STRIP_W * SAYA_STRIP_H];

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
    if (sprite_scratch_size < (uint32_t)SAYA_SPRITE_MAX_W * SAYA_STRIP_H * 2u) return false;

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
    lv_image_set_align(img->canvas, LV_IMAGE_ALIGN_TOP_LEFT);
    lv_obj_set_pos(img->canvas, 0, 0);

    // 下半块画面:1:1 画在画面区正下方,铺满屏幕剩下的 90 行(仍在文本框下面,
    // 因为 art_holder 先于 page_game 创建)。
    img->strip_pixels = s_strip_pixels;
    for (int i = 0; i < SAYA_STRIP_W * SAYA_STRIP_H; ++i) s_strip_pixels[i] = 0;
    img->strip_canvas = lv_canvas_create(parent);
    if (!img->strip_canvas) {
        ESP_LOGE(TAG, "条带画布创建失败");
        return false;
    }
    lv_canvas_set_buffer(img->strip_canvas, s_strip_pixels, SAYA_STRIP_W, SAYA_STRIP_H,
                         LV_COLOR_FORMAT_RGB565);
    // 关键:image/canvas 默认是在对象内居中绘制,必须显式左上对齐(否则图像会被
    // 挪到对象中间,顶部留出未绘制的黑块)。
    lv_image_set_align(img->strip_canvas, LV_IMAGE_ALIGN_TOP_LEFT);
    // 画布尺寸 = 图像尺寸:1:1 显示,不做任何拉伸(拉伸会让人物/背景变形)。
    lv_obj_set_pos(img->strip_canvas, 0, SAYA_ART_H);
    lv_obj_set_size(img->strip_canvas, SAYA_STRIP_W, SAYA_STRIP_H);
    return true;
}

// 下半块:清成黑色(包内没有该段时的回退,与旧行为一致)。
static void strip_clear(saya_image_t *img)
{
    if (!img || !img->strip_pixels) return;
    for (int i = 0; i < SAYA_STRIP_W * SAYA_STRIP_H; ++i) img->strip_pixels[i] = 0;
    if (img->strip_canvas) lv_obj_invalidate(img->strip_canvas);
}

// 下半块:解码背景的第 150..239 行,让半透明文本框透出真实画面。
static void strip_show(saya_image_t *img, const saya_pack_t *pack, uint16_t bg_id)
{
    if (!img || !img->strip_pixels) return;
    saya_bg_t strip;
    if (pack && saya_pack_strip(pack, bg_id, &strip) && strip.w == SAYA_STRIP_W &&
        strip.h == SAYA_STRIP_H &&
        decode_jpeg(strip.jpeg, strip.jpeg_len, (uint8_t *)img->strip_pixels,
                    (uint32_t)SAYA_STRIP_W * SAYA_STRIP_H * 2u, SAYA_STRIP_W, SAYA_STRIP_H,
                    NULL)) {
        lv_obj_invalidate(img->strip_canvas);
        return;
    }
    strip_clear(img);
}

// 立绘下半段:1:1 解码后合成进下半块画布(人物在文本框后面原样继续)。
static void sprite_lower_composite(saya_image_t *img, const saya_pack_t *pack, uint16_t fg_id)
{
    if (!img || !img->strip_pixels || !pack) return;
    saya_fg_t low;
    if (!saya_pack_fg_low(pack, fg_id, &low)) return;   // 这张立绘没有下半段:条带保持背景
    const uint32_t need = (uint32_t)low.w * low.h * 2u;
    if (need > img->sprite_scratch_size) return;
    if (!decode_jpeg(low.jpeg, low.jpeg_len, img->sprite_scratch, img->sprite_scratch_size,
                     low.w, low.h, NULL)) {
        return;
    }

    const uint16_t *src = (const uint16_t *)img->sprite_scratch;
    const int x_off = SAYA_ART_W - (int)low.w;          // 与上半段一致:贴画面区右侧
    const uint32_t row_bytes = ((uint32_t)low.w + 7u) / 8u;
    for (uint16_t sy = 0; sy < SAYA_STRIP_H; ++sy) {
        const uint16_t ry = sy;                    // 1:1,不纵向压缩
        if (ry >= low.h) break;
        uint16_t *dst_row = img->strip_pixels + (size_t)sy * SAYA_STRIP_W;
        const uint8_t *mask_row = low.mask + (size_t)ry * row_bytes;
        const uint16_t *src_row = src + (size_t)ry * low.w;
        for (uint16_t sx = 0; sx < SAYA_STRIP_W; ++sx) {
            const int rx = (int)sx - x_off;   // 横向 1:1,与上半段完全对齐
            if (rx < 0 || rx >= (int)low.w) continue;
            if (mask_row[rx >> 3] & (uint8_t)(0x80u >> (rx & 7u))) dst_row[sx] = src_row[rx];
        }
    }
    if (img->strip_canvas) lv_obj_invalidate(img->strip_canvas);
}

// 上半块:解码背景第 0..149 行到画面区画布。
static bool bg_upper_show(saya_image_t *img, const saya_pack_t *pack, uint16_t bg_id)
{
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
    if (bg.w != SAYA_ART_W || bg.h != SAYA_ART_H) {
        ESP_LOGE(TAG, "背景尺寸与画布不符: %ux%u", (unsigned)bg.w, (unsigned)bg.h);
        return false;
    }
    uint32_t elapsed = 0;
    const uint32_t need = (uint32_t)SAYA_ART_W * SAYA_ART_H * 2u;
    if (!decode_jpeg(bg.jpeg, bg.jpeg_len, (uint8_t *)img->pixels, need, SAYA_ART_W,
                     SAYA_ART_H, &elapsed)) {
        return false;
    }
    img->last_decode_ms = elapsed;
    ESP_LOGI(TAG, "背景 %u 上半段解码 %u ms", (unsigned)bg_id, (unsigned)elapsed);
    lv_obj_invalidate(img->canvas);
    return true;
}

bool saya_image_show_bg(saya_image_t *img, const saya_pack_t *pack, uint16_t bg_id)
{
    if (!img || !img->canvas || !pack) return false;
    if (!bg_upper_show(img, pack, bg_id)) return false;
    strip_show(img, pack, bg_id);
    return true;
}

// 立绘上半段:解码到下半块的缓冲里当暂存(上半段最大 180x150x2=54KB < 57.6KB),
// 再按遮罩拷进画面区画布。等下半块的背景解码时这块缓冲会被覆盖。
static void sprite_upper_composite(saya_image_t *img, const saya_pack_t *pack, uint16_t fg_id)
{
    saya_fg_t fg;
    if (!saya_pack_fg(pack, fg_id, &fg)) {
        ESP_LOGW(TAG, "立绘下标越界: %u", (unsigned)fg_id);
        return;
    }
    if (fg.w > SAYA_SPRITE_MAX_W || fg.h > SAYA_ART_H) {
        // 打包时已限宽限高,这里只防御损坏的包。
        ESP_LOGW(TAG, "立绘尺寸超缓冲: %ux%u", (unsigned)fg.w, (unsigned)fg.h);
        return;
    }
    const uint32_t need = (uint32_t)fg.w * fg.h * 2u;
    // 优先用下半块缓冲(54KB 够放),不够再退回调用方给的暂存。
    uint8_t *scratch = (uint8_t *)img->strip_pixels;
    uint32_t scratch_size = (uint32_t)SAYA_STRIP_W * SAYA_STRIP_H * 2u;
    if (need > scratch_size) {
        scratch = img->sprite_scratch;
        scratch_size = img->sprite_scratch_size;
    }
    if (need > scratch_size) {
        ESP_LOGW(TAG, "立绘解码缓冲不足: 需要 %u", (unsigned)need);
        return;
    }
    uint32_t elapsed = 0;
    if (!decode_jpeg(fg.jpeg, fg.jpeg_len, scratch, scratch_size, fg.w, fg.h, &elapsed)) return;
    img->last_decode_ms += elapsed;
    ESP_LOGI(TAG, "立绘 %u(%ux%u) 解码 + 合成 %u ms", (unsigned)fg_id, (unsigned)fg.w,
             (unsigned)fg.h, (unsigned)elapsed);

    // 按 1bpp 遮罩把立绘拷进画布:遮罩位为 1 才覆盖,MSB 在左。
    // 立绘贴画面区右侧(不再居中),像视觉小说里站在镜头右边的角色。
    const uint16_t *src = (const uint16_t *)scratch;
    const int x_off = SAYA_ART_W - (int)fg.w;
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
}

bool saya_image_show(saya_image_t *img, const saya_pack_t *pack, uint16_t bg_id, uint16_t fg_id)
{
    if (!img || !img->canvas || !pack) return false;
    if (!bg_upper_show(img, pack, bg_id)) return false;
    // 顺序要紧:立绘上半段借用下半块缓冲,之后才解码下半块背景把它覆盖掉。
    if (fg_id != SAYA_NONE && fg_id != SAYA_FG_KEEP) sprite_upper_composite(img, pack, fg_id);
    strip_show(img, pack, bg_id);
    if (fg_id != SAYA_NONE && fg_id != SAYA_FG_KEEP) sprite_lower_composite(img, pack, fg_id);
    return true;
}
