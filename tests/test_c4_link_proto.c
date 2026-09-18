// tests/test_c4_link_proto.c —— 联机报文与停等可靠层的宿主测试。
//
// 覆盖:握手事件、去重、捎带确认、重传与耗尽、乱序/损坏帧的丢弃、版本不一致、
// 以及“停等下新包覆盖旧包”的语义。这些是两台设备棋盘不一致的全部防线。
#include "c4_link_proto.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static c4_link_proto_t a;
static c4_link_proto_t b;

static void reset_pair(void)
{
    c4_link_proto_init(&a);
    c4_link_proto_init(&b);
}

// 把一端发的东西交给另一端,返回对端回给发送方的帧(通常是一条确认)。
static c4_link_result_t deliver(c4_link_proto_t *to, const c4_link_result_t *from,
                               uint8_t *ack_out, uint8_t *ack_len)
{
    const c4_link_result_t r = c4_link_proto_recv(to, from->tx, from->tx_len);
    if (ack_out && ack_len) {
        memcpy(ack_out, r.tx, r.tx_len);
        *ack_len = r.tx_len;
    }
    return r;
}

static void test_hello_handshake(void)
{
    reset_pair();

    const uint8_t hello_a[3] = { C4_LINK_PROTO_VERSION, 1 /* P1 */, 1 /* game 1 */ };
    c4_link_result_t out = c4_link_proto_send(&a, C4_LINK_TYPE_HELLO, hello_a, 3);
    assert(out.tx_len == 8);
    assert(c4_link_proto_busy(&a));

    uint8_t ack[8];
    uint8_t ack_len = 0;
    const c4_link_result_t got = deliver(&b, &out, ack, &ack_len);
    assert(got.ev.kind == C4_LINK_EV_PEER_READY);
    assert(got.ev.first_side == 1);
    assert(got.ev.game_id == 1);
    assert(b.peer_ready);
    assert(ack_len == 5);                     // 纯确认帧没有负载

    // 确认回到发送方:待确认包被清掉,不再重传。
    const c4_link_result_t ack_at_a = c4_link_proto_recv(&a, ack, ack_len);
    assert(ack_at_a.ev.kind == C4_LINK_EV_NONE);
    assert(ack_at_a.tx_len == 0);             // 纯确认帧不需要再回确认
    assert(!c4_link_proto_busy(&a));
    assert(!c4_link_proto_busy(&a));
}

static void test_dedup_and_ack(void)
{
    reset_pair();

    const uint8_t hello[3] = { C4_LINK_PROTO_VERSION, 2, 3 };
    c4_link_result_t out = c4_link_proto_send(&a, C4_LINK_TYPE_HELLO, hello, 3);
    uint8_t ack[8];
    uint8_t ack_len = 0;
    (void)deliver(&b, &out, ack, &ack_len);

    // 对端没收到确认时的重传:接收方必须只交付一次,且仍然回确认。
    const c4_link_result_t again = c4_link_proto_recv(&b, out.tx, out.tx_len);
    assert(again.ev.kind == C4_LINK_EV_NONE);
    assert(again.tx_len == 5);

    const c4_link_result_t move = c4_link_proto_send(&a, C4_LINK_TYPE_MOVE, (const uint8_t[]){ 4, 2 }, 2);
    const c4_link_result_t at_b = c4_link_proto_recv(&b, move.tx, move.tx_len);
    assert(at_b.ev.kind == C4_LINK_EV_MOVE);
    assert(at_b.ev.col == 4);
    assert(at_b.ev.ply == 2);

    // 同一手重复到达:不重复交付。
    const c4_link_result_t dup = c4_link_proto_recv(&b, move.tx, move.tx_len);
    assert(dup.ev.kind == C4_LINK_EV_NONE);
    assert(dup.tx_len == 5);
}

static void test_retransmit_and_loss(void)
{
    reset_pair();

    const uint8_t hello[3] = { C4_LINK_PROTO_VERSION, 1, 1 };
    const c4_link_result_t out = c4_link_proto_send(&a, C4_LINK_TYPE_HELLO, hello, 3);
    assert(out.tx_len == 8);

    // 400ms 之前什么都不做。
    c4_link_result_t tick = c4_link_proto_tick(&a, C4_LINK_RETRY_MS - 1);
    assert(tick.tx_len == 0 && tick.ev.kind == C4_LINK_EV_NONE);

    // 到点重传同一帧(序号不变,接收方才能识别成重复)。
    tick = c4_link_proto_tick(&a, 1);
    assert(tick.tx_len == out.tx_len);
    assert(memcmp(tick.tx, out.tx, out.tx_len) == 0);
    assert(a.retries == 1);

    // 一直收不到确认:重传耗尽后报 LOST 并清掉待确认包。
    for (uint8_t i = 0; i < C4_LINK_MAX_RETRIES; i++) {
        tick = c4_link_proto_tick(&a, C4_LINK_RETRY_MS);
    }
    assert(tick.tx_len == 0);
    assert(tick.ev.kind == C4_LINK_EV_LOST);
    assert(!c4_link_proto_busy(&a));

    // 耗尽之后再 tick 不应重复报错。
    tick = c4_link_proto_tick(&a, 4 * C4_LINK_RETRY_MS);
    assert(tick.tx_len == 0 && tick.ev.kind == C4_LINK_EV_NONE);
}

static void test_out_of_order_is_ignored(void)
{
    reset_pair();

    // 序号 1 先到(期望 0):不交付、不确认,让对端按超时重传。
    uint8_t frame[8] = { 0 };
    frame[0] = C4_LINK_PROTO_MAGIC;
    frame[1] = C4_LINK_PROTO_VERSION;
    frame[2] = C4_LINK_TYPE_MOVE;
    frame[3] = (uint8_t)(0x80 | 1);
    frame[4] = 0;
    frame[5] = 2;
    frame[6] = 1;

    const c4_link_result_t r = c4_link_proto_recv(&b, frame, 7);
    assert(r.ev.kind == C4_LINK_EV_NONE);
    assert(r.tx_len == 0);
    assert(!b.have_peer_seq);

    // 序号 0 到达:正常交付。
    frame[3] = 0x80 | 0;
    const c4_link_result_t ok = c4_link_proto_recv(&b, frame, 7);
    assert(ok.ev.kind == C4_LINK_EV_MOVE);
    assert(ok.tx_len == 5);
    assert(b.have_peer_seq);
}

static void test_bad_frames(void)
{
    reset_pair();

    uint8_t frame[8] = { 0 };
    frame[0] = 0x00;                       // 魔数不对
    frame[1] = C4_LINK_PROTO_VERSION;
    frame[3] = 0x80 | 0;
    frame[4] = 0;
    const c4_link_result_t bad_magic = c4_link_proto_recv(&b, frame, 5);
    assert(bad_magic.tx_len == 0 && bad_magic.ev.kind == C4_LINK_EV_NONE);

    // 长度过载/过短
    uint8_t long_frame[16] = { C4_LINK_PROTO_MAGIC, C4_LINK_PROTO_VERSION, 0, 0, 0 };
    c4_link_result_t r = c4_link_proto_recv(&b, long_frame, sizeof(long_frame));
    assert(r.tx_len == 0 && r.ev.kind == C4_LINK_EV_NONE);
    r = c4_link_proto_recv(&b, long_frame, 4);
    assert(r.tx_len == 0);

    // 负载长度与类型不匹配
    uint8_t move_frame[6] = { C4_LINK_PROTO_MAGIC, C4_LINK_PROTO_VERSION,
                              C4_LINK_TYPE_MOVE, 0x80 | 0, 0, 3 };
    r = c4_link_proto_recv(&b, move_frame, 6);
    assert(r.tx_len == 0 && r.ev.kind == C4_LINK_EV_NONE);
    assert(!b.have_peer_seq);              // 坏包不能消耗序号
}

static void test_version_mismatch(void)
{
    reset_pair();

    uint8_t frame[8] = { C4_LINK_PROTO_MAGIC, C4_LINK_PROTO_VERSION + 1,
                         C4_LINK_TYPE_HELLO, 0x80 | 0, 0, 9, 1, 1 };
    const c4_link_result_t r = c4_link_proto_recv(&b, frame, 8);
    assert(r.ev.kind == C4_LINK_EV_BAD_VERSION);
    assert(r.ev.version == C4_LINK_PROTO_VERSION + 1);
    assert(r.tx_len == 5);                  // 仍然回确认,免得对端一直重传
    assert(!b.peer_ready);
}

static void test_stop_and_wait_replaces_pending(void)
{
    reset_pair();

    const c4_link_result_t first = c4_link_proto_send(&a, C4_LINK_TYPE_MOVE, (const uint8_t[]){ 0, 1 }, 2);
    const c4_link_result_t second = c4_link_proto_send(&a, C4_LINK_TYPE_MOVE, (const uint8_t[]){ 1, 2 }, 2);
    assert(first.tx[3] != second.tx[3]);    // 序号推进

    // 旧包的确认不应清掉新包的待确认状态。
    uint8_t ack_old[8] = { C4_LINK_PROTO_MAGIC, C4_LINK_PROTO_VERSION, 0, 0,
                           (uint8_t)(0x80 | (first.tx[3] & 0x7F)) };
    (void)c4_link_proto_recv(&a, ack_old, 5);
    assert(c4_link_proto_busy(&a));

    uint8_t ack_new[8] = { C4_LINK_PROTO_MAGIC, C4_LINK_PROTO_VERSION, 0, 0,
                           (uint8_t)(0x80 | (second.tx[3] & 0x7F)) };
    (void)c4_link_proto_recv(&a, ack_new, 5);
    assert(!c4_link_proto_busy(&a));
}

static void test_rematch_and_leave(void)
{
    reset_pair();

    const c4_link_result_t rematch = c4_link_proto_send(&a, C4_LINK_TYPE_REMATCH, (const uint8_t[]){ 7 }, 1);
    const c4_link_result_t at_b = c4_link_proto_recv(&b, rematch.tx, rematch.tx_len);
    assert(at_b.ev.kind == C4_LINK_EV_REMATCH);
    assert(at_b.ev.game_id == 7);

    const c4_link_result_t leave = c4_link_proto_send(&a, C4_LINK_TYPE_LEAVE, NULL, 0);
    assert(leave.tx_len == 5);
    const c4_link_result_t leave_at_b = c4_link_proto_recv(&b, leave.tx, leave.tx_len);
    assert(leave_at_b.ev.kind == C4_LINK_EV_LEAVE);

    // 负载长度写错时干脆不发,避免对端收到语义不明的帧。
    const c4_link_result_t bad = c4_link_proto_send(&a, C4_LINK_TYPE_LEAVE, (const uint8_t[]){ 1 }, 1);
    assert(bad.tx_len == 0);
}

static void test_seq_wraps(void)
{
    reset_pair();

    // 序号是 7 位:127 之后回到 0,两边仍然能对上。
    a.next_seq = 127;
    const c4_link_result_t last = c4_link_proto_send(&a, C4_LINK_TYPE_LEAVE, NULL, 0);
    assert((last.tx[3] & 0x7F) == 127);
    assert(a.next_seq == 0);

    const c4_link_result_t next = c4_link_proto_send(&a, C4_LINK_TYPE_LEAVE, NULL, 0);
    assert((next.tx[3] & 0x7F) == 0);
}

int main(void)
{
    test_hello_handshake();
    test_dedup_and_ack();
    test_retransmit_and_loss();
    test_out_of_order_is_ignored();
    test_bad_frames();
    test_version_mismatch();
    test_stop_and_wait_replaces_pending();
    test_rematch_and_leave();
    test_seq_wraps();

    printf("test_c4_link_proto: PASS\n");
    return 0;
}
