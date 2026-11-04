// tests/test_summer_image.c —— 真实 TJpgDec + 生产 summer_image.c 的 JPEG/遮罩回归。
//
// 不依赖 LVGL：用 summer_image_bind() 绑定像素缓冲。覆盖场景/立绘/CG 解码、
// 透明遮罩、坏 JPEG、截断遮罩、声明尺寸不符，以及越界图片 id。
#include "summer_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SUMMER_PACK_PATH
#define SUMMER_PACK_PATH "main/summer_data/summer_pack.bin"
#endif

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        failures++; \
    } \
} while (0)

static uint8_t *read_all(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) {
        fclose(f);
        return NULL;
    }
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

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint32_t section_offset(const uint8_t *blob, int type)
{
    const uint32_t count = rd16(blob + 12);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *s = blob + 20 + i * 16;
        if (rd16(s) == (uint32_t)type) return rd32(s + 4);
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : SUMMER_PACK_PATH;
    size_t size = 0;
    uint8_t *blob = read_all(path, &size);
    if (!blob) {
        fprintf(stderr, "SKIP: cannot read %s\n", path);
        return 77;
    }
    static uint16_t pixels[SUMMER_ART_W * SUMMER_ART_H];
    summer_canvas_t canvas;
    CHECK(summer_image_bind(&canvas, pixels), "bind");
    summer_pack_t pack;
    const char *err = NULL;
    CHECK(summer_pack_open(&pack, blob, (uint32_t)size, &err), "pack: %s", err ? err : "?");

    uint16_t scene_id = 1;
    uint16_t portrait_id = 0;
    for (uint16_t id = 10001; id <= 16045; ++id) {
        const summer_image_t *img = summer_image(&pack, id);
        if (img && img->kind == SUMMER_IMG_PORTRAIT) {
            portrait_id = id;
            break;
        }
    }
    CHECK(portrait_id != 0, "no portrait id");
    printf("scene=%u portrait=%u\n", scene_id, portrait_id);
    CHECK(summer_image_probe(&pack, scene_id), "probe scene");
    CHECK(summer_image_probe(&pack, portrait_id), "probe portrait");
    CHECK(summer_image_probe(&pack, 1001), "probe cg");

    CHECK(summer_image_scene(&canvas, &pack, scene_id, portrait_id), "scene decode");
    int nonzero = 0;
    for (int i = 0; i < SUMMER_ART_W * SUMMER_ART_H; ++i) if (pixels[i]) nonzero++;
    CHECK(nonzero > 30000, "scene pixels %d", nonzero);
    CHECK(summer_image_cg(&canvas, &pack, 1001), "cg decode");
    nonzero = 0;
    for (int i = 0; i < SUMMER_ART_W * SUMMER_ART_H; ++i) if (pixels[i]) nonzero++;
    CHECK(nonzero > 30000, "cg pixels %d", nonzero);

    // Mask transparency: overlay a portrait/logo on a known background; the
    // image's transparent margin must leave the background untouched.
    memset(pixels, 0x34, sizeof(pixels));
    uint16_t logo_index = pack.meta.logo_image;
    CHECK(logo_index < pack.image_count, "logo index");
    if (logo_index < pack.image_count) {
        CHECK(summer_image_logo(&canvas, &pack, logo_index, 8, 8, 200, 58), "logo decode");
        int kept = 0, changed = 0;
        for (int i = 0; i < SUMMER_ART_W * SUMMER_ART_H; ++i) {
            if (pixels[i] == 0x3434) kept++;
            else changed++;
        }
        CHECK(kept > 10000, "logo kept %d", kept);
        CHECK(changed > 1000, "logo changed %d", changed);
        int mask_w = 0, mask_h = 0;
        uint8_t mask[4096];
        CHECK(summer_mask_expand(&pack, logo_index, mask, sizeof(mask), &mask_w, &mask_h),
              "logo mask");
        CHECK(mask_w > 0 && mask_h > 0, "logo mask size");
    }

    // Bad JPEG: flip the SOI byte in a writable copy.
    uint8_t *copy = malloc(size);
    CHECK(copy != NULL, "copy alloc");
    if (copy) {
        memcpy(copy, blob, size);
        summer_pack_t bad_pack;
        CHECK(summer_pack_open(&bad_pack, copy, (uint32_t)size, &err), "copy pack");
        const summer_image_t *img = summer_image(&bad_pack, scene_id);
        if (img) {
            const uint32_t data_off = section_offset(copy, 23);
            copy[data_off + img->off] ^= 0xFF;
            CHECK(!summer_image_probe(&bad_pack, scene_id), "bad jpeg accepted");
            CHECK(!summer_image_cg(&canvas, &bad_pack, scene_id), "bad jpeg decoded");
        }
        // Wrong declared size: the JPEG header still says the original size.
        memcpy(copy, blob, size);
        CHECK(summer_pack_open(&bad_pack, copy, (uint32_t)size, &err), "copy pack 2");
        const summer_image_t *scene = summer_image(&bad_pack, scene_id);
        if (scene) {
            uint8_t *records = copy + section_offset(copy, 8);
            const uint32_t index = (uint32_t)(scene - bad_pack.images);
            uint8_t *rec = records + index * 24;
            const uint16_t w = rd16(rec + 16) + 1;
            rec[16] = (uint8_t)(w & 0xFF);
            rec[17] = (uint8_t)(w >> 8);
            CHECK(!summer_image_probe(&bad_pack, scene_id), "wrong size accepted");
        }
        // Truncated mask: shrink mask_len of a portrait record.
        memcpy(copy, blob, size);
        CHECK(summer_pack_open(&bad_pack, copy, (uint32_t)size, &err), "copy pack 3");
        const summer_image_t *portrait = summer_image(&bad_pack, portrait_id);
        if (portrait) {
            uint8_t *records = copy + section_offset(copy, 8);
            const uint32_t index = (uint32_t)(portrait - bad_pack.images);
            uint8_t *rec = records + index * 24;
            rec[12] = (uint8_t)(size & 0xFF);
            rec[13] = (uint8_t)((size >> 8) & 0xFF);
            rec[14] = (uint8_t)((size >> 16) & 0xFF);
            rec[15] = (uint8_t)((size >> 24) & 0xFF);
            // mask_len = 1 byte; the RLE cannot fill the portrait.
            rec[12] = 1;
            rec[13] = 0;
            rec[14] = 0;
            rec[15] = 0;
            CHECK(!summer_image_probe(&bad_pack, portrait_id), "truncated mask accepted");
            uint8_t mask[4096];
            int mw = 0, mh = 0;
            CHECK(!summer_mask_expand(&bad_pack, index, mask, sizeof(mask), &mw, &mh),
                  "truncated mask expanded");
        }
        free(copy);
    }

    // A truncated portrait must fail *mid-stream*; the frame must roll back to
    // the complete background, then a restored stream must succeed on retry.
    {
        static uint16_t reference[SUMMER_ART_W * SUMMER_ART_H];
        static uint16_t blank_reference[SUMMER_ART_W * SUMMER_ART_H];
        summer_canvas_t ref_canvas;
        CHECK(summer_image_bind(&ref_canvas, reference), "ref bind");
        CHECK(summer_image_scene(&ref_canvas, &pack, scene_id, 0), "ref background");
        memset(blank_reference, 0, sizeof(blank_reference));

        uint8_t *bad = malloc(size);
        CHECK(bad != NULL, "rollback copy alloc");
        if (bad) {
            memcpy(bad, blob, size);
            summer_pack_t bad_pack;
            CHECK(summer_pack_open(&bad_pack, bad, (uint32_t)size, &err), "rollback pack");
            const summer_image_t *portrait = summer_image(&bad_pack, portrait_id);
            CHECK(portrait != NULL, "rollback portrait");
            if (portrait) {
                const uint32_t index = (uint32_t)(portrait - bad_pack.images);
                uint8_t *records = bad + section_offset(bad, 8);
                uint8_t *rec = records + index * 24;
                const uint32_t original = rd32(rec + 4);
                CHECK(original > 64, "portrait len");
                const uint32_t cut = original / 2;
                rec[4] = (uint8_t)(cut & 0xFF);
                rec[5] = (uint8_t)((cut >> 8) & 0xFF);
                rec[6] = (uint8_t)((cut >> 16) & 0xFF);
                rec[7] = (uint8_t)((cut >> 24) & 0xFF);
                const bool ok = summer_image_scene(&canvas, &bad_pack, scene_id, portrait_id);
                CHECK(!ok, "truncated portrait reported success");
                CHECK(memcmp(pixels, reference, sizeof(reference)) == 0,
                      "failure frame is not the complete background");
                // Background 0: a failed body must leave a defined black frame.
                const bool ok_black = summer_image_scene(&canvas, &bad_pack, 0, portrait_id);
                CHECK(!ok_black, "truncated portrait on black reported success");
                CHECK(memcmp(pixels, blank_reference, sizeof(blank_reference)) == 0,
                      "failure frame on black is not clear");
                // Restore the stream and retry the same scene.
                rec[4] = (uint8_t)(original & 0xFF);
                rec[5] = (uint8_t)((original >> 8) & 0xFF);
                rec[6] = (uint8_t)((original >> 16) & 0xFF);
                rec[7] = (uint8_t)((original >> 24) & 0xFF);
                CHECK(summer_image_scene(&canvas, &bad_pack, scene_id, portrait_id),
                      "restored portrait retry");
                int diff = 0;
                for (int i = 0; i < SUMMER_ART_W * SUMMER_ART_H; ++i) {
                    if (pixels[i] != reference[i]) diff++;
                }
                CHECK(diff > 1000, "restored portrait did not change the background (%d)", diff);
            }
            free(bad);
        }
    }

    // Out-of-range ids must not crash or be reported as valid.
    CHECK(!summer_image_probe(&pack, 0x7FFF), "out-of-range id valid");
    CHECK(!summer_image_cg(&canvas, &pack, 0x7FFF), "out-of-range cg");

    free(blob);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_summer_image: PASS\n");
    return 0;
}
