// Malformed pack must be rejected before any app state is touched.
#include "gal_pack.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); failures++; } \
} while (0)

static uint8_t *load_file(const char *path, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) exit(2);
    fclose(f);
    *size = (uint32_t)n;
    return b;
}

static uint32_t sec_off(const uint8_t *d, int sec)
{
    return *(const uint32_t *)(d + 24 + 16 * sec + 4);
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    uint32_t size = 0;
    uint8_t *good = load_file(argv[1], &size);
    gal_pack_t p;
    const char *err = NULL;
    CHECK(gal_pack_open(&p, good, size, &err), "good pack: %s", err);

    uint8_t *d = malloc(size);
    if (!d) return 2;
    const uint32_t ops = sec_off(good, GAL_SEC_OPS);
    const uint32_t scenes = sec_off(good, GAL_SEC_SCENES);
    const uint32_t images = sec_off(good, GAL_SEC_IMAGES);
    const uint32_t stroff = sec_off(good, GAL_SEC_STROFF);
    const uint32_t strs = sec_off(good, GAL_SEC_STR);
    const uint32_t str_size = rd32(good + 24 + 16 * GAL_SEC_STR + 12);

    #define CASE(name, code) do { \
        memcpy(d, good, size); code; \
        const char *e = NULL; \
        CHECK(!gal_pack_open(&p, d, size, &e), name " accepted"); \
        CHECK(e != NULL, name " error missing"); \
    } while (0)

    CASE("magic", d[0] ^= 1);
    CASE("version", d[8] = 2);
    CASE("sections", rd32(d + 16) == 0 ? (d[16] = 1) : (d[16] = 0));
    CASE("total", d[12] ^= 1);
    CASE("section offset", *(uint32_t *)(d + 24 + 16 * 5 + 4) = size + 4);
    CASE("section size", *(uint32_t *)(d + 24 + 16 * 5 + 12) = 12);
    CASE("scene count", *(uint32_t *)(d + scenes + 16) = 0xFFFFFFFFu);
    CASE("scene op range", *(uint32_t *)(d + scenes + 12) = 0xFFFFFFu);
    CASE("op jump target", *(uint16_t *)(d + ops + 4) = 0xFFFFu);
    CASE("text range", *(uint32_t *)(d + ops + 6 * 12 + 8) = 0xFFFFFFu);
    CASE("string offset", *(uint32_t *)(d + stroff) = 0xFFFFFFu);
    CASE("string terminator", d[strs + str_size - 1] = 'X');
    CASE("image data length", *(uint32_t *)(d + images + 8) = 0xFFFFFFu);
    CASE("image type", d[images + 38] = 9);
    CASE("image mask", *(uint32_t *)(d + images + 24) = 0xFFFFFFu);
    CASE("text unit count", *(uint32_t *)(d + 24 + 16 * GAL_SEC_TEXTS + 8) = 0xFFu);
    CASE("truncated", size = size - 1);

    printf("test_gal_pack_bad: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
