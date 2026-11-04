// main/gal_unlock.h —— 全局解锁(鉴赏 CG、章节入口)独立版本化存储。
//
// 位图只增不减;旧档/旧版本读入后按位 OR 合并,置 dirty 由上层节流重试。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GAL_UNLOCK_GALLERY_BYTES 22   // 172 张
#define GAL_UNLOCK_CHAPTER_BYTES 47   // 371 个入口
#define GAL_UNLOCK_BLOB 81

typedef struct {
    uint8_t version;
    uint8_t dirty;
    uint8_t gallery[GAL_UNLOCK_GALLERY_BYTES];
    uint8_t chapters[GAL_UNLOCK_CHAPTER_BYTES];
} gal_unlock_t;

void gal_unlock_init(gal_unlock_t *u);

// 解析版本化 blob;失败返回 false(不修改 u)。
bool gal_unlock_decode(gal_unlock_t *u, const uint8_t *data, size_t len);

// 编码为定长 blob;容量不足返回 0。
size_t gal_unlock_encode(const gal_unlock_t *u, uint8_t *out, size_t cap);

// 合并位图;返回是否新增了位(新增时置 dirty)。
bool gal_unlock_merge(gal_unlock_t *u, const uint8_t *gallery, const uint8_t *chapters);
bool gal_unlock_merge_blob(gal_unlock_t *u, const uint8_t *data, size_t len);

static inline bool gal_unlock_gallery(const gal_unlock_t *u, uint16_t item)
{
    return u && item < GAL_UNLOCK_GALLERY_BYTES * 8 &&
           (u->gallery[item >> 3] & (1u << (item & 7))) != 0;
}
static inline bool gal_unlock_chapter(const gal_unlock_t *u, uint16_t entry)
{
    return u && entry < GAL_UNLOCK_CHAPTER_BYTES * 8 &&
           (u->chapters[entry >> 3] & (1u << (entry & 7))) != 0;
}
