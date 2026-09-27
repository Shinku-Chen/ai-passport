// main/dracu_inflate.h —— 解压:自带的非包装 inflate(DEFLATE/zlib),不依赖 zlib 组件。
//
// 为什么不用 ROM 里的 miniz(tinfl):它的非包装模式要求输出缓冲至少 32 KB,而本板空闲堆
// 只有十几 KB;32 KB 环形字典同样给不起。这里的实现把"整个输出都在缓冲里"当作前提,
// 于是回溯引用直接落在输出缓冲内 —— 不需要任何字典,状态只有几十字节。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 解压一段 zlib 流(带 2 字节头 + adler32;包里的所有压缩数据都是这种)到 dst。
// 成功返回写入 dst 的字节数;失败返回 0,原因见 dracu_inflate_last_error()。
uint32_t dracu_inflate(const uint8_t *src, uint32_t src_len, uint8_t *dst, uint32_t dst_cap);

// 最近一次失败原因(静态字符串,单线程使用)。
const char *dracu_inflate_last_error(void);
