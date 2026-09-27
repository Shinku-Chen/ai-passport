// main/senren_inflate.c —— 用 ROM 里的 miniz(tinfl) 解 zlib 流。
//
// ESP-IDF 5.5.5 不带 zlib 组件,但 ROM 里有一套 tinfl(esp_rom 的 miniz 符号表里有
// tinfl_decompress),既省 Flash 又省依赖。这里自己管 32 KB 环形字典,而不是用
// tinfl_decompress_mem_to_callback —— 后者把 tinfl_decompressor(近 4 KB)放在栈上,
// 本工程的解码都发生在 4 KB 栈的输入任务里。
#include "senren_inflate.h"

#include "miniz.h"

#include <stdlib.h>
#include <string.h>

#define SENREN_DICT_SIZE 32768u
#define SENREN_DICT_MASK (SENREN_DICT_SIZE - 1u)

// 32 KB 环形字典:与 tinfl 要求一致(2 的幂,至少一个字典大小)。
// 欠着用堆分配:本板 DRAM 静态段吃紧(画布 153 KB + LVGL 内存池),堆还有余地。
static uint8_t *s_dict;

static bool dict_ensure(void)
{
    if (s_dict == NULL) {
        s_dict = (uint8_t *)malloc(SENREN_DICT_SIZE);
    }
    return s_dict != NULL;
}

bool senren_inflate_stream(const uint8_t *src, uint32_t src_len, senren_sink_t sink, void *context)
{
    if (src == NULL || sink == NULL || src_len == 0 || !dict_ensure()) {
        return false;
    }
    tinfl_decompressor decompressor;
    tinfl_init(&decompressor);

    uint32_t consumed = 0;
    size_t write = 0;
    for (int guard = 0; guard < 1000000; guard++) {
        size_t input_avail = src_len - consumed;
        // 每次只交给 tinfl「从 write 到字典末尾」这一段,写满就回头消费
        size_t output_avail = SENREN_DICT_SIZE - write;
        const tinfl_status status = tinfl_decompress(
            &decompressor, src + consumed, &input_avail, s_dict, s_dict + write,
            &output_avail, TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_HAS_MORE_INPUT);
        consumed += (uint32_t)((src_len - consumed) - input_avail);
        const uint32_t produced = (uint32_t)((SENREN_DICT_SIZE - write) - output_avail);

        // 环形段可能跨末尾,分成两段交给消费端
        if (produced > 0) {
            const size_t first = SENREN_DICT_SIZE - write < produced ? SENREN_DICT_SIZE - write : produced;
            if (!sink(s_dict + write, (uint32_t)first, context)) {
                return false;
            }
            if (produced > first && !sink(s_dict, produced - (uint32_t)first, context)) {
                return false;
            }
            write = (write + produced) & SENREN_DICT_MASK;
        }

        if (status == TINFL_STATUS_DONE) {
            return true;
        }
        if (status < 0) {
            return false;   // 坏数据 / adler 不符
        }
        if (status == TINFL_STATUS_NEEDS_MORE_INPUT && consumed >= src_len) {
            return false;   // 输入耗尽但流没结束
        }
        if (status == TINFL_STATUS_HAS_MORE_OUTPUT && produced == 0) {
            return false;   // 字典无法推进,避免死循环
        }
    }
    return false;
}

typedef struct {
    uint8_t *dst;
    uint32_t capacity;
    uint32_t written;
    bool overflow;
} block_sink_t;

static bool block_sink(const uint8_t *data, uint32_t length, void *context)
{
    block_sink_t *sink = (block_sink_t *)context;
    if (sink->written + length > sink->capacity) {
        sink->overflow = true;
        return false;
    }
    memcpy(sink->dst + sink->written, data, length);
    sink->written += length;
    return true;
}

uint32_t senren_inflate(const uint8_t *src, uint32_t src_len, uint8_t *dst, uint32_t dst_cap)
{
    if (dst == NULL || dst_cap == 0) {
        return 0;
    }
    block_sink_t sink = { dst, dst_cap, 0, false };
    if (!senren_inflate_stream(src, src_len, block_sink, &sink) || sink.overflow) {
        return 0;
    }
    return sink.written;
}
