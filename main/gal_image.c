// main/gal_image.c —— esp_jpeg(TJpgDec) -> RGB565 画布/条带 + 遮罩合成。
#include "gal_image.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "jpeg_decoder.h"
#include "lvgl.h"

#include <string.h>

static const char *TAG = "gal_img";
struct gal_overlay {
    struct _lv_obj_t *canvas;
    uint16_t *px;
};
#define GAL_OVERLAY_H (GAL_IMAGE_BODY_BOTTOM - GAL_ART_H) // 26
#define JPEG_WORK_SIZE 4096
static uint8_t s_jpeg_work[JPEG_WORK_SIZE];

static bool decode_jpeg(const uint8_t *data, uint32_t len, uint8_t *out, uint32_t out_size,
                        esp_jpeg_image_output_t *info)
{
    if (!data || !out || len == 0 || out_size == 0) return false;
    esp_jpeg_image_cfg_t cfg = {
        .indata = (uint8_t *)data,
        .indata_size = len,
        .outbuf = out,
        .outbuf_size = out_size,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
        .flags = { .swap_color_bytes = 0 },
        .advanced = { .working_buffer = s_jpeg_work, .working_buffer_size = sizeof(s_jpeg_work) },
    };
    const esp_err_t err = esp_jpeg_decode(&cfg, info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "JPEG 解码失败: %s (%u B)", esp_err_to_name(err), (unsigned)len);
        return false;
    }
    return true;
}

bool gal_image_init(gal_image_t *img, struct _lv_obj_t *parent,
                    uint16_t *art_px, uint16_t *strip_px,
                    uint8_t *scratch, uint32_t scratch_size)
{
    if (!img || !parent || !art_px || !strip_px || !scratch) return false;
    if (scratch_size < (uint32_t)GAL_PORTRAIT_MAX * GAL_PORTRAIT_MAX * 2u) return false;
    memset(img, 0, sizeof(*img));
    img->art_px = art_px;
    img->strip_px = strip_px;
    img->scratch = scratch;
    img->scratch_size = scratch_size;
    img->shown = GAL_NONE;
    memset(art_px, 0, sizeof(uint16_t) * GAL_ART_W * GAL_ART_H);
    memset(strip_px, 0, sizeof(uint16_t) * GAL_STRIP_W * GAL_STRIP_H);

    img->art_canvas = lv_canvas_create(parent);
    if (!img->art_canvas) return false;
    lv_canvas_set_buffer(img->art_canvas, art_px, GAL_ART_W, GAL_ART_H, LV_COLOR_FORMAT_RGB565);
    lv_image_set_align(img->art_canvas, LV_IMAGE_ALIGN_TOP_LEFT);
    lv_obj_set_pos(img->art_canvas, 0, 0);
    lv_obj_set_size(img->art_canvas, GAL_ART_W, GAL_ART_H);
    lv_obj_remove_flag(img->art_canvas, LV_OBJ_FLAG_SCROLLABLE);

    img->strip_canvas = lv_canvas_create(parent);
    if (!img->strip_canvas) return false;
    lv_canvas_set_buffer(img->strip_canvas, strip_px, GAL_STRIP_W, GAL_STRIP_H,
                         LV_COLOR_FORMAT_RGB565);
    lv_image_set_align(img->strip_canvas, LV_IMAGE_ALIGN_TOP_LEFT);
    // LVGL's default image pivot is the center. Anchor the 160x45 strip at
    // (0,0) before scaling. With scale 518/256, the last source pixel maps to
    // x=159*518/256=321.73 and y=44*518/256=89.03 relative to this object, so
    // the 320x90 object is fully covered; the slight overflow is clipped by
    // the object. Scale 512 would stop at x=318/y=88 and leave the last
    // column/row exposed.
    lv_image_set_pivot(img->strip_canvas, 0, 0);
    lv_image_set_scale(img->strip_canvas, GAL_STRIP_SCALE);
    lv_obj_set_pos(img->strip_canvas, 0, GAL_ART_H);
    lv_obj_set_size(img->strip_canvas, GAL_STRIP_W * 2, GAL_STRIP_H * 2);
    lv_obj_remove_flag(img->strip_canvas, LV_OBJ_FLAG_SCROLLABLE);

    // The body bottom moves from 150 to the panel top (176). Keep the existing
    // 320x150 art buffer and add only a 320x26 lower-body layer from LVGL's
    // heap; it sits above the strip and below the panel, so no large BSS array.
    const size_t ov_bytes = sizeof(struct gal_overlay) +
                            (size_t)GAL_ART_W * GAL_OVERLAY_H * 2u;
    struct gal_overlay *ov = lv_malloc(ov_bytes);
    if (ov) {
        memset(ov, 0, ov_bytes);
        ov->px = (uint16_t *)(ov + 1);
        ov->canvas = lv_canvas_create(parent);
        if (ov->canvas) {
            lv_canvas_set_buffer(ov->canvas, ov->px, GAL_ART_W, GAL_OVERLAY_H,
                                 LV_COLOR_FORMAT_RGB565);
            lv_image_set_align(ov->canvas, LV_IMAGE_ALIGN_TOP_LEFT);
            lv_obj_set_pos(ov->canvas, 0, GAL_ART_H);
            lv_obj_set_size(ov->canvas, GAL_ART_W, GAL_OVERLAY_H);
            lv_obj_remove_flag(ov->canvas, LV_OBJ_FLAG_SCROLLABLE);
            img->overlay = ov;
        } else {
            lv_free(ov);
        }
    }
    return true;
}

void gal_image_clear(gal_image_t *img)
{
    if (!img || !img->art_px) return;
    memset(img->art_px, 0, sizeof(uint16_t) * GAL_ART_W * GAL_ART_H);
    memset(img->strip_px, 0, sizeof(uint16_t) * GAL_STRIP_W * GAL_STRIP_H);
    img->shown = GAL_NONE;
    img->art_valid = img->strip_valid = false;
    img->body_valid = false;
    if (img->overlay) {
        memset(img->overlay->px, 0, (size_t)GAL_ART_W * GAL_OVERLAY_H * 2u);
        lv_obj_invalidate(img->overlay->canvas);
    }
    if (img->art_canvas) lv_obj_invalidate(img->art_canvas);
    if (img->strip_canvas) lv_obj_invalidate(img->strip_canvas);
}

bool gal_image_is_full(const gal_pack_t *pack, uint16_t image_id, const uint8_t **top,
                       uint32_t *top_len, const uint8_t **bottom, uint32_t *bottom_len)
{
    const gal_imgrec_t *im = gal_pack_image(pack, image_id);
    if (!im || im->kind != GAL_IMG_FULL || !im->aux_len) return false;
    if (top) *top = (const uint8_t *)pack->sec[GAL_SEC_IMGDATA] + im->data_off;
    if (top_len) *top_len = im->data_len;
    if (bottom) *bottom = (const uint8_t *)pack->sec[GAL_SEC_IMGDATA] + im->aux_off;
    if (bottom_len) *bottom_len = im->aux_len;
    return true;
}

static void overlay_fill(gal_image_t *img)
{
    if (!img || !img->overlay) return;
    for (int y = 0; y < GAL_OVERLAY_H; ++y) {
        int sy = ((y + GAL_IMAGE_BG_OFFSET + 1) * GAL_STRIP_H) /
                 (GAL_SCREEN_H - GAL_ART_H);
        // The viewport ends at 175; do not sample the packer's black lower
        // letterbox for the last visible row.  Clamp to the last view row.
        const int max_sy = ((GAL_OVERLAY_H - 1) * GAL_STRIP_H) /
                           (GAL_SCREEN_H - GAL_ART_H);
        if (sy > max_sy) sy = max_sy;
        for (int x = 0; x < GAL_ART_W; ++x) {
            int sx = (x * GAL_STRIP_W) / GAL_ART_W;
            if (sx >= GAL_STRIP_W) sx = GAL_STRIP_W - 1;
            img->overlay->px[y * GAL_ART_W + x] = img->strip_px[sy * GAL_STRIP_W + sx];
        }
    }
    lv_obj_invalidate(img->overlay->canvas);
}

static void shift_full_background(gal_image_t *img)
{
    if (!img || !img->art_px || !img->strip_px) return;
    const int off = GAL_IMAGE_BG_OFFSET;
    if (off <= 0 || off >= GAL_ART_H) return;
    memmove(img->art_px, img->art_px + (size_t)off * GAL_ART_W,
            (size_t)(GAL_ART_H - off) * GAL_ART_W * 2u);
    const int aux_h = GAL_SCREEN_H - GAL_ART_H;
    for (int k = 0; k < off; ++k) {
        int sy = (k * GAL_STRIP_H) / aux_h;
        if (sy >= GAL_STRIP_H) sy = GAL_STRIP_H - 1;
        for (int x = 0; x < GAL_ART_W; ++x) {
            int sx = (x * GAL_STRIP_W) / GAL_ART_W;
            if (sx >= GAL_STRIP_W) sx = GAL_STRIP_W - 1;
            img->art_px[(GAL_ART_H - off + k) * GAL_ART_W + x] =
                img->strip_px[sy * GAL_STRIP_W + sx];
        }
    }
}

bool gal_image_show(gal_image_t *img, const gal_pack_t *pack, uint16_t image_id,
                    const char **err)
{
    if (err) *err = NULL;
    if (!img || !pack) { if (err) *err = "画面参数无效"; return false; }
    gal_image_clear(img);
    if (image_id == GAL_NONE) {
        img->art_valid = img->strip_valid = true;
        img->body_valid = true;
        return true;
    }
    const gal_imgrec_t *im = gal_pack_image(pack, image_id);
    if (!im) { if (err) *err = "图片不存在"; return false; }
    const uint8_t *data = (const uint8_t *)pack->sec[GAL_SEC_IMGDATA] + im->data_off;
    esp_jpeg_image_output_t info = { 0 };
    const int64_t start = esp_timer_get_time();
    (void)start;
    if (!decode_jpeg(data, im->data_len, (uint8_t *)img->art_px,
                     (uint32_t)GAL_ART_W * GAL_ART_H * 2u, &info) ||
        info.width != GAL_ART_W || info.height != GAL_ART_H) {
        if (err) *err = "画面解码失败";
        gal_image_clear(img);
        return false;
    }
    if (im->kind == GAL_IMG_FULL) {
        if (!im->aux_len) {
            if (err) *err = "缺少条带数据";
            gal_image_clear(img);
            return false;
        }
        esp_jpeg_image_output_t low = { 0 };
        if (!decode_jpeg((const uint8_t *)pack->sec[GAL_SEC_IMGDATA] + im->aux_off, im->aux_len,
                         (uint8_t *)img->strip_px,
                         (uint32_t)GAL_STRIP_W * GAL_STRIP_H * 2u, &low) ||
            low.width != GAL_STRIP_W || low.height != GAL_STRIP_H) {
            if (err) *err = "条带解码失败";
            gal_image_clear(img);
            return false;
        }
    }
    // Only publish valid flags after every component succeeded.
    if (im->kind == GAL_IMG_FULL) shift_full_background(img);
    overlay_fill(img);
    img->art_valid = true;
    img->strip_valid = true;   // full: bottom decoded; art/book: strip intentionally black
    img->shown = image_id;
    img->last_ms = (uint32_t)((esp_timer_get_time() - start) / 1000);
    img->body_valid = true;
    if (img->art_canvas) lv_obj_invalidate(img->art_canvas);
    if (img->strip_canvas) lv_obj_invalidate(img->strip_canvas);
    return true;
}

static void blit_masked_split(gal_image_t *img, int x, int y,
                              const uint16_t *src, int w, int h, const uint8_t *mask)
{
    if (!img || !src || !mask || w <= 0 || h <= 0) return;
    const int stride = (w + 7) / 8;
    for (int row = 0; row < h; ++row) {
        const int dy = y + row;
        if (dy < 0 || dy >= GAL_IMAGE_BODY_BOTTOM) continue;
        uint16_t *dst;
        uint16_t *drow;
        if (dy < GAL_ART_H) {
            dst = img->art_px;
            drow = dst + (size_t)dy * GAL_ART_W;
        } else {
            if (!img->overlay) continue;
            drow = img->overlay->px + (size_t)(dy - GAL_ART_H) * GAL_ART_W;
        }
        const uint8_t *mrow = mask + (size_t)row * (size_t)stride;
        const uint16_t *srow = src + (size_t)row * (size_t)w;
        for (int col = 0; col < w; ++col) {
            const int dx = x + col;
            if (dx < 0 || dx >= GAL_ART_W) continue;
            if (!(mrow[col >> 3] & (uint8_t)(0x80u >> (col & 7)))) continue;
            drow[dx] = srow[col];
        }
    }
}

static int portrait_x(const gal_image_t *img, int index, int count, int w)
{
    int center;
    if (count <= 1) center = GAL_SCREEN_W / 2;
    else if (count == 2) center = index ? 240 : 80;
    else center = index == 0 ? 53 : (index == 1 ? 160 : 267);
    int x = center - w / 2;
    if (x + w > GAL_ART_W) x = GAL_ART_W - w;
    if (x < 0) x = 0;
    (void)img;
    return x;
}

static int draw_one_body(gal_image_t *img, const gal_pack_t *pack, const gal_sprite_t *s,
                         int index, int count)
{
    const gal_imgrec_t *im = gal_pack_image(pack, s->image);
    if (!im || im->kind != GAL_IMG_PORTRAIT) return 0;
    if (im->w == 0 || im->h == 0 || im->w > GAL_PORTRAIT_MAX || im->h > GAL_PORTRAIT_MAX) {
        return -1;
    }
    const uint32_t need = (uint32_t)im->w * im->h * 2u;
    if (need > img->scratch_size) return -1;
    const uint32_t stride = (im->w + 7u) / 8u;
    if (im->mask_len < stride * im->h) return -1;
    esp_jpeg_image_output_t info = { 0 };
    if (!decode_jpeg((const uint8_t *)pack->sec[GAL_SEC_IMGDATA] + im->data_off, im->data_len, img->scratch, need, &info) ||
        info.width != im->w || info.height != im->h) {
        return -1;
    }
    const int x = portrait_x(img, index, count, im->w);
    // im->h is the decoded/scaled portrait height; bottom is the panel top.
    const int y = GAL_IMAGE_BODY_BOTTOM - im->h;
    blit_masked_split(img, x, y, (const uint16_t *)img->scratch, im->w, im->h,
                      (const uint8_t *)pack->sec[GAL_SEC_IMGDATA] + im->mask_off);
    return 1;
}

static void restore_background(gal_image_t *img, const gal_pack_t *pack)
{
    const uint16_t bg = img->shown;
    const char *err = NULL;
    if (bg == GAL_NONE || !gal_image_show(img, pack, bg, &err)) {
        gal_image_clear(img);
    }
}

int gal_image_draw_bodies(gal_image_t *img, const gal_pack_t *pack,
                          const gal_sprite_t *bodies, uint8_t count)
{
    if (!img || !pack || !bodies) return -1;
    bool failed = false;
    int drawn = 0;
    for (uint8_t i = 0; i < count; ++i) {
        if (bodies[i].image == GAL_NONE) continue;
        const int r = draw_one_body(img, pack, &bodies[i], i, count);
        if (r == 1) drawn++;
        else if (r < 0) failed = true;
    }
    if (failed) {
        // A partial sprite/body composite must never remain visible or cacheable.
        restore_background(img, pack);
        img->body_valid = false;
        if (img->art_canvas) lv_obj_invalidate(img->art_canvas);
        return -1;
    }
    if (drawn && img->art_canvas) lv_obj_invalidate(img->art_canvas);
    img->body_valid = true;
    return drawn ? 1 : 0;
}

bool gal_image_redraw_bodies(gal_image_t *img, const gal_pack_t *pack,
                             const gal_sprite_t *bodies, uint8_t count)
{
    return gal_image_draw_bodies(img, pack, bodies, count) >= 0;
}

const gal_image_t *gal_image_state(const gal_image_t *img)
{
    return img;
}

bool gal_image_overlay_pixel(const gal_image_t *img, int x, int y, uint16_t *out)
{
    if (!img || !img->overlay || !out || x < 0 || x >= GAL_ART_W ||
        y < GAL_ART_H || y >= GAL_IMAGE_BODY_BOTTOM) {
        return false;
    }
    *out = img->overlay->px[(y - GAL_ART_H) * GAL_ART_W + x];
    return true;
}
