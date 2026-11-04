// tests/test_steins0_image.c —— 真实 TJpgDec + 遮罩/裁剪/坏图回归(无设备)。
#include "lvgl.h"
#include "steins0_image.h"
#include "steins0_pack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef STEINS0_PACK_PATH
#define STEINS0_PACK_PATH "assets/steins0/steins0_pack.bin"
#endif
#ifndef STEINS0_IMAGE_REF
#define STEINS0_IMAGE_REF "tests/fixtures/steins0_image_ref.txt"
#endif

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); ++failures; } } while (0)

static uint16_t s_pixels[SG0_SCREEN_W * SG0_SCREEN_H];
static uint8_t *s_blob;
static uint32_t s_blob_size;

static int asset_by_name(const sg0_pack_t *pack, const char *name)
{
    for (uint32_t i = 0; i < pack->sec[SG0_SEC_ASSETID].count; ++i) {
        sg0_assetid_t asset;
        if (!sg0_pack_asset(pack, (uint16_t)i, &asset)) continue;
        const char *text = NULL;
        uint16_t len = 0;
        if (!sg0_pack_string(pack, asset.name, &text, &len)) continue;
        if (strlen(name) == len && memcmp(text, name, len) == 0) return (int)i;
    }
    return -1;
}

static void patch_u16(uint8_t *data, uint32_t off, uint16_t value)
{
    data[off] = (uint8_t)(value & 0xFFu);
    data[off + 1] = (uint8_t)(value >> 8);
}

static uint32_t pixels_checksum(void)
{
    uint32_t sum = 2166136261u;
    for (int i = 0; i < SG0_SCREEN_W * SG0_SCREEN_H; ++i) {
        sum = (sum ^ s_pixels[i]) * 16777619u;
    }
    return sum;
}

static int channel_delta(uint16_t a, uint16_t b, int channel)
{
    int va = 0, vb = 0;
    if (channel == 0) { va = (a >> 11) & 0x1F; vb = (b >> 11) & 0x1F; }
    else if (channel == 1) { va = (a >> 5) & 0x3F; vb = (b >> 5) & 0x3F; }
    else { va = a & 0x1F; vb = b & 0x1F; }
    return va > vb ? va - vb : vb - va;
}

static uint16_t u16_at(const uint8_t *data, uint32_t off)
{
    return (uint16_t)(data[off] | ((uint16_t)data[off + 1] << 8));
}

static void test_reference(sg0_pack_t *pack, const steins0_image_t *image)
{
    FILE *fh = fopen(STEINS0_IMAGE_REF, "r");
    CHECK(fh != NULL, "open fixture %s", STEINS0_IMAGE_REF);
    if (!fh) return;
    char line[2048];
    int current = -1;
    int dw = 0, dh = 0, dx = 0, dy = 0, pw = 0, ph = 0;
    int checked = 0;
    while (fgets(line, sizeof(line), fh)) {
        if (line[0] == '#') continue;
        if (strncmp(line, "asset ", 6) == 0) {
            char name[64];
            int w = 0, h = 0;
            if (sscanf(line + 6, "%63s %d %d", name, &w, &h) != 3) continue;
            current = asset_by_name(pack, name);
            CHECK(current >= 0, "fixture asset %s", name);
            if (current < 0) continue;
            sg0_assetid_t asset;
            CHECK(sg0_pack_asset(pack, (uint16_t)current, &asset), "asset %s", name);
            sg0_part_t part;
            CHECK(sg0_pack_part(pack, asset.part_first, &part), "part %s", name);
            CHECK(part.w == w && part.h == h, "%s dims %ux%u", name, part.w, part.h);
            CHECK(steins0_image_show_cg((steins0_image_t *)image, pack, (uint16_t)current,
                                        true), "show_cg %s", name);
            pw = part.w;
            ph = part.h;
            dw = SG0_IMG_W;
            dh = SG0_IMG_H;
            if ((int64_t)part.ow * dh > (int64_t)part.oh * dw) {
                dh = (int)((int64_t)part.oh * dw / part.ow);
            } else {
                dw = (int)((int64_t)part.ow * dh / part.oh);
            }
            dx = (SG0_SCREEN_W - dw) / 2;
            dy = (SG0_SCREEN_H - dh) / 2;
            continue;
        }
        if (strncmp(line, "sample ", 7) == 0 && current >= 0) {
            char *cursor = line + 7;
            for (int k = 0; k < 8; ++k) {
                int x = 0, y = 0, want = 0, used = 0;
                if (sscanf(cursor, "%d %d %d%n", &x, &y, &want, &used) != 3) break;
                cursor += used;
                const int cx = dx + x * dw / pw;
                const int cy = dy + y * dh / ph;
                CHECK(cx >= 0 && cx < SG0_SCREEN_W && cy >= 0 && cy < SG0_SCREEN_H,
                      "sample inside canvas");
                const uint16_t got = image->pixels[(size_t)cy * SG0_SCREEN_W + cx];
                const int dr = channel_delta(want, got, 0);
                const int dg = channel_delta(want, got, 1);
                const int db = channel_delta(want, got, 2);
                CHECK(dr <= 6 && dg <= 10 && db <= 6,
                      "asset sample (%d,%d) delta %d/%d/%d got %04x want %04x",
                      x, y, dr, dg, db, got, want);
                ++checked;
            }
        }
    }
    fclose(fh);
    CHECK(checked >= 400, "reference samples %d", checked);
    printf("image reference: %d sampled pixels\n", checked);
}

int main(void)
{
    FILE *fh = fopen(STEINS0_PACK_PATH, "rb");
    CHECK(fh != NULL, "open pack");
    if (!fh) return 1;
    fseek(fh, 0, SEEK_END);
    long size = ftell(fh);
    fseek(fh, 0, SEEK_SET);
    s_blob = malloc((size_t)size);
    CHECK(fread(s_blob, 1, (size_t)size, fh) == (size_t)size, "read pack");
    fclose(fh);
    s_blob_size = (uint32_t)size;
    sg0_pack_t pack;
    CHECK(sg0_pack_open(&pack, s_blob, s_blob_size), "open pack");
    steins0_image_t image;
    CHECK(steins0_image_init(&image, lv_screen_active(), s_pixels), "image init");

    test_reference(&pack, &image);

    // 全量资源解码:两个部分/遮罩/尺寸都要真解码。
    uint32_t assets = 0;
    uint32_t failed = 0;
    for (uint32_t i = 0; i < pack.sec[SG0_SEC_ASSETID].count; ++i) {
        if (!steins0_image_validate_asset(&pack, (uint16_t)i)) ++failed;
        ++assets;
    }
    CHECK(failed == 0, "asset decode failures %u/%u", failed, assets);
    printf("image assets decoded: %u/%u\n", assets - failed, assets);

    // 全量 view 几何/图层合法 + 画布外的行不被写入。
    uint32_t views = 0;
    uint32_t view_failed = 0;
    for (uint32_t i = 0; i < pack.sec[SG0_SEC_VIEW].count; ++i) {
        if (!steins0_image_validate_view(&pack, (uint16_t)i)) {
            ++view_failed;
            fprintf(stderr, "failed view %u\n", i);
        }
        ++views;
    }
    CHECK(view_failed == 0, "view decode failures %u/%u", view_failed, views);
    printf("image views validated: %u/%u\n", views - view_failed, views);
    for (uint32_t i = 0; i < 80 && i < pack.sec[SG0_SEC_VIEW].count; ++i) {
        CHECK(steins0_image_show_scene(&image, &pack, (uint16_t)i, true), "show view %u", i);
        for (int y = SG0_IMG_H; y < SG0_SCREEN_H; ++y) {
            for (int x = 0; x < SG0_SCREEN_W; ++x) {
                if (s_pixels[(size_t)y * SG0_SCREEN_W + x] != 0x10A2u) {
                    CHECK(0, "layer wrote below image area view %u y %d", i, y);
                    y = SG0_SCREEN_H;
                    break;
                }
            }
        }
    }

    // 坏图:损坏 JPEG -> 失败;错误尺寸/遮罩 -> 失败;坏 view -> 不破坏当前画布。
    {
        uint8_t *copy = malloc(s_blob_size);
        memcpy(copy, s_blob, s_blob_size);
        sg0_pack_t bad;
        CHECK(sg0_pack_open(&bad, copy, s_blob_size), "bad pack open");
        const int asset_index = asset_by_name(&bad, "gal369");
        CHECK(asset_index >= 0, "gal369 asset");
        sg0_assetid_t asset;
        CHECK(sg0_pack_asset(&bad, (uint16_t)asset_index, &asset), "asset");
        sg0_part_t part;
        CHECK(sg0_pack_part(&bad, asset.part_first, &part), "part");
        const uint32_t part_off = bad.sec[SG0_SEC_PART].offset + (uint32_t)asset.part_first * 30u;
        const uint32_t jpeg_off = bad.sec[SG0_SEC_BLOB].offset + part.off;
        copy[jpeg_off] = 0x00;
        copy[jpeg_off + 1] = 0x00;
        CHECK(!steins0_image_validate_asset(&bad, (uint16_t)asset_index), "corrupt JPEG rejected");
        memcpy(copy, s_blob, s_blob_size);
        patch_u16(copy, part_off + 16, (uint16_t)(part.w + 1));
        CHECK(!steins0_image_validate_asset(&bad, (uint16_t)asset_index), "wrong width rejected");
        memcpy(copy, s_blob, s_blob_size);
        patch_u16(copy, part_off + 18, (uint16_t)(part.h - 1));
        CHECK(!steins0_image_validate_asset(&bad, (uint16_t)asset_index), "wrong height rejected");
        memcpy(copy, s_blob, s_blob_size);
        {
            const int sprite = asset_by_name(&bad, "p0_1");
            CHECK(sprite >= 0, "sprite asset");
            sg0_assetid_t sa;
            CHECK(sg0_pack_asset(&bad, (uint16_t)sprite, &sa), "sprite asset load");
            const uint32_t sp_off = bad.sec[SG0_SEC_PART].offset + (uint32_t)sa.part_first * 30u;
            patch_u16(copy, sp_off + 12, 1);
            CHECK(!steins0_image_validate_asset(&bad, (uint16_t)sprite), "short mask rejected");
        }
        memcpy(copy, s_blob, s_blob_size);
        {
            sg0_view_t view;
            CHECK(sg0_pack_view(&bad, 0, &view), "view0");
            const uint32_t ref = u16_at(copy, bad.sec[SG0_SEC_VIEW].offset);
            const uint32_t layer_index = u16_at(copy, bad.sec[SG0_SEC_LAYERREF].offset + ref * 2u);
            patch_u16(copy, bad.sec[SG0_SEC_LAYER].offset + layer_index * 12u, 60000);
            const uint32_t before = pixels_checksum();
            CHECK(!steins0_image_show_scene(&image, &bad, 0, true), "bad asset view rejected");
            CHECK(pixels_checksum() == before, "canvas preserved on failed view");
        }
        free(copy);
    }

    // 21 个真实贴纸:96x96 目标 + 前后 guard words。旧代码按 320 stride
    // 写入会在 96 行后越过贴纸 buffer;这里逐张检查 canary 和边界。
    {
        uint16_t stamps[32];
        int stamp_count = 0;
        for (uint32_t i = 0; i < pack.sec[SG0_SEC_RMSG].count; ++i) {
            sg0_rine_msg_t msg;
            if (!sg0_pack_rine_msg(&pack, (uint16_t)i, &msg)) continue;
            if (msg.stamp == 0xFFFFu) continue;
            bool seen = false;
            for (int k = 0; k < stamp_count; ++k) {
                if (stamps[k] == msg.stamp) { seen = true; break; }
            }
            if (!seen && stamp_count < (int)(sizeof(stamps) / sizeof(stamps[0]))) {
                stamps[stamp_count++] = msg.stamp;
            }
        }
        CHECK(stamp_count == 21, "real stamp assets %d", stamp_count);
        bool has_1753 = false;
        for (int s = 0; s < stamp_count; ++s) if (stamps[s] == 1753) has_1753 = true;
        CHECK(has_1753, "asset 1753 included in stamp guard");
        const uint16_t canary = 0xA55Au;
        uint16_t guarded[96 * 96 + 32];
        for (uint32_t i = 0; i < sizeof(guarded) / sizeof(guarded[0]); ++i) guarded[i] = canary;
        steins0_image_t stamp_image;
        uint16_t *stamp_pixels = guarded + 16;
        CHECK(steins0_image_init_ex(&stamp_image, lv_screen_active(), stamp_pixels, 96, 96),
              "stamp image init");
        for (uint32_t i = 0; i < sizeof(guarded) / sizeof(guarded[0]); ++i) guarded[i] = canary;
        steins0_image_clear(&stamp_image);
        for (int i = 0; i < 16; ++i) {
            CHECK(guarded[i] == canary, "stamp clear low guard %d", i);
            CHECK(guarded[16 + 96 * 96 + i] == canary, "stamp clear high guard %d", i);
        }
        for (int s = 0; s < stamp_count; ++s) {
            for (uint32_t i = 0; i < sizeof(guarded) / sizeof(guarded[0]); ++i) guarded[i] = canary;
            uint32_t nonzero = 0, zero = 0;
            CHECK(steins0_image_show_stamp(&stamp_image, &pack, stamps[s], true),
                  "stamp %d show", s);
            for (int i = 0; i < 96 * 96; ++i) {
                if (stamp_pixels[i] == 0) ++zero;
                else if (stamp_pixels[i] != canary) ++nonzero;
            }
            CHECK(zero > 0 && nonzero > 0, "stamp %d RGB/transparent pixels %u/%u", s, zero,
                  nonzero);
            for (int i = 0; i < 16; ++i) {
                CHECK(guarded[i] == canary, "stamp %d low guard %d", s, i);
                CHECK(guarded[16 + 96 * 96 + i] == canary, "stamp %d high guard %d",
                      s, i);
            }
        }

        printf("stamp guard: %d assets, canaries and RGB/transparency OK ", stamp_count);
        // Same asset, corrupt JPEG: preflight fails and the 96x96 target is untouched.
        uint8_t *copy = malloc(s_blob_size);
        CHECK(copy != NULL, "stamp bad copy");
        if (copy) {
            memcpy(copy, s_blob, s_blob_size);
            sg0_pack_t bad;
            CHECK(sg0_pack_open(&bad, copy, s_blob_size), "stamp bad pack open");
            sg0_assetid_t asset;
            CHECK(sg0_pack_asset(&bad, stamps[0], &asset), "stamp bad asset");
            sg0_part_t part;
            CHECK(sg0_pack_part(&bad, asset.part_first, &part), "stamp bad part");
            const uint32_t jpeg_off = bad.sec[SG0_SEC_BLOB].offset + part.off;
            copy[jpeg_off] = 0;
            copy[jpeg_off + 1] = 0;
            for (uint32_t i = 0; i < sizeof(guarded) / sizeof(guarded[0]); ++i) guarded[i] = canary;
            for (int i = 0; i < 96 * 96; ++i) stamp_pixels[i] = (uint16_t)(0x1200u + (i & 0xFFu));
            uint32_t before = 2166136261u;
            for (int i = 0; i < 96 * 96; ++i) before = (before ^ stamp_pixels[i]) * 16777619u;
            CHECK(!steins0_image_show_stamp(&stamp_image, &bad, stamps[0], true),
                  "corrupt stamp rejected");
            uint32_t after = 2166136261u;
            for (int i = 0; i < 96 * 96; ++i) after = (after ^ stamp_pixels[i]) * 16777619u;
            CHECK(after == before, "corrupt stamp leaves target surface unchanged");
            for (int i = 0; i < 16; ++i) {
                CHECK(guarded[i] == canary, "bad stamp low guard %d", i);
                CHECK(guarded[16 + 96 * 96 + i] == canary, "bad stamp high guard %d", i);
            }
            CHECK(steins0_image_show_stamp(&stamp_image, &pack, stamps[0], true),
                  "same stamp id retries after bad copy");
            (void)before;
            free(copy);
        }
    }

    // 几何:432x243 源视口 -> 320x180,等比缩放。
    {
        sg0_layer_t layer = {.x = 0, .y = 0, .w = SG0_VIEW_W, .h = SG0_VIEW_H};
        int x, y, w, h;
        steins0_image_layer_box(&layer, &x, &y, &w, &h);
        CHECK(x == 0 && y == 0 && w == 320 && h == 180, "image area scaling %d,%d %dx%d", x, y,
              w, h);
    }

    free(s_blob);
    if (failures) {
        fprintf(stderr, "FAIL %d checks\n", failures);
        return 1;
    }
    printf("PASS real TJpgDec assets/views/masks/corruption/geometry\n");
    return 0;
}
