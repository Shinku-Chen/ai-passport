// tests/test_clannad_image.c —— 生产 clannad_image.c + 真实 TJpgDec 宿主回归。
#include "clannad_image.h"
#include "clannad_pack.h"

#include "lvgl.h"
#include "tjpgd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;
#define CHECK(cond, msg) do { checks++; if (!(cond)) { failures++; \
    printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); } } while (0)

static uint8_t *read_all(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    uint8_t *p = malloc((size_t)n);
    if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) {
        free(p);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return p;
}

static uint32_t section_offset(const uint8_t *pack, uint32_t type)
{
    const uint16_t n = clannad_rd16(pack + 16);
    for (uint16_t i = 0; i < n; ++i) {
        const uint8_t *s = pack + 24 + i * 16;
        if (clannad_rd32(s) == type) return clannad_rd32(s + 4);
    }
    return 0;
}

static uint64_t hash_pixels(const uint16_t *px)
{
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < CLAN_IMG_W * CLAN_IMG_H; ++i) {
        h ^= px[i];
        h *= 1099511628211ull;
    }
    return h;
}

static void set_asset_jpeg_len(uint8_t *asset_table, uint16_t asset, uint32_t len)
{
    uint8_t *rec = asset_table + (size_t)asset * CLAN_ASSET_BYTES;
    rec[4] = (uint8_t)len;
    rec[5] = (uint8_t)(len >> 8);
    rec[6] = (uint8_t)(len >> 16);
    rec[7] = (uint8_t)(len >> 24);
}

static uint32_t asset_jpeg_len(const uint8_t *asset_table, uint16_t asset)
{
    const uint8_t *rec = asset_table + (size_t)asset * CLAN_ASSET_BYTES;
    return clannad_rd32(rec + 4);
}

static void set_asset_mask_len(uint8_t *asset_table, uint16_t asset, uint32_t len)
{
    uint8_t *rec = asset_table + (size_t)asset * CLAN_ASSET_BYTES;
    rec[16] = (uint8_t)len;
    rec[17] = (uint8_t)(len >> 8);
    rec[18] = (uint8_t)(len >> 16);
    rec[19] = (uint8_t)(len >> 24);
}

typedef struct {
    const uint8_t *data;
    uint32_t len;
    uint32_t pos;
} jpeg_input_t;

static size_t jpeg_test_in(JDEC *jd, uint8_t *buff, size_t nbyte)
{
    jpeg_input_t *in = (jpeg_input_t *)jd->device;
    const size_t left = in->pos < in->len ? (size_t)(in->len - in->pos) : 0;
    const size_t take = nbyte < left ? nbyte : left;
    if (buff && take) memcpy(buff, in->data + in->pos, take);
    in->pos += (uint32_t)take;
    return take;
}

static bool jpeg_prepare_ok(const uint8_t *data, uint32_t len)
{
    static uint8_t pool[4096];
    JDEC jd;
    jpeg_input_t in = {data, len, 0};
    return jd_prepare(&jd, jpeg_test_in, pool, sizeof(pool), &in) == JDR_OK;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "assets/clannad/clannad_pack.bin";
    size_t size = 0;
    uint8_t *blob = read_all(path, &size);
    CHECK(blob != NULL, "read pack");
    if (!blob) return 1;
    clannad_pack_t pack;
    CHECK(clannad_pack_open(&pack, blob, (uint32_t)size), "pack open");
    char why[128];
    CHECK(clannad_pack_validate(&pack, why, sizeof(why)), "pack validate");

    static uint16_t pixels[CLAN_IMG_W * CLAN_IMG_H];
    clannad_image_t img;
    lv_obj_t root;
    CHECK(clannad_image_init(&img, &root, pixels), "image init");
    static uint16_t clear_pixels[CLAN_IMG_W * CLAN_IMG_H];
    clannad_image_clear(&img);
    memcpy(clear_pixels, pixels, sizeof(clear_pixels));

    uint16_t bg = CLAN_NONE, gallery = CLAN_NONE;
    uint16_t sprites[3] = {CLAN_NONE, CLAN_NONE, CLAN_NONE};
    int sprite_count = 0;
    int decoded = 0;
    for (uint16_t i = 0; i < pack.asset_count; ++i) {
        clannad_asset_t a;
        CHECK(clannad_pack_asset(&pack, i, &a), "asset record");
        if (a.kind == CLAN_ASSET_BG && bg == CLAN_NONE) bg = i;
        if (a.kind == CLAN_ASSET_GALLERY && gallery == CLAN_NONE) gallery = i;
        if (a.kind == CLAN_ASSET_SPRITE && sprite_count < 3) sprites[sprite_count++] = i;
        const bool ok = clannad_image_validate(&pack, i);
        if (!ok && i < 3) printf("asset %u validate failed\n", i);
        CHECK(ok, "every asset validates");
        const int w = a.w < CLAN_IMG_W ? a.w : CLAN_IMG_W;
        const int h = a.h < CLAN_IMG_H ? a.h : CLAN_IMG_H;
        CHECK(clannad_image_show_asset(&img, &pack, i, 0, 0, w, h,
                                       a.mask_len > 0),
              "every asset decodes");
        decoded++;
    }
    CHECK(decoded == (int)pack.asset_count && decoded >= 920, "decoded every asset");
    CHECK(bg != CLAN_NONE && gallery != CLAN_NONE && sprite_count == 3,
          "bg/gallery/sprites present");

    // 场景：背景 + 1/2/3 立绘；立绘底边贴画面区，盒子在屏内。
    CHECK(clannad_image_show_scene(&img, &pack, bg, sprites, 3), "scene 3 sprites");
    CHECK(clannad_image_show_scene(&img, &pack, bg, sprites, 1), "scene 1 sprite");
    uint16_t one = sprites[0];
    CHECK(clannad_image_show_scene(&img, &pack, bg, &one, 1), "scene one sprite");
    CHECK(clannad_image_show_cg(&img, &pack, gallery), "cg fit");

    clannad_asset_t sa[3];
    for (int i = 0; i < 3; ++i) CHECK(clannad_pack_asset(&pack, sprites[i], &sa[i]),
                                     "sprite record");
    int x1, y1, w1, h1, x2, y2, w2, h2, x3, y3, w3, h3;
    CHECK(clannad_image_portrait_box(1, 0, &sa[0], &x1, &y1, &w1, &h1), "box 1");
    CHECK(clannad_image_portrait_box(2, 1, &sa[1], &x2, &y2, &w2, &h2), "box 2");
    CHECK(clannad_image_portrait_box(3, 2, &sa[2], &x3, &y3, &w3, &h3), "box 3");
    CHECK(y1 + h1 <= CLAN_ART_H + 1 && y2 + h2 <= CLAN_ART_H + 1 &&
              y3 + h3 <= CLAN_ART_H + 1, "portrait bottoms stay in art area");
    CHECK(h1 > 0 && h2 > 0 && h3 > 0, "portrait heights positive");
    CHECK(w2 <= 160 && w3 <= 106, "cells bound portrait widths");
    CHECK(x3 >= 0 && x3 + w3 <= CLAN_SCREEN_W, "third portrait on screen");

    // 透明遮罩：同一背景，带立绘时透明像素应保持背景色。
    CHECK(clannad_image_show_scene(&img, &pack, bg, NULL, 0), "bg only");
    const clannad_asset_t *masked = &sa[0];
    int bx, by, bw, bh;
    CHECK(clannad_image_portrait_box(1, 0, masked, &bx, &by, &bw, &bh), "masked box");
    bool transparent_verified = false;
    int opaque_verified = false;
    if (masked->mask_len && bw > 8 && bh > 8) {
        const uint8_t *mask = pack.blob + masked->mask_off;
        const uint32_t stride = ((uint32_t)masked->w + 7u) / 8u;
        for (uint32_t sy = 2; sy < masked->h - 2 && !transparent_verified; ++sy) {
            for (uint32_t sx = 2; sx < masked->w - 2; ++sx) {
                const bool on = (mask[sy * stride + (sx >> 3)] >> (7u - (sx & 7u))) & 1u;
                if (on) continue;
                const int dx = bx + (int)(sx * bw / masked->w);
                const int dy = by + (int)(sy * bh / masked->h);
                if (dx < 0 || dx >= CLAN_IMG_W || dy < 0 || dy >= CLAN_IMG_H) continue;
                const uint16_t before = pixels[dy * CLAN_IMG_W + dx];
                (void)before;
                // 记录后再画立绘并比较
                static uint16_t saved;
                saved = before;
                CHECK(clannad_image_show_scene(&img, &pack, bg, &one, 1), "mask scene");
                const uint16_t after = pixels[dy * CLAN_IMG_W + dx];
                if (after == saved) {
                    transparent_verified = true;
                    break;
                }
            }
        }
        for (uint32_t sy = 2; sy < masked->h - 2 && !opaque_verified; ++sy) {
            for (uint32_t sx = 2; sx < masked->w - 2; ++sx) {
                const bool on = (mask[sy * stride + (sx >> 3)] >> (7u - (sx & 7u))) & 1u;
                if (!on) continue;
                opaque_verified = true;
                break;
            }
        }
    }
    CHECK(transparent_verified, "masked transparent pixel keeps background");
    CHECK(opaque_verified, "masked opaque pixel path covered");

    // 坏 JPEG：保留 SOI，破坏熵数据。
    uint8_t *bad = malloc(size);
    CHECK(bad != NULL, "bad alloc");
    if (bad) {
        memcpy(bad, blob, size);
        clannad_pack_t bp;
        CHECK(clannad_pack_open(&bp, bad, (uint32_t)size), "bad pack open");
        clannad_asset_t ba;
        CHECK(clannad_pack_asset(&bp, bg, &ba), "bad asset");
        // 声明长度只给一半：真实 TJpgDec 必须在截断流上失败。
        const uint32_t asset_sec = section_offset(bad, CLAN_SEC_ASSET);
        uint8_t *rec = bad + asset_sec + (uint32_t)bg * CLAN_ASSET_BYTES;
        const uint32_t jlen = clannad_rd32(rec + 4);
        const uint32_t half = jlen / 2;
        rec[4] = (uint8_t)half;
        rec[5] = (uint8_t)(half >> 8);
        rec[6] = (uint8_t)(half >> 16);
        rec[7] = (uint8_t)(half >> 24);
        CHECK(!clannad_image_validate(&bp, bg), "corrupt JPEG rejected");
        CHECK(!clannad_image_show_scene(&img, &bp, bg, NULL, 0),
              "corrupt JPEG scene fails");
        free(bad);
    }

    // 截断遮罩 / 声明尺寸不符：在校验层拒绝。
    uint8_t *patched = malloc(size);
    CHECK(patched != NULL, "patch alloc");
    if (patched) {
        memcpy(patched, blob, size);
        const uint32_t asset_off = section_offset(patched, CLAN_SEC_ASSET);
        const uint32_t sprite_off = asset_off + (uint32_t)sprites[1] * CLAN_ASSET_BYTES;
        const uint32_t full_mask = clannad_rd32(patched + sprite_off + 16);
        if (full_mask > 2) {
            const uint32_t small = full_mask - 2;
            patched[sprite_off + 16] = (uint8_t)small;
            patched[sprite_off + 17] = (uint8_t)(small >> 8);
            patched[sprite_off + 18] = (uint8_t)(small >> 16);
            patched[sprite_off + 19] = (uint8_t)(small >> 24);
            clannad_pack_t pp;
            CHECK(clannad_pack_open(&pp, patched, (uint32_t)size), "patched pack");
            CHECK(!clannad_image_validate(&pp, sprites[1]), "truncated mask rejected");
            // 恢复后再改声明宽度
            patched[sprite_off + 16] = (uint8_t)full_mask;
            patched[sprite_off + 17] = (uint8_t)(full_mask >> 8);
            patched[sprite_off + 18] = (uint8_t)(full_mask >> 16);
            patched[sprite_off + 19] = (uint8_t)(full_mask >> 24);
            const uint16_t w = clannad_rd16(patched + sprite_off + 8);
            patched[sprite_off + 8] = (uint8_t)(w + 1);
            CHECK(clannad_pack_open(&pp, patched, (uint32_t)size), "patched pack 2");
            CHECK(!clannad_image_validate(&pp, sprites[1]), "declared size mismatch rejected");
        }
        free(patched);
    }

    // ---------------------------------------------------------------- failure clear
    // A failed in-place composition must clear the complete canvas, not leave
    // the background/earlier sprites on the display. The clear baseline above
    // is the product's theme clear, not an arbitrary black rectangle.
    {
        static uint16_t scene_pixels[CLAN_IMG_W * CLAN_IMG_H];
        static uint16_t one_pixels[CLAN_IMG_W * CLAN_IMG_H];
        static uint16_t cg_pixels[CLAN_IMG_W * CLAN_IMG_H];
        static uint16_t title_pixels[CLAN_IMG_W * CLAN_IMG_H];
        CHECK(clannad_image_show_scene(&img, &pack, bg, sprites, 3),
              "failure baseline scene 3");
        memcpy(scene_pixels, pixels, sizeof(scene_pixels));
        const uint64_t scene_hash = hash_pixels(scene_pixels);

        uint8_t *mid = malloc(size);
        CHECK(mid != NULL, "failure pack copy");
        if (mid) {
            memcpy(mid, blob, size);
            clannad_pack_t mp;
            CHECK(clannad_pack_open(&mp, mid, (uint32_t)size), "failure pack open");
            uint8_t *asset_table = mid + section_offset(mid, CLAN_SEC_ASSET);
            const uint32_t full_mask = clannad_rd32(asset_table +
                                                     (size_t)sprites[1] * CLAN_ASSET_BYTES + 16);
            CHECK(full_mask > 8, "second sprite has mask");
            set_asset_mask_len(asset_table, sprites[1], full_mask - 8);
            lv_stub_invalidate_count = 0;
            CHECK(!clannad_image_show_scene(&img, &mp, bg, sprites, 3),
                  "bad second mask fails");
            CHECK(memcmp(pixels, clear_pixels, sizeof(clear_pixels)) == 0,
                  "bad second mask clears complete canvas");
            CHECK(lv_stub_invalidate_count > 0, "bad second mask invalidates canvas");
            set_asset_mask_len(asset_table, sprites[1], full_mask);
            CHECK(clannad_image_show_scene(&img, &mp, bg, sprites, 3),
                  "scene recovers after mask restore");
            CHECK(hash_pixels(pixels) == scene_hash,
                  "A->bad B->A restores the exact scene hash");

            // A real JPEG truncation that still has a valid header and starts
            // producing output before TJpgDec fails.
            const uint32_t one_len = asset_jpeg_len(asset_table, one);
            CHECK(clannad_image_show_scene(&img, &pack, bg, &one, 1),
                  "jpeg truncation baseline one sprite");
            memcpy(one_pixels, pixels, sizeof(one_pixels));
            const uint64_t one_hash = hash_pixels(one_pixels);
            uint32_t found = 0;
            for (uint32_t cut = one_len - 1; cut > 16 && cut > one_len - 600; cut -= 7) {
                set_asset_jpeg_len(asset_table, one, cut);
                if (!jpeg_prepare_ok(mp.blob + clannad_rd32(asset_table +
                                                            (size_t)one * CLAN_ASSET_BYTES),
                                     cut)) {
                    continue;
                }
                lv_stub_invalidate_count = 0;
                if (!clannad_image_show_scene(&img, &mp, bg, &one, 1)) {
                    found = cut;
                    break;
                }
            }
            CHECK(found != 0, "find truncated JPEG with valid header");
            CHECK(memcmp(pixels, clear_pixels, sizeof(clear_pixels)) == 0,
                  "truncated JPEG failure clears complete canvas");
            CHECK(lv_stub_invalidate_count > 0,
                  "truncated JPEG failure invalidates canvas");
            set_asset_jpeg_len(asset_table, one, one_len);
            CHECK(clannad_image_show_scene(&img, &mp, bg, &one, 1),
                  "one sprite recovers after JPEG restore");
            CHECK(hash_pixels(pixels) == one_hash,
                  "truncated JPEG restore preserves one-sprite hash");

            // CG path: same clear contract for a mid-decode failure.
            CHECK(clannad_image_show_cg(&img, &pack, gallery), "cg failure baseline");
            memcpy(cg_pixels, pixels, sizeof(cg_pixels));
            const uint64_t cg_hash = hash_pixels(cg_pixels);
            const uint32_t cg_len = asset_jpeg_len(asset_table, gallery);
            found = 0;
            for (uint32_t cut = cg_len - 1; cut > 16 && cut > cg_len - 600; cut -= 7) {
                set_asset_jpeg_len(asset_table, gallery, cut);
                if (!jpeg_prepare_ok(mp.blob + clannad_rd32(asset_table +
                                                            (size_t)gallery * CLAN_ASSET_BYTES),
                                     cut)) {
                    continue;
                }
                lv_stub_invalidate_count = 0;
                if (!clannad_image_show_cg(&img, &mp, gallery)) {
                    found = cut;
                    break;
                }
            }
            CHECK(found != 0, "find truncated CG with valid header");
            CHECK(memcmp(pixels, clear_pixels, sizeof(clear_pixels)) == 0,
                  "truncated CG failure clears complete canvas");
            CHECK(lv_stub_invalidate_count > 0,
                  "truncated CG failure invalidates canvas");
            set_asset_jpeg_len(asset_table, gallery, cg_len);
            CHECK(clannad_image_show_cg(&img, &mp, gallery), "CG recovers after restore");
            CHECK(hash_pixels(pixels) == cg_hash, "CG restore preserves hash");

            // Title composition: bg succeeds, logo then fails. show_asset must
            // clear the whole half-composition; the app caller also sets notice.
            const uint16_t title_bg =
                (uint16_t)clannad_pack_meta_int(&pack, "title_before", 0);
            const uint16_t title_logo =
                (uint16_t)clannad_pack_meta_int(&pack, "logo", 0);
            const uint16_t title_sub =
                (uint16_t)clannad_pack_meta_int(&pack, "subtitle", 0);
            CHECK(title_bg && title_logo && title_sub, "title assets present");
            // 与应用一致：标题合成前先清整块画布（否则上一次场景的残留会进哈希）。
            clannad_image_clear(&img);
            CHECK(clannad_image_show_asset(&img, &pack, title_bg, CLAN_BG_X, 0,
                                           CLAN_BG_W, CLAN_BG_H, false) &&
                      clannad_image_show_asset(&img, &pack, title_logo, 4, 6,
                                               180, 41, true) &&
                      clannad_image_show_asset(&img, &pack, title_sub, 10, 52,
                                               130, 25, true),
                  "title composition baseline");
            memcpy(title_pixels, pixels, sizeof(title_pixels));
            const uint64_t title_hash = hash_pixels(title_pixels);
            const uint32_t logo_mask = clannad_rd32(asset_table +
                                                     (size_t)title_logo * CLAN_ASSET_BYTES + 16);
            CHECK(logo_mask > 8, "title logo has mask");
            set_asset_mask_len(asset_table, title_logo, logo_mask - 8);
            CHECK(clannad_image_show_asset(&img, &mp, title_bg, CLAN_BG_X, 0,
                                           CLAN_BG_W, CLAN_BG_H, false),
                  "title bg before broken logo");
            lv_stub_invalidate_count = 0;
            CHECK(!clannad_image_show_asset(&img, &mp, title_logo, 4, 6, 180, 41, true),
                  "broken title logo fails");
            CHECK(memcmp(pixels, clear_pixels, sizeof(clear_pixels)) == 0,
                  "title composition failure clears complete canvas");
            CHECK(lv_stub_invalidate_count > 0,
                  "title composition failure invalidates canvas");
            set_asset_mask_len(asset_table, title_logo, logo_mask);
            CHECK(clannad_image_show_asset(&img, &mp, title_bg, CLAN_BG_X, 0,
                                           CLAN_BG_W, CLAN_BG_H, false) &&
                      clannad_image_show_asset(&img, &mp, title_logo, 4, 6,
                                               180, 41, true) &&
                      clannad_image_show_asset(&img, &mp, title_sub, 10, 52,
                                               130, 25, true),
                  "title composition recovers");
            CHECK(hash_pixels(pixels) == title_hash,
                  "title restore preserves composition hash");
            free(mid);
        }
    }

    printf("%s: checks %d, failures %d\n", failures ? "FAIL" : "PASS", checks, failures);
    free(blob);
    return failures ? 1 : 0;
}
