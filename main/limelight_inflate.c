// main/limelight_inflate.c —— 用 ROM 里的 tinfl_decompress 解压剧本块。
#include "limelight_inflate.h"

#include "esp_log.h"

// esp_rom 暴露 ROM 中的 miniz:tinfl_decompress 就在 ROM 的固定地址上
// (components/esp_rom/esp32c3/ld/esp32c3.rom.ld),所以不占固件体积。
#include "miniz.h"

#include <string.h>

static const char *TAG = "lime_inflate";

// tinfl_decompressor 约 11 KB。**静态分配**:不放在栈上(app_main 的栈只有几 KB),
// 也不能放堆上 —— 这块板子启动后堆只剩 ~11.7 KB 空闲、最大连续块 7.6 KB,申请
// 11 KB 会直接失败(真机上就是这么暴的)。放 .bss 后每次解压复用同一份。
static tinfl_decompressor s_decompressor;

bool lime_inflate_raw(const uint8_t *src, uint32_t src_len, uint8_t *dst, uint32_t dst_cap,
                      uint32_t *out_len)
{
    if (!src || !dst || !out_len || src_len == 0 || dst_cap == 0) return false;

    tinfl_init(&s_decompressor);
    size_t in_size = src_len;
    size_t out_size = dst_cap;
    // 裸 deflate(不设 PARSE_ZLIB_HEADER)、输入已完整(不设 HAS_MORE_INPUT)、
    // 输出缓冲足够容纳整个流(设 USING_NON_WRAPPING_OUTPUT_BUF)。
    const tinfl_status status = tinfl_decompress(&s_decompressor, src, &in_size, dst, dst,
                                                 &out_size,
                                                 TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (status != TINFL_STATUS_DONE) {
        ESP_LOGE(TAG, "解压失败(status=%d, 输入 %u 字节, 输出 %u 字节)",
                 (int)status, (unsigned)src_len, (unsigned)out_size);
        return false;
    }
    *out_len = (uint32_t)out_size;
    return true;
}
