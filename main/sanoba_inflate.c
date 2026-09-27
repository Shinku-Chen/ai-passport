// main/sanoba_inflate.c —— 用 ROM 里的 miniz(tinfl) 解 zlib 流。
//
// ESP-IDF 5.5.5 不带 zlib 组件,但 ROM 里有一套 tinfl(esp_rom 的 miniz 符号表里有
// tinfl_decompress),既省 Flash 又省依赖。这里自己管 32 KB 环形字典,而不是用
// tinfl_decompress_mem_to_callback —— 后者把 tinfl_decompressor(近 4 KB)放在栈上,
// 本工程的解码都发生在 4 KB 栈的输入任务里。
#include "sanoba_inflate.h"

#include "miniz.h"

#include <stdlib.h>
#include <string.h>

#define SANOBA_DICT_SIZE 32768u
#define SANOBA_DICT_MASK (SANOBA_DICT_SIZE - 1u)

// 32 KB 环形字典(流式解压才需要):用完即还,
// 不然常驻会把空闲堆吃到只剩十几 KB。
static bool dict_ensure(uint8_t **out)
{
    *out = (uint8_t *)malloc(SANOBA_DICT_SIZE);
    return *out != NULL;
}

bool sanoba_inflate_stream(const uint8_t *src, uint32_t src_len, sanoba_sink_t sink, void *context)
{
    uint8_t *dict = NULL;
    if (src == NULL || sink == NULL || src_len == 0 || !dict_ensure(&dict)) {
        return false;
    }
    bool ok = false;
    tinfl_decompressor decompressor;
    tinfl_init(&decompressor);

    uint32_t consumed = 0;
    size_t write = 0;
    for (int guard = 0; guard < 1000000; guard++) {
        size_t input_avail = src_len - consumed;
        // 每次只交给 tinfl「从 write 到字典末尾」这一段,写满就回头消费
        size_t output_avail = SANOBA_DICT_SIZE - write;
        const tinfl_status status = tinfl_decompress(
            &decompressor, src + consumed, &input_avail, dict, dict + write,
            &output_avail, TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_HAS_MORE_INPUT);
        consumed += (uint32_t)((src_len - consumed) - input_avail);
        const uint32_t produced = (uint32_t)((SANOBA_DICT_SIZE - write) - output_avail);

        // 环形段可能跨末尾,分成两段交给消费端
        if (produced > 0) {
            const size_t first = SANOBA_DICT_SIZE - write < produced ? SANOBA_DICT_SIZE - write : produced;
            if (!sink(dict + write, (uint32_t)first, context)) {
                goto done;
            }
            if (produced > first && !sink(dict, produced - (uint32_t)first, context)) {
                goto done;
            }
            write = (write + produced) & SANOBA_DICT_MASK;
        }

        if (status == TINFL_STATUS_DONE) {
            ok = true;
            goto done;
        }
        if (status < 0) {
            goto done;   // 坏数据 / adler 不符
        }
        if (status == TINFL_STATUS_NEEDS_MORE_INPUT && consumed >= src_len) {
            goto done;   // 输入耗尽但流没结束
        }
        if (status == TINFL_STATUS_HAS_MORE_OUTPUT && produced == 0) {
            goto done;   // 字典无法推进,避免死循环
        }
    }
done:
    free(dict);
    return ok;
}

uint32_t sanoba_inflate(const uint8_t *src, uint32_t src_len, uint8_t *dst, uint32_t dst_cap)
{
    if (dst == NULL || dst_cap == 0 || src == NULL || src_len == 0) {
        return 0;
    }
    // 整块解压让 tinfl 直接把输出缓冲当字典(TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF):
    // 调用方保证 dst 装得下整块(剧本块最大 27.6 KB),就不必再占 32 KB 环形字典 ——
    // 本板空闲堆只有几十 KB,画布与 LVGL 内存池已经占掉大半。
    tinfl_decompressor decompressor;
    tinfl_init(&decompressor);
    size_t input_avail = src_len;
    size_t output_avail = dst_cap;
    const tinfl_status status = tinfl_decompress(
        &decompressor, src, &input_avail, dst, dst, &output_avail,
        TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (status != TINFL_STATUS_DONE) {
        return 0;
    }
    return (uint32_t)output_avail;
}
