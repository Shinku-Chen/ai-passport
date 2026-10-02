// main/oc_proto.h —— 对讲机线协议(v1)的纯逻辑实现:分帧、重组、文本合并、丢帧统计。
//
// 权威规范见 docs/development/engineering/intercom-wire-protocol.md;本文件只实现它,
// 不引入任何 ESP-IDF / BLE / LVGL 依赖,因此可以直接在主机侧做单测。
//
// 帧格式:
//   [MAGIC0 0xA5][MAGIC1 0x5A][TYPE:1B][FLAGS:1B][LEN:2B 大端][payload]
//
// 为什么需要 magic:旧实现只有 1 字节 type,而 PCM/Opus 载荷里大量出现 0x01..0x05,
// 一旦丢一次 notify,接收端会把载荷字节当成帧头并永久失步。magic 让接收端在任意
// 位置都能重新对齐;type + 长度合理性作为二次校验。
//
// 三个必须保留的规则(踩过的坑):
//   1. 等帧头时逐字节向前扫描,直到 magic + 合法 type + 合法长度同时成立。
//   2. 攒 payload 时也要扫描"被插进来的新帧头"(固件会把 EVENT 插在音频帧中间),
//      命中即丢弃半截帧,从该处重新解析。
//   3. 重组缓冲必须容得下最大合法帧(6 + 2048)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 协议版本(hello 的 proto 字段),与规范文档同步升级。
#define OC_PROTO_VERSION 1u

// ---- 帧头 ----
#define OC_MAGIC0 0xA5u
#define OC_MAGIC1 0x5Au
#define OC_HEADER_SIZE 6u
#define OC_MAGIC_SIZE 2u

// ---- 帧类型 ----
typedef enum {
    OC_FRAME_AUDIO_PCM  = 0x01,  // 设备→手机: [SEQ:1B] + int16 小端单声道 PCM(兜底路径)
    OC_FRAME_TEXT       = 0x02,  // 手机→设备: [role:1B] + UTF-8 文本
    OC_FRAME_CONTROL    = 0x03,  // 双向:      JSON 命令/确认
    OC_FRAME_EVENT      = 0x04,  // 设备→手机: JSON 事件
    OC_FRAME_AUDIO_OPUS = 0x05,  // 设备→手机: [SEQ:1B] + 一个 Opus 包(默认上行)
    OC_FRAME_TTS_OPUS   = 0x06,  // 手机→设备: [SEQ:1B][rate_khz:1B][frame_ms:1B] + 一个 Opus 包(下行 TTS)
} oc_frame_type_t;

// ---- 标志位 ----
#define OC_FLAG_MORE  0x01u
#define OC_FLAG_FIRST 0x02u
#define OC_FLAG_LAST  0x04u

// ---- 各类载荷上限(超限的帧一律按错位处理) ----
#define OC_PCM_PAYLOAD_MAX  1024u   // 512 samples × 2B,不含 SEQ
#define OC_OPUS_PAYLOAD_MAX 512u

// 下行 TTS_OPUS 载荷:[SEQ][rate_khz][frame_ms] + Opus 包。
// 512 是"单个 Opus 包"的上限(与上行 OC_OPUS_PAYLOAD_MAX 同一量级);整帧载荷的上限
// 是 3+512,因此 TTS 帧合法长度是 4..515 —— 两处都不要混用(见 oc_payload_limit)。
#define OC_TTS_OPUS_HEADER      3u
#define OC_TTS_OPUS_PAYLOAD_MAX 512u
#define OC_TTS_OPUS_FRAME_MAX   (OC_TTS_OPUS_HEADER + OC_TTS_OPUS_PAYLOAD_MAX)
#define OC_TEXT_PAYLOAD_MAX 2048u   // 含 role 字节
#define OC_JSON_PAYLOAD_MAX 512u
#define OC_PAYLOAD_MAX      OC_TEXT_PAYLOAD_MAX
#define OC_FRAME_MAX        (OC_HEADER_SIZE + OC_PAYLOAD_MAX)

// 文本合并缓冲:text 载荷上限 + NUL。设备端把分片合并成一条再上屏。
#define OC_TEXT_MERGE_CAP (OC_TEXT_PAYLOAD_MAX + 1u)

// ---- 编码 ----
// 把一帧写进 out(out 至少 6+len 字节)。返回写入字节数;参数非法返回 0。
// 音频类载荷需要调用方自行在 payload[0] 放 SEQ 字节。
size_t oc_encode(uint8_t *out, size_t out_cap, uint8_t type, uint8_t flags,
                 const uint8_t *payload, size_t len);

// 该 type 是否为已知类型;未知类型按错位处理,不进入重组。
bool oc_type_known(uint8_t type);

// 该 type 允许的最大载荷长度;未知类型返回 0。
size_t oc_payload_limit(uint8_t type);

// 从 len 处向前回退到最近的 UTF-8 字符边界,避免把多字节字符切成两半。
// 返回 ≤ len 的可用长度;len 为 0 时返回 0。
size_t oc_utf8_safe_len(const uint8_t *bytes, size_t len);

// ---- 重组 ----
// 收到一整帧时回调。payload 指向内部缓冲,仅在回调期间有效;需要保留请自行拷贝。
typedef void (*oc_frame_cb_t)(uint8_t type, uint8_t flags,
                              const uint8_t *payload, size_t len, void *ctx);

typedef struct {
    uint8_t buf[OC_FRAME_MAX];  // 当前帧(magic 起)
    size_t  len;                // 已攒字节数
    size_t  need;               // 还差多少字节到整帧
    size_t  frame_len;          // 当前帧总长
    bool    in_frame;           // 已确认帧头,正在收 payload
    oc_frame_cb_t cb;
    void   *ctx;
    // 诊断计数(定位“手机推来的帧到底有没有到应用层”):只增不减,真机日志可直接引用。
    uint32_t stat_delivered;      // 成功回调给应用的帧数
    uint32_t stat_by_type[16];    // 按 type 的帧计数(0..15)
    uint32_t stat_half_dropped;   // 半截帧作废次数(命中 find_inserted_header)
} oc_reassembler_t;

void oc_reassembler_init(oc_reassembler_t *rx, oc_frame_cb_t cb, void *ctx);

// 推入一段 notify 字节,可能触发零次或多次回调;可在任意位置切分。
void oc_reassembler_push(oc_reassembler_t *rx, const uint8_t *bytes, size_t n);

// 丢弃半截帧,回到"等帧头"状态(断连/重新订阅时调用)。
void oc_reassembler_reset(oc_reassembler_t *rx);

// ---- 文本分片合并(设备端) ----
typedef struct {
    char   text[OC_TEXT_MERGE_CAP];  // NUL 结尾
    size_t len;
    char   role;                     // 'U' / 'A' / 'R'
    bool   active;                   // 正在合并分片
    bool   overflow;                 // 本条约有内容被截断
} oc_text_merge_t;

void oc_text_merge_init(oc_text_merge_t *m);

// 推入一个 TEXT 帧的载荷。返回 true 表示本条消息已完整,text 可直接上屏。
// 分片规则:FIRST 开启新一条、MORE 续写、LAST 收尾;无标志位视为单帧完整消息。
// 超出缓冲的内容会被截断(记 overflow),且不会切在多字节 UTF-8 字符中间。
bool oc_text_merge_push(oc_text_merge_t *m, uint8_t flags,
                        const uint8_t *payload, size_t len);

// ---- 音频序号统计 ----
// SEQ 只用于发现丢帧,不触发重传:音频丢一帧就是丢 60ms,识别端能容忍。
typedef struct {
    uint8_t  last;
    bool     started;
    uint32_t received;
    uint32_t lost;
} oc_seq_tracker_t;

void oc_seq_init(oc_seq_tracker_t *t);

// 记录一个收到的 SEQ(1 字节回绕)。重复帧不计入丢失,只不计 received。
void oc_seq_push(oc_seq_tracker_t *t, uint8_t seq);

// 本轮累计丢失的帧数。
uint32_t oc_seq_lost(const oc_seq_tracker_t *t);
