// tests/test_gal_badpack.c —— 坏包回归:每类损坏都必须被 gal_pack_open 拒绝。
//
// 用真实包做基线,逐项复制并破坏,断言拒绝且 err 非空;同时确认未损坏的副本仍能打开。
#include "gal_pack.h"

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

static uint8_t *g_base;
static uint32_t g_base_size;

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

static uint32_t sec_count(void) { uint32_t n = 0; memcpy(&n, g_base + 16, 4); return n; }
static void sec_get(int i, uint32_t *type, uint32_t *off, uint32_t *cnt, uint32_t *size)
{
    memcpy(type, g_base + 20 + 16 * i, 4);
    memcpy(off, g_base + 24 + 16 * i, 4);
    memcpy(cnt, g_base + 28 + 16 * i, 4);
    memcpy(size, g_base + 32 + 16 * i, 4);
}

static int expect_reject(const char *name, void (*mutate)(uint8_t *))
{
    uint8_t *copy = malloc(g_base_size);
    if (!copy) return 0;
    memcpy(copy, g_base, g_base_size);
    mutate(copy);
    gal_pack_t pack;
    const char *err = NULL;
    const bool ok = gal_pack_open(&pack, copy, g_base_size, &err);
    CHECK(!ok, "%s: 坏包被接受了", name);
    CHECK(err != NULL && *err, "%s: 没有给出原因", name);
    free(copy);
    return ok ? 0 : 1;
}

static void mutate_bad_magic(uint8_t *b) { b[0] = 'X'; }
static void mutate_version(uint8_t *b) { put32(b + 8, 99); }
static void mutate_total(uint32_t *unused) { (void)unused; }
static void mutate_size_field(uint8_t *b) { put32(b + 12, g_base_size + 4); }
static void mutate_dup_section(uint8_t *b)
{
    uint32_t t0, o0, c0, s0, t1;
    sec_get(0, &t0, &o0, &c0, &s0);
    sec_get(1, &t1, &o0, &c0, &s0);
    put32(b + 20 + 16 + 0, t0);      // 第 2 段改成与第 1 段同 id
    (void)t1;
}
static void mutate_overlap(uint8_t *b)
{
    uint32_t t, o, c, s;
    sec_get(1, &t, &o, &c, &s);
    put32(b + 24 + 16 * 2, o + 1);   // 第 3 段起点挪进第 2 段
    (void)t; (void)c; (void)s;
}
static void mutate_into_table(uint8_t *b)
{
    put32(b + 24 + 16 * 1, 4);       // 段起点落在段表里
}
static void mutate_count(uint8_t *b) { put32(b + 16, 999); }
static void mutate_event_count(uint8_t *b)
{
    for (uint32_t i = 0; i < sec_count(); ++i) {
        uint32_t t, o, c, s;
        sec_get(i, &t, &o, &c, &s);
        if (t == 2) put32(b + 28 + 16 * i, c + 1);   // 事件数 +1 -> size 不匹配
    }
}
static void mutate_line_text_off(uint8_t *b)
{
    for (uint32_t i = 0; i < sec_count(); ++i) {
        uint32_t t, o, c, s;
        sec_get(i, &t, &o, &c, &s);
        if (t == 3) put32(b + o, 0xFFFFFF00u);       // 第一句文本偏移越界
    }
}
static void mutate_line_nul(uint8_t *b)
{
    uint32_t text_off = 0, line_off = 0;
    for (uint32_t i = 0; i < sec_count(); ++i) {
        uint32_t t, o, c, s;
        sec_get(i, &t, &o, &c, &s);
        if (t == 0) text_off = o;
        if (t == 3) line_off = o;
    }
    // 把第 0 句的结尾 NUL 改成 'A'
    uint32_t off;
    uint16_t len;
    memcpy(&off, b + line_off, 4);
    memcpy(&len, b + line_off + 4, 2);
    b[text_off + off + len] = 'A';
}
static void mutate_title_image(uint8_t *b)
{
    for (uint32_t i = 0; i < sec_count(); ++i) {
        uint32_t t, o, c, s;
        sec_get(i, &t, &o, &c, &s);
        if (t == 13 && s >= 2) {
            uint16_t bad = 0xFFFF;
            memcpy(b + o, &bad, 2);
        }
    }
}

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "main/gal_data/gal_pack.bin";
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
    fseek(f, 0, SEEK_END);
    const long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_base = malloc((size_t)len);
    if (!g_base || fread(g_base, 1, (size_t)len, f) != (size_t)len) return 2;
    fclose(f);
    g_base_size = (uint32_t)len;

    // 基线必须能打开
    {
        gal_pack_t pack;
        const char *err = NULL;
        CHECK(gal_pack_open(&pack, g_base, g_base_size, &err), "baseline: %s",
              err ? err : "?");
    }

    expect_reject("magic", mutate_bad_magic);
    expect_reject("version", mutate_version);
    expect_reject("size", mutate_size_field);
    expect_reject("duplicate section", mutate_dup_section);
    expect_reject("overlap", mutate_overlap);
    expect_reject("into table", mutate_into_table);
    expect_reject("section count", mutate_count);
    expect_reject("event count/size", mutate_event_count);
    expect_reject("line text offset", mutate_line_text_off);
    expect_reject("text not NUL", mutate_line_nul);
    expect_reject("title image", mutate_title_image);
    (void)mutate_total;

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("badpack ok\n");
    free(g_base);
    return 0;
}
