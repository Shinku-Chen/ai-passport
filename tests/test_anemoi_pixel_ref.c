// Production C pixel regression: source contain + paper letterbox + alpha.
#include "anemoi_canvas.h"
#include "anemoi_image.h"
#include "anemoi_pack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAPER 0xFFDEu

static unsigned char *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = (unsigned char *)malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return buf;
}

static int meta_id(const anem_pack_t *pack, const char *key)
{
    const char *v = NULL;
    uint32_t n = 0;
    char buf[24];
    if (!anem_pack_meta(pack, key, &v, &n) || n == 0 || n >= sizeof(buf)) return -1;
    memcpy(buf, v, n);
    buf[n] = 0;
    return atoi(buf);
}

static int all_paper(const anem_canvas_t *c, int x0, int x1, int y0, int y1)
{
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            if (c->pixels[y * ANEM_CANVAS_W + x] != PAPER) return 0;
        }
    }
    return 1;
}

static int nonpaper(const anem_canvas_t *c, int x0, int x1, int y0, int y1)
{
    int n = 0;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            if (c->pixels[y * ANEM_CANVAS_W + x] != PAPER) n++;
        }
    }
    return n;
}

static int draw_scene(const anem_pack_t *pack, anem_canvas_t *canvas, anem_image_ctx_t *ctx,
                      int id)
{
    anem_canvas_fill(canvas, 0xFFFF);
    return anem_image_draw_scene(ctx, canvas, pack, (uint16_t)id) ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s pack.bin\n", argv[0]);
        return 2;
    }
    size_t size = 0;
    unsigned char *blob = read_file(argv[1], &size);
    if (!blob) return 2;
    anem_pack_t pack;
    anem_canvas_t canvas;
    anem_image_ctx_t ctx;
    if (!anem_pack_open(&pack, blob, size) || !anem_canvas_init(&canvas)) return 2;
    anem_image_init(&ctx);
    int fail = 0;

    if (draw_scene(&pack, &canvas, &ctx, 1729)) {
        fprintf(stderr, "FAIL scene 1729 decode\n"); fail++;
    } else {
        if (!all_paper(&canvas, 0, 74, 0, 179) || !all_paper(&canvas, 244, 319, 0, 179)) {
            fprintf(stderr, "FAIL scene 1729 edge bands are not paper\n"); fail++;
        }
        if (nonpaper(&canvas, 75, 243, 0, 179) < 1000) {
            fprintf(stderr, "FAIL scene 1729 center blank\n"); fail++;
        }
    }

    if (draw_scene(&pack, &canvas, &ctx, 1739)) {
        fprintf(stderr, "FAIL scene 1739 decode\n"); fail++;
    } else {
        if (!all_paper(&canvas, 0, 319, 0, 4) || !all_paper(&canvas, 0, 319, 174, 179)) {
            fprintf(stderr, "FAIL scene 1739 edge bands are not paper\n"); fail++;
        }
        if (nonpaper(&canvas, 0, 319, 5, 173) < 1000) {
            fprintf(stderr, "FAIL scene 1739 center blank\n"); fail++;
        }
    }

    const char *keys[] = {"title_image_normal", "title_image_anemoi", "title_image_eternicle"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        const int id = meta_id(&pack, keys[i]);
        if (id < 0 || draw_scene(&pack, &canvas, &ctx, id)) {
            fprintf(stderr, "FAIL title decode %s=%d\n", keys[i], id); fail++;
            continue;
        }
        if (!all_paper(&canvas, 0, 83, 0, 179) || !all_paper(&canvas, 235, 319, 0, 179)) {
            fprintf(stderr, "FAIL title %s gutters not paper\n", keys[i]); fail++;
        }
        if (nonpaper(&canvas, 84, 234, 0, 179) < 1000) {
            fprintf(stderr, "FAIL title %s center blank\n", keys[i]); fail++;
        }
    }

    anem_canvas_fill(&canvas, 0xFFFF);
    if (!anem_image_draw_sprite(&ctx, &canvas, &pack, 0, 10, 20)) {
        fprintf(stderr, "FAIL sprite 0 decode\n"); fail++;
    } else {
        if (canvas.pixels[20 * ANEM_CANVAS_W + 10] != 0xFFFF) {
            fprintf(stderr, "FAIL sprite 0 corner painted\n"); fail++;
        }
        if (canvas.pixels[83 * ANEM_CANVAS_W + 46] == 0xFFFF) {
            fprintf(stderr, "FAIL sprite 0 sample blank\n"); fail++;
        }
    }

    uint32_t jpeg_n = 0, block_n = 0;
    for (uint16_t id = 0; id < 65535; id++) {
        anem_image_t info;
        if (!anem_pack_image(&pack, id, &info)) break;
        bool ok = true;
        if (info.kind == ANEM_IMG_JPEG) {
            jpeg_n++;
            ok = draw_scene(&pack, &canvas, &ctx, id) == 0;
        } else if (info.kind == ANEM_IMG_BLOCK) {
            block_n++;
            anem_canvas_fill(&canvas, 0xFFFF);
            ok = anem_image_draw_sprite(&ctx, &canvas, &pack, id, 0, 0);
        }
        if (!ok) {
            fprintf(stderr, "FAIL image id=%u kind=%u w=%u h=%u\n", id, info.kind, info.w, info.h);
            fail++;
            if (fail > 10) break;
        }
    }
    printf("pixel-ref jpeg=%u block=%u\n", jpeg_n, block_n);
    free(blob);
    if (fail) {
        fprintf(stderr, "test_anemoi_pixel_ref: FAIL (%d)\n", fail);
        return 1;
    }
    printf("test_anemoi_pixel_ref: PASS\n");
    return 0;
}
