// main/dracu_inflate.c —— 自带的非包装 inflate(DEFLATE / zlib)。
//
// 设计前提:整段输出都装得进调用方给的缓冲 —— 我们的剧本小块(<=3 KB)、立绘分块(<=3 KB)
// 和补丁掩码(<=10 KB)都满足。于是:
//   * 不需要 32 KB 滑窗/字典(回溯距离直接落在已写出的输出里);
//   * 状态只有几十字节,不占栈、不占堆,不依赖 ROM/组件里的任何第三方解压实现。
// 解码用经典的"码长计数 + 逐位比较"法(counts/symbols),比查表慢一点,但块很小、够快。
//
// 栈纪律:这张表里的临时数组(码长表 320 字节 + 固定表 320 字节)是 static 的,
// 本板任务的栈只有 8 KB,把它们放栈上会在"画立绘"这种深调用链里触发
// Stack protection fault(实测)。static 的前提是**同一时刻只有一个任务在解压** ——
// 所有调用方(应用状态机、画面合成、剧本读取)都在 LVGL 锁里跑,满足这个前提。
// 新增调用方时务必同样持锁,或改成自己传缓冲。
#include "dracu_inflate.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define MAX_BITS 15
#define MAX_LIT_SYMBOLS 288
#define MAX_DIST_SYMBOLS 32
#define MAX_CODELEN_SYMBOLS 19

// 最近一次失败原因(静态,单线程)
static char s_error[128];

static void fail(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(s_error, sizeof(s_error), fmt, args);
    va_end(args);
}

const char *dracu_inflate_last_error(void)
{
    return s_error[0] ? s_error : "无";
}

typedef struct {
    const uint8_t *in;
    uint32_t in_len;
    uint32_t in_pos;
    uint32_t bit_buf;
    int bit_count;
    uint8_t *out;
    uint32_t out_cap;
    uint32_t out_pos;
} inflate_t;

typedef struct {
    uint16_t counts[MAX_BITS + 1];
    uint16_t symbols[MAX_LIT_SYMBOLS];
} huffman_t;

static bool need_bits(inflate_t *state, int count, uint32_t *value)
{
    while (state->bit_count < count) {
        if (state->in_pos >= state->in_len) {
            fail("输入提前结束(需要 %d 位,已有 %d)", count, state->bit_count);
            return false;
        }
        state->bit_buf |= (uint32_t)state->in[state->in_pos++] << state->bit_count;
        state->bit_count += 8;
    }
    *value = state->bit_buf & ((1u << count) - 1u);
    state->bit_buf >>= count;
    state->bit_count -= count;
    return true;
}

// 从码长表构造 counts/symbols(与 puff 同法):先数码长,再按码字顺序排符号
static bool huffman_build(huffman_t *table, const uint8_t *lengths, int count, const char *what)
{
    memset(table->counts, 0, sizeof(table->counts));
    for (int index = 0; index < count; index++) {
        if (lengths[index] > MAX_BITS) {
            fail("%s 码长 %u 越界", what, (unsigned)lengths[index]);
            return false;
        }
        table->counts[lengths[index]]++;
    }
    table->counts[0] = 0;
    int left = 1;   // 长度前缀用完检测:防止无效码表导致解码时越界
    for (int length = 1; length <= MAX_BITS; length++) {
        left <<= 1;
        left -= table->counts[length];
        if (left < 0) {
            fail("%s 码表非法(长度 %d 处码字不够)", what, length);
            return false;
        }
    }
    int offsets[MAX_BITS + 2];
    offsets[1] = 0;
    for (int length = 1; length <= MAX_BITS; length++) {
        offsets[length + 1] = offsets[length] + table->counts[length];
    }
    for (int index = 0; index < count; index++) {
        if (lengths[index] != 0) {
            table->symbols[offsets[lengths[index]]++] = (uint16_t)index;
        }
    }
    return true;
}

static int huffman_decode(inflate_t *state, const huffman_t *table)
{
    int code = 0;
    int first = 0;
    int index = 0;
    for (int length = 1; length <= MAX_BITS; length++) {
        uint32_t bit = 0;
        if (!need_bits(state, 1, &bit)) {
            return -1;
        }
        code |= (int)bit;
        const int count = table->counts[length];
        if (code - first < count) {
            return table->symbols[index + (code - first)];
        }
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    fail("哈夫曼码字超过 %d 位", MAX_BITS);
    return -1;
}

static const uint16_t LENGTH_BASE[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
                                          31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227,
                                          258 };
static const uint8_t LENGTH_EXTRA[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                         2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t DIST_BASE[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                        193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                                        6145, 8193, 12289, 16385, 24577 };
static const uint8_t DIST_EXTRA[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                       6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static bool out_append(inflate_t *state, uint8_t value)
{
    if (state->out_pos >= state->out_cap) {
        fail("输出超过缓冲(%u 字节)", (unsigned)state->out_cap);
        return false;
    }
    state->out[state->out_pos++] = value;
    return true;
}

static bool out_copy(inflate_t *state, uint32_t distance, uint32_t length)
{
    if (distance == 0 || distance > state->out_pos) {
        fail("回溯距离 %u 超过已输出 %u", (unsigned)distance, (unsigned)state->out_pos);
        return false;
    }
    for (uint32_t index = 0; index < length; index++) {
        if (!out_append(state, state->out[state->out_pos - distance])) {
            return false;
        }
    }
    return true;
}

static bool inflate_codes(inflate_t *state, const huffman_t *literals, const huffman_t *distances)
{
    for (;;) {
        const int symbol = huffman_decode(state, literals);
        if (symbol < 0) {
            return false;
        }
        if (symbol < 256) {
            if (!out_append(state, (uint8_t)symbol)) {
                return false;
            }
        } else if (symbol == 256) {
            return true;   // 本块结束
        } else {
            const int index = symbol - 257;
            if (index >= 29) {
                fail("非法长度码 %d", symbol);
                return false;
            }
            uint32_t extra = 0;
            if (LENGTH_EXTRA[index] && !need_bits(state, LENGTH_EXTRA[index], &extra)) {
                return false;
            }
            const uint32_t length = (uint32_t)LENGTH_BASE[index] + extra;
            const int dist_symbol = huffman_decode(state, distances);
            if (dist_symbol < 0 || dist_symbol >= 30) {
                fail("非法距离码 %d", dist_symbol);
                return false;
            }
            extra = 0;
            if (DIST_EXTRA[dist_symbol] && !need_bits(state, DIST_EXTRA[dist_symbol], &extra)) {
                return false;
            }
            const uint32_t distance = (uint32_t)DIST_BASE[dist_symbol] + extra;
            if (!out_copy(state, distance, length)) {
                return false;
            }
        }
    }
}

static bool inflate_stored(inflate_t *state)
{
    state->bit_buf = 0;
    state->bit_count = 0;   // 对齐到字节边界
    if (state->in_pos + 4 > state->in_len) {
        fail("stored 块头不完整");
        return false;
    }
    const uint32_t length = (uint32_t)state->in[state->in_pos] | ((uint32_t)state->in[state->in_pos + 1] << 8);
    const uint32_t inverse = (uint32_t)state->in[state->in_pos + 2] | ((uint32_t)state->in[state->in_pos + 3] << 8);
    state->in_pos += 4;
    if ((length ^ 0xFFFFu) != inverse) {
        fail("stored 块长度校验失败");
        return false;
    }
    if (state->in_pos + length > state->in_len) {
        fail("stored 块数据不完整");
        return false;
    }
    for (uint32_t index = 0; index < length; index++) {
        if (!out_append(state, state->in[state->in_pos++])) {
            return false;
        }
    }
    return true;
}

static bool inflate_fixed(inflate_t *state)
{
    static huffman_t literals;
    static huffman_t distances;
    static bool ready = false;
    // static:见文件头的"栈纪律"
    static uint8_t literal_lengths[MAX_LIT_SYMBOLS];
    static uint8_t distance_lengths[MAX_DIST_SYMBOLS];
    if (!ready) {
        for (int index = 0; index < 144; index++) literal_lengths[index] = 8;
        for (int index = 144; index < 256; index++) literal_lengths[index] = 9;
        for (int index = 256; index < 280; index++) literal_lengths[index] = 7;
        for (int index = 280; index < 288; index++) literal_lengths[index] = 8;
        for (int index = 0; index < 32; index++) distance_lengths[index] = 5;
        if (!huffman_build(&literals, literal_lengths, MAX_LIT_SYMBOLS, "固定字面表") ||
            !huffman_build(&distances, distance_lengths, MAX_DIST_SYMBOLS, "固定距离表")) {
            return false;
        }
        ready = true;
    }
    return inflate_codes(state, &literals, &distances);
}

static const uint8_t CODELEN_ORDER[MAX_CODELEN_SYMBOLS] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11,
                                                            4, 12, 3, 13, 2, 14, 1, 15 };

static bool inflate_dynamic(inflate_t *state)
{
    uint32_t value = 0;
    if (!need_bits(state, 5, &value)) return false;
    const int literal_count = (int)value + 257;
    if (!need_bits(state, 5, &value)) return false;
    const int distance_count = (int)value + 1;
    if (!need_bits(state, 4, &value)) return false;
    const int codelen_count = (int)value + 4;
    if (literal_count > MAX_LIT_SYMBOLS || distance_count > MAX_DIST_SYMBOLS) {
        fail("动态块符号数越界(%d/%d)", literal_count, distance_count);
        return false;
    }
    // static:见文件头的"栈纪律"
    static uint8_t codelen_lengths[MAX_CODELEN_SYMBOLS];
    memset(codelen_lengths, 0, sizeof(codelen_lengths));
    for (int index = 0; index < codelen_count; index++) {
        if (!need_bits(state, 3, &value)) return false;
        codelen_lengths[CODELEN_ORDER[index]] = (uint8_t)value;
    }
    static huffman_t codelen_table;
    if (!huffman_build(&codelen_table, codelen_lengths, MAX_CODELEN_SYMBOLS, "码长表")) {
        return false;
    }

    static uint8_t lengths[MAX_LIT_SYMBOLS + MAX_DIST_SYMBOLS];
    memset(lengths, 0, sizeof(lengths));
    const int total = literal_count + distance_count;
    int index = 0;
    while (index < total) {
        const int symbol = huffman_decode(state, &codelen_table);
        if (symbol < 0) {
            return false;
        }
        if (symbol < 16) {
            lengths[index++] = (uint8_t)symbol;
        } else if (symbol == 16) {
            if (index == 0) {
                fail("码长重复但没有前一个值");
                return false;
            }
            if (!need_bits(state, 2, &value)) return false;
            const uint8_t previous = lengths[index - 1];
            for (int repeat = 0; repeat < 3 + (int)value && index < total; repeat++) {
                lengths[index++] = previous;
            }
        } else if (symbol == 17) {
            if (!need_bits(state, 3, &value)) return false;
            for (int repeat = 0; repeat < 3 + (int)value && index < total; repeat++) {
                lengths[index++] = 0;
            }
        } else {
            if (!need_bits(state, 7, &value)) return false;
            for (int repeat = 0; repeat < 11 + (int)value && index < total; repeat++) {
                lengths[index++] = 0;
            }
        }
    }
    if (index != total) {
        fail("码长个数不符(%d/%d)", index, total);
        return false;
    }
    huffman_t literals;
    huffman_t distances;
    if (!huffman_build(&literals, lengths, literal_count, "字面表") ||
        !huffman_build(&distances, lengths + literal_count, distance_count, "距离表")) {
        return false;
    }
    return inflate_codes(state, &literals, &distances);
}

uint32_t dracu_inflate(const uint8_t *src, uint32_t src_len, uint8_t *dst, uint32_t dst_cap)
{
    s_error[0] = '\0';
    if (src == NULL || dst == NULL || src_len < 2 || dst_cap == 0) {
        fail("参数不合法");
        return 0;
    }
    // zlib 头:CMF/FLG。CM 必须是 8(deflate),FDICT 必须为 0(无预设字典)
    const uint8_t cmf = src[0];
    const uint8_t flg = src[1];
    if ((cmf & 0x0Fu) != 8 || ((cmf >> 4) & 0x0Fu) != 7) {
        fail("zlib 头不合法(CMF 0x%02x)", cmf);
        return 0;
    }
    if (flg & 0x20u) {
        fail("zlib 头声明了预设字典,本实现不支持");
        return 0;
    }

    inflate_t state;
    memset(&state, 0, sizeof(state));
    state.in = src;
    state.in_len = src_len;
    state.in_pos = 2;   // 跳过 zlib 头
    state.out = dst;
    state.out_cap = dst_cap;

    for (;;) {
        uint32_t final = 0;
        uint32_t type = 0;
        if (!need_bits(&state, 1, &final) || !need_bits(&state, 2, &type)) {
            return 0;
        }
        bool ok = false;
        if (type == 0) {
            ok = inflate_stored(&state);
        } else if (type == 1) {
            ok = inflate_fixed(&state);
        } else if (type == 2) {
            ok = inflate_dynamic(&state);
        } else {
            fail("非法块类型 %u", (unsigned)type);
            return 0;
        }
        if (!ok) {
            return 0;
        }
        if (final) {
            return state.out_pos;
        }
    }
}
