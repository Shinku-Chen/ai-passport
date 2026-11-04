// Real TJpgDec image regression: every packed image decodes at its declared
// geometry; portrait masks composite pixel-exactly; truncated/wrong-size
// images fail, then retry succeeds without unlocking anything.
#include "gal_image.h"
#include "gal_pack.h"
#include "gal_vm.h"
#include "jpeg_decoder.h"
#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); failures++; } \
} while (0)

static uint8_t *load_file(const char *path, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) exit(2);
    fclose(f);
    *size = (uint32_t)n;
    return b;
}

static uint32_t image_record_offset(const gal_pack_t *pack, uint16_t idx)
{
    const uint8_t *tab = pack->base + 24 + 16 * GAL_SEC_IMAGES;
    (void)tab;
    const uint32_t sec_off = (uint32_t)(pack->sec[GAL_SEC_IMAGES] - pack->base);
    return sec_off + (uint32_t)idx * 40u;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s pack\n", argv[0]); return 2; }
    uint32_t size = 0;
    uint8_t *data = load_file(argv[1], &size);
    gal_pack_t pack;
    const char *err = NULL;
    if (!gal_pack_open(&pack, data, size, &err)) {
        fprintf(stderr, "pack open: %s\n", err);
        return 1;
    }
    lv_stub_reset();
    lv_stub_require_lock(false);
    static uint16_t art[320 * 150], strip[160 * 45];
    static uint8_t scratch[150 * 150 * 2];
    static gal_image_t img;
    if (!gal_image_init(&img, lv_screen_active(), art, strip, scratch, sizeof(scratch))) {
        fprintf(stderr, "image init failed\n");
        return 1;
    }
    // LVGL scales around a center pivot by default. The lower 160x45 strip must
    // be anchored at (0,0), otherwise 2x expands it to x=-80..239/y=-22..66
    // around the object origin and exposes the light screen below it.
    CHECK(img.art_canvas && img.art_canvas->x == 0 && img.art_canvas->y == 0 &&
          img.art_canvas->w == 320 && img.art_canvas->h == 150, "art canvas geometry");
    CHECK(img.strip_canvas && img.strip_canvas->x == 0 && img.strip_canvas->y == 150 &&
          img.strip_canvas->w == 320 && img.strip_canvas->h == 90, "strip canvas geometry");
    CHECK(img.strip_canvas->scale == GAL_STRIP_SCALE, "strip scale=%d", img.strip_canvas->scale);
    CHECK(img.strip_canvas->pivot_x == 0 && img.strip_canvas->pivot_y == 0,
          "strip pivot=%d,%d", img.strip_canvas->pivot_x, img.strip_canvas->pivot_y);
    CHECK(img.overlay != NULL, "lower-body overlay missing");
    uint32_t decoded = 0, portraits = 0;
    for (uint16_t i = 0; i < pack.count[GAL_SEC_IMAGES]; ++i) {
        const gal_imgrec_t *rec = gal_pack_image(&pack, i);
        CHECK(rec != NULL, "image %u missing", i);
        if (!rec) continue;
        if (rec->kind == GAL_IMG_PORTRAIT) {
            CHECK(gal_image_show(&img, &pack, 0, &err), "bg for portrait %u: %s", i, err ? err : "");
            static gal_sprite_t body;
            memset(&body, 0, sizeof(body));
            body.image = i;
            const int r = gal_image_draw_bodies(&img, &pack, &body, 1);
            CHECK(r == 1, "portrait %u draw=%d (%ux%u)", i, r, rec->w, rec->h);
            CHECK(img.body_valid, "portrait %u body_valid", i);
            // 逐像素遮罩对照:独立解码到临时缓冲,与画布按遮罩比较
            static uint16_t origin[320 * 150];
            memcpy(origin, art, sizeof(origin));
            const uint32_t stride = (rec->w + 7u) / 8u;
            static uint16_t tmp[150 * 150];
            esp_jpeg_image_cfg_t cfg = {
                .indata = data + (uint32_t)(pack.sec[GAL_SEC_IMGDATA] - data) + rec->data_off,
                .indata_size = rec->data_len,
                .outbuf = (uint8_t *)tmp,
                .outbuf_size = sizeof(tmp),
                .out_format = JPEG_IMAGE_FORMAT_RGB565,
                .out_scale = JPEG_IMAGE_SCALE_0,
            };
            esp_jpeg_image_output_t info = { 0 };
            CHECK(esp_jpeg_decode(&cfg, &info) == ESP_OK && info.width == rec->w &&
                  info.height == rec->h, "portrait %u standalone decode", i);
            if (info.width == rec->w && info.height == rec->h) {
                int x = 160 - rec->w / 2;
                if (x + rec->w > 320) x = 320 - rec->w;
                if (x < 0) x = 0;
                int y = GAL_IMAGE_BODY_BOTTOM - rec->h;
                int mismatches = 0, checked = 0;
                for (uint16_t yy = 0; yy < rec->h; ++yy) {
                    if ((int)yy + y >= GAL_ART_H) break; // lower rows live in overlay
                    for (uint16_t xx = 0; xx < rec->w; ++xx) {
                        const bool on = (data[(uint32_t)(pack.sec[GAL_SEC_IMGDATA] - data) + rec->mask_off +
                                             yy * stride + (xx >> 3)] & (0x80u >> (xx & 7))) != 0;
                        const uint16_t want = on ? tmp[yy * rec->w + xx] : origin[(y + yy) * 320 + x + xx];
                        const uint16_t got = art[(y + yy) * 320 + x + xx];
                        if (want != got) mismatches++;
                        checked++;
                    }
                }
                CHECK(mismatches == 0, "portrait %u mask/pixels mismatch %d/%d", i, mismatches, checked);
            }
            portraits++;
        } else {
            CHECK(gal_image_show(&img, &pack, i, &err), "image %u show: %s (%ux%u)",
                  i, err ? err : "", rec->w, rec->h);
            CHECK(img.art_valid, "image %u art", i);
            if (rec->kind == GAL_IMG_FULL) CHECK(img.strip_valid, "image %u strip", i);
            bool nonblack = false;
            for (int p = 0; p < 320 * 150; ++p) if (art[p]) { nonblack = true; break; }
            const char *name = gal_pack_str(&pack, (uint16_t)rec->name);
            const bool solid = strstr(name, "solid_black") || strstr(name, "transparent");
            CHECK(nonblack || solid, "image %u (%s) decoded all-black", i, name);
        }
        decoded++;
    }
    printf("decoded images=%u portraits=%u\n", decoded, portraits);

    // Background must reach the panel top (global y 176) through the overlay,
    // and a masked portrait pixel must overwrite that background.
    {
        uint16_t p150 = 0, p175 = 0;
        CHECK(gal_image_show(&img, &pack, 3, &err), "background 3 show: %s", err ? err : "");
        CHECK(art[160] != 0 || art[0] != 0, "background first row is black");
        CHECK(gal_image_overlay_pixel(&img, 160, 150, &p150) && p150 != 0,
              "background missing at y=150: %04x", p150);
        CHECK(gal_image_overlay_pixel(&img, 160, 175, &p175) && p175 != 0,
              "background missing at y=175: %04x", p175);
        CHECK(!gal_image_overlay_pixel(&img, 160, 176, &p175),
              "overlay must not write the panel row y=176");
        int seam_diff = 0;
        uint16_t seam = 0;
        for (int x = 0; x < 320; ++x) {
            if (gal_image_overlay_pixel(&img, x, 150, &seam) &&
                art[149 * 320 + x] != seam) seam_diff++;
        }
        CHECK(seam_diff > 0, "seam repeats the art row (no differing pixels)");
        uint16_t pid = GAL_NONE;
        for (uint16_t i = 0; i < pack.count[GAL_SEC_IMAGES]; ++i) {
            const gal_imgrec_t *r = gal_pack_image(&pack, i);
            if (r && r->kind == GAL_IMG_PORTRAIT) { pid = i; break; }
        }
        CHECK(pid != GAL_NONE, "no portrait for overlay priority test");
        if (pid != GAL_NONE) {
            const gal_imgrec_t *r = gal_pack_image(&pack, pid);
            static uint16_t body_px[150 * 150];
            esp_jpeg_image_cfg_t cfg = {
                .indata = data + (uint32_t)(pack.sec[GAL_SEC_IMGDATA] - data) + r->data_off,
                .indata_size = r->data_len,
                .outbuf = (uint8_t *)body_px,
                .outbuf_size = sizeof(body_px),
                .out_format = JPEG_IMAGE_FORMAT_RGB565,
                .out_scale = JPEG_IMAGE_SCALE_0,
            };
            esp_jpeg_image_output_t info = { 0 };
            CHECK(esp_jpeg_decode(&cfg, &info) == ESP_OK &&
                  info.width == r->w && info.height == r->h, "overlay portrait decode");
            if (info.width == r->w && info.height == r->h) {
                const int x = 160 - r->w / 2;
                const int y = GAL_IMAGE_BODY_BOTTOM - r->h;
                const int stride = (r->w + 7) / 8;
                int xx = -1, yy = -1;
                for (int row = r->h - 1; row >= 0 && xx < 0; --row) {
                    for (int c = 0; c < r->w; ++c) {
                        if (data[(uint32_t)(pack.sec[GAL_SEC_IMGDATA] - data) + r->mask_off +
                                 row * stride + (c >> 3)] & (0x80u >> (c & 7))) {
                            xx = c; yy = row; break;
                        }
                    }
                }
                if (xx >= 0 && y + yy >= GAL_ART_H) {
                    uint16_t bgpx = 0;
                    CHECK(gal_image_overlay_pixel(&img, x + xx, y + yy, &bgpx),
                          "overlay sample outside");
                    static gal_sprite_t body;
                    memset(&body, 0, sizeof(body));
                    body.image = pid;
                    CHECK(gal_image_show(&img, &pack, 3, &err), "overlay bg retry");
                    CHECK(gal_image_draw_bodies(&img, &pack, &body, 1) == 1, "overlay body");
                    uint16_t after = 0;
                    CHECK(gal_image_overlay_pixel(&img, x + xx, y + yy, &after),
                          "overlay body sample outside");
                    CHECK(after == body_px[yy * r->w + xx],
                          "body did not override background at (%d,%d): %04x != %04x",
                          x + xx, y + yy, after, body_px[yy * r->w + xx]);
                }
            }
        }
    }

    // A -> bad B -> A: every component flag is cleared and pixels are fully
    // cleared on a bottom-strip or top failure, then recovery matches A.
    uint16_t full_idx = GAL_NONE;
    for (uint16_t i = 0; i < pack.count[GAL_SEC_IMAGES]; ++i) {
        const gal_imgrec_t *r = gal_pack_image(&pack, i);
        if (r && r->kind == GAL_IMG_FULL && r->aux_len) { full_idx = i; break; }
    }
    if (full_idx != GAL_NONE) {
        static uint16_t good_art[320 * 150], good_strip[160 * 45];
        CHECK(gal_image_show(&img, &pack, full_idx, &err), "A show: %s", err ? err : "");
        memcpy(good_art, art, sizeof(good_art));
        memcpy(good_strip, strip, sizeof(good_strip));
        gal_imgrec_t *m = (gal_imgrec_t *)(uintptr_t)gal_pack_image(&pack, full_idx);
        const uint32_t top_len = m->data_len;
        const uint32_t aux_len = m->aux_len;
        m->aux_len = 1;
        CHECK(!gal_image_show(&img, &pack, full_idx, &err), "bad strip accepted");
        CHECK(!img.art_valid && !img.strip_valid && !img.body_valid,
              "bad strip flags art=%d strip=%d body=%d", img.art_valid, img.strip_valid,
              img.body_valid);
        bool zero = true;
        for (int i = 0; i < 320 * 150; ++i) if (art[i] != 0) zero = false;
        for (int i = 0; i < 160 * 45; ++i) if (strip[i] != 0) zero = false;
        CHECK(zero, "bad strip left a half frame");
        m->aux_len = aux_len;
        CHECK(gal_image_show(&img, &pack, full_idx, &err), "A retry: %s", err ? err : "");
        CHECK(memcmp(art, good_art, sizeof(good_art)) == 0 &&
              memcmp(strip, good_strip, sizeof(good_strip)) == 0,
              "A pixels changed after bad strip");

        m->data_len = 1;
        CHECK(!gal_image_show(&img, &pack, full_idx, &err), "bad top accepted");
        CHECK(!img.art_valid && !img.strip_valid && !img.body_valid,
              "bad top flags art=%d strip=%d body=%d", img.art_valid, img.strip_valid,
              img.body_valid);
        zero = true;
        for (int i = 0; i < 320 * 150; ++i) if (art[i] != 0) zero = false;
        for (int i = 0; i < 160 * 45; ++i) if (strip[i] != 0) zero = false;
        CHECK(zero, "bad top left a half frame");
        m->data_len = top_len;
        CHECK(gal_image_show(&img, &pack, full_idx, &err), "A after top failure: %s",
              err ? err : "");
        CHECK(memcmp(art, good_art, sizeof(good_art)) == 0 &&
              memcmp(strip, good_strip, sizeof(good_strip)) == 0,
              "A pixels changed after bad top");
    }

    // Third portrait mid-composite failure: the full background is restored,
    // no partial bodies remain, and the cache is invalid until a retry.
    {
        uint16_t pidx[3] = { GAL_NONE, GAL_NONE, GAL_NONE };
        int pn = 0;
        for (uint16_t i = 0; i < pack.count[GAL_SEC_IMAGES] && pn < 3; ++i) {
            const gal_imgrec_t *r = gal_pack_image(&pack, i);
            if (r && r->kind == GAL_IMG_PORTRAIT) pidx[pn++] = i;
        }
        if (pn == 3 && full_idx != GAL_NONE) {
            CHECK(gal_image_show(&img, &pack, full_idx, &err), "body bg: %s", err ? err : "");
            static uint16_t bg_art[320 * 150];
            memcpy(bg_art, art, sizeof(bg_art));
            gal_sprite_t bs[3];
            memset(bs, 0, sizeof(bs));
            for (int i = 0; i < 3; ++i) { bs[i].image = pidx[i]; bs[i].used = 1; }
            CHECK(gal_image_draw_bodies(&img, &pack, bs, 3) == 1, "three bodies initial");
            gal_imgrec_t *m = (gal_imgrec_t *)(uintptr_t)gal_pack_image(&pack, pidx[2]);
            const uint32_t save_len = m->data_len;
            m->data_len = 1;
            CHECK(gal_image_draw_bodies(&img, &pack, bs, 3) == -1, "third body failure accepted");
            CHECK(!img.body_valid, "failed body cache valid");
            CHECK(memcmp(art, bg_art, sizeof(bg_art)) == 0,
                  "third body failure left partial pixels");
            m->data_len = save_len;
            CHECK(gal_image_draw_bodies(&img, &pack, bs, 3) == 1, "body retry");
            CHECK(img.body_valid, "body retry cache invalid");
        }
    }

    // 截断 JPEG:失败并清空 art_valid;恢复后同一资源可重试成功。
    const gal_imgrec_t *rec = gal_pack_image(&pack, 0);
    const uint32_t roff = image_record_offset(&pack, 0);
    const uint32_t save_len = rd32(data + roff + 8);
    const uint32_t save_aux = rd32(data + roff + 16);
    const uint32_t save_mask = rd32(data + roff + 24);
    (void)save_aux; (void)save_mask;
    const uint16_t save_w = rd16(data + roff + 32);
    // data_len 字段在记录里的偏移:name,off,len => +8
    data[roff + 8] = 100; data[roff + 9] = 0; data[roff + 10] = 0; data[roff + 11] = 0;
    CHECK(!gal_image_show(&img, &pack, 0, &err), "truncated image accepted");
    CHECK(!img.art_valid, "truncated image left art_valid");
    data[roff + 8] = (uint8_t)save_len; data[roff + 9] = (uint8_t)(save_len >> 8);
    data[roff + 10] = (uint8_t)(save_len >> 16); data[roff + 11] = (uint8_t)(save_len >> 24);
    CHECK(gal_image_show(&img, &pack, 0, &err), "retry after truncation failed: %s", err ? err : "");
    CHECK(img.art_valid, "retry art_valid");
    (void)rec; (void)save_w;
    printf("test_gal_image_real: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
