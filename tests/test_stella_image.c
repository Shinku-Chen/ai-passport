// Real stella_image.c + project TJpgDec host regression: JPEG, mask, scaling,
// geometry, corrupt image, truncated mask. Only LVGL is a thin flag/canvas stub.
#include "stella_image.h"
#include "lvgl.h"
#include "stella_pack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef STELLA_PACK_PATH
#define STELLA_PACK_PATH "assets/stella/stella_pack.bin"
#endif

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); failures++; } } while (0)

static uint8_t *read_all(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    uint8_t *p = malloc((size_t)n);
    if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); fclose(f); return NULL; }
    fclose(f);
    *size = (size_t)n;
    return p;
}

static uint32_t section_offset(const uint8_t *pack, uint32_t type)
{
    const uint32_t n = stella_rd32(pack + 16);
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t *s = pack + 20 + i * 16;
        if (stella_rd32(s) == type) return stella_rd32(s + 4);
    }
    return 0;
}

#define SEC_ASSET 5u
#define SEC_BLOB 6u

int main(void)
{
    size_t size = 0;
    uint8_t *blob = read_all(STELLA_PACK_PATH, &size);
    CHECK(blob != NULL, "read pack");
    if (!blob) return 1;
    stella_pack_t pack;
    CHECK(stella_pack_open(&pack, blob, (uint32_t)size), "pack open");
    stella_image_t img;
    static uint16_t pixels[STELLA_SCREEN_W * STELLA_SCREEN_H];
    lv_obj_t root;
    CHECK(stella_image_init(&img, &root, pixels), "image init");

    // Find real background/sprite assets from the package (not build report).
    uint16_t bg = STELLA_NONE, sprite = STELLA_NONE;
    for (uint16_t i = 0; i < pack.asset_count; ++i) {
        stella_asset_t a;
        if (!stella_pack_asset(&pack, i, &a)) continue;
        if (a.kind == STELLA_ASSET_BG && bg == STELLA_NONE) bg = i;
        if (a.kind == STELLA_ASSET_SPRITE && sprite == STELLA_NONE) sprite = i;
    }
    CHECK(bg != STELLA_NONE && sprite != STELLA_NONE, "bg/sprite assets");
    CHECK(stella_image_validate_scene(&pack, bg, &sprite, 1), "validate scene");
    CHECK(stella_image_show_scene(&img, &pack, bg, &sprite, 1), "show scene");
    CHECK(stella_image_validate_scene(&pack, bg, NULL, 0), "validate bg only");
    uint16_t bodies[3] = {sprite, sprite, sprite};
    CHECK(stella_image_show_scene(&img, &pack, bg, bodies, 3), "show three sprites");
    CHECK(stella_image_portrait_box, "portrait box symbol");

    // Geometry/anchor scaling for 1/2/3 portraits.
    stella_asset_t a = { .w = 100, .h = 200, .ow = 100, .oh = 200 };
    int dx, dy, dw, dh;
    stella_image_portrait_box(1, 0, &a, &dx, &dy, &dw, &dh);
    CHECK(dw > 0 && dh > 0 && dy + dh <= STELLA_SCREEN_H, "one portrait box %d,%d,%d,%d",
          dx, dy, dw, dh);
    stella_image_portrait_box(2, 1, &a, &dx, &dy, &dw, &dh);
    CHECK(dw > 0 && dh > 0 && dx >= 160, "second portrait cell %d,%d,%d,%d", dx, dy, dw, dh);
    stella_image_portrait_box(3, 2, &a, &dx, &dy, &dw, &dh);
    CHECK(dw > 0 && dh > 0 && dx >= 212, "third portrait cell %d,%d,%d,%d", dx, dy, dw, dh);

    // Real CG path.
    stella_group_t group;
    CHECK(stella_pack_group(&pack, 0, &group) && group.count > 0, "gallery group");
    stella_gentry_t entry;
    CHECK(stella_pack_gentry(&pack, group.first, &entry), "gallery entry");
    CHECK(stella_image_show_cg(&img, &pack, entry.asset), "show cg");

    // Corrupt a real JPEG stream in a private copy; SOI stays valid, decoder must fail.
    uint8_t *bad = malloc(size);
    CHECK(bad != NULL, "alloc bad");
    if (bad) {
        memcpy(bad, blob, size);
        stella_pack_t bad_pack;
        CHECK(stella_pack_open(&bad_pack, bad, (uint32_t)size), "bad copy open");
        stella_asset_t ba;
        CHECK(stella_pack_asset(&bad_pack, bg, &ba), "bad asset");
        const uint32_t jpeg_off = (uint32_t)(ba.jpeg - bad);
        bad[jpeg_off + 20] ^= 0xFFu;
        CHECK(!stella_image_validate_asset(&bad_pack, bg), "corrupt JPEG must fail validation");
        CHECK(!stella_image_show_scene(&img, &bad_pack, bg, NULL, 0),
              "corrupt JPEG must fail show");
        free(bad);
    }

    // Truncated mask metadata must be rejected before any mask stride read.
    uint8_t *bad_mask = malloc(size);
    CHECK(bad_mask != NULL, "alloc bad mask");
    if (bad_mask) {
        memcpy(bad_mask, blob, size);
        const uint32_t asset_off = section_offset(bad_mask, SEC_ASSET);
        stella_asset_t sa;
        uint16_t sprite_index = STELLA_NONE;
        for (uint16_t i = 0; i < pack.asset_count; ++i) {
            if (stella_pack_asset(&pack, i, &sa) && sa.kind == STELLA_ASSET_SPRITE) {
                sprite_index = i;
                break;
            }
        }
        CHECK(sprite_index != STELLA_NONE, "sprite index");
        if (sprite_index != STELLA_NONE) {
            const uint32_t stride = ((uint32_t)sa.w + 7u) / 8u;
            const uint32_t short_len = stride * sa.h - 1u;
            uint8_t *mp = bad_mask + asset_off + (size_t)sprite_index * STELLA_ASSET_BYTES;
            mp[16] = (uint8_t)(short_len & 0xFFu);
            mp[17] = (uint8_t)((short_len >> 8) & 0xFFu);
            mp[18] = (uint8_t)((short_len >> 16) & 0xFFu);
            mp[19] = (uint8_t)((short_len >> 24) & 0xFFu);
            stella_pack_t bm;
            CHECK(!stella_pack_open(&bm, bad_mask, (uint32_t)size), "short mask pack rejected");
        }
        free(bad_mask);
    }
    (void)section_offset; (void)SEC_BLOB;

    free(blob);
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("PASS: real JPEG/mask/scaling/geometry and corrupt-pack rejection\n");
    return 0;
}
