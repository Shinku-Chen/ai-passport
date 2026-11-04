// tests/test_summer_pack_format.c — top-level section-table hardening.
// All mutations are made to a RAM copy; the shipped pack is never modified.
#include "summer_pack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        failures++; \
    } \
} while (0)

static uint8_t *read_all(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    uint8_t *p = malloc((size_t)n);
    if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); fclose(f); return NULL; }
    fclose(f);
    *size = (size_t)n;
    return p;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint8_t *entry(uint8_t *blob, uint16_t type)
{
    const uint16_t count = (uint16_t)blob[12] | ((uint16_t)blob[13] << 8);
    for (uint16_t i = 0; i < count; ++i) {
        uint8_t *e = blob + SUMMER_PACK_HEADER_SIZE + (size_t)i * SUMMER_PACK_SECTION_SIZE;
        const uint16_t t = (uint16_t)e[0] | ((uint16_t)e[1] << 8);
        if (t == type) return e;
    }
    return NULL;
}

static bool opens(const uint8_t *blob, size_t size)
{
    summer_pack_t pack;
    const char *err = NULL;
    return summer_pack_open(&pack, blob, (uint32_t)size, &err);
}

static void test_mutation(const uint8_t *base, size_t size, const char *name,
                          void (*mutate)(uint8_t *, size_t))
{
    uint8_t *copy = malloc(size);
    if (!copy) return;
    memcpy(copy, base, size);
    mutate(copy, size);
    CHECK(!opens(copy, size), "%s accepted", name);
    free(copy);
}

static void mutate_duplicate(uint8_t *blob, size_t size)
{
    (void)size;
    uint8_t *second = blob + SUMMER_PACK_HEADER_SIZE + SUMMER_PACK_SECTION_SIZE;
    second[0] = 0;
    second[1] = 0;  // duplicate TEXT type
}

static void mutate_directory(uint8_t *blob, size_t size)
{
    (void)size;
    uint8_t *e = entry(blob, SUMMER_SEC_TEXT);
    wr32(e + 4, SUMMER_PACK_HEADER_SIZE - 4);  // aligned, but inside the table
}

static void mutate_overlap(uint8_t *blob, size_t size)
{
    (void)size;
    uint8_t *text = entry(blob, SUMMER_SEC_TEXT);
    uint8_t *ops = entry(blob, SUMMER_SEC_OPS);
    wr32(ops + 4, rd32(text + 4));
    wr32(ops + 12, rd32(text + 12));
}

static void mutate_remainder(uint8_t *blob, size_t size)
{
    (void)size;
    uint8_t *imap = entry(blob, SUMMER_SEC_IMAP);
    wr32(imap + 12, rd32(imap + 12) + 1u);  // count*2 no longer equals len
}

static void mutate_count_overflow(uint8_t *blob, size_t size)
{
    (void)size;
    uint8_t *imap = entry(blob, SUMMER_SEC_IMAP);
    wr32(imap + 8, 0xFFFFFFFFu);
}


// A raw 20-byte header can be readable while the 400-byte section table is
// truncated.  total is rewritten to the actual bad size so the old total-size
// check cannot mask the header/section-table read.
static void test_short_header(const uint8_t *base)
{
    uint8_t tiny[20] = { 0 };
    int tested = 0;
    for (size_t n = 0; n < 20; ++n) {
        memset(tiny, 0, sizeof(tiny));
        memcpy(tiny, base, n);
        CHECK(!opens(tiny, n), "initial raw header truncation %zu accepted", n);
        tested++;
    }
    static const size_t sizes[] = { 20, 24, 28, 32, 64, 419, 420 };
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        const size_t n = sizes[i];
        uint8_t *buf = malloc(n);
        if (!buf) continue;
        memset(buf, 0, n);
        memcpy(buf, base, n < 20 ? n : 20);
        if (n >= 20) wr32(buf + 16, (uint32_t)n);  // total must equal the bad size
        if (n > 24) {
            const size_t copy = n < 420 ? n : 420;
            memcpy(buf, base, copy);
            wr32(buf + 16, (uint32_t)n);
        }
        CHECK(!opens(buf, n), "short section table %zu accepted", n);
        free(buf);
        tested++;
    }
    printf("pack-format short buffers: %d/%d rejected", tested, tested);
    putchar(10);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "main/summer_data/summer_pack.bin";
    size_t size = 0;
    uint8_t *blob = read_all(path, &size);
    if (!blob) {
        fprintf(stderr, "SKIP: cannot read %s\n", path);
        return 77;
    }
    CHECK(opens(blob, size), "normal pack rejected");
    printf("pack-format normal pack: open");
    putchar(10);
    test_short_header(blob);
    test_mutation(blob, size, "duplicate section", mutate_duplicate);
    test_mutation(blob, size, "directory offset", mutate_directory);
    test_mutation(blob, size, "overlapping spans", mutate_overlap);
    test_mutation(blob, size, "fixed width remainder", mutate_remainder);
    test_mutation(blob, size, "fixed count overflow", mutate_count_overflow);
    free(blob);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_summer_pack_format: PASS\n");
    return 0;
}
