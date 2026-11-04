// main/gal_image.c —— 画面合成实现(esp_jpeg / TJpgDec -> RGB565 画布)。
#include "gal_image.h"

#include "gal_model.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "jpeg_decoder.h"
#include "lvgl.h"

#include <string.h>

static int s_blit_count;
int gal_image_test_blits(void) { return s_blit_count; }
void gal_image_test_reset_blits(void) { s_blit_count = 0; }

static const char *TAG = "gal_img";

// TJpgDec 工作区(esp_jpeg 说明:默认 3.1KB,JD_FASTDECODE=2 时 65KB)。
// C3 上用默认的 32bit 优化,4KB 足够;放静态区避免每次解码都 malloc。
#define GAL_JPEG_WORK_SIZE 4096
static uint8_t s_jpeg_work[GAL_JPEG_WORK_SIZE];

static bool decode_jpeg(const uint8_t *data, uint32_t len, uint8_t *out, uint32_t out_size,
                        esp_jpeg_image_output_t *info)
{
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
    if (scratch_size < (uint32_t)GAL_SPRITE_MAX_W * GAL_ART_H * 2u) return false;
    memset(img, 0, sizeof(*img));
    img->art_px = art_px;
    img->strip_px = strip_px;
    img->scratch = scratch;
    img->scratch_size = scratch_size;
    img->shown = GAL_NONE;

    for (int i = 0; i < GAL_ART_W * GAL_ART_H; ++i) art_px[i] = 0;
    for (int i = 0; i < GAL_STRIP_W * GAL_STRIP_H; ++i) strip_px[i] = 0;

    img->art_canvas = lv_canvas_create(parent);
    if (!img->art_canvas) return false;
    lv_canvas_set_buffer(img->art_canvas, art_px, GAL_ART_W, GAL_ART_H, LV_COLOR_FORMAT_RGB565);
    // image/canvas 默认在对象内居中绘制,必须显式左上对齐,否则画面会偏移。
    lv_image_set_align(img->art_canvas, LV_IMAGE_ALIGN_TOP_LEFT);
    lv_obj_set_pos(img->art_canvas, 0, 0);
    lv_obj_remove_flag(img->art_canvas, LV_OBJ_FLAG_SCROLLABLE);

    img->strip_canvas = lv_canvas_create(parent);
    if (!img->strip_canvas) return false;
    lv_canvas_set_buffer(img->strip_canvas, strip_px, GAL_STRIP_W, GAL_STRIP_H,
                         LV_COLOR_FORMAT_RGB565);
    lv_image_set_align(img->strip_canvas, LV_IMAGE_ALIGN_TOP_LEFT);
    // 2x 放大必须以左上角为轴心，否则按中心缩放，下半条会整体偏移 1~3px。
    lv_image_set_pivot(img->strip_canvas, 0, 0);
    lv_image_set_scale(img->strip_canvas, GAL_STRIP_SCALE);
    lv_obj_set_pos(img->strip_canvas, 0, GAL_ART_H);
    lv_obj_set_size(img->strip_canvas, GAL_STRIP_DISPLAY_W, GAL_STRIP_DISPLAY_H);
    lv_obj_remove_flag(img->strip_canvas, LV_OBJ_FLAG_SCROLLABLE);
    return true;
}

void gal_image_clear(gal_image_t *img)
{
    if (!img || !img->art_px) return;
    memset(img->art_px, 0, sizeof(uint16_t) * GAL_ART_W * GAL_ART_H);
    memset(img->strip_px, 0, sizeof(uint16_t) * GAL_STRIP_W * GAL_STRIP_H);
    img->shown = GAL_NONE;
    if (img->art_canvas) lv_obj_invalidate(img->art_canvas);
    if (img->strip_canvas) lv_obj_invalidate(img->strip_canvas);
}

bool gal_image_show(gal_image_t *img, const gal_pack_t *pack, uint16_t image_id)
{
    if (!img || !pack) return false;
    const uint8_t *top = NULL, *bot = NULL, *mt = NULL, *mb = NULL;
    uint32_t top_len = 0, bot_len = 0, mt_len = 0, mb_len = 0;
    uint16_t w = 0, h = 0;
    if (!gal_image_parts(pack, image_id, &top, &top_len, &bot, &bot_len,
                         &mt, &mt_len, &mb, &mb_len, &w, &h)) {
        return false;
    }
    esp_jpeg_image_output_t info = { 0 };
    if (!decode_jpeg(top, top_len, (uint8_t *)img->art_px,
                     (uint32_t)GAL_ART_W * GAL_ART_H * 2u, &info) ||
        info.width != GAL_ART_W || info.height != GAL_ART_H) {
        ESP_LOGE(TAG, "画面区解码尺寸不符: %ux%u", info.width, info.height);
        return false;
    }
    if (!decode_jpeg(bot, bot_len, (uint8_t *)img->strip_px,
                     (uint32_t)GAL_STRIP_W * GAL_STRIP_H * 2u, &info) ||
        info.width != GAL_STRIP_W || info.height != GAL_STRIP_H) {
        ESP_LOGE(TAG, "条带解码尺寸不符: %ux%u", info.width, info.height);
        return false;
    }
    img->shown = image_id;
    if (img->art_canvas) lv_obj_invalidate(img->art_canvas);
    if (img->strip_canvas) lv_obj_invalidate(img->strip_canvas);
    return true;
}

// 把一块解码后的 RGB565 按 1bpp 遮罩拷进目标画布。
static void blit_masked(uint16_t *dst, int dst_w, int dst_h, int x, int y,
                        const uint16_t *src, int w, int h, const uint8_t *mask)
{
    if (!dst || !src || !mask || w <= 0 || h <= 0) return;
    const int stride = (w + 7) / 8;
    for (int row = 0; row < h; ++row) {
        const int dy = y + row;
        if (dy < 0 || dy >= dst_h) continue;
        const uint8_t *mrow = mask + (size_t)row * (size_t)stride;
        const uint16_t *srow = src + (size_t)row * (size_t)w;
        uint16_t *drow = dst + (size_t)dy * (size_t)dst_w;
        for (int col = 0; col < w; ++col) {
            const int dx = x + col;
            if (dx < 0 || dx >= dst_w) continue;
            if (!(mrow[col >> 3] & (uint8_t)(0x80u >> (col & 7)))) continue;
            drow[dx] = srow[col];
        }
    }
}

static int slot_x(const gal_image_t *img, int slot, int w)
{
    (void)img;
    return gal_sprite_x(slot, w, GAL_ART_W);
}

// 立绘下半段画进半分辨率条带:源坐标每 2x2 取一点。
static void blit_masked_half(uint16_t *dst, int dst_w, int dst_h, int x, int y,
                             const uint16_t *src, int w, int h, const uint8_t *mask)
{
    if (!dst || !src || !mask || w <= 0 || h <= 0) return;
    const int stride = (w + 7) / 8;
    for (int row = 0; row < h; row += 2) {
        const int dy = y + row / 2;
        if (dy < 0 || dy >= dst_h) continue;
        const uint8_t *mrow = mask + (size_t)row * (size_t)stride;
        const uint16_t *srow = src + (size_t)row * (size_t)w;
        uint16_t *drow = dst + (size_t)dy * (size_t)dst_w;
        for (int col = 0; col < w; col += 2) {
            const int dx = x + col / 2;
            if (dx < 0 || dx >= dst_w) continue;
            if (!(mrow[col >> 3] & (uint8_t)(0x80u >> (col & 7)))) continue;
            drow[dx] = srow[col];
        }
    }
}

bool gal_image_draw_sprites(gal_image_t *img, const gal_pack_t *pack, const uint16_t spr[3])
{
    const int r = gal_image_draw_sprites_checked(img, pack, spr);
    return r > 0;
}

// 0 = 没有立绘(正常),1 = 全部画好,-1 = 至少一张要求的立绘失败。
int gal_image_draw_sprites_checked(gal_image_t *img, const gal_pack_t *pack, const uint16_t spr[3])
{
    if (!img || !pack || !spr) return -1;
    int drawn = 0;
    bool any_failed = false;

    // ── 同一角色的多份立绘占多个槽位 ──────────────────────────────────────
    // 这个包的脚本会把**同一个人的不同表情**分别塞进不同槽位（实机=同一个人
    // 左/中/右各一份）。包里没有"角色 id"，只能用立绘的**几何 + JPEG 字节数**
    // 做聚类：同一角色的各表情都是同尺寸、同底图重编码，长度只差几十字节；
    // 不同角色差几百到上千。同簇只保留**最后设置的那个槽位**（后写覆盖先写，
    // 与原作"同一角色只有一个位置"的语义一致）。
    bool keep[3] = { false, true, true };
    {
        const gal_pack_image_t *rec[3] = { NULL, NULL, NULL };
        for (int slot = 0; slot < 3; ++slot) {
            if (spr[slot] == GAL_NONE) continue;
            if ((uint32_t)spr[slot] >= pack->image_count) continue;
            rec[slot] = &pack->images[spr[slot]];
        }
        for (int slot = 0; slot < 3; ++slot) {
            if (spr[slot] == GAL_NONE || rec[slot] == NULL) continue;
            for (int later = slot + 1; later < 3; ++later) {
                if (spr[later] == GAL_NONE || rec[later] == NULL) continue;
                if (rec[slot]->w != rec[later]->w || rec[slot]->h != rec[later]->h) continue;
                const uint32_t a = rec[slot]->top_len;
                const uint32_t b = rec[later]->top_len;
                const uint32_t diff = (a > b) ? (a - b) : (b - a);
                if (diff <= GAL_SPRITE_SAME_CHAR_LEN) {
                    keep[slot] = false;   // 后面的槽位代表这个角色
                    break;
                }
            }
        }
    }

    // 同帧去重按**图像内容**（JPEG 指针 + 长度）而不是 id：
    // 实机"左手/中间/右手各画一个同一个人"的根因就是包里同一个角色的三个 slot
    // 用了**三个不同 id 指向同一份 JPEG**，按 id 去重完全拦不住。
    const uint8_t *drawn_top[3] = { NULL, NULL, NULL };
    uint32_t drawn_top_len[3] = { 0, 0, 0 };
    for (int slot = 0; slot < 3; ++slot) {
        if (spr[slot] == GAL_NONE || !keep[slot]) continue;
        const uint8_t *top = NULL, *bot = NULL, *mt = NULL, *mb = NULL;
        uint32_t top_len = 0, bot_len = 0, mt_len = 0, mb_len = 0;
        uint16_t w = 0, h = 0;
        if (!gal_image_parts(pack, spr[slot], &top, &top_len, &bot, &bot_len,
                             &mt, &mt_len, &mb, &mb_len, &w, &h)) {
            any_failed = true;
            continue;
        }
        bool dup = false;
        for (int k = 0; k < 3; ++k) {
            if (drawn_top[k] == top && drawn_top_len[k] == top_len) { dup = true; break; }
        }
        if (dup) {
            continue;
        }
        if (w == 0 || w > GAL_SPRITE_MAX_W || h == 0 || h > GAL_SCREEN_H) {
            any_failed = true;
            continue;
        }
        // 遮罩必须覆盖"记录声明的几何":上段 min(h,150) 行,下段 h-150 行,
        // 每行 ceil(w/8) 字节。声明长度不足就直接拒绝,绝不读越界。
        const uint32_t rows_top = (h < GAL_ART_H) ? h : GAL_ART_H;
        const uint32_t rows_bot = (h > GAL_ART_H) ? (uint32_t)(h - GAL_ART_H) : 0;
        const uint32_t mask_stride = ((uint32_t)w + 7u) / 8u;
        if (mt_len < mask_stride * rows_top) {
            any_failed = true;
            continue;
        }
        if (rows_bot && (mb == NULL || mb_len < mask_stride * rows_bot)) {
            any_failed = true;
            continue;
        }
        esp_jpeg_image_output_t info = { 0 };
        const uint32_t need = (uint32_t)GAL_SPRITE_MAX_W * GAL_ART_H * 2u;
        if (!decode_jpeg(top, top_len, img->scratch, need, &info)) {
            any_failed = true;
            continue;
        }
        // 解码尺寸必须与记录一致(否则遮罩几何对不上)。
        if (info.width != w || info.height != rows_top) {
            any_failed = true;
            continue;
        }
        const int x = slot_x(img, slot, (int)w);
        blit_masked(img->art_px, GAL_ART_W, GAL_ART_H, x, 0,
                    (const uint16_t *)img->scratch, (int)w, (int)rows_top, mt);
        drawn++;
        s_blit_count++;
        for (int k = 0; k < 3; ++k) {
            if (drawn_top[k] == NULL) { drawn_top[k] = top; drawn_top_len[k] = top_len; break; }
        }
        // 立绘下半段(屏幕第 150..239 行)画进条带,自然延续到文本框背后。
        if (rows_bot) {
            esp_jpeg_image_output_t low = { 0 };
            if (decode_jpeg(bot, bot_len, img->scratch,
                            (uint32_t)GAL_SPRITE_MAX_W * GAL_ART_H * 2u, &low) &&
                low.width == w && low.height == rows_bot) {
                blit_masked_half(img->strip_px, GAL_STRIP_W, GAL_STRIP_H, x / 2, 0,
                                 (const uint16_t *)img->scratch, (int)w, (int)rows_bot, mb);
            } else {
                any_failed = true;
            }
        }
    }
    if (drawn) {
        if (img->art_canvas) lv_obj_invalidate(img->art_canvas);
        if (img->strip_canvas) lv_obj_invalidate(img->strip_canvas);
    }
    if (any_failed) return -1;
    return drawn ? 1 : 0;
}
