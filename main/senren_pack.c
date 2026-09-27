// main/senren_pack.c —— 图片包只读解析实现(纯字节映射,不依赖 ESP-IDF,宿主机可测)。
#include "senren_pack.h"

#include <string.h>

#define SENREN_HEADER_SIZE 20u
#define SENREN_SECTION_SIZE 16u
#define SENREN_ENTRY_SIZE 36u

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int compare_bytes(const uint8_t *a, uint32_t a_len, const uint8_t *b, uint32_t b_len)
{
    uint32_t shared = a_len < b_len ? a_len : b_len;
    int cmp = shared ? memcmp(a, b, shared) : 0;
    if (cmp != 0) {
        return cmp < 0 ? -1 : 1;
    }
    if (a_len == b_len) {
        return 0;
    }
    return a_len < b_len ? -1 : 1;
}

static const uint8_t *entry_record(const senren_pack_t *pack, uint32_t index)
{
    if (index >= pack->entry_count) {
        return NULL;
    }
    return pack->entries + index * SENREN_ENTRY_SIZE;
}

bool senren_pack_open(senren_pack_t *pack, const uint8_t *data, uint32_t size)
{
    if (pack == NULL || data == NULL || size < SENREN_HEADER_SIZE) {
        return false;
    }
    if (memcmp(data, SENREN_PACK_MAGIC, 8) != 0) {
        return false;
    }
    uint16_t version = rd16(data + 8);
    uint16_t header_size = rd16(data + 10);
    uint16_t section_count = rd16(data + 12);
    uint32_t total = rd32(data + 16);
    if (version != SENREN_PACK_VERSION || total != size) {
        return false;
    }
    if (header_size < SENREN_HEADER_SIZE + (uint32_t)section_count * SENREN_SECTION_SIZE ||
        (uint64_t)header_size > size) {
        return false;
    }

    senren_pack_t view;
    memset(&view, 0, sizeof(view));
    view.blob = data;
    view.blob_size = size;
    // 段表紧跟在 20 字节头部之后;header_size 是「段体起点」(头部 + 段表)
    for (uint16_t index = 0; index < section_count; index++) {
        const uint8_t *section = data + SENREN_HEADER_SIZE + (uint32_t)index * SENREN_SECTION_SIZE;
        uint32_t type = rd32(section);
        uint32_t offset = rd32(section + 4);
        uint32_t count = rd32(section + 8);
        uint32_t length = rd32(section + 12);
        if (offset > size || length > size - offset) {
            return false;
        }
        switch (type) {
        case SENREN_SEC_NAME:
            view.names = data + offset;
            view.names_size = length;
            break;
        case SENREN_SEC_ASSET:
            view.entries = data + offset;
            view.entries_size = length;
            view.entry_count = count;
            break;
        case SENREN_SEC_BLOB:
            view.payload = data + offset;
            view.payload_size = length;
            break;
        case SENREN_SEC_META:
            view.meta = data + offset;
            view.meta_size = length;
            break;
        default:
            break;
        }
    }
    if (view.names == NULL || view.entries == NULL || view.payload == NULL) {
        return false;
    }
    if (view.entry_count == 0 || (uint64_t)view.entry_count * SENREN_ENTRY_SIZE > view.entries_size) {
        return false;
    }
    *pack = view;
    return true;
}

bool senren_pack_at(const senren_pack_t *pack, uint16_t index, senren_asset_t *out)
{
    if (pack == NULL || out == NULL) {
        return false;
    }
    const uint8_t *record = entry_record(pack, index);
    if (record == NULL) {
        return false;
    }
    uint32_t name_off = rd32(record);
    uint16_t name_len = rd16(record + 4);
    uint32_t data_off = rd32(record + 22);
    uint32_t data_len = rd32(record + 26);
    if (name_off > pack->names_size || name_len > pack->names_size - name_off) {
        return false;
    }
    if (data_off > pack->payload_size || data_len > pack->payload_size - data_off) {
        return false;
    }

    senren_asset_t asset;
    memset(&asset, 0, sizeof(asset));
    asset.name = (const char *)(pack->names + name_off);
    asset.name_len = name_len;
    asset.kind = record[6];
    asset.pool = record[7];
    asset.index = index;
    asset.base = rd16(record + 8);
    asset.w = rd16(record + 10);
    asset.h = rd16(record + 12);
    asset.dx = rd16(record + 14);
    asset.dy = rd16(record + 16);
    asset.dw = rd16(record + 18);
    asset.dh = rd16(record + 20);
    asset.data = data_len ? pack->payload + data_off : NULL;
    asset.data_len = data_len;
    asset.alias = (rd32(record + 30) & SENREN_ASSET_ALIAS) != 0;
    *out = asset;
    return true;
}

bool senren_pack_find(const senren_pack_t *pack, uint8_t pool, const char *name, senren_asset_t *out)
{
    if (pack == NULL || pack->entries == NULL || name == NULL) {
        return false;
    }
    uint32_t name_len = (uint32_t)strlen(name);
    uint32_t low = 0;
    uint32_t high = pack->entry_count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        const uint8_t *record = entry_record(pack, middle);
        uint32_t entry_name_off = rd32(record);
        uint32_t entry_name_len = rd16(record + 4);
        int cmp;
        if (pool != record[7]) {
            // cmp 的语义是「条目前还是目标前」:条目所在池更小就先走右边
            cmp = record[7] < pool ? -1 : 1;
        } else {
            cmp = compare_bytes(pack->names + entry_name_off, entry_name_len,
                                (const uint8_t *)name, name_len);
        }
        if (cmp == 0) {
            return senren_pack_at(pack, (uint16_t)middle, out);
        }
        if (cmp < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return false;
}

size_t senren_pack_name(const senren_asset_t *asset, char *out, size_t capacity)
{
    if (asset == NULL || out == NULL || capacity == 0) {
        return 0;
    }
    size_t copy = asset->name_len < capacity - 1 ? asset->name_len : capacity - 1;
    memcpy(out, asset->name, copy);
    out[copy] = '\0';
    return copy;
}

bool senren_patch_parts(const senren_asset_t *asset, const uint8_t **mask, uint32_t *mask_len,
                        const uint8_t **pixels, uint32_t *pixels_len)
{
    if (asset == NULL || asset->kind != SENREN_KIND_CG_DIFF || asset->data == NULL ||
        asset->data_len < SENREN_PATCH_HEADER) {
        return false;
    }
    uint32_t packed_mask_len = rd32(asset->data);
    uint32_t packed_pixels_len = rd32(asset->data + 4);
    if (packed_mask_len + packed_pixels_len != asset->data_len - SENREN_PATCH_HEADER) {
        return false;
    }
    if (mask != NULL) {
        *mask = asset->data + SENREN_PATCH_HEADER;
    }
    if (mask_len != NULL) {
        *mask_len = packed_mask_len;
    }
    if (pixels != NULL) {
        *pixels = asset->data + SENREN_PATCH_HEADER + packed_mask_len;
    }
    if (pixels_len != NULL) {
        *pixels_len = packed_pixels_len;
    }
    return true;
}
