// main/limelight_inflate.h —— 设备端 inflate(ESP32-C3 ROM 里的 tinfl_decompress)。
//
// 剧本包把对白按 250 条一块做了 raw deflate(见 tools/limelight_script_pack.py),
// 解压用 ROM 里的 miniz tinfl:零固件开销,也不用往仓库里塞 zlib/miniz 源码。
// 必须喂**裸 deflate 流**(没有 zlib 头、没有 adler32 校验),与打包器的
// zlib.compressobj(9, DEFLATED, -15) 对应。
#pragma once

#include <stdbool.h>
#include <stdint.h>

// 解压 src 到 dst(容量 dst_cap),成功写 *out_len 并返回 true。
// 失败返回 false(数据损坏、缓冲不足、参数不对)。
bool lime_inflate_raw(const uint8_t *src, uint32_t src_len, uint8_t *dst, uint32_t dst_cap,
                      uint32_t *out_len);
