// Full media index/CRC and all 287 OP frame decode through production bands.
#include "cafe_pack.h"
#include "cafe_image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #c); return 1; } } while (0)

static unsigned char *rf(const char *p, size_t *n)
{
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long s = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)s);
    if (!b || fread(b, 1, (size_t)s, f) != (size_t)s) { free(b); fclose(f); return NULL; }
    fclose(f); *n = (size_t)s; return b;
}

int main(int ac, char **av)
{
    (void)ac;
    size_t n = 0;
    unsigned char *b = rf(av[1], &n);
    CHECK(b && cafe_pack_set_data(b, n));
    cafe_audio_track_t a;
    CHECK(cafe_audio_track_get(0, &a)); CHECK(a.samples == 1027198 && a.sample_rate == 4000 && a.block_align == 1024);
    CHECK(cafe_audio_track_get(1, &a)); CHECK(a.samples == 743959 && a.sample_rate == 8000 && a.block_align == 1024);
    cafe_op_t op;
    CHECK(cafe_op_get(&op));
    CHECK(op.frames == 287 && op.fps == 3 && op.duration_ms == 95460 && op.audio_guard_ms == 92400);
    cafe_image_t first_img;
    CHECK(cafe_image_get(cafe_op_image_id(0), &first_img));
    CHECK(first_img.w == 224 && first_img.h == 126);
    cafe_bands_t bands;
    CHECK(cafe_image_bands_init(&bands));
    unsigned nonzero = 0;
    for (uint16_t f = 0; f < op.frames; ++f) {
        const uint16_t id = cafe_op_image_id(f);
        CHECK(id != CAFE_IMAGE_NONE);
        CHECK(cafe_image_verify(id));
        CHECK(cafe_image_render_op(&bands, id));
        for (int k = 0; k < CAFE_BAND_COUNT; ++k)
            for (int i = 0; i < CAFE_BAND_W * CAFE_BAND_H; ++i)
                if (bands.pixels[k][i]) nonzero++;
    }
    CHECK(nonzero > 0);
    printf("media: audio samples 1027198/743959, OP %u frames, nonzero=%u PASS\n",
           (unsigned)op.frames, nonzero);
    cafe_image_bands_free(&bands);
    free(b);
    return 0;
}
