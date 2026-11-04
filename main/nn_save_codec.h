// main/nn_save_codec.h —— 存档记录的编解码（纯逻辑，宿主可测）。
//
// NVS 里存的是一条定长记录，而不是把 C 结构体直接倒进去：结构体布局会随编译器/
// 未来的字段调整而变，而且坏数据没有任何可校验的字段。这条记录带魔数、版本、
// 载荷长度，解码时逐项校验，并保证 chapter/preview 一定以 NUL 结尾。
#pragma once

#include "nn_pack.h"
#include "nn_save.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NN_SAVE_RECORD_MAGIC 0x314E534Eu   // "NSN1"（小端读作 4E 53 4E 31）
#define NN_SAVE_RECORD_VERSION 1u
#define NN_SAVE_RECORD_BYTES 72u

// 把存档编码成定长记录。cap 不足返回 false。
bool nn_save_encode(const nn_save_t *save, uint8_t *out, size_t cap, size_t *out_len);

// 解码并校验。任何一项不满足（长度/魔数/版本/载荷长度）都返回 false，
// 调用方应当把该槽位当成“没有存档”，而不是拿它去恢复。
bool nn_save_decode(const uint8_t *blob, size_t len, nn_save_t *out);

// 记录与当前资源包是否自洽：场景/操作下标、图像 id 必须在包里存在。
// 通过后才允许拿它去 nn_rt_seek()。
bool nn_save_record_fits_pack(const nn_save_t *save, const nn_pack_t *pack);

#ifdef __cplusplus
}
#endif
