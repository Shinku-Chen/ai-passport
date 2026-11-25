// Real image decode/composite acceptance: every asset in the pack must decode,
// the production band compositor must match a source-art reference within the
// JPEG38/palette tolerance, and a corrupt JPEG must fail without leaving stale
// pixels or a half-updated frame.
#include "nine_image.h"
#include "nine_pack.h"
#include "nine_assets.h"
#include "nine_assets_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xffffffffu;
    while (n--) {
        c ^= *p++;
        for (unsigned b = 0; b < 8; ++b) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static int failures;
#define CHECK(c, m) do { if (!(c)) { failures++; printf("FAIL: %s\n", m); } } while (0)

static void rgb565(uint16_t px, int *r, int *g, int *b)
{
    *r = ((px >> 11) & 31) * 255 / 31;
    *g = ((px >> 5) & 63) * 255 / 63;
    *b = (px & 31) * 255 / 31;
}

int main(void)
{
    CHECK(nine_assets_host_load("assets/nine/yukiiro/nine_script.bin",
                                "assets/nine/yukiiro/nine_images.bin"),
          "assets loaded");
    size_t script_size = 0, image_size = 0;
    const uint8_t *script = nine_assets_script(&script_size);
    const uint8_t *images = nine_assets_images(&image_size);
    nine_pack_t pack;
    CHECK(nine_pack_open(&pack, script, script_size), "script pack opens");

    CHECK(nine_image_init(NULL), "band compositor init");
    CHECK(nine_image_bind_bundle(script, script_size, images, image_size),
          "bundle binds");
    CHECK(nine_image_asset_count() == 1167, "1167 image assets (726 story/CG + 441 OP)");
    CHECK(nine_image_canvas_bytes() == 115200, "five 320x36 bands, no full canvas");
    CHECK(nine_image_test_largest_allocation() <= 32768,
          "largest allocation <= 32 KiB");
    CHECK(nine_image_scratch_bytes() <= 16384, "scratch <= 16 KiB");

    unsigned bad_assets = 0;
    for (uint32_t i = 0; i < nine_image_asset_count(); ++i)
        if (!nine_image_validate_asset(i)) ++bad_assets;
    CHECK(bad_assets == 0, "every asset decodes (JPEG and indexed blocks)");

    CHECK(nine_image_show_visual(30), "reference visual composes");
    FILE *ref = fopen("tests/nine/ref_visual30.txt", "r");
    CHECK(ref != NULL, "reference sheet available");
    int samples = 0, total = 0, maximum = 0, over32 = 0;
    if (ref) {
        int x, y, r, g, b;
        while (fscanf(ref, "%d %d %d %d %d", &x, &y, &r, &g, &b) == 5) {
            int ar, ag, ab;
            rgb565(nine_image_test_pixel((unsigned)x, (unsigned)y), &ar, &ag, &ab);
            int d = abs(ar - r) + abs(ag - g) + abs(ab - b);
            total += d;
            if (d > maximum) maximum = d;
            if (d > 96) {
                ++over32;
                if (over32 <= 12)
                    printf("outlier %d,%d ref=%d,%d,%d got=%d,%d,%d d=%d\n",
                           x, y, r, g, b, ar, ag, ab, d);
            }
            ++samples;
        }
        fclose(ref);
    }
    CHECK(samples >= 250, "reference samples parsed");
    const int mean = samples ? total / samples : 999;
    printf("image reference: samples=%d mean_abs_sum=%d max_abs_sum=%d outliers=%d\n",
           samples, mean, maximum, over32);
    CHECK(mean <= 24, "mean per-pixel error within JPEG/palette tolerance");
    CHECK(maximum <= 150, "no catastrophic reference mismatch");
    CHECK(over32 <= samples / 20, "fewer than 5% large deviations");

    // CRC corruption must be rejected before any pixels are touched.
    uint8_t *bad = (uint8_t *)malloc(image_size);
    CHECK(bad != NULL, "bad image copy");
    if (bad) {
        memcpy(bad, images, image_size);
        const uint32_t table = rd32(bad + 24);
        const uint32_t asset = 5;    // n23/10 background
        const uint32_t off = rd32(bad + table + asset * 32 + 12);
        const uint32_t len = rd32(bad + table + asset * 32 + 16);
        CHECK(len > 8, "asset 5 has payload");
        bad[off + 1] ^= 0x40;
        CHECK(!nine_image_bind_bundle(script, script_size, bad, image_size),
              "payload CRC mismatch rejected at bind");
        // Recompute the record CRC so bind succeeds; the JPEG itself is broken.
        wr32(bad + table + asset * 32 + 20, crc32(bad + off, len));
        wr32(bad + 16, 0);
        wr32(bad + 16, crc32(bad, image_size));
        const bool rebind = nine_image_bind_bundle(script, script_size, bad, image_size);
        printf("rebind=%d crc=%08x stored=%08x\n", (int)rebind,
               crc32(bad + off, len), rd32(bad + table + asset * 32 + 20));
        CHECK(rebind, "re-signed corrupt bundle binds structurally");
        CHECK(nine_image_show_visual(0), "good frame before failure");
        const uint16_t before = nine_image_test_pixel(160, 90);
        CHECK(before != 0, "good frame has pixels");
        CHECK(!nine_image_show_asset(asset), "corrupt JPEG decode fails");
        bool black = true;
        for (unsigned y = 0; y < NINE_IMAGE_H && black; y += 7)
            for (unsigned x = 0; x < NINE_IMAGE_W; x += 11)
                if (nine_image_test_pixel(x, y) != 0) { black = false; break; }
        CHECK(black, "failed frame is cleared, never stale/mixed");

        // The real OP path must clear all five bands before its one strict
        // asset check may fail: bad CRC, valid-CRC/broken JPEG, and a
        // bounds/record failure are all no-half-frame contracts.
        uint32_t op_image = 0;
        CHECK(nine_image_op_frame(0, &op_image, NULL, NULL), "OP frame available");
        const uint32_t op_off = rd32(bad + table + op_image * 32 + 12);
        const uint32_t op_len = rd32(bad + table + op_image * 32 + 16);
        CHECK(op_len > 8, "OP payload available");

        memcpy(bad, images, image_size);
        CHECK(nine_image_bind_bundle(script, script_size, bad, image_size),
              "OP recovery bundle binds");
        nine_image_clear(0x07e0);
        uint32_t nonzero = 0;
        for (unsigned y = 0; y < NINE_IMAGE_H; ++y)
            for (unsigned x = 0; x < NINE_IMAGE_W; ++x)
                if (nine_image_test_pixel(x, y) != 0x07e0) ++nonzero;
        CHECK(nonzero == 0, "OP fill covers all pixels");

        // Bad CRC after binding: record CRC is intentionally stale.
        bad[op_off + 1] ^= 0x40;
        CHECK(!nine_image_show_asset_top(op_image), "OP bad CRC rejected");
        nonzero = 0;
        for (unsigned y = 0; y < NINE_IMAGE_H; ++y)
            for (unsigned x = 0; x < NINE_IMAGE_W; ++x)
                if (nine_image_test_pixel(x, y) != 0) ++nonzero;
        CHECK(nonzero == 0, "OP bad CRC clears all bands before return");

        // Valid record CRC, malformed JPEG: the decode itself must fail and
        // still clear the previous canvas.
        memcpy(bad, images, image_size);
        bad[op_off] = 0;
        wr32(bad + table + op_image * 32 + 20, crc32(bad + op_off, op_len));
        nine_image_clear(0x07e0);
        CHECK(!nine_image_show_asset_top(op_image), "OP malformed JPEG rejected");
        nonzero = 0;
        for (unsigned y = 0; y < NINE_IMAGE_H; ++y)
            for (unsigned x = 0; x < NINE_IMAGE_W; ++x)
                if (nine_image_test_pixel(x, y) != 0) ++nonzero;
        CHECK(nonzero == 0, "OP malformed JPEG clears all bands");

        // A record/range failure occurs in asset_at before a decode.
        memcpy(bad, images, image_size);
        wr32(bad + table + op_image * 32 + 12, image_size + 1u);
        nine_image_clear(0x07e0);
        CHECK(!nine_image_show_asset_top(op_image), "OP bounds failure rejected");
        nonzero = 0;
        for (unsigned y = 0; y < NINE_IMAGE_H; ++y)
            for (unsigned x = 0; x < NINE_IMAGE_W; ++x)
                if (nine_image_test_pixel(x, y) != 0) ++nonzero;
        CHECK(nonzero == 0, "OP bounds failure clears all bands");

        // Recovery: restore the real bytes and bind again.
        memcpy(bad, images, image_size);
        CHECK(nine_image_bind_bundle(script, script_size, bad, image_size),
              "OP recovery rebind");
        CHECK(nine_image_show_asset_top(op_image), "OP recovery decodes");
        nonzero = 0;
        for (unsigned y = 0; y < NINE_IMAGE_H; ++y)
            for (unsigned x = 0; x < NINE_IMAGE_W; ++x)
                if (nine_image_test_pixel(x, y) != 0) ++nonzero;
        CHECK(nonzero > 1000, "OP recovery restores real pixels");
        free(bad);
    }

    nine_image_deinit();
    if (failures) {
        printf("FAILED: %d checks\n", failures);
        return 1;
    }
    printf("PASS: nine image decode/composite\n");
    return 0;
}
