// main/oc_tts.c —— TTS 下行播放的纯逻辑(见 oc_tts.h)。
#include "oc_tts.h"

#include <string.h>

oc_tts_parse_err_t oc_tts_parse(const uint8_t *payload, size_t len, oc_tts_packet_t *out)
{
    if (payload == NULL) {
        return OC_TTS_PARSE_ERR_LEN;
    }
    // 整帧载荷 = 3 字节头 + Opus 包,所以合法范围是 4..515。
    if (len < OC_TTS_OPUS_HEADER + 1u || len > OC_TTS_OPUS_FRAME_MAX) {
        return OC_TTS_PARSE_ERR_LEN;
    }
    uint8_t rate_khz = payload[1];
    // 只认协议里写明的两种速率;其它值说明手机侧编码器不是我们商量的那个。
    if (rate_khz != 16u && rate_khz != 24u) {
        return OC_TTS_PARSE_ERR_RATE;
    }
    // 本设备只实现 60ms 帧长:与其让解码器按错的帧长解出噪声,不如拒收并让 App 看到。
    if (payload[2] != OC_TTS_FRAME_MS) {
        return OC_TTS_PARSE_ERR_FRAME_MS;
    }
    size_t opus_len = len - OC_TTS_OPUS_HEADER;
    if (opus_len > OC_TTS_PACKET_MAX) {
        return OC_TTS_PARSE_ERR_LEN;
    }
    if (out != NULL) {
        out->seq = payload[0];
        out->rate_khz = rate_khz;
        out->opus = payload + OC_TTS_OPUS_HEADER;
        out->opus_len = opus_len;
    }
    return OC_TTS_PARSE_OK;
}

void oc_tts_queue_init(oc_tts_queue_t *q)
{
    if (q == NULL) {
        return;
    }
    memset(q, 0, sizeof(*q));
    oc_seq_init(&q->seq);
}

void oc_tts_queue_reset(oc_tts_queue_t *q)
{
    if (q == NULL) {
        return;
    }
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    oc_seq_init(&q->seq);
    q->frames = 0;
    q->decoded = 0;
    q->dropped = 0;
    q->underruns = 0;
    q->decode_us_max = 0;
    q->overflow = 0;
    q->draining = false;
}

bool oc_tts_queue_push(oc_tts_queue_t *q, uint8_t seq, uint8_t rate_khz,
                       const uint8_t *opus, size_t len)
{
    if (q == NULL || opus == NULL || len == 0 || len > OC_TTS_PACKET_MAX) {
        return false;
    }
    // 序号缺口:丢一帧就是丢 60ms 语音,只统计不重传(协议明确规定)。
    // 重复序号由 oc_seq_push 自己忽略,不计入丢失。
    uint32_t lost_before = oc_seq_lost(&q->seq);
    oc_seq_push(&q->seq, seq);
    q->dropped += oc_seq_lost(&q->seq) - lost_before;

    if (q->count >= OC_TTS_QUEUE_DEPTH) {
        q->overflow++;
        if (!q->draining) {
            // 还没开始播(设备刚进播放态、采集任务还在交 codec):这一段突发要**保住开头** ——
            // 真机"首句前几个字丢失"就是这里丢了最旧的包(那几包正好是这句话的开头)。
            // 反正队列马上会被消费,丢掉这一包不影响后文(后面按实时节奏到,队列不会一直满)。
            q->dropped++;
            return true;
        }
        // 已经在播:满则丢最旧,给刚合成出来的新音频让位(用户等的是最新那句)。
        q->tail = (q->tail + 1u) % OC_TTS_QUEUE_DEPTH;
        q->count--;
        q->dropped++;
    }
    oc_tts_slot_t *slot = &q->slots[q->head];
    slot->len = (uint16_t)len;
    slot->rate_khz = rate_khz;
    memcpy(slot->opus, opus, len);
    q->head = (q->head + 1u) % OC_TTS_QUEUE_DEPTH;
    q->count++;
    q->frames++;
    return true;
}

bool oc_tts_queue_pop(oc_tts_queue_t *q, uint8_t *dst, size_t cap,
                      size_t *len_out, uint8_t *rate_out)
{
    if (len_out != NULL) {
        *len_out = 0;
    }
    if (q == NULL || q->count == 0) {
        return false;
    }
    // 消费者开始取包了:之后队列满就按"丢最旧"处理(保持追最新)。
    q->draining = true;
    oc_tts_slot_t *slot = &q->slots[q->tail];
    size_t len = slot->len;
    uint8_t rate_khz = slot->rate_khz;
    q->tail = (q->tail + 1u) % OC_TTS_QUEUE_DEPTH;
    q->count--;

    if (dst == NULL || cap < len) {
        // 调用方缓冲给小了:丢掉这个包而不是把它留在队首(否则播放任务会永远取不出来)。
        q->dropped++;
        return false;
    }
    memcpy(dst, slot->opus, len);
    if (len_out != NULL) {
        *len_out = len;
    }
    if (rate_out != NULL) {
        *rate_out = rate_khz;
    }
    return true;
}

bool oc_tts_queue_empty(const oc_tts_queue_t *q)
{
    return (q == NULL) || (q->count == 0);
}

uint32_t oc_tts_queue_depth(const oc_tts_queue_t *q)
{
    return (q == NULL) ? 0u : q->count;
}

uint32_t oc_tts_queue_discard(oc_tts_queue_t *q)
{
    if (q == NULL) {
        return 0;
    }
    uint32_t n = q->count;
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    q->dropped += n;   // 没播出去的也要如实计入 dropped
    return n;
}

void oc_tts_queue_note_decode(oc_tts_queue_t *q, uint32_t decode_us)
{
    if (q == NULL) {
        return;
    }
    q->decoded++;
    if (decode_us > q->decode_us_max) {
        q->decode_us_max = decode_us;
    }
}

void oc_tts_queue_note_underrun(oc_tts_queue_t *q)
{
    if (q != NULL) {
        q->underruns++;
    }
}

void oc_tts_queue_stats(const oc_tts_queue_t *q, oc_tts_play_stats_t *out)
{
    if (q == NULL || out == NULL) {
        return;
    }
    out->frames = q->frames;
    out->decoded = q->decoded;
    out->dropped = q->dropped;
    out->underruns = q->underruns;
    out->decode_us_max = q->decode_us_max;
}
