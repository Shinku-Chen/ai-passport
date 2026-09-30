// tests/test_oc_tts.c —— TTS 下行播放纯逻辑的主机测试(帧解析 / 队列溢出 / 统计计数)。
//
// 覆盖四个真机上最难复现的场景:
//   1. 队列满时丢的是"最旧"的包(用户等的是最新合成出来的那句),且 dropped 只加一次;
//   2. SEQ 缺口计入 dropped、重复序号不计丢、1 字节序号回绕不产生假缺口;
//   3. 非法载荷(长度/速率/帧长)一律拒收,不会进队列;
//   4. 打断(flush)要清空队列并把没播出去的如实计入 dropped,统计在新一轮清零。
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "oc_tts.h"

// 组装一个 TTS_OPUS 载荷:[SEQ][rate_khz][frame_ms] + 包。
static size_t make_payload(uint8_t *buf, size_t cap, uint8_t seq, uint8_t rate_khz,
                           uint8_t frame_ms, uint8_t fill, size_t opus_len)
{
    assert(cap >= OC_TTS_OPUS_HEADER + opus_len);
    buf[0] = seq;
    buf[1] = rate_khz;
    buf[2] = frame_ms;
    for (size_t i = 0; i < opus_len; i++) {
        buf[OC_TTS_OPUS_HEADER + i] = (uint8_t)(fill + i);
    }
    return OC_TTS_OPUS_HEADER + opus_len;
}

static void test_parse(void)
{
    uint8_t buf[OC_TTS_OPUS_FRAME_MAX + 8];
    oc_tts_packet_t pkt;

    // 正常:3 字节头被剥掉,Opus 包与速率按协议解释
    size_t n = make_payload(buf, sizeof(buf), 7, 24, 60, 0x10, 100);
    assert(oc_tts_parse(buf, n, &pkt) == OC_TTS_PARSE_OK);
    assert(pkt.seq == 7 && pkt.rate_khz == 24 && pkt.opus_len == 100);
    assert(pkt.opus == buf + OC_TTS_OPUS_HEADER);
    assert(pkt.opus[0] == 0x10 && pkt.opus[99] == (uint8_t)(0x10 + 99));

    // 16kHz 同样合法;刚好 1 字节与刚好 512 字节的包都要收
    assert(oc_tts_parse(buf, make_payload(buf, sizeof(buf), 0, 16, 60, 1, 1), &pkt) == OC_TTS_PARSE_OK);
    assert(pkt.opus_len == 1);
    n = make_payload(buf, sizeof(buf), 0, 16, 60, 1, OC_TTS_PACKET_MAX);
    assert(n == OC_TTS_OPUS_FRAME_MAX && oc_tts_parse(buf, n, &pkt) == OC_TTS_PARSE_OK);
    assert(pkt.opus_len == OC_TTS_PACKET_MAX);

    // 长度边界:3 字节(只有头)与 516 字节(包超上限)都拒收
    assert(oc_tts_parse(buf, 3, &pkt) == OC_TTS_PARSE_ERR_LEN);
    assert(oc_tts_parse(buf, 0, &pkt) == OC_TTS_PARSE_ERR_LEN);
    assert(oc_tts_parse(buf, OC_TTS_OPUS_FRAME_MAX + 1u, &pkt) == OC_TTS_PARSE_ERR_LEN);
    assert(oc_tts_parse(NULL, 4, &pkt) == OC_TTS_PARSE_ERR_LEN);

    // 速率只认 16/24
    n = make_payload(buf, sizeof(buf), 0, 8, 60, 1, 10);
    assert(oc_tts_parse(buf, n, &pkt) == OC_TTS_PARSE_ERR_RATE);
    n = make_payload(buf, sizeof(buf), 0, 48, 60, 1, 10);
    assert(oc_tts_parse(buf, n, &pkt) == OC_TTS_PARSE_ERR_RATE);
    n = make_payload(buf, sizeof(buf), 0, 0, 60, 1, 10);
    assert(oc_tts_parse(buf, n, &pkt) == OC_TTS_PARSE_ERR_RATE);

    // 帧长只认 60ms(本设备只实现一种帧长,不猜)
    n = make_payload(buf, sizeof(buf), 0, 16, 20, 1, 10);
    assert(oc_tts_parse(buf, n, &pkt) == OC_TTS_PARSE_ERR_FRAME_MS);
    n = make_payload(buf, sizeof(buf), 0, 16, 40, 1, 10);
    assert(oc_tts_parse(buf, n, &pkt) == OC_TTS_PARSE_ERR_FRAME_MS);

    // out 可为空:只做校验
    n = make_payload(buf, sizeof(buf), 0, 16, 60, 1, 10);
    assert(oc_tts_parse(buf, n, NULL) == OC_TTS_PARSE_OK);
}

static void test_fifo_order_and_depth(void)
{
    oc_tts_queue_t q;
    oc_tts_queue_init(&q);
    assert(oc_tts_queue_empty(&q) && oc_tts_queue_depth(&q) == 0);

    uint8_t pkt[64];
    for (uint32_t i = 0; i < OC_TTS_QUEUE_DEPTH; i++) {
        memset(pkt, (int)(i + 1), sizeof(pkt));
        assert(oc_tts_queue_push(&q, (uint8_t)i, 16, pkt, 40));
    }
    assert(oc_tts_queue_depth(&q) == OC_TTS_QUEUE_DEPTH);
    assert(q.overflow == 0 && q.dropped == 0);
    assert(q.frames == OC_TTS_QUEUE_DEPTH);

    // 先进先出:第一个出队的还是第 0 个包
    uint8_t out[OC_TTS_PACKET_MAX];
    size_t len = 0;
    uint8_t rate = 0;
    assert(oc_tts_queue_pop(&q, out, sizeof(out), &len, &rate));
    assert(len == 40 && rate == 16 && out[0] == 1);

    // 空队列出队不改变任何东西
    while (oc_tts_queue_pop(&q, out, sizeof(out), &len, &rate)) {
    }
    assert(oc_tts_queue_empty(&q));
    assert(!oc_tts_queue_pop(&q, out, sizeof(out), &len, &rate) && len == 0);
}

static void test_overflow_drops_oldest(void)
{
    oc_tts_queue_t q;
    oc_tts_queue_init(&q);

    uint8_t pkt[16];
    // 连续推 DEPTH+1 个包:最旧的必须被丢掉,留下的是"最新"的 DEPTH 个
    for (uint32_t i = 0; i <= OC_TTS_QUEUE_DEPTH; i++) {
        memset(pkt, (int)(i + 1), sizeof(pkt));
        assert(oc_tts_queue_push(&q, (uint8_t)i, 16, pkt, sizeof(pkt)));
    }
    assert(oc_tts_queue_depth(&q) == OC_TTS_QUEUE_DEPTH);
    assert(q.overflow == 1 && q.dropped == 1);
    assert(q.frames == OC_TTS_QUEUE_DEPTH + 1);

    uint8_t out[OC_TTS_PACKET_MAX];
    size_t len = 0;
    uint8_t rate = 0;
    assert(oc_tts_queue_pop(&q, out, sizeof(out), &len, &rate));
    assert(out[0] == 2);   // 1 号(最旧)已被丢掉,队首是 2 号
    // 队尾是最新那个
    uint32_t remaining = OC_TTS_QUEUE_DEPTH - 1u;
    for (uint32_t i = 0; i < remaining; i++) {
        assert(oc_tts_queue_pop(&q, out, sizeof(out), &len, &rate));
    }
    assert(out[0] == (uint8_t)(OC_TTS_QUEUE_DEPTH + 1));
    assert(oc_tts_queue_empty(&q));

    // 再溢出两次:计数按次累加(不重复累加)
    oc_tts_queue_reset(&q);
    for (uint32_t i = 0; i <= OC_TTS_QUEUE_DEPTH + 1u; i++) {
        assert(oc_tts_queue_push(&q, (uint8_t)i, 24, pkt, sizeof(pkt)));
    }
    assert(q.overflow == 2 && q.dropped == 2);
}

static void test_seq_accounting(void)
{
    oc_tts_queue_t q;
    oc_tts_queue_init(&q);

    uint8_t pkt[8] = { 0 };
    assert(oc_tts_queue_push(&q, 0, 16, pkt, sizeof(pkt)));
    assert(q.dropped == 0);
    // 丢了 1、2 两帧
    assert(oc_tts_queue_push(&q, 3, 16, pkt, sizeof(pkt)));
    assert(q.dropped == 2);
    // 重复序号:不计丢、不计收
    assert(oc_tts_queue_push(&q, 3, 16, pkt, sizeof(pkt)));
    assert(q.dropped == 2);

    // 只缺 1 帧
    oc_tts_queue_reset(&q);
    assert(oc_tts_queue_push(&q, 0, 16, pkt, sizeof(pkt)));
    assert(oc_tts_queue_push(&q, 2, 16, pkt, sizeof(pkt)));
    assert(q.dropped == 1);

    // 1 字节序号回绕 255 → 0:不产生假缺口
    oc_tts_queue_reset(&q);
    assert(oc_tts_queue_push(&q, 254, 16, pkt, sizeof(pkt)));
    assert(oc_tts_queue_push(&q, 255, 16, pkt, sizeof(pkt)));
    assert(oc_tts_queue_push(&q, 0, 16, pkt, sizeof(pkt)));
    assert(q.dropped == 0);
}

static void test_discard_and_stats(void)
{
    oc_tts_queue_t q;
    oc_tts_queue_init(&q);

    uint8_t pkt[8] = { 0 };
    for (uint32_t i = 0; i < 5; i++) {
        assert(oc_tts_queue_push(&q, (uint8_t)i, 16, pkt, sizeof(pkt)));
    }
    oc_tts_queue_note_decode(&q, 900);
    oc_tts_queue_note_decode(&q, 1500);
    oc_tts_queue_note_decode(&q, 700);
    oc_tts_queue_note_underrun(&q);
    oc_tts_queue_note_underrun(&q);

    // 打断:队列里剩的 5 个包全部计入 dropped
    assert(oc_tts_queue_discard(&q) == 5);
    assert(oc_tts_queue_empty(&q));
    assert(oc_tts_queue_discard(&q) == 0);   // 幂等

    oc_tts_play_stats_t st = { 0 };
    oc_tts_queue_stats(&q, &st);
    assert(st.frames == 5 && st.decoded == 3 && st.dropped == 5);
    assert(st.underruns == 2 && st.decode_us_max == 1500);

    // 新一轮:统计清零、序号跟踪复位
    oc_tts_queue_reset(&q);
    oc_tts_queue_stats(&q, &st);
    assert(st.frames == 0 && st.decoded == 0 && st.dropped == 0 && st.underruns == 0);
    assert(st.decode_us_max == 0);
    assert(!q.seq.started);
}

static void test_reject_bad_push(void)
{
    oc_tts_queue_t q;
    oc_tts_queue_init(&q);
    uint8_t pkt[OC_TTS_PACKET_MAX + 1];

    assert(!oc_tts_queue_push(&q, 0, 16, NULL, 4));
    assert(!oc_tts_queue_push(&q, 0, 16, pkt, 0));
    // 超过单包上限的载荷不该进队(协议里长度非法应在解析阶段被拒)
    assert(!oc_tts_queue_push(&q, 0, 16, pkt, sizeof(pkt)));
    assert(oc_tts_queue_empty(&q) && q.frames == 0);

    // 出队缓冲太小:包被丢弃并计数,队列不卡死
    assert(oc_tts_queue_push(&q, 0, 16, pkt, 100));
    assert(oc_tts_queue_push(&q, 1, 16, pkt, 100));
    uint8_t small[8];
    size_t len = 123;
    uint8_t rate = 9;
    assert(!oc_tts_queue_pop(&q, small, sizeof(small), &len, &rate) && len == 0);
    assert(q.dropped == 1);
    assert(oc_tts_queue_depth(&q) == 1);   // 下一个包仍可取出
    uint8_t out[OC_TTS_PACKET_MAX];
    assert(oc_tts_queue_pop(&q, out, sizeof(out), &len, &rate) && len == 100 && rate == 16);
}

int main(void)
{
    test_parse();
    test_fifo_order_and_depth();
    test_overflow_drops_oldest();
    test_seq_accounting();
    test_discard_and_stats();
    test_reject_bad_push();
    return 0;
}
