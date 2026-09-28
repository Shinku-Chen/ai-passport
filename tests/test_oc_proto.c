// tests/test_oc_proto.c —— 对讲机线协议纯逻辑的主机测试(分帧/重组/文本合并/丢帧统计)。
//
// 覆盖的三个真实故障场景:
//   1. 丢一次 notify 后,接收端必须靠 magic 重新对齐,而不是把载荷当成帧头卡死;
//   2. 固件会在音频帧中途插入 EVENT 帧,接收端必须丢弃半截音频帧并切到新帧;
//   3. 长文本分片必须合并成一条,且截断不能落在多字节 UTF-8 字符中间。
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "oc_proto.h"

typedef struct {
    uint8_t type;
    uint8_t flags;
    size_t len;
    uint8_t data[OC_PAYLOAD_MAX];
    int count;
} capture_t;

static void on_frame(uint8_t type, uint8_t flags, const uint8_t *payload, size_t len, void *ctx)
{
    capture_t *cap = (capture_t *)ctx;
    assert(len <= sizeof(cap->data));
    cap->type = type;
    cap->flags = flags;
    cap->len = len;
    if (len != 0) {
        memcpy(cap->data, payload, len);
    }
    cap->count++;
}

static void test_encode(void)
{
    uint8_t out[OC_FRAME_MAX];
    const uint8_t payload[3] = { 0x11, 0x22, 0x33 };

    size_t n = oc_encode(out, sizeof(out), OC_FRAME_TEXT, OC_FLAG_FIRST | OC_FLAG_MORE, payload, sizeof(payload));
    assert(n == OC_HEADER_SIZE + 3);
    assert(out[0] == OC_MAGIC0 && out[1] == OC_MAGIC1);
    assert(out[2] == OC_FRAME_TEXT);
    assert(out[3] == (OC_FLAG_FIRST | OC_FLAG_MORE));
    assert(out[4] == 0x00 && out[5] == 0x03);  // 大端长度
    assert(out[6] == 0x11 && out[8] == 0x33);

    // 长度按类型区分:Opus 帧 513B 超限,PCM 1024B 合法
    uint8_t big[OC_TEXT_PAYLOAD_MAX];
    memset(big, 0x5A, sizeof(big));
    assert(oc_encode(out, sizeof(out), OC_FRAME_AUDIO_OPUS, 0, big, 513) == 0);
    assert(oc_encode(out, sizeof(out), OC_FRAME_AUDIO_OPUS, 0, big, 512) == OC_HEADER_SIZE + 512);
    assert(oc_encode(out, sizeof(out), OC_FRAME_AUDIO_PCM, 0, big, 1024) == OC_HEADER_SIZE + 1024);
    assert(oc_encode(out, sizeof(out), OC_FRAME_TEXT, 0, big, OC_TEXT_PAYLOAD_MAX) ==
           OC_HEADER_SIZE + OC_TEXT_PAYLOAD_MAX);

    // 未知类型 / 缓冲不足 / 空指针
    assert(oc_encode(out, sizeof(out), 0x7F, 0, payload, 1) == 0);
    assert(oc_encode(out, OC_HEADER_SIZE, OC_FRAME_TEXT, 0, payload, 1) == 0);
    assert(oc_encode(NULL, sizeof(out), OC_FRAME_TEXT, 0, payload, 1) == 0);
    assert(oc_encode(out, sizeof(out), OC_FRAME_TEXT, 0, NULL, 1) == 0);

    // 类型与上限查询
    assert(oc_type_known(OC_FRAME_AUDIO_OPUS));
    assert(!oc_type_known(0x00));
    assert(oc_payload_limit(0x00) == 0);
    assert(oc_payload_limit(OC_FRAME_EVENT) == OC_JSON_PAYLOAD_MAX);
}

static void test_single_frame_and_split(void)
{
    uint8_t frame[64];
    const uint8_t body[4] = { 'A', 'h', 'o', 'i' };
    size_t flen = oc_encode(frame, sizeof(frame), OC_FRAME_TEXT, OC_FLAG_LAST, body, sizeof(body));
    assert(flen != 0);

    capture_t cap;
    oc_reassembler_t rx;

    // ① 一次推入
    memset(&cap, 0, sizeof(cap));
    oc_reassembler_init(&rx, on_frame, &cap);
    oc_reassembler_push(&rx, frame, flen);
    assert(cap.count == 1);
    assert(cap.type == OC_FRAME_TEXT && cap.flags == OC_FLAG_LAST && cap.len == sizeof(body));
    assert(memcmp(cap.data, body, sizeof(body)) == 0);

    // ② 逐字节推入(等价于任意 MTU 下的分片)
    memset(&cap, 0, sizeof(cap));
    oc_reassembler_init(&rx, on_frame, &cap);
    for (size_t i = 0; i < flen; i++) {
        oc_reassembler_push(&rx, frame + i, 1);
    }
    assert(cap.count == 1);
    assert(cap.len == sizeof(body) && memcmp(cap.data, body, sizeof(body)) == 0);

    // ③ 一格里两帧
    uint8_t two[128];
    size_t a = oc_encode(two, sizeof(two), OC_FRAME_EVENT, 0, (const uint8_t *)"{\"ev\":\"turn_start\"}", 19);
    size_t b = oc_encode(two + a, sizeof(two) - a, OC_FRAME_EVENT, 0, (const uint8_t *)"x", 1);
    memset(&cap, 0, sizeof(cap));
    oc_reassembler_init(&rx, on_frame, &cap);
    oc_reassembler_push(&rx, two, a + b);
    assert(cap.count == 2);
    assert(cap.len == 1);  // 第二次回调覆盖,留下的是后一帧
}

static void test_resync_after_garbage(void)
{
    uint8_t frame[32];
    size_t flen = oc_encode(frame, sizeof(frame), OC_FRAME_EVENT, 0, (const uint8_t *)"ok", 2);
    assert(flen != 0);

    capture_t cap;
    oc_reassembler_t rx;
    memset(&cap, 0, sizeof(cap));
    oc_reassembler_init(&rx, on_frame, &cap);

    // ① 先灌入与 magic 无关的垃圾(PCM 里丢失头部后的典型情况)
    const uint8_t junk[9] = { 0x01, 0x02, 0x03, 0xA5, 0x00, 0xFF, 0x5A, 0x07, 0x07 };
    oc_reassembler_push(&rx, junk, sizeof(junk));
    assert(cap.count == 0);

    // ② magic 对但 type 未知 → 继续丢字节,不能当成帧
    const uint8_t fake[OC_HEADER_SIZE] = { 0xA5, 0x5A, 0x7F, 0x00, 0x00, 0x02 };
    oc_reassembler_push(&rx, fake, sizeof(fake));
    assert(cap.count == 0);

    // ③ 真正的帧仍然能解出来
    oc_reassembler_push(&rx, frame, flen);
    assert(cap.count == 1);
    assert(cap.type == OC_FRAME_EVENT && cap.len == 2 && cap.data[0] == 'o' && cap.data[1] == 'k');
}

static void test_resync_after_oversized_length(void)
{
    capture_t cap;
    oc_reassembler_t rx;
    memset(&cap, 0, sizeof(cap));
    oc_reassembler_init(&rx, on_frame, &cap);

    // 声明长度超过该类型上限(2049 > 2048):必须按错位处理,继续找下一个帧头
    const uint8_t bogus[OC_HEADER_SIZE] = { 0xA5, 0x5A, OC_FRAME_TEXT, 0x04, 0x08, 0x01 };
    oc_reassembler_push(&rx, bogus, sizeof(bogus));
    assert(cap.count == 0);

    uint8_t frame[32];
    size_t flen = oc_encode(frame, sizeof(frame), OC_FRAME_TEXT, 0, (const uint8_t *)"\x02hi", 3);
    oc_reassembler_push(&rx, frame, flen);
    assert(cap.count == 1);
    assert(cap.len == 3 && cap.data[0] == 0x02);
}

static void test_inserted_frame_mid_audio(void)
{
    // 造一个 Opus 音频帧,只发前半截,然后插一个 EVENT,再发下一帧。
    // 固件在音频帧发送中途插入 EVENT 时就是这种字节序列。
    const size_t audio_payload = 400;
    uint8_t audio[OC_HEADER_SIZE + 400];
    uint8_t opus[400];
    for (size_t i = 0; i < sizeof(opus); i++) {
        opus[i] = (uint8_t)(i & 0xFFu);
    }
    opus[0] = 0x07;  // SEQ
    size_t alen = oc_encode(audio, sizeof(audio), OC_FRAME_AUDIO_OPUS, 0, opus, audio_payload);
    assert(alen == OC_HEADER_SIZE + audio_payload);

    uint8_t event[32];
    size_t elen = oc_encode(event, sizeof(event), OC_FRAME_EVENT, 0, (const uint8_t *)"{\"ev\":\"x\"}", 10);
    assert(elen != 0);

    capture_t cap;
    oc_reassembler_t rx;
    memset(&cap, 0, sizeof(cap));
    oc_reassembler_init(&rx, on_frame, &cap);

    oc_reassembler_push(&rx, audio, 300);          // 音频帧前半截
    assert(cap.count == 0);
    oc_reassembler_push(&rx, event, elen);         // 中途插入的 EVENT
    assert(cap.count == 1);
    assert(cap.type == OC_FRAME_EVENT && cap.len == 10);

    // 剩下的半截音频帧作废;紧随其后的完整音频帧不受影响
    oc_reassembler_push(&rx, audio + 300, alen - 300);
    assert(cap.count == 1);
    oc_reassembler_push(&rx, audio, alen);
    assert(cap.count == 2);
    assert(cap.type == OC_FRAME_AUDIO_OPUS && cap.len == audio_payload);
    assert(cap.data[0] == 0x07);
}

static void test_audio_payload_is_not_cut_as_nested_audio(void)
{
    capture_t cap;
    oc_reassembler_t rx;
    memset(&cap, 0, sizeof(cap));
    oc_reassembler_init(&rx, on_frame, &cap);

    // 音频载荷里嵌入一个"看起来像音频帧头"的字节序列:不应当作边界切开。
    uint8_t pcm[64];
    memset(pcm, 0x11, sizeof(pcm));
    pcm[0] = 0x01;
    const uint8_t nested[OC_HEADER_SIZE] = { 0xA5, 0x5A, OC_FRAME_AUDIO_PCM, 0x00, 0x00, 0x10 };
    memcpy(pcm + 20, nested, sizeof(nested));

    uint8_t frame[OC_HEADER_SIZE + sizeof(pcm)];
    size_t flen = oc_encode(frame, sizeof(frame), OC_FRAME_AUDIO_PCM, 0, pcm, sizeof(pcm));
    assert(flen != 0);

    oc_reassembler_push(&rx, frame, 30);
    oc_reassembler_push(&rx, frame + 30, flen - 30);
    assert(cap.count == 1);
    assert(cap.len == sizeof(pcm));
    assert(memcmp(cap.data, pcm, sizeof(pcm)) == 0);
}

static void test_text_merge(void)
{
    oc_text_merge_t m;
    oc_text_merge_init(&m);

    // 单帧消息(无标志位)= 完整
    assert(oc_text_merge_push(&m, 0, (const uint8_t *)"Ahi", 3));
    assert(strcmp(m.text, "hi") == 0 && m.role == 'A');

    // FIRST / MORE / LAST 合并
    oc_text_merge_init(&m);
    assert(!oc_text_merge_push(&m, OC_FLAG_FIRST, (const uint8_t *)"Uhe", 3));
    assert(!oc_text_merge_push(&m, OC_FLAG_MORE, (const uint8_t *)"Ullo", 4));
    assert(oc_text_merge_push(&m, OC_FLAG_LAST, (const uint8_t *)"U!", 2));
    assert(strcmp(m.text, "hello!") == 0 && m.role == 'U');

    // 上一条已结束后,没有 FIRST 的续帧按新消息开头处理,内容不串条
    oc_text_merge_init(&m);
    assert(oc_text_merge_push(&m, 0, (const uint8_t *)"Adone", 5));
    assert(!oc_text_merge_push(&m, OC_FLAG_MORE, (const uint8_t *)"Astray", 6));
    assert(strcmp(m.text, "stray") == 0);

    // 单帧完整消息打断未完成的分片(barge:新回复覆盖旧回复)
    oc_text_merge_init(&m);
    assert(!oc_text_merge_push(&m, OC_FLAG_FIRST, (const uint8_t *)"Aaa", 3));
    assert(oc_text_merge_push(&m, 0, (const uint8_t *)"Bbb", 3));
    assert(strcmp(m.text, "bb") == 0 && m.role == 'B');

    // 溢出与 UTF-8 边界:缓冲只剩 1 字节时,三字节字符整体丢弃而不是切一半
    oc_text_merge_init(&m);
    uint8_t big[OC_TEXT_PAYLOAD_MAX];
    big[0] = 'A';
    memset(big + 1, 'x', sizeof(big) - 1);          // 2047 个 'x'
    assert(!oc_text_merge_push(&m, OC_FLAG_FIRST, big, sizeof(big)));
    assert(strlen(m.text) == OC_TEXT_PAYLOAD_MAX - 1);
    const uint8_t han[4] = { 'A', 0xE4, 0xB8, 0xAD };  // role + "中"
    assert(oc_text_merge_push(&m, OC_FLAG_LAST, han, sizeof(han)));
    assert(m.overflow);
    assert(strlen(m.text) == OC_TEXT_PAYLOAD_MAX - 1);
    assert(m.text[OC_TEXT_PAYLOAD_MAX - 2] == 'x');
    size_t n = strlen(m.text);
    assert(oc_utf8_safe_len((const uint8_t *)m.text, n) == n);
}

static void test_utf8_safe_len(void)
{
    // "中" = E4 B8 AD
    const uint8_t ok[3] = { 0xE4, 0xB8, 0xAD };
    assert(oc_utf8_safe_len(ok, 3) == 3);
    assert(oc_utf8_safe_len(ok, 2) == 0);   // 半截字符整段丢弃
    assert(oc_utf8_safe_len(ok, 1) == 0);
    const uint8_t mixed[5] = { 'a', 0xE4, 0xB8, 0xAD, 'b' };
    assert(oc_utf8_safe_len(mixed, 4) == 4);
    assert(oc_utf8_safe_len(mixed, 3) == 1);  // 只保留 'a'
    assert(oc_utf8_safe_len(mixed, 0) == 0);
    assert(oc_utf8_safe_len(NULL, 3) == 0);
}

static void test_seq_tracker(void)
{
    oc_seq_tracker_t t;
    oc_seq_init(&t);

    oc_seq_push(&t, 10);   // 起点
    assert(t.received == 1 && oc_seq_lost(&t) == 0);
    oc_seq_push(&t, 11);
    assert(t.received == 2 && oc_seq_lost(&t) == 0);
    oc_seq_push(&t, 11);   // 重复:不计收也不计丢
    assert(t.received == 2 && oc_seq_lost(&t) == 0);
    oc_seq_push(&t, 14);   // 丢 12、13
    assert(t.received == 3 && oc_seq_lost(&t) == 2);
    oc_seq_push(&t, 15);
    assert(oc_seq_lost(&t) == 2);
    oc_seq_push(&t, 250);  // 15 → 250 之间丢 234 帧
    assert(t.received == 5 && oc_seq_lost(&t) == 2 + 234);
    oc_seq_push(&t, 251);
    assert(oc_seq_lost(&t) == 2 + 234);
}

static void test_reassembler_reset(void)
{
    capture_t cap;
    oc_reassembler_t rx;
    memset(&cap, 0, sizeof(cap));
    oc_reassembler_init(&rx, on_frame, &cap);

    uint8_t frame[32];
    size_t flen = oc_encode(frame, sizeof(frame), OC_FRAME_EVENT, 0, (const uint8_t *)"hello", 5);
    assert(flen != 0);

    oc_reassembler_push(&rx, frame, 4);   // 只有半个帧头
    oc_reassembler_reset(&rx);            // 断连:半截帧必须被丢弃
    oc_reassembler_push(&rx, frame + 2, flen - 2);
    assert(cap.count == 0);               // 剩下的是半截,不足以成帧

    oc_reassembler_push(&rx, frame, flen);
    assert(cap.count == 1 && cap.len == 5);
}

int main(void)
{
    test_encode();
    test_single_frame_and_split();
    test_resync_after_garbage();
    test_resync_after_oversized_length();
    test_inserted_frame_mid_audio();
    test_audio_payload_is_not_cut_as_nested_audio();
    test_text_merge();
    test_utf8_safe_len();
    test_seq_tracker();
    test_reassembler_reset();
    return 0;
}
