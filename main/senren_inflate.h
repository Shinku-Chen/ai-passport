// main/senren_inflate.h —— 解压:ROM 里的 miniz(tinfl),不依赖 zlib 组件。
//
// 包里所有压缩数据都是 zlib 格式(zlib.compress),所以调用时带上
// TINFL_FLAG_PARSE_ZLIB_HEADER。两套接口:
//   senren_inflate        输出放得下(整块解)时用它:剧本块、CG 补丁掩码;
//   senren_inflate_stream 输出很大(立绘 PNG、补丁像素流)时用它:内部 32 KB 环形
//                         字典,边解边把数据交给 sink,不需要整块缓冲。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 流式解压的数据出口:返回 false 表示消费端出错(解压会立即中止)。
typedef bool (*senren_sink_t)(const uint8_t *data, uint32_t length, void *context);

// 整块解压。成功返回写入 dst 的字节数,失败返回 0。
uint32_t senren_inflate(const uint8_t *src, uint32_t src_len, uint8_t *dst, uint32_t dst_cap);

// 流式解压:内部维护 32 KB 环形字典(senren_inflate 也用它,所以不可重入)。
bool senren_inflate_stream(const uint8_t *src, uint32_t src_len, senren_sink_t sink, void *context);
