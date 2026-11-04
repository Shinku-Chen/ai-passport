// main/anemoi_data/anemoi_data.h —— 嵌入固件的资源包（由 tools/anemoi_pack.py 生成）。
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 资源包在 Flash 里的起点与长度（链接进 .rodata，不做文件系统）。
const uint8_t *anemoi_pack_blob(void);
size_t anemoi_pack_blob_size(void);

#ifdef __cplusplus
}
#endif
