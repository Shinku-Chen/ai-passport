// tests/test_gal_image.c —— 画面合成回归(真实 gal_image.c + JPEG/LVGL 薄桩)。
//
// 覆盖:场景上下两半解码、尺寸不符/解码失败必须返回 false、立绘按 1bpp 遮罩合成
// 到上半画面与半分辨率条带、无立绘返回 0、坏立绘返回 -1、越界图片 id 不崩。
#include "gal_image.h"
#include "gal_model.h"

#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        failures++; \
    } \
} while (0)

// ---- 构造一个假的 GALPK001 包 ------------------------------------------
#define NIMG 6
static uint8_t g_blob[4096];
static uint32_t g_size;

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }

// 生成一张图的 JPEG/掩码数据:tag_top/tag_bot 是桩解码器认的首字节。
static void build_pack(void)
{
    enum { NTAB = 15 };
    const uint32_t header = 20 + 16 * NTAB;

    // 图片数据放进 imgdata:每张图 top/bot 各 32 字节(桩只看首字节)。
    static uint8_t imgdata[4096];
    uint32_t rel_top[NIMG], rel_bot[NIMG];
    uint32_t used = 0;
    const char tags[NIMG][2] = { { 'T', 'B' }, { 'S', 's' }, { 'F', 'B' },
                                 { 'W', 'B' }, { 'T', 'F' }, { 'S', 's' } };
    for (int i = 0; i < NIMG; ++i) {
        rel_top[i] = used;
        memset(imgdata + used, 0, 32);
        imgdata[used] = (uint8_t)tags[i][0];
        used += 32;
        rel_bot[i] = used;
        memset(imgdata + used, 0, 32);
        imgdata[used] = (uint8_t)tags[i][1];
        used += 32;
    }
    uint8_t mask_top[((90 + 7) / 8) * 150];
    uint8_t mask_bot[((90 + 7) / 8) * 90];
    memset(mask_top, 0, sizeof(mask_top));
    memset(mask_bot, 0, sizeof(mask_bot));
    for (int y = 0; y < 150; ++y) {
        for (int x = 0; x < 45; ++x) mask_top[y * 12 + (x >> 3)] |= (uint8_t)(0x80u >> (x & 7));
    }
    for (int y = 0; y < 90; ++y) {
        for (int x = 0; x < 45; ++x) mask_bot[y * 12 + (x >> 3)] |= (uint8_t)(0x80u >> (x & 7));
    }
    const uint32_t mt_rel = used; memcpy(imgdata + used, mask_top, sizeof(mask_top)); used += sizeof(mask_top);
    const uint32_t mb_rel = used; memcpy(imgdata + used, mask_bot, sizeof(mask_bot)); used += sizeof(mask_bot);

    static uint8_t text[] = { 0, 0x41, 0x62, 0x63, 0 };
    static uint8_t names[8] = { 1, 0, 0, 0, 3, 0, 0, 0 };
    static uint8_t events[8] = { 9, 0, 0, 0, 0, 0, 0, 0 };   // EVENT END
    static uint8_t lines[8] = { 0 };
    static uint8_t gallery[8] = { 0 };
    static uint8_t title[4] = { 0, 0, 0, 0 };
    static uint8_t imgrec[36 * NIMG];

    const uint32_t sizes[NTAB] = {
        sizeof(text), sizeof(names), sizeof(events), sizeof(lines), 0, 0, 0, 0,
        sizeof(imgrec), 0, 0, sizeof(gallery), 4, sizeof(title), used,
    };
    const void *data[NTAB] = { text, names, events, lines, NULL, NULL, NULL, NULL,
                               imgrec, NULL, NULL, gallery, "{}", title, imgdata };
    const uint32_t counts[NTAB] = { 0, 1, 1, 1, 0, 0, 0, 0, NIMG, 0, 0, 1, 0, 1, 0 };

    uint32_t offsets[NTAB];
    uint32_t cursor = header;
    for (int i = 0; i < NTAB; ++i) {
        cursor = (cursor + 3u) & ~3u;
        offsets[i] = cursor;
        cursor += sizes[i];
    }
    const uint32_t imgdata_off = offsets[14];

    for (int i = 0; i < NIMG; ++i) {
        const uint16_t w = (i == 1 || i == 5) ? 90 : 320;
        const uint16_t h = 240;
        uint8_t *r = imgrec + i * 36;
        put16(r + 0, w);
        put16(r + 2, h);
        put32(r + 4, imgdata_off + rel_top[i]);
        put32(r + 8, 32);
        put32(r + 12, imgdata_off + rel_bot[i]);
        put32(r + 16, 32);
        if (i == 1) {
            put32(r + 20, imgdata_off + mt_rel);
            put32(r + 24, sizeof(mask_top));
            put32(r + 28, imgdata_off + mb_rel);
            put32(r + 32, sizeof(mask_bot));
        }
    }

    for (int i = 0; i < NTAB; ++i) {
        if (sizes[i] && data[i]) memcpy(g_blob + offsets[i], data[i], sizes[i]);
    }
    memcpy(g_blob, "GALPK001", 8);
    put32(g_blob + 8, 1);
    g_size = cursor;
    put32(g_blob + 12, g_size);
    put32(g_blob + 16, NTAB);
    for (int i = 0; i < NTAB; ++i) {
        uint8_t *p = g_blob + 20 + 16 * i;
        put32(p, (uint32_t)i);
        put32(p + 4, offsets[i]);
        put32(p + 8, counts[i]);
        put32(p + 12, sizes[i]);
    }
}

int main(void)
{
    lv_stub_reset();
    build_pack();
    gal_pack_t pack;
    const char *err = NULL;
    CHECK(gal_pack_open(&pack, g_blob, g_size, &err), "open: %s", err ? err : "?");
    CHECK(pack.image_count == NIMG, "images %u", (unsigned)pack.image_count);

    static uint16_t art[320 * 150];
    static uint16_t strip[160 * 45];
    static uint8_t scratch[96 * 150 * 2];
    gal_image_t img;
    lv_obj_t *page = lv_obj_create(lv_screen_active());
    CHECK(gal_image_init(&img, page, art, strip, scratch, sizeof(scratch)), "init");

    // 1) 正常场景:上半 320x150(0x1234),条带 160x45(0x5678)
    memset(art, 0, sizeof(art));
    memset(strip, 0, sizeof(strip));
    CHECK(gal_image_show(&img, &pack, 0), "show scene 0");
    CHECK(art[0] == 0x1234, "art[0]=0x%04x", art[0]);
    CHECK(strip[0] == 0x5678, "strip[0]=0x%04x", strip[0]);

    // 2) 解码失败 / 尺寸不符 -> false,且不能假装成功
    CHECK(!gal_image_show(&img, &pack, 2), "broken jpeg must fail");
    CHECK(!gal_image_show(&img, &pack, 3), "wrong dims must fail");
    CHECK(!gal_image_show(&img, &pack, 999), "out of range must fail");

    // 3) 无立绘:返回 0
    const uint16_t none[3] = { GAL_NONE, GAL_NONE, GAL_NONE };
    CHECK(gal_image_draw_sprites_checked(&img, &pack, none) == 0, "no sprites -> 0");

    // 4) 立绘合成:左半遮罩内的像素被写成 0x9ABC,遮罩外保持背景
    CHECK(gal_image_show(&img, &pack, 0), "reshow scene");
    const uint16_t one[3] = { 1, GAL_NONE, GAL_NONE };
    CHECK(gal_image_draw_sprites_checked(&img, &pack, one) == 1, "sprite ok -> 1");
    // 左对齐槽位 x=6;遮罩只点亮前 45 列
    CHECK(art[0 * 320 + 6] == 0x9ABC, "sprite masked pixel art[6]=0x%04x", art[6]);
    CHECK(art[0 * 320 + 6 + 44] == 0x9ABC, "sprite masked edge");
    CHECK(art[0 * 320 + 6 + 45] == 0x1234, "sprite unmasked pixel stays bg");
    // 下半段进入半分辨率条带:x/2 = 3,填 0xDEF0
    CHECK(strip[3] == 0xDEF0, "sprite lower half strip[3]=0x%04x", strip[3]);
    // 半分辨率:x 折半后,源列 0..44(遮罩左半)映射到目标列 0..22
    CHECK(strip[3 + 22] == 0xDEF0, "sprite lower mask edge strip[25]=0x%04x", strip[3 + 22]);
    CHECK(strip[3 + 23] == 0x5678, "sprite lower unmasked keeps bg");

    // 4b) 同一帧内同一 sprite 填满三个 slot：只能 blit 一次（否则立绘重复三个）。
    {
        gal_image_test_reset_blits();
        const uint16_t dup[3] = { 1, 1, 1 };
        (void)gal_image_draw_sprites_checked(&img, &pack, dup);
        CHECK(gal_image_test_blits() == 1, "duplicate sprite must blit once (got %d)",
              gal_image_test_blits());
    }

    // 5) 坏立绘(上半解码失败)-> -1,调用方据此不解锁/不缓存
    const uint16_t bad[3] = { 4, GAL_NONE, GAL_NONE };
    CHECK(gal_image_draw_sprites_checked(&img, &pack, bad) == -1, "broken sprite -> -1");
    const uint16_t missing[3] = { 999, GAL_NONE, GAL_NONE };
    CHECK(gal_image_draw_sprites_checked(&img, &pack, missing) == -1, "missing sprite -> -1");

    // 5b) 遮罩声明长度不足必须拒绝(不能读越声明区)
    {
        gal_pack_image_t *rec = (gal_pack_image_t *)(void *)&pack.images[1];
        const uint32_t old = rec->mask_top_len;
        rec->mask_top_len = 1;
        CHECK(gal_image_draw_sprites_checked(&img, &pack, one) == -1,
              "short declared mask must be rejected");
        rec->mask_top_len = old;
    }
    // 5c) 记录宽度与 JPEG 实际宽度不一致必须拒绝
    {
        gal_pack_image_t *rec = (gal_pack_image_t *)(void *)&pack.images[1];
        const uint16_t old = rec->w;
        rec->w = 1;
        CHECK(gal_image_draw_sprites_checked(&img, &pack, one) == -1,
              "record width must match decoded width");
        rec->w = old;
    }

    // 6) 三个槽位:中/右各自有不同 x,不越界
    CHECK(gal_image_show(&img, &pack, 0), "reshow");
    const uint16_t right[3] = { GAL_NONE, GAL_NONE, 1 };
    CHECK(gal_image_draw_sprites_checked(&img, &pack, right) == 1, "right slot");
    const int rx = gal_sprite_x(GAL_POS_RIGHT, 90, 320);
    CHECK(art[rx] == 0x9ABC, "right slot pixel at %d", rx);

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("image ok\n");
    return 0;
}
