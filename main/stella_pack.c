// main/stella_pack.c —— 资源包只读解析实现。
#include "stella_pack.h"

#include <stdlib.h>
#include <string.h>

#define SEC_TEXT 0u
#define SEC_NAME 1u
#define SEC_SCENE 2u
#define SEC_OPS 3u
#define SEC_GALLERY 4u
#define SEC_ASSET 5u
#define SEC_BLOB 6u
#define SEC_META 7u
#define SEC_COUNT 8u

static bool section_ok(uint32_t size, uint32_t off, uint32_t len)
{
    return off <= size && len <= size - off;
}


static bool text_range_ok(const stella_pack_t *pack, uint32_t off, uint16_t len)
{
    return off <= pack->text_size && len <= pack->text_size - off;
}

static bool name_ok(const stella_pack_t *pack, uint16_t index)
{
    if (index == STELLA_NONE) return true;
    if (index >= pack->name_count) return false;
    const uint8_t *p = pack->names + (size_t)index * 8u;
    return text_range_ok(pack, stella_rd32(p), stella_rd16(p + 4));
}

static bool asset_ref_ok(const stella_pack_t *pack, uint16_t index)
{
    return index == STELLA_NONE || index < pack->asset_count;
}

static bool scene_id_ok(const stella_pack_t *pack, uint16_t id)
{
    if (id == STELLA_NONE) return true;
    for (uint16_t i = 0; i < pack->scene_count; ++i) {
        if (stella_rd16(pack->scenes + (size_t)i * STELLA_SCENE_BYTES) == id) return true;
    }
    return false;
}

static bool raw_op_ok(const stella_pack_t *pack, const uint8_t *ops, uint16_t count,
                      uint16_t index, uint16_t scene_id)
{
    if (!ops || index >= count) return false;
    const uint8_t *p = ops + (size_t)index * STELLA_OP_BYTES;
    const uint8_t type = p[0];
    const uint8_t bodies = p[1];
    const uint16_t bg = stella_rd16(p + 2);
    const uint16_t b0 = stella_rd16(p + 4), b1 = stella_rd16(p + 6), b2 = stella_rd16(p + 8);
    const uint16_t speaker = stella_rd16(p + 10);
    const uint32_t text_off = stella_rd32(p + 12);
    const uint16_t text_len = stella_rd16(p + 16);
    const uint16_t arg = stella_rd16(p + 18);
    (void)scene_id;
    if (type > STELLA_OP_END || bodies > 3) return false;
    if (!text_range_ok(pack, text_off, text_len) || !name_ok(pack, speaker)) return false;
    if (!asset_ref_ok(pack, bg) || !asset_ref_ok(pack, b0) || !asset_ref_ok(pack, b1) ||
        !asset_ref_ok(pack, b2)) return false;
    if (type == STELLA_OP_TEXT) return bodies <= 3;
    if (type == STELLA_OP_CHOICE) {
        if (arg >= count) return false;
        const uint8_t *target = ops + (size_t)arg * STELLA_OP_BYTES;
        return target[0] == STELLA_OP_TEXT || target[0] == STELLA_OP_CHOICE;
    }
    return bodies == 0 && bg == STELLA_NONE && b0 == STELLA_NONE && b1 == STELLA_NONE &&
           b2 == STELLA_NONE && speaker == STELLA_NONE;
}

static bool validate_pack(const stella_pack_t *pack)
{
    if (!pack || pack->scene_count == 0 || pack->asset_count == 0 ||
        pack->name_count == 0) return false;
    // names
    for (uint16_t i = 0; i < pack->name_count; ++i) {
        if (!name_ok(pack, i)) return false;
    }
    // assets: JPEG SOI + mask stride; reject truncated/oversized masks.
    for (uint16_t i = 0; i < pack->asset_count; ++i) {
        const uint8_t *p = pack->assets + (size_t)i * STELLA_ASSET_BYTES;
        const uint32_t off = stella_rd32(p), len = stella_rd32(p + 4);
        const uint32_t mask_off = stella_rd32(p + 12), mask_len = stella_rd32(p + 16);
        const uint16_t w = stella_rd16(p + 8), h = stella_rd16(p + 10);
        const uint8_t kind = p[20];
        if (kind > STELLA_ASSET_TITLE || w == 0 || h == 0) return false;
        if (off > pack->blob_size || len == 0 || len > pack->blob_size - off) return false;
        if (pack->blob[off] != 0xFFu || pack->blob[off + 1] != 0xD8u) return false;
        if (kind == STELLA_ASSET_SPRITE) {
            const uint32_t stride = ((uint32_t)w + 7u) / 8u;
            const uint32_t need = stride * h;
            if (mask_len < need || mask_off > pack->blob_size ||
                mask_len > pack->blob_size - mask_off) return false;
        } else if (mask_len != 0) {
            return false;
        }
    }
    // gallery tables
    for (uint16_t i = 0; i < pack->group_count; ++i) {
        const uint8_t *p = pack->groups + (size_t)i * 8u;
        const uint16_t label = stella_rd16(p);
        const uint16_t first = stella_rd16(p + 2), count = stella_rd16(p + 4);
        if (!name_ok(pack, label) || first > pack->gentry_count ||
            count > pack->gentry_count - first || count == 0) return false;
    }
    for (uint16_t i = 0; i < pack->gentry_count; ++i) {
        const uint8_t *p = pack->gentries + (size_t)i * 4u;
        if (!asset_ref_ok(pack, stella_rd16(p)) || !name_ok(pack, stella_rd16(p + 2))) {
            return false;
        }
    }
    // scenes, op ranges, cross-scene targets, text/name/asset refs.
    for (uint16_t i = 0; i < pack->scene_count; ++i) {
        const uint8_t *p = pack->scenes + (size_t)i * STELLA_SCENE_BYTES;
        const uint16_t id = stella_rd16(p);
        const uint8_t route = p[2], flags = p[3];
        const uint16_t name = stella_rd16(p + 4), next = stella_rd16(p + 6);
        const uint32_t first = stella_rd32(p + 8);
        const uint16_t count = stella_rd16(p + 12);
        const uint32_t off = stella_rd32(p + 16);
        const uint32_t len = stella_rd32(p + 20), raw = stella_rd32(p + 24);
        if (id == 0 || id == STELLA_NONE || route > STELLA_ROUTE_LEGACY) return false;
        if (flags & ~(STELLA_SCENE_MAIN_CLEAR | STELLA_SCENE_AFTER_CLEAR)) return false;
        if (!name_ok(pack, name) || !scene_id_ok(pack, next)) return false;
        if (count == 0 || len != raw || raw % STELLA_OP_BYTES != 0 ||
            len / STELLA_OP_BYTES != count) return false;
        const uint32_t total_ops = pack->ops_size / STELLA_OP_BYTES;
        if (first > total_ops || count > total_ops - first ||
            off > pack->ops_size || len > pack->ops_size - off) return false;
        const uint8_t *ops = pack->ops + off;
        for (uint16_t j = 0; j < count; ++j) {
            if (!raw_op_ok(pack, ops, count, j, id)) return false;
        }
        for (uint16_t j = 0; j < i; ++j) {
            if (stella_rd16(pack->scenes + (size_t)j * STELLA_SCENE_BYTES) == id) return false;
        }
    }
    return true;
}

bool stella_pack_open(stella_pack_t *pack, const uint8_t *data, uint32_t size)
{
    if (!pack || !data || size < 20u) return false;
    memset(pack, 0, sizeof(*pack));
    if (memcmp(data, STELLA_PACK_MAGIC, 8) != 0) return false;
    const uint32_t version = stella_rd32(data + 8);
    const uint32_t total = stella_rd32(data + 12);
    const uint32_t sections = stella_rd32(data + 16);
    if (version != STELLA_PACK_VERSION || total != size || sections != SEC_COUNT) return false;
    if (size < 20u + sections * 16u) return false;

    pack->data = data;
    pack->size = size;
    bool seen[SEC_COUNT] = {false};
    for (uint32_t i = 0; i < sections; ++i) {
        const uint8_t *sec = data + 20u + i * 16u;
        const uint32_t type = stella_rd32(sec);
        const uint32_t off = stella_rd32(sec + 4);
        const uint32_t count = stella_rd32(sec + 8);
        const uint32_t len = stella_rd32(sec + 12);
        if (type >= SEC_COUNT || seen[type]) return false;
        if (!section_ok(size, off, len)) return false;
        seen[type] = true;
        // Section payloads must not overlap each other or the header/table.
        const uint32_t table_end = 20u + sections * 16u;
        if (len && off < table_end) return false;
        for (uint32_t k = 0; k < i; ++k) {
            const uint8_t *other = data + 20u + k * 16u;
            const uint32_t oo = stella_rd32(other + 4);
            const uint32_t ol = stella_rd32(other + 12);
            if (len && ol && off < oo + ol && oo < off + len) return false;
        }
        switch (type) {
        case SEC_TEXT:
            pack->text = data + off;
            pack->text_size = len;
            break;
        case SEC_NAME:
            if (len % 8u || count != len / 8u) return false;
            pack->names = data + off;
            pack->name_count = len / 8u;
            break;
        case SEC_SCENE:
            if (len % STELLA_SCENE_BYTES || count != len / STELLA_SCENE_BYTES) return false;
            pack->scenes = data + off;
            pack->scene_count = len / STELLA_SCENE_BYTES;
            break;
        case SEC_OPS:
            pack->ops = data + off;
            pack->ops_size = len;
            break;
        case SEC_GALLERY: {
            const uint32_t group_len = count * 8u;
            const uint32_t entry_len = len - group_len;
            if (group_len > len || entry_len % 4u) return false;
            pack->groups = data + off;
            pack->group_count = count;
            pack->gentries = data + off + group_len;
            pack->gentry_count = entry_len / 4u;
            break;
        }
        case SEC_ASSET:
            if (len % STELLA_ASSET_BYTES || count != len / STELLA_ASSET_BYTES) return false;
            pack->assets = data + off;
            pack->asset_count = len / STELLA_ASSET_BYTES;
            break;
        case SEC_BLOB:
            pack->blob = data + off;
            pack->blob_size = len;
            break;
        case SEC_META:
            pack->meta = data + off;
            pack->meta_size = len;
            break;
        default:
            return false;
        }
    }
    for (uint32_t i = 0; i < SEC_COUNT; ++i) {
        if (!seen[i]) return false;
    }
    return validate_pack(pack);
}

bool stella_pack_scene(const stella_pack_t *pack, uint16_t index, stella_scene_t *out)
{
    if (!pack || !out || index >= pack->scene_count) return false;
    const uint8_t *p = pack->scenes + (size_t)index * STELLA_SCENE_BYTES;
    out->id = stella_rd16(p);
    out->route = p[2];
    out->flags = p[3];
    out->name = stella_rd16(p + 4);
    out->next = stella_rd16(p + 6);
    out->first_op = stella_rd32(p + 8);
    out->op_count = stella_rd16(p + 12);
    out->data_off = stella_rd32(p + 16);
    out->data_len = stella_rd32(p + 20);
    out->raw_len = stella_rd32(p + 24);
    if (out->data_off > pack->ops_size || out->data_len > pack->ops_size - out->data_off) {
        return false;
    }
    return true;
}

bool stella_pack_find_scene(const stella_pack_t *pack, uint16_t id, uint16_t *index_out)
{
    if (!pack || !index_out) return false;
    for (uint16_t i = 0; i < pack->scene_count; ++i) {
        const uint8_t *p = pack->scenes + (size_t)i * STELLA_SCENE_BYTES;
        if (stella_rd16(p) == id) {
            *index_out = i;
            return true;
        }
    }
    return false;
}

const uint8_t *stella_pack_ops(const stella_pack_t *pack, const stella_scene_t *scene,
                               uint16_t *count_out)
{
    if (!pack || !scene) return NULL;
    if (scene->data_off > pack->ops_size || scene->data_len > pack->ops_size - scene->data_off) {
        return NULL;
    }
    if (scene->data_len != scene->raw_len || scene->raw_len % STELLA_OP_BYTES != 0) return NULL;
    const uint32_t count = scene->raw_len / STELLA_OP_BYTES;
    if (count != scene->op_count) return NULL;
    if (count_out) *count_out = (uint16_t)count;
    return pack->ops + scene->data_off;
}

bool stella_pack_op(const uint8_t *ops, uint32_t op_count, uint16_t index, stella_op_t *out)
{
    if (!ops || !out || index >= op_count) return false;
    const uint8_t *p = ops + (size_t)index * STELLA_OP_BYTES;
    out->type = p[0];
    out->body_count = p[1];
    out->bg = stella_rd16(p + 2);
    out->body[0] = stella_rd16(p + 4);
    out->body[1] = stella_rd16(p + 6);
    out->body[2] = stella_rd16(p + 8);
    out->speaker = stella_rd16(p + 10);
    out->text_off = stella_rd32(p + 12);
    out->text_len = stella_rd16(p + 16);
    out->arg = stella_rd16(p + 18);
    return out->type <= STELLA_OP_END && out->body_count <= 3;
}

static const char *text_at(const stella_pack_t *pack, uint32_t off, uint16_t len)
{
    if (!pack || off >= pack->text_size || len > pack->text_size - off) return "";
    // 包内字符串不保证 NUL 结尾，调用方按 len 使用；返回空串时的静态 NUL 由下面兜底。
    return (const char *)(pack->text + off);
}

const char *stella_pack_text(const stella_pack_t *pack, uint32_t off, uint16_t len)
{
    return text_at(pack, off, len);
}

const char *stella_pack_name(const stella_pack_t *pack, uint16_t index)
{
    if (!pack || index >= pack->name_count) return "";
    const uint8_t *p = pack->names + (size_t)index * 8u;
    const uint32_t off = stella_rd32(p);
    const uint16_t len = stella_rd16(p + 4);
    return text_at(pack, off, len);
}

bool stella_pack_text_copy(const stella_pack_t *pack, uint32_t off, uint16_t len,
                           char *out, size_t cap)
{
    if (!pack || !out || cap == 0) return false;
    if (off >= pack->text_size || len > pack->text_size - off || (size_t)len >= cap) {
        out[0] = 0;
        return false;
    }
    memcpy(out, pack->text + off, len);
    out[len] = 0;
    return true;
}

bool stella_pack_name_copy(const stella_pack_t *pack, uint16_t index,
                           char *out, size_t cap)
{
    if (!pack || index >= pack->name_count) {
        if (out && cap) out[0] = 0;
        return false;
    }
    const uint8_t *p = pack->names + (size_t)index * 8u;
    return stella_pack_text_copy(pack, stella_rd32(p), stella_rd16(p + 4), out, cap);
}

int stella_pack_meta_int(const stella_pack_t *pack, const char *key, int fallback)
{
    if (!pack || !pack->meta || !key) return fallback;
    const size_t key_len = strlen(key);
    const char *p = (const char *)pack->meta;
    const char *end = p + pack->meta_size;
    while (p < end) {
        const char *line = p;
        const char *nl = memchr(p, 0x0A, (size_t)(end - p));
        const char *line_end = nl ? nl : end;
        if ((size_t)(line_end - line) > key_len && strncmp(line, key, key_len) == 0 &&
            line[key_len] == '=') {
            return (int)strtol(line + key_len + 1, NULL, 10);
        }
        p = nl ? nl + 1 : end;
    }
    return fallback;
}

bool stella_pack_asset(const stella_pack_t *pack, uint16_t index, stella_asset_t *out)
{
    if (!pack || !out || index >= pack->asset_count) return false;
    const uint8_t *p = pack->assets + (size_t)index * STELLA_ASSET_BYTES;
    const uint32_t off = stella_rd32(p);
    const uint32_t len = stella_rd32(p + 4);
    const uint32_t mask_off = stella_rd32(p + 12);
    const uint32_t mask_len = stella_rd32(p + 16);
    if (off > pack->blob_size || len > pack->blob_size - off) return false;
    if (mask_len && (mask_off > pack->blob_size || mask_len > pack->blob_size - mask_off)) {
        return false;
    }
    out->jpeg = pack->blob + off;
    out->jpeg_len = len;
    out->w = stella_rd16(p + 8);
    out->h = stella_rd16(p + 10);
    out->mask = mask_len ? pack->blob + mask_off : NULL;
    out->mask_len = mask_len;
    out->kind = p[20];
    if (out->kind == STELLA_ASSET_SPRITE) {
        const uint32_t stride = ((uint32_t)out->w + 7u) / 8u;
        if (mask_len < stride * out->h) return false;
    } else if (mask_len != 0) {
        return false;
    }
    out->ax = (int16_t)stella_rd16(p + 22);
    out->ay = (int16_t)stella_rd16(p + 24);
    out->ow = stella_rd16(p + 26);
    out->oh = stella_rd16(p + 28);
    return out->kind <= STELLA_ASSET_TITLE && out->w > 0 && out->h > 0;
}

bool stella_pack_group(const stella_pack_t *pack, uint16_t index, stella_group_t *out)
{
    if (!pack || !out || index >= pack->group_count) return false;
    const uint8_t *p = pack->groups + (size_t)index * 8u;
    out->label = stella_rd16(p);
    out->first = stella_rd16(p + 2);
    out->count = stella_rd16(p + 4);
    return out->first <= pack->gentry_count && out->count <= pack->gentry_count - out->first;
}

bool stella_pack_gentry(const stella_pack_t *pack, uint16_t index, stella_gentry_t *out)
{
    if (!pack || !out || index >= pack->gentry_count) return false;
    const uint8_t *p = pack->gentries + (size_t)index * 4u;
    out->asset = stella_rd16(p);
    out->label = stella_rd16(p + 2);
    return out->asset < pack->asset_count;
}
