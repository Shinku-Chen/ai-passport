// tests/test_cafe_image.c —— 真实 JPEG/mask/立绘/表情合成 vs Pillow 参考条带。
#include "cafe_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #cond); return 1; } } while (0)
#define BAND_PIXELS (CAFE_BAND_W * CAFE_BAND_H)

static unsigned char *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f); *size = (size_t)n; return b;
}

static unsigned rd16(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static unsigned rd32(const unsigned char *p)
{ return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24); }

int main(int argc, char **argv)
{
    (void)argc;
    size_t pack_size = 0, ref_size = 0;
    unsigned char *pack = read_file(argv[1], &pack_size);
    unsigned char *ref = read_file(argv[2], &ref_size);
    CHECK(pack && ref && cafe_pack_set_data(pack, pack_size));
    CHECK(ref_size > 8 && memcmp(ref, "CIR1", 4) == 0);
    const unsigned cases = rd32(ref + 4);
    size_t ref_pos = 8;

    // 全图片 CRC：每张图片的真实 JPEG+mask 数据必须匹配包头。
    const unsigned images = cafe_meta_u32(10);
    unsigned bad = 0;
    for (unsigned i = 0; i < images; ++i) if (!cafe_image_verify((uint16_t)i)) bad++;
    CHECK(bad == 0);
    printf("image crc: %u/%u PASS\n", images - bad, images);

    cafe_bands_t bands;
    CHECK(cafe_image_bands_init(&bands));
    for (unsigned c = 0; c < cases; ++c) {
        CHECK(ref_pos + 4 + 3 * BAND_PIXELS * 2 <= ref_size);
        const unsigned nid = rd16(ref + ref_pos);
        const unsigned idx = rd16(ref + ref_pos + 2);
        ref_pos += 4;
        cafe_frame_t frame;
        CHECK(cafe_frame_get((uint16_t)nid, (uint16_t)idx, &frame));
        CHECK(frame.kind == 0);
        cafe_sprite_t sprites[5];
        unsigned sp_count = frame.sprite_count > 5 ? 5 : frame.sprite_count;
        for (unsigned i = 0; i < sp_count; ++i) CHECK(cafe_frame_sprite(&frame, i, &sprites[i]));
        CHECK(cafe_image_render_scene(&bands, frame.bg_asset, sprites, sp_count));
        unsigned long long total = 0;
        unsigned max_diff = 0, over = 0;
        for (int b = 0; b < CAFE_BAND_COUNT; ++b) {
            const uint16_t *want = (const uint16_t *)(ref + ref_pos + (size_t)b * BAND_PIXELS * 2);
            for (int i = 0; i < BAND_PIXELS; ++i) {
                const uint16_t a = bands.pixels[b][i];
                const uint16_t e = want[i];
                const unsigned dr = (unsigned)abs((int)((a >> 11) & 31) - (int)((e >> 11) & 31));
                const unsigned dg = (unsigned)abs((int)((a >> 5) & 63) - (int)((e >> 5) & 63));
                const unsigned db = (unsigned)abs((int)(a & 31) - (int)(e & 31));
                const unsigned d = dr > dg ? (dr > db ? dr : db) : (dg > db ? dg : db);
                total += dr + dg + db;
                if (d > max_diff) max_diff = d;
                if (d > 4) over++;
            }
        }
        ref_pos += 3 * BAND_PIXELS * 2;
        const double mae = (double)total / (3.0 * BAND_PIXELS * 3.0);
        printf("frame n%u[%u] sprites=%u max=%u over4=%u mae=%.2f\n",
               nid, idx, sp_count, max_diff, over, mae);
        CHECK(max_diff <= 8);
        CHECK(over <= (unsigned)(3 * BAND_PIXELS / 50));   // < 2% 像素超过 4 级
    }
    cafe_image_bands_free(&bands);
    printf("cafe image reference PASS (%u cases)\n", cases);
    free(ref);
    free(pack);
    return 0;
}
