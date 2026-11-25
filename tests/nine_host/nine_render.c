// Host pixel dumper for the production compositor. Compiled with
// NINE_IMAGE_HOST; no alternate renderer is used for acceptance.
#include "nine_format.h"
#include "nine_image.h"
#include "nine_pack.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f); *size = (size_t)n; return b;
}
static int write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fprintf(f, "P6\n%u %u\n255\n", NINE_IMAGE_W, NINE_IMAGE_H);
    for (unsigned y = 0; y < NINE_IMAGE_H; ++y) {
        for (unsigned x = 0; x < NINE_IMAGE_W; ++x) {
            uint16_t p = nine_image_test_pixel(x, y);
            unsigned r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
            unsigned char rgb[3] = {(unsigned char)((r << 3) | (r >> 2)),
                                    (unsigned char)((g << 2) | (g >> 4)),
                                    (unsigned char)((b << 3) | (b >> 2))};
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    return 1;
}
int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s script images (node index|asset image) out.ppm\n", argv[0]);
        return 2;
    }
    size_t sn, imn;
    uint8_t *script = read_file(argv[1], &sn);
    uint8_t *images = read_file(argv[2], &imn);
    if (!script || !images) return 2;
    static nine_pack_t pack;
    if (!nine_pack_open(&pack, script, sn) || !nine_image_init(NULL) ||
        !nine_image_bind_bundle(script, sn, images, imn)) {
        fprintf(stderr, "pack/bind failed\n");
        return 2;
    }
    int ok = 0;
    if (!strcmp(argv[3], "asset")) {
        ok = nine_image_show_asset((uint32_t)atoi(argv[4])) && write_ppm(argv[5]);
    } else {
        int node = nine_pack_find_node(&pack, argv[3]);
        int index = atoi(argv[4]);
        nine_node_t n;
        nine_frame_t f;
        if (node >= 0 && nine_pack_node(&pack, (uint16_t)node, &n) &&
            index >= 0 && (uint32_t)index < n.frame_count &&
            nine_pack_frame(&pack, n.first_frame + (uint32_t)index, &f) && f.kind == NINE_FRAME_TEXT) {
            ok = nine_image_show_visual(f.visual) && write_ppm(argv[5]);
        }
    }
    nine_image_deinit();
    free(script); free(images);
    return ok ? 0 : 1;
}
