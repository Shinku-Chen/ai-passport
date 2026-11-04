// main/eden_image.c —— 画面区合成实现。
//
// 两条解码路径,都不需要整帧 RGBA,也不需要额外的大缓冲:
//   1. 底图:JPEG 264x168,esp_jpeg 直接解进画面缓冲(输出缓冲就是画布);
//   2. 补丁:JPEG 不超过 104x168,解进暂存条后按 eden_blit_span() 算出的范围逐行拷进画面。
//
// 失败语义(坏包/截断 JPEG):
//   * 绘制之前先用 eden_pack_frame_checked() 校验几何,再用 esp_jpeg_get_image_info()
//     预检每张图的 JPEG 头——两者都不过就不碰画布,上一帧原样保留;
//   * 解码过程中仍然失败时,重画底图(底图本身是一张完整画面),函数返回 false;
//   * 只要返回 false,调用方不得解锁 CG、不得提示成功。
#include "eden_image.h"

#include "eden_image_geom.h"

#include "esp_log.h"
#include "jpeg_decoder.h"
#include "lvgl.h"

#include <string.h>

static const char *TAG = "eden_img";

// TJpgDec 的工作区大小(esp_jpeg 的 JPEG_WORK_BUF_SIZE 默认值 3.1 KB + 余量)。
#define EDEN_JPEG_WORK_BUF 4096u

// 画布底色:纯黑,与源作画幅两侧的留边一致。
#define EDEN_COLOR_BLACK 0x0000u

static uint8_t s_work_buf[EDEN_JPEG_WORK_BUF];

// 预检:只解析 JPEG 头(解码器不解像素),拿到宽高并与包内声明的尺寸对照。
static bool probe_jpeg(const uint8_t *jpeg, uint32_t len, uint16_t width, uint16_t height)
{
    if (jpeg == NULL || len < 4) {
        return false;
    }
    esp_jpeg_image_cfg_t cfg = { 0 };
    cfg.indata = (uint8_t *)jpeg;
    cfg.indata_size = len;
    cfg.out_format = JPEG_IMAGE_FORMAT_RGB565;
    cfg.out_scale = JPEG_IMAGE_SCALE_0;
    esp_jpeg_image_output_t info = { 0 };
    if (esp_jpeg_get_image_info(&cfg, &info) != ESP_OK) {
        return false;
    }
    return info.width == width && info.height == height;
}

// 解码一张图到 dest(尺寸为图片的存储尺寸,行距 = width)。
// dest 必须能容纳 width*height 个 RGB565 像素。
static bool decode_to(const uint8_t *jpeg, uint32_t len, uint16_t width, uint16_t height,
                      uint16_t *dest)
{
    esp_jpeg_image_cfg_t cfg = { 0 };
    cfg.indata = (uint8_t *)jpeg;
    cfg.indata_size = len;
    cfg.outbuf = (uint8_t *)dest;
    cfg.outbuf_size = (uint32_t)width * height * 2u;
    cfg.out_format = JPEG_IMAGE_FORMAT_RGB565;
    cfg.out_scale = JPEG_IMAGE_SCALE_0;
    cfg.flags.swap_color_bytes = 0;   // TJpgDec 写 LOBYTE 在前 = LVGL 要的小端
    cfg.advanced.working_buffer = s_work_buf;
    cfg.advanced.working_buffer_size = sizeof(s_work_buf);

    esp_jpeg_image_output_t info = { 0 };
    const esp_err_t err = esp_jpeg_decode(&cfg, &info);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "JPEG 解码失败 err=%d len=%u", (int)err, (unsigned)len);
        return false;
    }
    if (info.width != width || info.height != height) {
        ESP_LOGW(TAG, "JPEG 尺寸不符: %ux%u != %ux%u", (unsigned)info.width,
                 (unsigned)info.height, (unsigned)width, (unsigned)height);
        return false;
    }
    return true;
}

bool eden_image_init(eden_image_t *image, struct _lv_obj_t *parent,
                     uint16_t *canvas_pixels, uint16_t *patch_pixels)
{
    if (image == NULL || parent == NULL || canvas_pixels == NULL || patch_pixels == NULL) {
        return false;
    }
    memset(image, 0, sizeof(*image));
    image->pixels = canvas_pixels;
    image->patch_pixels = patch_pixels;
    eden_image_fill(image, EDEN_COLOR_BLACK);

    image->canvas = lv_canvas_create(parent);
    if (image->canvas == NULL) {
        ESP_LOGE(TAG, "画布创建失败");
        return false;
    }
    lv_canvas_set_buffer(image->canvas, canvas_pixels, EDEN_CANVAS_W, EDEN_CANVAS_H,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(image->canvas, EDEN_CANVAS_X, EDEN_CANVAS_Y);
    return true;
}

void eden_image_set_pack(eden_image_t *image, const eden_pack_t *pack)
{
    if (image != NULL) {
        image->pack = pack;
    }
}

void eden_image_fill(eden_image_t *image, uint16_t color565)
{
    if (image == NULL || image->pixels == NULL) {
        return;
    }
    const size_t count = (size_t)EDEN_CANVAS_W * EDEN_CANVAS_H;
    for (size_t index = 0; index < count; index++) {
        image->pixels[index] = color565;
    }
    if (image->canvas != NULL) {
        lv_obj_invalidate(image->canvas);
    }
}

void eden_image_clear(eden_image_t *image)
{
    eden_image_fill(image, EDEN_COLOR_BLACK);
}

// 把暂存条里的可见范围贴到画面上。坐标与裁剪全部由 eden_blit_span() 判定,
// 这里不再做任何减法式长度计算(避免 x >= 宽度时的无符号回绕)。
static bool blit_patch(eden_image_t *image, const eden_pack_image_t *patch,
                       uint16_t x, uint16_t y)
{
    eden_blit_span_t span;
    if (!eden_blit_span(x, y, patch->content_w, patch->content_h,
                        EDEN_CANVAS_W, EDEN_CANVAS_H, &span)) {
        ESP_LOGW(TAG, "补丁坐标越界 (%u,%u) %ux%u", (unsigned)x, (unsigned)y,
                 (unsigned)patch->content_w, (unsigned)patch->content_h);
        return false;
    }
    for (uint16_t row = 0; row < span.rows; row++) {
        const uint16_t *src = image->patch_pixels + (size_t)row * patch->width;
        uint16_t *dst = image->pixels + (size_t)(span.y + row) * EDEN_CANVAS_W + span.x;
        memcpy(dst, src, (size_t)span.cols * sizeof(uint16_t));
    }
    return true;
}

bool eden_image_draw(eden_image_t *image, uint16_t frame_index)
{
    if (image == NULL || image->pack == NULL) {
        return false;
    }

    // 1) 几何校验:底图放得下、每条补丁整块在画面内且不超过暂存条。
    eden_frame_t frame;
    if (!eden_pack_frame_checked(image->pack, frame_index, EDEN_CANVAS_W, EDEN_CANVAS_H,
                                 EDEN_PATCH_W, &frame)) {
        ESP_LOGW(TAG, "画面 %u 几何非法,保留上一帧", (unsigned)frame_index);
        return false;
    }

    // 2) JPEG 头预检:任何一张图坏了都不碰画布。
    eden_pack_image_t base;
    if (!eden_pack_image(image->pack, frame.base, &base) ||
        !probe_jpeg(base.data, base.len, base.width, base.height)) {
        ESP_LOGW(TAG, "画面 %u 底图损坏,保留上一帧", (unsigned)frame_index);
        return false;
    }
    for (uint16_t index = 0; index < frame.patch_count; index++) {
        eden_patch_t patch;
        eden_pack_image_t source;
        if (!eden_pack_patch(image->pack, (uint16_t)(frame.patch_first + index), &patch) ||
            !eden_pack_image(image->pack, patch.image, &source) ||
            !probe_jpeg(source.data, source.len, source.width, source.height)) {
            ESP_LOGW(TAG, "画面 %u 补丁 %u 损坏,保留上一帧", (unsigned)frame_index,
                     (unsigned)index);
            return false;
        }
    }

    // 3) 解码:底图 → 画布,补丁 → 暂存条 → 画面。
    if (!decode_to(base.data, base.len, base.width, base.height, image->pixels)) {
        eden_image_clear(image);
        return false;
    }

    bool complete = true;
    for (uint16_t index = 0; index < frame.patch_count; index++) {
        eden_patch_t patch;
        eden_pack_image_t source;
        if (!eden_pack_patch(image->pack, (uint16_t)(frame.patch_first + index), &patch) ||
            !eden_pack_image(image->pack, patch.image, &source)) {
            complete = false;
            break;
        }
        if (!decode_to(source.data, source.len, source.width, source.height,
                       image->patch_pixels)) {
            complete = false;
            break;
        }
        if (!blit_patch(image, &source, patch.x, patch.y)) {
            complete = false;
            break;
        }
    }

    if (!complete) {
        // 别把"底图 + 一半补丁"的残缺帧留在屏幕上:重画底图,它本身就是完整画面。
        ESP_LOGW(TAG, "画面 %u 合成失败,回退到底图", (unsigned)frame_index);
        (void)decode_to(base.data, base.len, base.width, base.height, image->pixels);
    }

    if (image->canvas != NULL) {
        lv_obj_invalidate(image->canvas);
    }
    return complete;
}

// ---------------------------------------------------------------- CG 全屏分带
// image->pixels 里已经是解码好的 264x168 帧；这里按 cover 映射到 320x240，
// 只算第 band 条带（60 行）。scale = max(320/264, 240/168) = 240/168 ≈ 1.4286，
// 横向多出的 ~57 px 居中裁掉。
bool eden_image_cg_band(eden_image_t *image, uint16_t *band_pixels, int band)
{
    if (image == NULL || image->pixels == NULL || band_pixels == NULL) return false;
    if (band < 0 || band >= EDEN_CG_BANDS) return false;

    const int sw = EDEN_FRAME_W;      // 264
    const int sh = EDEN_FRAME_H;      // 165（内容高，画布 168 里下面 3 行是黑边）
    const int dw = EDEN_CG_BAND_W;    // 320
    const int dh = EDEN_CG_BANDS * EDEN_CG_BAND_H;   // 240

    const int x_q = (dw << 16) / sw;
    const int y_q = (dh << 16) / sh;
    const int scale_q16 = x_q > y_q ? x_q : y_q;
    const int scaled_w = (int)(((long)sw * scale_q16) >> 16);
    const int off_x = (dw - scaled_w) / 2;

    const int y0 = band * EDEN_CG_BAND_H;
    for (int row = 0; row < EDEN_CG_BAND_H; row++) {
        const int dy = y0 + row;
        const int sy = (int)(((long)dy * 65536) / scale_q16);   // off_y == 0（纵向正好铺满）
        uint16_t *dst = band_pixels + (size_t)row * dw;
        if (sy < 0 || sy >= sh) {
            memset(dst, 0, (size_t)dw * sizeof(uint16_t));
            continue;
        }
        const uint16_t *src = image->pixels + (size_t)sy * EDEN_CANVAS_W;
        for (int x = 0; x < dw; x++) {
            const int sx = (int)(((long)(x - off_x) * 65536) / scale_q16);
            dst[x] = (sx < 0 || sx >= sw) ? 0 : src[sx];
        }
    }
    return true;
}
