// Manual/book text regression: every packed version converts fully, pages
// losslessly cover the source by byte ranges, and conversion failures leave a
// NUL-terminated valid prefix.
#include "gal_pack.h"
#include "gal_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BOOK_CAP 1280
#define PAGE_CAP 768

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

static int utf8_ok(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        if (*p < 0x80) { p++; continue; }
        int n = (*p & 0xE0) == 0xC0 ? 2 : (*p & 0xF0) == 0xE0 ? 3 :
                (*p & 0xF8) == 0xF0 ? 4 : 0;
        if (!n) return 0;
        for (int i = 1; i < n; ++i) {
            if ((p[i] & 0xC0) != 0x80) return 0;
        }
        p += n;
    }
    return 1;
}

static int conversion_negative(void)
{
    static const uint16_t ascii_and_cjk[] = { 'A', 0x4E2D };
    static const uint16_t high_alone[] = { 0xD800, 'A' };
    static const uint16_t low_alone[] = { 0xDC00, 'A' };
    char out[16];
    gal_pack_t p;
    memset(&p, 0, sizeof(p));
    p.sec[GAL_SEC_TEXTS] = (const uint8_t *)ascii_and_cjk;
    p.count[GAL_SEC_TEXTS] = 2;

    memset(out, 0xAA, sizeof(out));
    CHECK(gal_pack_text_utf8(&p, 0, 2, out, 3) == -1, "cap3 should fail");
    CHECK(strcmp(out, "A") == 0, "cap3 prefix=%s", out);
    memset(out, 0xAA, sizeof(out));
    CHECK(gal_pack_text_utf8(&p, 0, 2, out, 1) == -1, "cap1 should fail");
    CHECK(out[0] == 0, "cap1 not NUL");
    memset(out, 0xAA, sizeof(out));
    CHECK(gal_pack_text_utf8(&p, 0, 2, out, 0) == -1, "cap0 accepted");
    CHECK((unsigned char)out[0] == 0xAA, "cap0 wrote output");

    p.sec[GAL_SEC_TEXTS] = (const uint8_t *)high_alone;
    memset(out, 0xAA, sizeof(out));
    CHECK(gal_pack_text_utf8(&p, 0, 2, out, sizeof(out)) == -1, "high alone accepted");
    CHECK(out[0] == 0, "high alone not NUL");
    p.sec[GAL_SEC_TEXTS] = (const uint8_t *)low_alone;
    memset(out, 0xAA, sizeof(out));
    CHECK(gal_pack_text_utf8(&p, 0, 2, out, sizeof(out)) == -1, "low alone accepted");
    CHECK(out[0] == 0, "low alone not NUL");

    p.sec[GAL_SEC_TEXTS] = (const uint8_t *)ascii_and_cjk;
    CHECK(gal_pack_text_utf8(&p, 0, 0, out, sizeof(out)) == 0, "empty accepted");
    CHECK(out[0] == 0, "empty not NUL");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    uint32_t size = 0;
    uint8_t *data = load_file(argv[1], &size);
    gal_pack_t pack;
    const char *err = NULL;
    if (!gal_pack_open(&pack, data, size, &err)) {
        fprintf(stderr, "pack open: %s\n", err);
        return 1;
    }
    conversion_negative();

    uint32_t max_bytes = 0, max_units = 0, max_pages = 0;
    uint32_t checked = 0;
    for (uint32_t i = 0; i < pack.count[GAL_SEC_BOOK]; ++i) {
        const gal_book_t *b = gal_pack_book(&pack, i);
        CHECK(b != NULL, "book %u missing", i);
        if (!b) continue;
        char full[BOOK_CAP];
        memset(full, 0xAA, sizeof(full));
        const int n = gal_pack_book_text_utf8(&pack, b, full, sizeof(full));
        CHECK(n >= 0, "book %u convert failed (units=%u)", i, b->text_len);
        if (n < 0) continue;
        CHECK((size_t)n < sizeof(full), "book %u no NUL", i);
        CHECK(utf8_ok(full), "book %u invalid UTF-8", i);
        if ((uint32_t)n > max_bytes) max_bytes = (uint32_t)n;
        if (b->text_len > max_units) max_units = b->text_len;

        size_t expected = 0;
        int pages = 0;
        char page[PAGE_CAP];
        char rebuilt[BOOK_CAP];
        size_t rebuilt_len = 0;
        for (int pg = 0; ; ++pg) {
            size_t start = 0, end = 0;
            const bool ok = gal_text_page_ex(full, pg, 304, 16, 3, page, sizeof(page),
                                             &pages, &start, &end);
            if (pg == 0) {
                CHECK(ok, "book %u page0", i);
            }
            if (!ok) break;
            CHECK(start == expected, "book %u page %d gap/overlap %zu != %zu",
                  i, pg, start, expected);
            CHECK(end >= start && end <= (size_t)n, "book %u page %d range", i, pg);
            CHECK(strlen(page) < sizeof(page) - 1, "book %u page %d truncated", i, pg);
            CHECK(utf8_ok(page), "book %u page %d invalid UTF-8", i, pg);
            const size_t take = end - start;
            CHECK(rebuilt_len + take <= sizeof(rebuilt), "book %u rebuilt overflow", i);
            if (rebuilt_len + take <= sizeof(rebuilt)) {
                memcpy(rebuilt + rebuilt_len, full + start, take);
                rebuilt_len += take;
            }
            expected = end;
            if (pages > 0 && pg + 1 >= pages) break;
        }
        CHECK(expected == (size_t)n, "book %u pages end %zu != %d", i, expected, n);
        CHECK(rebuilt_len == (size_t)n, "book %u rebuilt len %zu != %d", i, rebuilt_len, n);
        CHECK(memcmp(rebuilt, full, (size_t)n) == 0, "book %u reconstructed text differs", i);
        if ((uint32_t)pages > max_pages) max_pages = (uint32_t)pages;
        checked++;
    }
    printf("books=%u max_units=%u max_utf8=%u max_pages=%u\n",
           checked, max_units, max_bytes, max_pages);
    printf("test_gal_book: %s\n", failures ? "FAIL" : "PASS");
    free(data);
    return failures ? 1 : 0;
}
