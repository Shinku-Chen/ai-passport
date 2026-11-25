// Image compositor contracts: bounded allocations, CRC/decoder rejection, a
// failed decode clears stale art, and the flash-backed raw asset accessors.
#include "nine_format.h"
#include "nine_image.h"
#include "nine_pack.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

static uint8_t *load(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) exit(2);
    fclose(f); *size = (size_t)n; return b;
}
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
static uint32_t crc32_bytes(const uint8_t *p, size_t n)
{
    uint32_t c = ~0u;
    while (n--) {
        c ^= *p++;
        for (unsigned b = 0; b < 8; ++b) c = (c >> 1) ^ ((0u - (c & 1u)) & 0xedb88320u);
    }
    return ~c;
}
static uint32_t crc32_zero(const uint8_t *p, size_t n, size_t zoff, size_t zn)
{
    uint32_t c = ~0u;
    for (size_t i = 0; i < n; ++i) {
        uint8_t b = i >= zoff && i - zoff < zn ? 0 : p[i];
        c ^= b;
        for (unsigned j = 0; j < 8; ++j) c = (c >> 1) ^ ((0u - (c & 1u)) & 0xedb88320u);
    }
    return ~c;
}
static bool canvas_black(void)
{
    for (unsigned y = 0; y < NINE_IMAGE_H; y += 5)
        for (unsigned x = 0; x < NINE_IMAGE_W; x += 5)
            if (nine_image_test_pixel(x, y) != 0) return false;
    return true;
}

int main(int argc, char **argv)
{
    if (argc < 3) { printf("usage: %s script images\n", argv[0]); return 2; }
    size_t sn, imn;
    uint8_t *script = load(argv[1], &sn);
    uint8_t *images = load(argv[2], &imn);
    static nine_pack_t pack;
    CHECK(nine_pack_open(&pack, script, sn), "pack");

    CHECK(nine_image_init(NULL), "init");
    CHECK(nine_image_test_largest_allocation() <= 18432, "allocation <= 18432");
    CHECK(nine_image_bind_bundle(script, sn, images, imn), "bind");
    CHECK(nine_image_show_visual(0), "show visual 0");
    CHECK(nine_image_band_count() == 4, "four bands");
    CHECK(!canvas_black(), "visual has pixels");
    CHECK(nine_image_asset_count() > 600, "asset count");

    uint32_t tw = 0, th = 0;
    const uint16_t *tp = NULL;
    uint32_t title = NINE_NONE;
    CHECK(nine_image_title(0, &title) && title != NINE_NONE, "title id");
    CHECK(nine_image_raw_asset(title, &tw, &th, &tp) && tw == 320 && th == 240 &&
          tp != NULL, "title raw 320x240");

    /* Bounded allocation failure returns false and leaves no partial canvas. */
    nine_image_deinit();
    nine_image_test_fail_allocation(3);
    CHECK(!nine_image_init(NULL), "third band allocation failure");
    CHECK(nine_image_init(NULL), "retry init");
    CHECK(nine_image_bind_bundle(script, sn, images, imn), "rebind");

    /* A corrupt payload CRC keeps the previous binding untouched. */
    uint8_t *bad = malloc(imn);
    CHECK(bad != NULL, "bad copy");
    memcpy(bad, images, imn);
    uint32_t ioff = rd32(bad + 24);
    uint32_t off = rd32(bad + ioff + 12);
    uint32_t len = rd32(bad + ioff + 16);
    bad[off + 2] ^= 0xff;
    CHECK(!nine_image_bind_bundle(script, sn, bad, imn), "bad CRC rejected at bind");
    CHECK(nine_image_show_visual(0), "previous binding still works");

    /* A valid-CRC but undecodable JPEG is rejected at display time, and the
       canvas is cleared instead of leaving the old frame visible. */
    CHECK(nine_image_show_visual(0), "show another visual first");
    CHECK(!canvas_black(), "pixels before corruption");
    bad[off] = 0x00;   // break SOI
    wr32(bad + ioff + 20, crc32_bytes(bad + off, len));
    wr32(bad + 16, crc32_zero(bad, imn, 16, 4));
    CHECK(nine_image_bind_bundle(script, sn, bad, imn), "valid CRC bind");
    CHECK(!nine_image_show_visual(0), "broken JPEG rejected");
    CHECK(canvas_black(), "failed decode clears stale art");

    printf("fails=%d\n", fails);
    nine_image_deinit();
    free(script); free(images); free(bad);
    return fails ? 1 : 0;
}
