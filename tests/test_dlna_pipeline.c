#include <assert.h>
#include <string.h>
#include "dlna_pipeline.h"

// 用一块固定小缓冲跑各种 wrap 场景。
static uint8_t area[8];

static void test_empty_init(void)
{
    dlna_ring_t r;
    dlna_ring_init(&r, area, sizeof(area));
    assert(dlna_ring_used(&r) == 0);
    assert(dlna_ring_free(&r) == 8);
}

static void test_simple_write_read(void)
{
    dlna_ring_t r;
    dlna_ring_init(&r, area, sizeof(area));
    const char *msg = "hello";
    assert(dlna_ring_write(&r, (const uint8_t *)msg, 5) == 5);
    assert(dlna_ring_used(&r) == 5);
    assert(dlna_ring_free(&r) == 3);

    char out[8] = {0};
    assert(dlna_ring_read(&r, (uint8_t *)out, 5) == 5);
    assert(strcmp(out, "hello") == 0);
    assert(dlna_ring_used(&r) == 0);
}

static void test_wrap_around(void)
{
    dlna_ring_t r;
    dlna_ring_init(&r, area, sizeof(area));
    // 填满前 6 字节。
    assert(dlna_ring_write(&r, (const uint8_t *)"abcdef", 6) == 6);
    // 读走前 4 字节,此时 tail=4。
    char out[8] = {0};
    assert(dlna_ring_read(&r, (uint8_t *)out, 4) == 4);
    assert(strcmp(out, "abcd") == 0);
    // 再写 4 字节,会绕回(head 到 0 开头)。
    const char *msg = "wxyz";
    assert(dlna_ring_write(&r, (const uint8_t *)msg, 4) == 4);
    assert(dlna_ring_used(&r) == 6);   // "ef" + "wxyz"
    assert(dlna_ring_free(&r) == 2);

    // 读出全部 6 字节,应得到 "ef" 后接 "wxyz"。
    assert(dlna_ring_read(&r, (uint8_t *)out, 6) == 6);
    assert(out[0] == 'e' && out[1] == 'f' && out[2] == 'w'
           && out[3] == 'x' && out[4] == 'y' && out[5] == 'z');
}

static void test_overwrite_clamped(void)
{
    dlna_ring_t r;
    dlna_ring_init(&r, area, sizeof(area));
    // 一次写 10 字节但只有 8 空间，应只写进 8。
    assert(dlna_ring_write(&r, (const uint8_t *)"0123456789", 10) == 8);
    assert(dlna_ring_used(&r) == 8);
    assert(dlna_ring_free(&r) == 0);
}

static void test_read_into_partial(void)
{
    dlna_ring_t r;
    dlna_ring_init(&r, area, sizeof(area));
    assert(dlna_ring_write(&r, (const uint8_t *)"abc", 3) == 3);
    char out[8] = {0};
    assert(dlna_ring_read(&r, (uint8_t *)out, 9) == 3);   // 最多 3
    assert(dlna_ring_used(&r) == 0);
}

static void test_peek_no_advance(void)
{
    dlna_ring_t r;
    dlna_ring_init(&r, area, sizeof(area));
    assert(dlna_ring_write(&r, (const uint8_t *)"abc", 3) == 3);
    char out[8] = {0};
    assert(dlna_ring_peek(&r, (uint8_t *)out, 2) == 2);
    assert(strcmp(out, "ab") == 0);
    assert(dlna_ring_used(&r) == 3);   // peek 不推进
}

static void test_clear(void)
{
    dlna_ring_t r;
    dlna_ring_init(&r, area, sizeof(area));
    assert(dlna_ring_write(&r, (const uint8_t *)"abc", 3) == 3);
    dlna_ring_clear(&r);
    assert(dlna_ring_used(&r) == 0);
    assert(dlna_ring_free(&r) == 8);
}

int main(void)
{
    test_empty_init();
    test_simple_write_read();
    test_wrap_around();
    test_overwrite_clamped();
    test_read_into_partial();
    test_peek_no_advance();
    test_clear();
    return 0;
}
