// Pack-level security regression for the reviewer findings: link capacity,
// overlapping sections, directory-origin sections, duplicate types, oversized
// ranges and NUL inside a declared TEXT range. All mutations are in memory.
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

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static uint32_t sec_off(const uint8_t *b, int type)
{
    return rd32(b + 24 + 16 * type + 4);
}
static uint32_t sec_count(const uint8_t *b, int type)
{
    return rd32(b + 24 + 16 * type + 8);
}
static bool open_copy(uint8_t *d, uint32_t size, const char **err)
{
    gal_pack_t p;
    *err = NULL;
    return gal_pack_open(&p, d, size, err);
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    uint32_t size = 0;
    uint8_t *good = load_file(argv[1], &size);
    const char *err = NULL;
    CHECK(open_copy(good, size, &err), "good pack: %s", err ? err : "?");
    uint8_t *d = malloc(size);
    if (!d) return 2;

    const uint32_t lops = sec_off(good, GAL_SEC_LINK_OPS);
    const uint32_t links = sec_count(good, GAL_SEC_LINKS);
    CHECK(lops != 0 && links > 31, "link fixture");
    const uint8_t orig_count = good[lops + 6];
    CHECK(orig_count == 1, "first link count=%u", orig_count);

    memcpy(d, good, size);
    d[lops + 6] = 33;
    CHECK(!open_copy(d, size, &err), "count 33 accepted");
    CHECK(err != NULL, "count 33 error");
    memcpy(d, good, size);
    d[lops + 6] = 32;
    CHECK(!open_copy(d, size, &err), "count 32 + fixed continue accepted");
    CHECK(err != NULL, "count 32 error");
    memcpy(d, good, size);
    d[lops + 6] = 31;
    CHECK(open_copy(d, size, &err), "legal boundary 31 rejected: %s", err ? err : "?");
    memcpy(d, good, size);

    // Section overlap: redirect EXPR_DATA to exactly STR.
    const uint32_t str_off = sec_off(d, GAL_SEC_STR);
    const uint32_t str_len = rd32(d + 24 + 16 * GAL_SEC_STR + 12);
    const uint32_t expr_entry = 24 + 16 * GAL_SEC_EXPR_DATA;
    wr32(d + expr_entry + 4, str_off);
    wr32(d + expr_entry + 8, str_len);
    wr32(d + expr_entry + 12, str_len);
    CHECK(!open_copy(d, size, &err), "overlapping sections accepted");
    CHECK(err != NULL, "overlap error");

    // Section offset inside the directory.
    memcpy(d, good, size);
    wr32(d + 24 + 16 * GAL_SEC_OPS + 4, 0);
    CHECK(!open_copy(d, size, &err), "directory-origin section accepted");

    // Duplicate section type.
    memcpy(d, good, size);
    wr32(d + 24 + 16 * GAL_SEC_ROUTES, GAL_SEC_SCENES);
    CHECK(!open_copy(d, size, &err), "duplicate section type accepted");

    // Multiplication/size overflow-ish range.
    memcpy(d, good, size);
    wr32(d + 24 + 16 * GAL_SEC_OPS + 12, 0xFFFFFFFFu);
    CHECK(!open_copy(d, size, &err), "oversized section accepted");

    // NUL inside the first declared TEXT range.
    memcpy(d, good, size);
    const uint32_t ops = sec_off(d, GAL_SEC_OPS);
    const uint32_t nops = sec_count(d, GAL_SEC_OPS);
    const uint32_t texts = sec_off(d, GAL_SEC_TEXTS);
    bool mutated = false;
    for (uint32_t i = 0; i < nops; ++i) {
        const uint8_t *op = good + ops + (size_t)i * 12;
        if (op[0] != GAL_OP_TEXT) continue;
        const uint16_t len = (uint16_t)(op[4] | (op[5] << 8));
        const uint32_t off = rd32(op + 8);
        if (len == 0) continue;
        wr32(d, texts + off * 2);  // NUL unit
        mutated = true;
        break;
    }
    CHECK(mutated, "no TEXT fixture");
    CHECK(!open_copy(d, size, &err), "NUL inside TEXT accepted");

    printf("test_gal_pack_security: %s\n", failures ? "FAIL" : "PASS");
    free(d);
    free(good);
    return failures ? 1 : 0;
}
