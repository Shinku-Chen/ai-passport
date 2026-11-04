// main/gal_unlock.c —— 全局解锁位图的版本化编码/合并。
#include "gal_unlock.h"

#include <string.h>

#define UNLOCK_VERSION 1u
#define UNLOCK_MAGIC 0x314E554Du   // "MUN1" little endian

static uint32_t crc32_le(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = ~crc;
    while (len--) {
        crc ^= *data++;
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void gal_unlock_init(gal_unlock_t *u)
{
    if (!u) return;
    memset(u, 0, sizeof(*u));
    u->version = UNLOCK_VERSION;
    u->dirty = 0;
}

size_t gal_unlock_encode(const gal_unlock_t *u, uint8_t *out, size_t cap)
{
    if (!u || !out || cap < GAL_UNLOCK_BLOB) return 0;
    memset(out, 0, GAL_UNLOCK_BLOB);
    out[0] = 'M'; out[1] = 'U'; out[2] = 'N'; out[3] = '1';
    out[4] = UNLOCK_VERSION;
    out[5] = 0;
    out[6] = (uint8_t)GAL_UNLOCK_GALLERY_BYTES;
    out[7] = (uint8_t)GAL_UNLOCK_CHAPTER_BYTES;
    memcpy(out + 8, u->gallery, GAL_UNLOCK_GALLERY_BYTES);
    memcpy(out + 8 + GAL_UNLOCK_GALLERY_BYTES, u->chapters, GAL_UNLOCK_CHAPTER_BYTES);
    const uint32_t crc = crc32_le(0, out, GAL_UNLOCK_BLOB - 4);
    out[GAL_UNLOCK_BLOB - 4] = (uint8_t)crc;
    out[GAL_UNLOCK_BLOB - 3] = (uint8_t)(crc >> 8);
    out[GAL_UNLOCK_BLOB - 2] = (uint8_t)(crc >> 16);
    out[GAL_UNLOCK_BLOB - 1] = (uint8_t)(crc >> 24);
    return GAL_UNLOCK_BLOB;
}

bool gal_unlock_decode(gal_unlock_t *u, const uint8_t *data, size_t len)
{
    if (!u || !data || len != GAL_UNLOCK_BLOB) return false;
    if (memcmp(data, "MUN1", 4) != 0) return false;
    if (data[4] != UNLOCK_VERSION || data[6] != GAL_UNLOCK_GALLERY_BYTES ||
        data[7] != GAL_UNLOCK_CHAPTER_BYTES) {
        return false;
    }
    const uint32_t crc = (uint32_t)data[GAL_UNLOCK_BLOB - 4] |
                         ((uint32_t)data[GAL_UNLOCK_BLOB - 3] << 8) |
                         ((uint32_t)data[GAL_UNLOCK_BLOB - 2] << 16) |
                         ((uint32_t)data[GAL_UNLOCK_BLOB - 1] << 24);
    if (crc32_le(0, data, GAL_UNLOCK_BLOB - 4) != crc) return false;
    gal_unlock_t tmp;
    gal_unlock_init(&tmp);
    memcpy(tmp.gallery, data + 8, GAL_UNLOCK_GALLERY_BYTES);
    memcpy(tmp.chapters, data + 8 + GAL_UNLOCK_GALLERY_BYTES, GAL_UNLOCK_CHAPTER_BYTES);
    *u = tmp;
    return true;
}

bool gal_unlock_merge(gal_unlock_t *u, const uint8_t *gallery, const uint8_t *chapters)
{
    if (!u) return false;
    bool changed = false;
    if (gallery) {
        for (size_t i = 0; i < GAL_UNLOCK_GALLERY_BYTES; ++i) {
            const uint8_t merged = u->gallery[i] | gallery[i];
            if (merged != u->gallery[i]) {
                u->gallery[i] = merged;
                changed = true;
            }
        }
    }
    if (chapters) {
        for (size_t i = 0; i < GAL_UNLOCK_CHAPTER_BYTES; ++i) {
            const uint8_t merged = u->chapters[i] | chapters[i];
            if (merged != u->chapters[i]) {
                u->chapters[i] = merged;
                changed = true;
            }
        }
    }
    if (changed) u->dirty = 1;
    return changed;
}

bool gal_unlock_merge_blob(gal_unlock_t *u, const uint8_t *data, size_t len)
{
    gal_unlock_t tmp;
    if (!gal_unlock_decode(&tmp, data, len)) return false;
    return gal_unlock_merge(u, tmp.gallery, tmp.chapters);
}
