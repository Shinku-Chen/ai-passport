// tests/test_cafe_ending.c — ending records must be readable at their packed layout.
#include "cafe_pack.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #cond); return 1; } } while (0)

static unsigned char *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f); *size = (size_t)n; return b;
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    size_t n = 0;
    unsigned char *pack = read_file(argv[1], &n);
    CHECK(pack && cafe_pack_set_data(pack, n));
    CHECK(cafe_meta_u32(6) == 12);
    for (uint16_t i = 0; i < 12; ++i) {
        cafe_ending_t e;
        CHECK(cafe_ending_get(i, &e));
        CHECK(e.bit == i);
        CHECK(e.kind == (i < 7 ? 0 : 1));
        CHECK(e.title_off != 0);
        CHECK(e.body_off != 0);
        const char *title = cafe_pack_string(e.title_off);
        const char *body = cafe_pack_string(e.body_off);
        CHECK(title && title[0] != '\0');
        CHECK(body && body[0] != '\0');
        CHECK(strlen(title) < 160);
        CHECK(strlen(body) < 256);
    }
    free(pack);
    printf("cafe ending records: 12/12 nonempty PASS\n");
    return 0;
}
