// main/c4_link_proto.c —— 联机报文的编解码与停等可靠投递实现。
#include "c4_link_proto.h"

#include <string.h>

#define C4_SEQ_MASK   0x7Fu
#define C4_SEQ_FLAG   0x80u
#define C4_ACK_MASK   0x7Fu
#define C4_ACK_FLAG   0x80u

static void result_clear(c4_link_result_t *r)
{
    memset(r, 0, sizeof(*r));
}

static uint8_t payload_len_for_type(uint8_t type)
{
    switch (type) {
    case C4_LINK_TYPE_HELLO:   return 3;
    case C4_LINK_TYPE_MOVE:    return 2;
    case C4_LINK_TYPE_REMATCH: return 1;
    case C4_LINK_TYPE_LEAVE:   return 0;
    default:                   return 0;
    }
}

// 组装一帧:has_data 为 false 时是纯确认帧(序号位为 0)。
static uint8_t build_frame(const c4_link_proto_t *p, bool has_data, uint8_t seq,
                           uint8_t type, const uint8_t *payload, uint8_t payload_len,
                           uint8_t *out)
{
    out[0] = C4_LINK_PROTO_MAGIC;
    out[1] = C4_LINK_PROTO_VERSION;
    out[2] = has_data ? type : 0;
    out[3] = has_data ? (uint8_t)(C4_SEQ_FLAG | (seq & C4_SEQ_MASK)) : 0;
    out[4] = p->have_ack ? (uint8_t)(C4_ACK_FLAG | (p->ack_seq & C4_ACK_MASK)) : 0;

    uint8_t len = 5;
    if (has_data && payload_len > 0 && payload) {
        memcpy(&out[len], payload, payload_len);
        len = (uint8_t)(len + payload_len);
    }
    return len;
}

static uint8_t ack_frame(const c4_link_proto_t *p, uint8_t *out)
{
    return build_frame(p, false, 0, 0, NULL, 0, out);
}

void c4_link_proto_init(c4_link_proto_t *p)
{
    if (!p) return;
    memset(p, 0, sizeof(*p));
}

bool c4_link_proto_busy(const c4_link_proto_t *p)
{
    return p && p->pending_active;
}

c4_link_result_t c4_link_proto_send(c4_link_proto_t *p, uint8_t type,
                                    const uint8_t *payload, uint8_t payload_len)
{
    c4_link_result_t r;
    result_clear(&r);
    if (!p) return r;

    const uint8_t expect = payload_len_for_type(type);
    if (payload_len != expect) return r;              // 类型与负载长度不匹配,不发
    if (payload_len > 0 && !payload) return r;

    const uint8_t seq = p->next_seq;
    const uint8_t len = build_frame(p, true, seq, type, payload, payload_len, r.tx);
    r.tx_len = len;

    // 记成待确认:覆盖上一个未确认的包(见头文件里的停等说明)。
    memcpy(p->pending, r.tx, len);
    p->pending_len = len;
    p->pending_seq = seq;
    p->retries = 0;
    p->since_tx_ms = 0;
    p->pending_active = true;

    p->next_seq = (uint8_t)((seq + 1) & C4_SEQ_MASK);
    return r;
}

c4_link_result_t c4_link_proto_recv(c4_link_proto_t *p, const uint8_t *frame, uint8_t len)
{
    c4_link_result_t r;
    result_clear(&r);
    if (!p || !frame) return r;
    if (len < 5 || len > C4_LINK_PKT_MAX) return r;
    if (frame[0] != C4_LINK_PROTO_MAGIC) return r;
    if (frame[1] != C4_LINK_PROTO_VERSION) {
        // 版本对不上时仍然回确认,免得对端一直重传;但要明确告诉应用层。
        r.ev.kind = C4_LINK_EV_BAD_VERSION;
        r.ev.version = frame[1];
        r.tx_len = ack_frame(p, r.tx);
        return r;
    }

    // 对端确认:清掉已经送达的待确认包。
    if ((frame[4] & C4_ACK_FLAG) != 0) {
        const uint8_t ack = (uint8_t)(frame[4] & C4_ACK_MASK);
        if (p->pending_active && ack == p->pending_seq) {
            p->pending_active = false;
            p->retries = 0;
        }
    }

    if ((frame[3] & C4_SEQ_FLAG) == 0) {
        // 纯确认帧:没有新数据,不需要再回确认(否则两边会互发确认没完没了)。
        return r;
    }

    const uint8_t type = frame[2];
    const uint8_t seq = (uint8_t)(frame[3] & C4_SEQ_MASK);

    if (type == C4_LINK_TYPE_HELLO) {
        if (len != 5 + 3) return r;
        if (frame[5] != C4_LINK_PROTO_VERSION) {
            r.ev.kind = C4_LINK_EV_BAD_VERSION;
            r.ev.version = frame[5];
            r.tx_len = ack_frame(p, r.tx);
            return r;
        }
    } else if (payload_len_for_type(type) != (uint8_t)(len - 5)) {
        return r;
    }

    // 去重:停等下一次只接受“上一个 +1”,重复包只重发确认。
    const uint8_t expected = p->have_peer_seq ? (uint8_t)((p->ack_seq + 1) & C4_SEQ_MASK) : 0;
    if (p->have_peer_seq && seq == p->ack_seq) {
        r.tx_len = ack_frame(p, r.tx);
        return r;
    }
    if (seq != expected) {
        // 乱序/陈旧序号:不确认,让对端按超时重传。停等正常不会走到这里。
        return r;
    }

    p->ack_seq = seq;
    p->have_ack = true;
    p->have_peer_seq = true;
    r.tx_len = ack_frame(p, r.tx);

    switch (type) {
    case C4_LINK_TYPE_HELLO:
        p->peer_ready = true;
        p->peer_side = frame[6];
        p->peer_game_id = frame[7];
        r.ev.kind = C4_LINK_EV_PEER_READY;
        r.ev.first_side = frame[6];
        r.ev.game_id = frame[7];
        break;
    case C4_LINK_TYPE_MOVE:
        r.ev.kind = C4_LINK_EV_MOVE;
        r.ev.col = frame[5];
        r.ev.ply = frame[6];
        break;
    case C4_LINK_TYPE_REMATCH:
        r.ev.kind = C4_LINK_EV_REMATCH;
        r.ev.game_id = frame[5];
        break;
    case C4_LINK_TYPE_LEAVE:
        r.ev.kind = C4_LINK_EV_LEAVE;
        break;
    default:
        // 未知类型:已经确认过序号了,当作没发生(向前兼容新类型时也不至于卡死)。
        r.ev.kind = C4_LINK_EV_NONE;
        break;
    }
    return r;
}

c4_link_result_t c4_link_proto_tick(c4_link_proto_t *p, uint32_t elapsed_ms)
{
    c4_link_result_t r;
    result_clear(&r);
    if (!p || !p->pending_active) return r;

    p->since_tx_ms += elapsed_ms;
    if (p->since_tx_ms < C4_LINK_RETRY_MS) return r;

    if (p->retries >= C4_LINK_MAX_RETRIES) {
        p->pending_active = false;
        p->retries = 0;
        r.ev.kind = C4_LINK_EV_LOST;
        return r;
    }

    memcpy(r.tx, p->pending, p->pending_len);
    r.tx_len = p->pending_len;
    p->retries++;
    p->since_tx_ms = 0;
    return r;
}
