// tests/test_cafe_inflate.c — real pack text blocks through the low-level inflate path.
#include "cafe_compat.h"
#include "cafe_pack.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #c); return 1; } } while (0)
extern unsigned host_tinfl_lowlevel_calls;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static unsigned char *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n <= 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    unsigned char *p = (unsigned char *)malloc((size_t)n);
    if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); fclose(f); return NULL; }
    fclose(f);
    *size = (size_t)n;
    return p;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    size_t size = 0;
    unsigned char *pack = read_file(argv[1], &size);
    CHECK(pack && size >= 128);
    CHECK(memcmp(pack, CAFE_PACK_MAGIC, 8) == 0);
    const uint32_t section_count = rd32(pack + 20);
    CHECK(section_count > 0 && section_count <= 32);
    const uint8_t *blocks = NULL, *data = NULL;
    uint32_t blocks_size = 0, data_size = 0, blocks_count = 0;
    for (uint32_t i = 0; i < section_count; ++i) {
        const uint8_t *s = pack + 128 + (size_t)i * 24;
        const uint32_t type = rd32(s), off = rd32(s + 4), len = rd32(s + 8);
        CHECK(off <= size && len <= size - off);
        if (type == CAFE_SEC_TEXT_BLOCKS) { blocks = pack + off; blocks_size = len; blocks_count = rd32(s + 12); }
        if (type == CAFE_SEC_TEXT_DATA) { data = pack + off; data_size = len; }
    }
    CHECK(blocks && data && blocks_size == blocks_count * 20);
    unsigned char *out = (unsigned char *)malloc(CAFE_TEXT_CACHE_SIZE);
    CHECK(out);
    unsigned checked = 0;
    for (uint32_t i = 0; i < blocks_count; ++i) {
        const uint8_t *b = blocks + (size_t)i * 20;
        const uint16_t raw = rd16(b + 6);
        const uint32_t off = rd32(b + 8), comp = rd32(b + 12), want_crc = rd32(b + 16);
        CHECK(raw <= CAFE_TEXT_CACHE_SIZE && off <= data_size && comp <= data_size - off);
        size_t got = 0;
        CHECK(cafe_inflate(out, CAFE_TEXT_CACHE_SIZE, &got, data + off, comp));
        CHECK(got == raw);
        CHECK(cafe_crc32(0, out, raw) == want_crc);
        checked++;
    }
    CHECK(checked > 0);
    CHECK(host_tinfl_lowlevel_calls >= checked);
    printf("cafe inflate pack blocks PASS (%u blocks)\n", checked);

    // Corrupt/truncated streams must reject, including the zlib Adler trailer.
    const uint8_t *b0 = blocks;
    const uint32_t off0 = rd32(b0 + 8), comp0 = rd32(b0 + 12);
    CHECK(comp0 > 8 && off0 <= data_size && comp0 <= data_size - off0);
    unsigned char *bad = (unsigned char *)malloc(comp0);
    CHECK(bad);
    memcpy(bad, data + off0, comp0);
    bad[comp0 - 1] ^= 0x01;   // zlib Adler-32 trailer
    size_t got = 123;
    CHECK(!cafe_inflate(out, CAFE_TEXT_CACHE_SIZE, &got, bad, comp0));
    CHECK(got == 0);
    memcpy(bad, data + off0, comp0);
    CHECK(!cafe_inflate(out, CAFE_TEXT_CACHE_SIZE, &got, bad, comp0 - 1)); // truncated
    CHECK(!cafe_inflate(out, CAFE_TEXT_CACHE_SIZE, &got, bad, comp0 > 2 ? 2 : 1)); // header/short
    memset(bad, 0, comp0);
    bad[0] = 0x00; bad[1] = 0x00; // invalid zlib header
    CHECK(!cafe_inflate(out, CAFE_TEXT_CACHE_SIZE, &got, bad, comp0));
    memcpy(bad, data + off0, comp0);
    const uint16_t raw0 = rd16(b0 + 6);
    if (raw0 > 0) CHECK(!cafe_inflate(out, raw0 - 1, &got, bad, comp0));
    free(bad);
    free(out);
    free(pack);
    printf("cafe inflate corruption negative PASS\n");
    return 0;
}
