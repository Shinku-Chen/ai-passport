// main/dlna_pipeline.c —— 播放管道核心的纯逻辑部分(环形缓冲)。
// 这部分不依赖 ESP-IDF / LWIP / LVGL / minimp3,是唯一可直接 host 测试的域。
// 音频管道真正的"拉流 → 解码 → 写 I2S"编排见 main/dlna_player.c。
#include "dlna_pipeline.h"
#include <string.h>

void dlna_ring_init(dlna_ring_t *r, uint8_t *buf, size_t capacity)
{
    r->buf = buf;
    r->capacity = capacity;
    r->head = 0;
    r->tail = 0;
}

size_t dlna_ring_used(const dlna_ring_t *r)
{
    // head >= tail 时用 head - tail;否则 ring 绕回。
    return (r->head >= r->tail) ? (r->head - r->tail)
                                : (r->capacity - r->tail + r->head);
}

size_t dlna_ring_free(const dlna_ring_t *r)
{
    return r->capacity - dlna_ring_used(r);
}

size_t dlna_ring_write(dlna_ring_t *r, const uint8_t *data, size_t n)
{
    // 一次最多只能写到"绕回点"或"追上 tail",取两者较小值。
    if (n == 0 || r->capacity == 0) return 0;

    size_t free_space = dlna_ring_free(r);
    if (free_space == 0) return 0;
    if (n > free_space) n = free_space;

    size_t to_tail = r->capacity - r->head;   // 到物理末尾的字节数
    size_t chunk = (n <= to_tail) ? n : to_tail;
    memcpy(r->buf + r->head, data, chunk);
    r->head = (r->head + chunk) % r->capacity;

    if (n > chunk) {
        // 剩下绕回开头写。
        size_t rem = n - chunk;
        memcpy(r->buf, data + chunk, rem);
        r->head = rem;
    }
    return n;
}

size_t dlna_ring_read(dlna_ring_t *r, uint8_t *out, size_t n)
{
    if (n == 0 || r->capacity == 0) return 0;

    size_t used = dlna_ring_used(r);
    if (used == 0) return 0;
    if (n > used) n = used;

    size_t to_tail = r->capacity - r->tail;
    size_t chunk = (n <= to_tail) ? n : to_tail;
    if (out) memcpy(out, r->buf + r->tail, chunk);
    r->tail = (r->tail + chunk) % r->capacity;

    if (n > chunk) {
        size_t rem = n - chunk;
        if (out) memcpy(out + chunk, r->buf, rem);
        r->tail = rem;
    }
    return n;
}

size_t dlna_ring_peek(const dlna_ring_t *r, uint8_t *out, size_t n)
{
    // 只读不前进:把从 tail 到 min(n, used) 的连续数据拷出。
    // 环形缓冲可能绕回,peek 只保证"从 tail 起、不绕回"范围内的数据是一次性连续可见的
    // (前半段)。需要越绕回的转发由调用方在检测到边界时自行组装。
    size_t used = dlna_ring_used(r);
    if (used == 0) return 0;
    if (n > used) n = used;

    size_t to_tail = r->capacity - r->tail;
    size_t chunk = (n <= to_tail) ? n : to_tail;
    if (out) memcpy(out, r->buf + r->tail, chunk);
    return chunk;   // 只返回连续可读的这部分
}

void dlna_ring_clear(dlna_ring_t *r)
{
    r->head = 0;
    r->tail = 0;
}
