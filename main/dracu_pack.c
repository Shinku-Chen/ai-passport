// main/dracu_pack.c —— 图片包只读解析实现(纯字节映射,不依赖 ESP-IDF,宿主机可测)。
#include "dracu_pack.h"

#include <string.h>

#define DRACU_HEADER_SIZE 20u
#define DRACU_SECTION_SIZE 16u
#define DRACU_ENTRY_SIZE 36u
#define DRACU_SPRITE_ENTRY_SIZE 8u

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

static const uint8_t *entry_record(const dracu_pack_t *pack, uint32_t index)
{
    if (index >= pack->entry_count) {
        return NULL;
    }
    return pack->entries + index * DRACU_ENTRY_SIZE;
}

// 扫描一遍条目表,记下每个名字空间的起点与条数(表已按 (pool, name) 升序)。
static bool build_pool_index(dracu_pack_t *pack)
{
    for (uint32_t pool = 0; pool < DRACU_POOL_COUNT; pool++) {
        pack->pool_first[pool] = 0;
        pack->pool_count[pool] = 0;
    }
    for (uint32_t index = 0; index < pack->entry_count; index++) {
        const uint8_t *record = entry_record(pack, index);
        uint8_t pool = record[7];
        if (pool >= DRACU_POOL_COUNT) {
            continue;
        }
        if (pack->pool_count[pool] == 0) {
            pack->pool_first[pool] = (uint16_t)index;
        }
        pack->pool_count[pool]++;
    }
    // 同一个池必须连续,否则 id 映射不成立
    for (uint32_t pool = 0; pool < DRACU_POOL_COUNT; pool++) {
        if (pack->pool_count[pool] == 0) {
            continue;
        }
        uint32_t first = pack->pool_first[pool];
        uint32_t end = first + pack->pool_count[pool];
        for (uint32_t index = first; index < end; index++) {
            if (entry_record(pack, index)[7] != pool) {
                return false;
            }
        }
        if (pack->pool_count[pool] > 0xFFFFu) {
            return false;
        }
    }
    return true;
}

bool dracu_pack_open(dracu_pack_t *pack, const uint8_t *data, uint32_t size)
{
    if (pack == NULL || data == NULL || size < DRACU_HEADER_SIZE) {
        return false;
    }
    if (memcmp(data, DRACU_PACK_MAGIC, 8) != 0) {
        return false;
    }
    uint16_t version = rd16(data + 8);
    uint16_t header_size = rd16(data + 10);
    uint16_t section_count = rd16(data + 12);
    uint32_t total = rd32(data + 16);
    if (version != DRACU_PACK_VERSION || total != size) {
        return false;
    }
    if (header_size < DRACU_HEADER_SIZE + (uint32_t)section_count * DRACU_SECTION_SIZE ||
        (uint64_t)header_size > size) {
        return false;
    }

    dracu_pack_t view;
    memset(&view, 0, sizeof(view));
    view.blob = data;
    view.blob_size = size;
    for (uint16_t index = 0; index < section_count; index++) {
        const uint8_t *section = data + DRACU_HEADER_SIZE + (uint32_t)index * DRACU_SECTION_SIZE;
        uint32_t type = rd32(section);
        uint32_t offset = rd32(section + 4);
        uint32_t count = rd32(section + 8);
        uint32_t length = rd32(section + 12);
        if (offset > size || length > size - offset) {
            return false;
        }
        switch (type) {
        case DRACU_SEC_NAME:
            view.names = data + offset;
            view.names_size = length;
            break;
        case DRACU_SEC_ASSET:
            view.entries = data + offset;
            view.entries_size = length;
            view.entry_count = count;
            break;
        case DRACU_SEC_BLOB:
            view.payload = data + offset;
            view.payload_size = length;
            break;
        case DRACU_SEC_SPRITE:
            view.sprites = data + offset;
            if (length < 4) {
                return false;
            }
            view.sprite_count = rd32(view.sprites);
            if (4u + (uint64_t)view.sprite_count * DRACU_SPRITE_ENTRY_SIZE > length) {
                return false;
            }
            break;
        case DRACU_SEC_META:
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
    if (view.entry_count == 0 || (uint64_t)view.entry_count * DRACU_ENTRY_SIZE > view.entries_size) {
        return false;
    }
    if (!build_pool_index(&view)) {
        return false;
    }
    *pack = view;
    return true;
}

bool dracu_pack_at(const dracu_pack_t *pack, uint16_t index, dracu_asset_t *out)
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

    dracu_asset_t asset;
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
    asset.alias = (rd32(record + 30) & DRACU_ASSET_ALIAS) != 0;
    *out = asset;
    return true;
}

bool dracu_pack_id(const dracu_pack_t *pack, uint8_t pool, uint16_t id, dracu_asset_t *out)
{
    if (pack == NULL || pool >= DRACU_POOL_COUNT || id == DRACU_NO_ENTRY) {
        return false;
    }
    if (id >= pack->pool_count[pool]) {
        return false;
    }
    return dracu_pack_at(pack, (uint16_t)(pack->pool_first[pool] + id), out);
}

bool dracu_pack_find(const dracu_pack_t *pack, uint8_t pool, const char *name, dracu_asset_t *out)
{
    if (pack == NULL || pack->entries == NULL || name == NULL || pool >= DRACU_POOL_COUNT) {
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
            cmp = record[7] < pool ? -1 : 1;
        } else {
            cmp = compare_bytes(pack->names + entry_name_off, entry_name_len,
                                (const uint8_t *)name, name_len);
        }
        if (cmp == 0) {
            return dracu_pack_at(pack, (uint16_t)middle, out);
        }
        if (cmp < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return false;
}

bool dracu_pack_pool_id(const dracu_pack_t *pack, uint16_t index, uint8_t pool, uint16_t *id)
{
    if (pack == NULL || pool >= DRACU_POOL_COUNT || index >= pack->entry_count) {
        return false;
    }
    const uint8_t *record = entry_record(pack, index);
    if (record[7] != pool) {
        return false;
    }
    if (index < pack->pool_first[pool] || index >= pack->pool_first[pool] + pack->pool_count[pool]) {
        return false;
    }
    if (id != NULL) {
        *id = (uint16_t)(index - pack->pool_first[pool]);
    }
    return true;
}

bool dracu_pack_sprite(const dracu_pack_t *pack, uint16_t id, dracu_sprite_t *out)
{
    if (pack == NULL || out == NULL || pack->sprites == NULL || id >= pack->sprite_count) {
        return false;
    }
    const uint8_t *record = pack->sprites + 4u + (uint32_t)id * DRACU_SPRITE_ENTRY_SIZE;
    out->body = rd16(record);
    out->face = rd16(record + 2);
    out->facex = (int16_t)rd16(record + 4);
    out->facey = (int16_t)rd16(record + 6);
    return true;
}

size_t dracu_pack_name(const dracu_asset_t *asset, char *out, size_t capacity)
{
    if (asset == NULL || out == NULL || capacity == 0) {
        return 0;
    }
    size_t copy = asset->name_len < capacity - 1 ? asset->name_len : capacity - 1;
    memcpy(out, asset->name, copy);
    out[copy] = '\0';
    return copy;
}
