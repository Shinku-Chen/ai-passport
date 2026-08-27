// main/dlna_pipeline.h —— 播放管道核心状态机。
// 设计意图:把「音乐流拉取、MP3 帧解码、PCM 输出到 I2S」这条链路的
// 【可测试逻辑】—— 环形缓冲、水位/背压判定、帧边界提取 —— 与
// ESP-IDF/LWIP/LVGL 隔离出来,方便 host 测试。
//
// 剩下的「调 esp_http_client 拉流、调用 minimp3、调 bsp_audio_*」的
// 编排放在同名 .c 的 ESP-IDF 侧,这里只保留纯逻辑与可替换接口。
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 压缩字节环形缓冲 ---- */

// 环形缓冲把「生产者(http_pull 写入)」与「消费者(minimp3 读出)」解耦。
// 固定的非零容量,外部一次性分配;运行期不 realloc(见内存铁律)。
typedef struct {
    uint8_t *buf;      // 指向外部预分配的 MALLOC_CAP_8BIT 内存,不得为 NULL
    size_t   capacity; // 容量(字节)
    size_t   head;     // 写指针
    size_t   tail;     // 读指针
} dlna_ring_t;

// 初始化环形缓冲。buf 与 capacity 由调用方预分配;cap 会被钳到内部上限。
void dlna_ring_init(dlna_ring_t *r, uint8_t *buf, size_t capacity);

// 当前已用字节(生产端视角)。
size_t dlna_ring_used(const dlna_ring_t *r);

// 当前可写字节(消费端松开后的空余)。
size_t dlna_ring_free(const dlna_ring_t *r);

// 生产端写入 n 字节,返回实际写入量(<= n,可能少于 n 因缓存满)。
// 只要还需要写更多,生产者应持续调用直到返回值 < n 为止来决定是否等待。
size_t dlna_ring_write(dlna_ring_t *r, const uint8_t *data, size_t n);

// 消费端读 n 字节,返回实际读出量(<= n)。
size_t dlna_ring_read(dlna_ring_t *r, uint8_t *out, size_t n);

// 消费端"偷看"n 字节但不移动读指针(用于不解码就探测 MP3 帧头)。
// 返回实际可偷看量；若足够则把连续数据拷到 out。
size_t dlna_ring_peek(const dlna_ring_t *r, uint8_t *out, size_t n);

// 清空(跳到停/换源时用，丢弃残留)。
void dlna_ring_clear(dlna_ring_t *r);

#ifdef __cplusplus
}
#endif
