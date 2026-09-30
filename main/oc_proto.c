// main/oc_proto.c —— 对讲机线协议(v1)的纯逻辑实现,见 oc_proto.h 与规范文档。
//
// 本文件不包含任何 ESP-IDF / BLE 依赖:分帧与重组是协议里最容易出错、又最值得
// 单测的部分,放在主机测试里跑比在真机上看日志便宜得多。
#include "oc_proto.h"

#include <string.h>

bool oc_type_known(uint8_t type)
{
    switch (type) {
    case OC_FRAME_AUDIO_PCM:
    case OC_FRAME_TEXT:
    case OC_FRAME_CONTROL:
    case OC_FRAME_EVENT:
    case OC_FRAME_AUDIO_OPUS:
    case OC_FRAME_TTS_OPUS:
        return true;
    default:
        return false;
    }
}

size_t oc_payload_limit(uint8_t type)
{
    switch (type) {
    case OC_FRAME_AUDIO_PCM:
        return OC_PCM_PAYLOAD_MAX;
    case OC_FRAME_AUDIO_OPUS:
        return OC_OPUS_PAYLOAD_MAX;
    case OC_FRAME_TTS_OPUS:
        // 上限是整帧载荷(3 字节头 + 512 字节 Opus 包),不是 512:
        // 重组器用这个值判断帧头是否合理,给 512 会把合法的 515 字节帧当成错位。
        return OC_TTS_OPUS_FRAME_MAX;
    case OC_FRAME_TEXT:
        return OC_TEXT_PAYLOAD_MAX;
    case OC_FRAME_CONTROL:
    case OC_FRAME_EVENT:
        return OC_JSON_PAYLOAD_MAX;
    default:
        return 0;
    }
}

size_t oc_encode(uint8_t *out, size_t out_cap, uint8_t type, uint8_t flags,
                 const uint8_t *payload, size_t len)
{
    if (out == NULL) {
        return 0;
    }
    if (payload == NULL && len != 0) {
        return 0;
    }
    if (!oc_type_known(type) || len > oc_payload_limit(type)) {
        return 0;
    }
    size_t total = OC_HEADER_SIZE + len;
    if (out_cap < total) {
        return 0;
    }
    out[0] = (uint8_t)OC_MAGIC0;
    out[1] = (uint8_t)OC_MAGIC1;
    out[2] = type;
    out[3] = flags;
    out[4] = (uint8_t)((len >> 8) & 0xFFu);
    out[5] = (uint8_t)(len & 0xFFu);
    if (len != 0) {
        memcpy(out + OC_HEADER_SIZE, payload, len);
    }
    return total;
}

size_t oc_utf8_safe_len(const uint8_t *bytes, size_t len)
{
    if (bytes == NULL || len == 0) {
        return 0;
    }
    // 向前跨过续字节(10xxxxxx),让 i-1 落在最后一个字符的首字节上。
    size_t i = len;
    while (i > 0 && (bytes[i - 1] & 0xC0u) == 0x80u) {
        i--;
    }
    if (i == 0) {
        return 0;  // 尾段全是续字节:无法判断字符开头,保守丢弃
    }
    uint8_t lead = bytes[i - 1];
    size_t seq = (lead >= 0xF0u) ? 4u : (lead >= 0xE0u) ? 3u : (lead >= 0xC0u) ? 2u : 1u;
    size_t have = len - (i - 1);
    return (have >= seq) ? len : (i - 1);
}

// 帧头是否自洽:magic 对、type 已知、长度不超过该类型上限。
static bool header_valid(const uint8_t *h)
{
    if (h[0] != (uint8_t)OC_MAGIC0 || h[1] != (uint8_t)OC_MAGIC1) {
        return false;
    }
    size_t limit = oc_payload_limit(h[2]);
    if (limit == 0) {
        return false;
    }
    size_t len = ((size_t)h[4] << 8) | (size_t)h[5];
    return len <= limit;
}

static bool is_audio_type(uint8_t type)
{
    return type == OC_FRAME_AUDIO_PCM || type == OC_FRAME_AUDIO_OPUS ||
           type == OC_FRAME_TTS_OPUS;
}

// 在已攒的帧内容里寻找"被插进来的新帧头"(固件会在音频帧中途插 EVENT)。
// 从偏移 1 开始扫:偏移 0 是当前帧自己的帧头。
// 判据刻意收窄,避免把载荷里偶发的 a5 5a 误判成边界:
//   * magic 对 + type 已知 + 长度不超过该类型上限;
//   * 音频帧里不会再嵌套音频帧(固件不会这样做,载荷偶发命中概率也最高)。
// 返回命中的偏移;没命中返回 0。
static size_t find_inserted_header(const uint8_t *buf, size_t len, uint8_t cur_type)
{
    if (len < OC_HEADER_SIZE + 1u) {
        return 0;
    }
    bool cur_audio = is_audio_type(cur_type);
    for (size_t i = 1; i + OC_HEADER_SIZE <= len; i++) {
        if (buf[i] != (uint8_t)OC_MAGIC0 || buf[i + 1] != (uint8_t)OC_MAGIC1) {
            continue;
        }
        uint8_t type = buf[i + 2];
        size_t limit = oc_payload_limit(type);
        if (limit == 0) {
            continue;
        }
        if (cur_audio && is_audio_type(type)) {
            continue;
        }
        size_t plen = ((size_t)buf[i + 4] << 8) | (size_t)buf[i + 5];
        if (plen > limit) {
            continue;
        }
        return i;
    }
    return 0;
}

// 丢掉缓冲里的第一个字节(错位重同步用)。缓冲很小(≤ 2054B),逐字节搬移足够。
static void drop_one_byte(oc_reassembler_t *rx)
{
    if (rx->len == 0) {
        return;
    }
    memmove(rx->buf, rx->buf + 1, rx->len - 1);
    rx->len--;
}

void oc_reassembler_init(oc_reassembler_t *rx, oc_frame_cb_t cb, void *ctx)
{
    if (rx == NULL) {
        return;
    }
    rx->len = 0;
    rx->need = 0;
    rx->frame_len = 0;
    rx->in_frame = false;
    rx->cb = cb;
    rx->ctx = ctx;
}

void oc_reassembler_reset(oc_reassembler_t *rx)
{
    if (rx == NULL) {
        return;
    }
    rx->len = 0;
    rx->need = 0;
    rx->frame_len = 0;
    rx->in_frame = false;
}

void oc_reassembler_push(oc_reassembler_t *rx, const uint8_t *bytes, size_t n)
{
    if (rx == NULL || (bytes == NULL && n != 0)) {
        return;
    }
    size_t idx = 0;
    for (;;) {
        if (!rx->in_frame) {
            // ① 先凑帧头字节
            if (rx->len < OC_HEADER_SIZE && idx < n) {
                size_t take = OC_HEADER_SIZE - rx->len;
                if (take > n - idx) {
                    take = n - idx;
                }
                memcpy(rx->buf + rx->len, bytes + idx, take);
                rx->len += take;
                idx += take;
                continue;
            }
            // ② 对齐 magic:不合就丢一字节继续找(丢 notify 后靠这一步恢复)
            if (rx->len >= 1 && rx->buf[0] != (uint8_t)OC_MAGIC0) {
                drop_one_byte(rx);
                continue;
            }
            if (rx->len >= 2 && rx->buf[1] != (uint8_t)OC_MAGIC1) {
                drop_one_byte(rx);
                continue;
            }
            if (rx->len < OC_HEADER_SIZE) {
                break;  // 输入耗尽,等下一批
            }
            // ③ magic 对但 type/长度不合理 → 仍是错位,继续丢
            if (!header_valid(rx->buf)) {
                drop_one_byte(rx);
                continue;
            }
            size_t plen = ((size_t)rx->buf[4] << 8) | (size_t)rx->buf[5];
            rx->frame_len = OC_HEADER_SIZE + plen;
            rx->need = rx->frame_len - rx->len;
            rx->in_frame = true;
            continue;
        }

        // ④ 收 payload:优先吃掉输入,再回到循环顶部检查是否被插了新帧头
        if (rx->need > 0 && idx < n) {
            size_t take = rx->need;
            if (take > n - idx) {
                take = n - idx;
            }
            memcpy(rx->buf + rx->len, bytes + idx, take);
            rx->len += take;
            idx += take;
            rx->need -= take;
            continue;
        }

        size_t cut = find_inserted_header(rx->buf, rx->len, rx->buf[2]);
        if (cut > 0) {
            // 半截帧作废,把插入帧头搬到缓冲开头重新解析
            memmove(rx->buf, rx->buf + cut, rx->len - cut);
            rx->len -= cut;
            rx->need = 0;
            rx->frame_len = 0;
            rx->in_frame = false;
            continue;
        }

        if (rx->need == 0) {
            if (rx->cb != NULL) {
                rx->cb(rx->buf[2], rx->buf[3], rx->buf + OC_HEADER_SIZE,
                       rx->frame_len - OC_HEADER_SIZE, rx->ctx);
            }
            rx->len = 0;
            rx->need = 0;
            rx->frame_len = 0;
            rx->in_frame = false;
            continue;
        }
        break;  // 输入耗尽
    }
}

void oc_text_merge_init(oc_text_merge_t *m)
{
    if (m == NULL) {
        return;
    }
    memset(m, 0, sizeof(*m));
    m->role = 'A';
}

bool oc_text_merge_push(oc_text_merge_t *m, uint8_t flags,
                        const uint8_t *payload, size_t len)
{
    if (m == NULL) {
        return false;
    }
    bool first = (flags & OC_FLAG_FIRST) != 0;
    bool more = (flags & OC_FLAG_MORE) != 0;
    bool last = (flags & OC_FLAG_LAST) != 0;

    // 载荷首字节是角色('U'/'A'/'R'),其余为 UTF-8 正文。旧版 App 不带角色,按回复处理。
    char role = (len >= 1) ? (char)payload[0] : 'A';
    const uint8_t *body = (len >= 1) ? payload + 1 : payload;
    size_t blen = (len >= 1) ? len - 1 : 0;

    if (first || !m->active || (!first && !more && !last)) {
        // 新一条消息:重置缓冲。需要重置的三种情况:
        //   * FIRST:分片消息的开头;
        //   * 上一条已经收尾(active=false):孤立分片也当新消息,避免内容串条;
        //   * 无任何标志位:单帧完整消息,它打断未完成的分片(如新一轮回复覆盖旧回复)。
        m->len = 0;
        m->overflow = false;
        m->role = role;
        m->active = true;
    }

    size_t room = (OC_TEXT_MERGE_CAP - 1u) - m->len;
    size_t take = blen;
    if (take > room) {
        take = room;
        m->overflow = true;
    }
    take = oc_utf8_safe_len(body, take);
    if (take != 0) {
        memcpy(m->text + m->len, body, take);
    }
    m->len += take;
    m->text[m->len] = '\0';

    bool complete = last || (!first && !more && !last);
    if (complete) {
        m->active = false;
    }
    return complete;
}

void oc_seq_init(oc_seq_tracker_t *t)
{
    if (t == NULL) {
        return;
    }
    t->last = 0;
    t->started = false;
    t->received = 0;
    t->lost = 0;
}

void oc_seq_push(oc_seq_tracker_t *t, uint8_t seq)
{
    if (t == NULL) {
        return;
    }
    if (!t->started) {
        t->started = true;
        t->last = seq;
        t->received = 1;
        return;
    }
    uint8_t diff = (uint8_t)(seq - t->last);
    if (diff == 0) {
        return;  // 重复帧:不计收,也不计丢
    }
    t->received++;
    t->lost += (uint32_t)diff - 1u;
    t->last = seq;
}

uint32_t oc_seq_lost(const oc_seq_tracker_t *t)
{
    return (t == NULL) ? 0u : t->lost;
}
