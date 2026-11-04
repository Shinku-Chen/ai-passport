// main/parquet/parquet_inflate.h —— 共享的 zlib/deflate 解压入口。
//
// ESP32-C3 上 miniz 的 tinfl_decompressor 约 11 KB；把它放在调用者栈上会撑爆 7 KB
// 的 LVGL 任务栈，因此放静态区。资源包正文块与图片块都由同一个 UI 任务串行解码，
// 共用一份状态即可，避免两个模块各占 11 KB 静态 DRAM。
#pragma once

#include <stddef.h>
#include <stdint.h>

#define PQ_INFLATE_FAILED ((size_t)-1)

// 解压以 zlib 头封装的流到 dst；成功返回产出字节数，失败返回 PQ_INFLATE_FAILED。
size_t pq_inflate(void *dst, size_t dst_len, const uint8_t *src, size_t src_len);
// 仅供静态区账目/诊断。
size_t pq_inflate_state_bytes(void);
