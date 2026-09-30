// main/oc_tts.h —— 设备侧 TTS 下行播放的纯逻辑:帧解析、有界解码队列、统计。
//
// 权威规范:
//   * docs/development/engineering/intercom-wire-protocol.md —— "TTS audio downlink"
//     规定 TTS_OPUS 载荷是 [SEQ:1B][rate_khz:1B][frame_ms:1B] + 一个 Opus 包;
//   * docs/development/engineering/intercom-tts-playback.md —— 播放通路的内存预算、
//     队列深度(解码 24 包 / 播放 2 块)与统计字段。
//
// 本文件不依赖 ESP-IDF / BLE / LVGL(与 oc_proto.c 同一个理由:队列溢出与统计计数
// 是最容易算错的部分,放主机测试里跑比在真机上看日志便宜得多)。真正的解码与 I2S
// 写出在 oc_audio.c,本模块只管"收进来、排好队、如实计数"。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "oc_proto.h"   // OC_TTS_OPUS_* 载荷常量、oc_seq_tracker_t

// 解码队列深度:24 包 × 60ms ≈ 1.44s。手机最多领先设备约 2s(见协议文档),
// 再深就是白占内存(无 PSRAM,每个字节都要从堆里让出来)。
#define OC_TTS_QUEUE_DEPTH 24u

// 单个 Opus 包上限(与协议里的 OC_TTS_OPUS_PAYLOAD_MAX 同一常量,避免两处漂移)。
#define OC_TTS_PACKET_MAX OC_TTS_OPUS_PAYLOAD_MAX

// 本设备只实现一种帧长;手机给别的帧长直接拒收,不猜(见 oc_tts_parse)。
#define OC_TTS_FRAME_MS 60u

// ---- 帧解析 ----
// 载荷长度/参数非法一律拒收:错的是手机侧编码或链路,不是解码器。
typedef enum {
    OC_TTS_PARSE_OK = 0,
    OC_TTS_PARSE_ERR_LEN,       // 载荷长度不在 4..515(3 字节头 + 至少 1 字节 Opus 包)
    OC_TTS_PARSE_ERR_RATE,      // rate_khz 不是 16 / 24
    OC_TTS_PARSE_ERR_FRAME_MS,  // frame_ms 不是 60
} oc_tts_parse_err_t;

typedef struct {
    uint8_t        seq;        // 帧序号(1 字节回绕),只用于缺口统计
    uint8_t        rate_khz;   // 16 / 24
    const uint8_t *opus;       // 指向入参载荷内部,不拷贝;用完即弃
    size_t         opus_len;
} oc_tts_packet_t;

// 解析一个 TTS_OPUS 帧的载荷。out 可为空(只做校验)。
oc_tts_parse_err_t oc_tts_parse(const uint8_t *payload, size_t len, oc_tts_packet_t *out);

// ---- 统计 ----
// 字段与规范文档一致;tts_playback_done 事件就是把它原样报给手机,
// 让手机能分清"发出去了"和"设备真的播了"。
typedef struct {
    uint32_t frames;         // 收下(入队)的包数
    uint32_t decoded;        // 成功解码的包数
    uint32_t dropped;        // 丢弃的包数:队列溢出丢最旧 + flush/打断清空 + SEQ 缺口
    uint32_t underruns;      // 播放时队列空(欠载)的次数
    uint32_t decode_us_max;  // 单包最长解码耗时(微秒)
} oc_tts_play_stats_t;

// ---- 有界解码队列(环形,满则丢最旧) ----
// 槽位大小固定为 512 字节:协议允许的最大 Opus 包就是这么大,不必猜典型包长
// (24 × 516 B ≈ 12.1 KB,是这条通路最大的一块静态内存)。
typedef struct {
    uint16_t len;
    uint8_t  rate_khz;
    uint8_t  opus[OC_TTS_PACKET_MAX];
} oc_tts_slot_t;

typedef struct {
    oc_tts_slot_t    slots[OC_TTS_QUEUE_DEPTH];
    uint32_t         head;      // 下一个写入位置
    uint32_t         tail;      // 下一个读出位置
    uint32_t         count;     // 当前排队包数(0..DEPTH)
    oc_seq_tracker_t seq;       // 序号缺口统计(缺口计入 dropped)
    uint32_t         frames;
    uint32_t         decoded;
    uint32_t         dropped;
    uint32_t         underruns;
    uint32_t         decode_us_max;
    uint32_t         overflow;  // dropped 中"因队列满丢最旧"的部分(仅用于日志区分)
} oc_tts_queue_t;

void oc_tts_queue_init(oc_tts_queue_t *q);

// 新一轮开始:清空队列并清零统计(tts_start 时调用)。
void oc_tts_queue_reset(oc_tts_queue_t *q);

// 入队一个 Opus 包。返回 true 表示已收下,false 表示参数非法(调用方自己的 bug)。
// 队列满时不报错:丢最旧的包、dropped++/overflow++,最旧的让位给刚合成出来的新音频。
bool oc_tts_queue_push(oc_tts_queue_t *q, uint8_t seq, uint8_t rate_khz,
                       const uint8_t *opus, size_t len);

// 出队一个 Opus 包到 dst。空队列返回 false 且不改动 dst。
// cap 不足时视为调用方 bug:该包被丢弃(dropped++)并返回 false,避免队列卡死。
bool oc_tts_queue_pop(oc_tts_queue_t *q, uint8_t *dst, size_t cap,
                      size_t *len_out, uint8_t *rate_out);

bool     oc_tts_queue_empty(const oc_tts_queue_t *q);
uint32_t oc_tts_queue_depth(const oc_tts_queue_t *q);

// 清空队列(打断/中止用),返回被丢弃的包数并计入 dropped。
uint32_t oc_tts_queue_discard(oc_tts_queue_t *q);

// 播放侧计数(只有播放任务调用)。
void oc_tts_queue_note_decode(oc_tts_queue_t *q, uint32_t decode_us);
void oc_tts_queue_note_underrun(oc_tts_queue_t *q);

// 取统计快照。dropped 是累计值(溢出 + 清空 + 缺口),不存在重复累加。
void oc_tts_queue_stats(const oc_tts_queue_t *q, oc_tts_play_stats_t *out);
