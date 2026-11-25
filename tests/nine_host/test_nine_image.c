// Real decode/composite comparison against Pillow reference composites.
//
// nine_image_ref.py independently parses the package, decodes every JPEG and
// indexed asset with Pillow and composites the layer list with the documented
// integer blend rules. JPEG layers allow a ±2 channel tolerance for
// libjpeg-vs-TJpgDec rounding; indexed layers must be exact.
//
// Usage: test_nine_image <script.bin> <images.bin> <refdir>
#include "nine_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 256
#define H 144

static uint8_t *slurp(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    uint8_t *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f);
    *size = (size_t)n;
    return b;
}

static int diff_ch(uint16_t a, uint16_t b, int shift, int bits)
{
    int x = (a >> shift) & ((1 << bits) - 1);
    int y = (b >> shift) & ((1 << bits) - 1);
    return x > y ? x - y : y - x;
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s <script.bin> <images.bin> <refdir>\n", argv[0]);
        return 2;
    }
    size_t script_size = 0, image_size = 0;
    uint8_t *script = slurp(argv[1], &script_size);
    uint8_t *images = slurp(argv[2], &image_size);
    if (!script || !images) { fprintf(stderr, "cannot read packages\n"); return 2; }
    if (!nine_image_bind_bundle(script, script_size, images, image_size)) {
        fprintf(stderr, "image package rejected\n");
        return 1;
    }
    if (!nine_image_init(NULL)) { fprintf(stderr, "init failed\n"); return 1; }

    const uint32_t assets = nine_image_asset_count();
    for (uint32_t i = 0; i < assets; ++i) {
        if (!nine_image_validate_asset(i)) {
            fprintf(stderr, "asset %u failed validation\n", (unsigned)i);
            return 1;
        }
    }
    // Exercise the real asset path (full-screen display) for a spread of assets.
    for (uint32_t i = 0; i < assets; i += assets / 16 ? assets / 16 : 1) {
        if (!nine_image_show_asset(i)) {
            fprintf(stderr, "show_asset %u failed\n", (unsigned)i);
            return 1;
        }
    }
    if (nine_image_test_largest_allocation() > 25600) {
        fprintf(stderr, "allocation %zu exceeds 25600\n", nine_image_test_largest_allocation());
        return 1;
    }

    char manifest[1024];
    snprintf(manifest, sizeof(manifest), "%s/manifest.txt", argv[3]);
    FILE *mf = fopen(manifest, "r");
    if (!mf) { fprintf(stderr, "cannot open manifest\n"); return 2; }
    char line[256];
    int visuals = 0, mismatched = 0, worst = 0;
    while (fgets(line, sizeof(line), mf)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        unsigned visual = 0, tolerance = 0;
        char name[128];
        if (sscanf(line, "%u %127s %u", &visual, name, &tolerance) != 3) continue;
        char path[1200];
        snprintf(path, sizeof(path), "%s/%s", argv[3], name);
        size_t ref_size = 0;
        uint8_t *ref = slurp(path, &ref_size);
        if (!ref || ref_size != W * H * 2) {
            fprintf(stderr, "bad reference %s\n", path);
            free(ref);
            return 1;
        }
        if (!nine_image_show_visual(visual)) {
            fprintf(stderr, "visual %u failed to decode\n", visual);
            free(ref);
            return 1;
        }
        int bad = 0;
        int visual_worst = 0;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                const uint16_t want = (uint16_t)(ref[(y * W + x) * 2] |
                                                 ref[(y * W + x) * 2 + 1] << 8);
                const uint16_t got = nine_image_test_pixel((unsigned)x, (unsigned)y);
                const int dr = diff_ch(want, got, 11, 5);
                const int dg = diff_ch(want, got, 5, 6);
                const int db = diff_ch(want, got, 0, 5);
                const int d = dr > dg ? (dr > db ? dr : db) : (dg > db ? dg : db);
                if (d > visual_worst) visual_worst = d;
                if (d > (int)tolerance) ++bad;
            }
        }
        if (visual_worst > worst) worst = visual_worst;
        if (bad) {
            fprintf(stderr, "visual %u: %d pixels outside tolerance %u (worst %d)\n",
                    visual, bad, tolerance, visual_worst);
            ++mismatched;
        }
        ++visuals;
        free(ref);
    }
    fclose(mf);
    if (mismatched || !visuals) {
        fprintf(stderr, "%d/%d reference visuals mismatched\n", mismatched, visuals);
        return 1;
    }
    printf("PASS image: %d visuals, worst channel delta %d, all %u assets validated, "
           "largest allocation %zu\n", visuals, worst, (unsigned)assets,
           nine_image_test_largest_allocation());
    free(script);
    free(images);
    return 0;
}
