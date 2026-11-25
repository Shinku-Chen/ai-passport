// Production image path regression: NO NINE_IMAGE_HOST. Real LVGL objects, real
// pack, real decode; only the ESP heap allocator is a tracked host shim.
#include "nine_format.h"
#include "nine_image.h"
#include "nine_pack.h"
#include "lvgl.h"
#include "capture.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)
static uint8_t *load(const char *p, size_t *n)
{
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)l); if (!b || fread(b, 1, (size_t)l, f) != (size_t)l) return NULL;
    fclose(f); *n = (size_t)l; return b;
}
int main(int argc, char **argv)
{
    if (argc < 3) { printf("usage: %s script images\n", argv[0]); return 2; }
    size_t sn, imn;
    uint8_t *script = load(argv[1], &sn), *images = load(argv[2], &imn);
    if (!script || !images) return 2;
    lv_display_t *disp = capture_init();
    CHECK(disp != NULL, "real LVGL display");
    static nine_pack_t pack;
    CHECK(nine_pack_open(&pack, script, sn), "pack");
    heap_stub_reset();
    CHECK(!nine_image_init(NULL), "production rejects NULL parent");
    CHECK(nine_image_init(lv_screen_active()), "production init with real screen");
    CHECK(nine_image_band_count() == 4, "four real LVGL image objects");
    CHECK(lv_obj_get_child_count(lv_screen_active()) >= 4, "bands are screen children");
    CHECK(heap_stub_used() == 73728 && heap_stub_largest() == 18432,
          "four 18432-byte bands");
    for (unsigned i = 0; i < 4; ++i) {
        lv_obj_t *obj = lv_obj_get_child(lv_screen_active(), (int32_t)i);
        CHECK(obj && lv_obj_get_y(obj) == (int32_t)(i * 45) &&
              lv_obj_get_width(obj) == 256 && lv_obj_get_height(obj) == 36,
              "band object geometry");
    }
    /* Solid colors prove the 1.25x four-band display covers 0..179 exactly. */
    static const uint16_t colors[4] = {0xf800, 0x07e0, 0x001f, 0xffe0};
    for (unsigned b = 0; b < 4; ++b) {
        const uint16_t *p = NULL; unsigned bw = 0, bh = 0;
        CHECK(nine_image_band_info(b, &p, &bw, &bh) && bw == 256 && bh == 36,
              "band info for fill");
        for (unsigned i = 0; p && i < bw * bh; ++i) ((uint16_t *)p)[i] = colors[b];
    }
    capture_render();
    for (int y = 0; y < 180; ++y)
        CHECK(capture_pixel(160, y) != 0, "no stage gap");
    CHECK(capture_pixel(160, 22) == colors[0], "band 0 at y=22");
    CHECK(capture_pixel(160, 67) == colors[1], "band 1 at y=67");
    CHECK(capture_pixel(160, 112) == colors[2], "band 2 at y=112");
    CHECK(capture_pixel(160, 157) == colors[3], "band 3 at y=157");
    CHECK(capture_pixel(160, 180) != colors[0] && capture_pixel(160, 180) != colors[1] &&
          capture_pixel(160, 180) != colors[2] && capture_pixel(160, 180) != colors[3],
          "stage stops at y=180");
    CHECK(nine_image_bind_bundle(script, sn, images, imn), "bind real pack");
    CHECK(nine_image_show_visual(0), "decode first visual");
    const uint16_t *px = NULL; unsigned w = 0, h = 0;
    CHECK(nine_image_band_info(0, &px, &w, &h) && w == 256 && h == 36, "band info");
    unsigned nonzero = 0;
    for (unsigned i = 0; px && i < w * h; ++i) nonzero += px[i] != 0;
    CHECK(nonzero > 1000, "decoded pixels present");

    /* Every gallery variant and every retained OP frame must decode through
     * the same real compositor/decoder, not just the first visual. */
    unsigned gallery = nine_pack_gallery_count(&pack);
    CHECK(gallery == 73, "73 gallery variants");
    for (uint32_t i = 0; i < gallery; ++i) {
        nine_gallery_record_t g;
        CHECK(nine_pack_gallery(&pack, i, &g), "gallery record");
        bool ok = nine_image_show_asset(g.image);
        if (!ok) printf("FAIL: gallery decode %u\n", (unsigned)i);
        CHECK(ok, "gallery decode");
    }
    uint32_t op_count = nine_image_op_count();
    CHECK(op_count == 300, "300 OP frames");
    for (uint32_t i = 0; i < op_count; ++i) {
        uint32_t image = NINE_NONE, ts = 0, dur = 0;
        CHECK(nine_image_op_frame(i, &image, &ts, &dur), "OP frame record");
        bool ok = image != NINE_NONE && nine_image_show_asset(image);
        if (!ok) printf("FAIL: OP decode %u\n", (unsigned)i);
        CHECK(ok, "OP decode");
    }
    /* A null parent must remain rejected after the successful binding. */
    nine_image_deinit();
    CHECK(heap_stub_used() == 0, "deinit releases all bands");
    CHECK(!nine_image_init(NULL), "null parent rejection persists");
    free(script); free(images);
    printf("fails=%d\n", fails);
    return fails ? 1 : 0;
}
