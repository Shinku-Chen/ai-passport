// main/clannad_pack.c —— 资源包解析 + raw-DEFLATE 解压实现。
#include "clannad_pack.h"

#include <stdio.h>
#include <string.h>

static bool range_ok(uint32_t size, uint32_t off, uint32_t len)
{
    return off <= size && len <= size - off;
}

// ------------------------------------------------------------------ inflate
typedef struct {
    const uint8_t *src;
    uint32_t len;
    uint32_t pos;
    uint32_t bitbuf;
    uint32_t bitcnt;
    uint8_t *dst;
    uint32_t cap;
    uint32_t out;
} inflate_t;

typedef struct {
    uint16_t count[16];
    uint16_t symbol[288];
} huff_t;

// DEFLATE distance trees have at most 30 symbols; a 288-symbol table would put
// another 516 bytes on the input task's stack for every dynamic block.
typedef struct {
    uint16_t count[16];
    uint16_t symbol[30];
} huff_dist_t;

static bool need_bits(inflate_t *s, uint32_t n)
{
    while (s->bitcnt < n) {
        if (s->pos >= s->len) return false;
        s->bitbuf |= (uint32_t)s->src[s->pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    return true;
}

static bool get_bits(inflate_t *s, uint32_t n, uint32_t *out)
{
    if (n > 24 || !need_bits(s, n)) return false;
    *out = s->bitbuf & ((1u << n) - 1u);
    s->bitbuf >>= n;
    s->bitcnt -= n;
    return true;
}

static bool put_byte(inflate_t *s, uint8_t v)
{
    if (s->out >= s->cap) return false;
    s->dst[s->out++] = v;
    return true;
}

static bool build_huff(uint16_t *count, uint16_t *symbol,
                       const uint8_t *lengths, int n)
{
    memset(count, 0, 16 * sizeof(uint16_t));
    for (int i = 0; i < n; ++i) {
        if (lengths[i] > 15) return false;
        count[lengths[i]]++;
    }
    if (count[0] == n) return true;   // 全 0：合法但不可解码
    int left = 1;
    for (int len = 1; len <= 15; ++len) {
        left <<= 1;
        left -= count[len];
        if (left < 0) return false;      // 过度订阅
    }
    uint16_t offs[16];
    offs[0] = 0;
    offs[1] = 0;
    for (int len = 1; len < 15; ++len) offs[len + 1] = (uint16_t)(offs[len] + count[len]);
    for (int i = 0; i < n; ++i) {
        if (lengths[i]) symbol[offs[lengths[i]]++] = (uint16_t)i;
    }
    return true;
}

static int decode_symbol(inflate_t *s, const uint16_t *count, const uint16_t *symbol)
{
    int code = 0;
    int first = 0;
    int index = 0;
    for (int len = 1; len <= 15; ++len) {
        uint32_t bit;
        if (!get_bits(s, 1, &bit)) return -1;
        code |= (int)bit;
        const int n = count[len];
        if (code - first < n) return symbol[index + (code - first)];
        index += n;
        first = (first + n) << 1;
        code <<= 1;
    }
    return -1;
}

static const uint16_t LEN_BASE[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t LEN_EXTRA[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t DIST_BASE[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193,
    12289, 16385, 24577};
static const uint8_t DIST_EXTRA[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static bool inflate_block_data(inflate_t *s, const huff_t *lit,
                               const huff_dist_t *dist)
{
    for (;;) {
        const int sym = decode_symbol(s, lit->count, lit->symbol);
        if (sym < 0) return false;
        if (sym < 256) {
            if (!put_byte(s, (uint8_t)sym)) return false;
            continue;
        }
        if (sym == 256) return true;
        if (sym < 257 || sym > 285) return false;
        const int li = sym - 257;
        uint32_t extra = 0;
        if (LEN_EXTRA[li] && !get_bits(s, LEN_EXTRA[li], &extra)) return false;
        const uint32_t length = LEN_BASE[li] + extra;
        const int dsym = decode_symbol(s, dist->count, dist->symbol);
        if (dsym < 0 || dsym > 29) return false;
        extra = 0;
        if (DIST_EXTRA[dsym] && !get_bits(s, DIST_EXTRA[dsym], &extra)) return false;
        const uint32_t distance = DIST_BASE[dsym] + extra;
        if (distance == 0 || distance > s->out) return false;
        if (s->out + length > s->cap) return false;
        for (uint32_t i = 0; i < length; ++i) {
            s->dst[s->out] = s->dst[s->out - distance];
            s->out++;
        }
    }
}

static bool inflate_fixed(inflate_t *s)
{
    uint8_t lengths[288];
    for (int i = 0; i < 144; ++i) lengths[i] = 8;
    for (int i = 144; i < 256; ++i) lengths[i] = 9;
    for (int i = 256; i < 280; ++i) lengths[i] = 7;
    for (int i = 280; i < 288; ++i) lengths[i] = 8;
    huff_t lit;
    huff_dist_t dist;
    if (!build_huff(lit.count, lit.symbol, lengths, 288)) return false;
    memset(lengths, 5, 30);
    if (!build_huff(dist.count, dist.symbol, lengths, 30)) return false;
    return inflate_block_data(s, &lit, &dist);
}

static bool inflate_dynamic(inflate_t *s)
{
    static const uint8_t ORDER[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    uint32_t hlit, hdist, hclen;
    if (!get_bits(s, 5, &hlit) || !get_bits(s, 5, &hdist) ||
        !get_bits(s, 4, &hclen)) {
        return false;
    }
    hlit += 257;
    hdist += 1;
    hclen += 4;
    if (hlit > 286 || hdist > 30) return false;
    uint8_t cl[19];
    memset(cl, 0, sizeof(cl));
    for (uint32_t i = 0; i < hclen; ++i) {
        uint32_t v;
        if (!get_bits(s, 3, &v)) return false;
        cl[ORDER[i]] = (uint8_t)v;
    }
    huff_dist_t clh;   // 19 code-length symbols; no 288-entry table needed
    if (!build_huff(clh.count, clh.symbol, cl, 19)) return false;
    uint8_t lengths[288 + 30];
    uint32_t total = hlit + hdist;
    uint32_t i = 0;
    while (i < total) {
        const int sym = decode_symbol(s, clh.count, clh.symbol);
        if (sym < 0) return false;
        if (sym < 16) {
            lengths[i++] = (uint8_t)sym;
        } else if (sym == 16) {
            if (i == 0) return false;
            uint32_t rep;
            if (!get_bits(s, 2, &rep)) return false;
            const uint8_t prev = lengths[i - 1];
            for (uint32_t k = 0; k < 3 + rep && i < total; ++k) lengths[i++] = prev;
        } else if (sym == 17) {
            uint32_t rep;
            if (!get_bits(s, 3, &rep)) return false;
            for (uint32_t k = 0; k < 3 + rep && i < total; ++k) lengths[i++] = 0;
        } else if (sym == 18) {
            uint32_t rep;
            if (!get_bits(s, 7, &rep)) return false;
            for (uint32_t k = 0; k < 11 + rep && i < total; ++k) lengths[i++] = 0;
        } else {
            return false;
        }
    }
    if (lengths[256] == 0) return false;
    huff_t lit;
    huff_dist_t dist;
    if (!build_huff(lit.count, lit.symbol, lengths, (int)hlit)) return false;
    if (!build_huff(dist.count, dist.symbol, lengths + hlit, (int)hdist)) return false;
    return inflate_block_data(s, &lit, &dist);
}

bool clannad_inflate_raw(const uint8_t *src, uint32_t src_len,
                         uint8_t *dst, uint32_t dst_cap, uint32_t *out_len)
{
    if (!src || !dst) return false;
    inflate_t s;
    memset(&s, 0, sizeof(s));
    s.src = src;
    s.len = src_len;
    s.dst = dst;
    s.cap = dst_cap;
    for (;;) {
        uint32_t final, type;
        if (!get_bits(&s, 1, &final) || !get_bits(&s, 2, &type)) return false;
        if (type == 0) {
            s.bitbuf = 0;
            s.bitcnt = 0;
            if (s.pos + 4 > s.len) return false;
            const uint32_t len = clannad_rd16(s.src + s.pos);
            const uint32_t nlen = clannad_rd16(s.src + s.pos + 2);
            s.pos += 4;
            if ((len ^ 0xFFFFu) != nlen) return false;
            if (s.pos + len > s.len || s.out + len > s.cap) return false;
            memcpy(s.dst + s.out, s.src + s.pos, len);
            s.pos += len;
            s.out += len;
        } else if (type == 1) {
            if (!inflate_fixed(&s)) return false;
        } else if (type == 2) {
            if (!inflate_dynamic(&s)) return false;
        } else {
            return false;
        }
        if (final) break;
    }
    if (out_len) *out_len = s.out;
    return true;
}

// ---------------------------------------------------------------- sections
static const uint8_t *section_ptr(const clannad_pack_t *pack, uint32_t type,
                                   uint32_t *count, uint32_t *size)
{
    for (uint16_t i = 0; i < CLAN_SEC_COUNT; ++i) {
        const uint8_t *sec = pack->data + 24u + (size_t)i * 16u;
        if (clannad_rd32(sec) != type) continue;
        const uint32_t off = clannad_rd32(sec + 4);
        if (!range_ok(pack->size, off, clannad_rd32(sec + 12))) return NULL;
        if (count) *count = clannad_rd32(sec + 8);
        if (size) *size = clannad_rd32(sec + 12);
        return pack->data + off;
    }
    return NULL;
}

bool clannad_pack_open(clannad_pack_t *pack, const uint8_t *data, uint32_t size)
{
    if (!pack || !data || size < 24) return false;
    memset(pack, 0, sizeof(*pack));
    if (memcmp(data, CLAN_PACK_MAGIC, 8) != 0) return false;
    const uint32_t version = clannad_rd32(data + 8);
    const uint32_t total = clannad_rd32(data + 12);
    const uint16_t sections = clannad_rd16(data + 16);
    if (version != CLAN_PACK_VERSION || total != size) return false;
    if (sections != CLAN_SEC_COUNT || size < 24u + (uint32_t)sections * 16u) return false;
    pack->data = data;
    pack->size = size;

    uint32_t count = 0, sec_size = 0;
    pack->text = section_ptr(pack, CLAN_SEC_TEXT, &count, &sec_size);
    pack->text_size = sec_size;
    pack->speaker = section_ptr(pack, CLAN_SEC_SPEAKER, &count, &sec_size);
    pack->speaker_count = count;
    pack->catalog = section_ptr(pack, CLAN_SEC_CATALOG, &count, &sec_size);
    pack->catalog_size = sec_size;
    pack->entry = section_ptr(pack, CLAN_SEC_ENTRY, &count, &sec_size);
    pack->entry_count = count;
    pack->assetname = section_ptr(pack, CLAN_SEC_ASSETNAME, &count, &sec_size);
    pack->assetname_count = count;
    pack->asset = section_ptr(pack, CLAN_SEC_ASSET, &count, &sec_size);
    pack->asset_count = count;
    pack->gallery = section_ptr(pack, CLAN_SEC_GALLERY, &count, &sec_size);
    pack->gallery_group_count = count / CLAN_GALLERY_GROUP_BYTES;
    if (pack->gallery_group_count) {
        const uint32_t groups_bytes = pack->gallery_group_count * CLAN_GALLERY_GROUP_BYTES;
        if (groups_bytes > sec_size) return false;
        pack->gallery_entry_count = (sec_size - groups_bytes) / 2u;
    }
    pack->blob = section_ptr(pack, CLAN_SEC_BLOB, &count, &sec_size);
    pack->blob_size = sec_size;
    pack->ops = section_ptr(pack, CLAN_SEC_OPS, &count, &sec_size);
    pack->ops_size = sec_size;
    pack->meta = section_ptr(pack, CLAN_SEC_META, &count, &sec_size);
    pack->meta_size = sec_size;
    pack->alias = section_ptr(pack, CLAN_SEC_ALIAS, &count, &sec_size);
    pack->alias_count = count;

    if (!pack->text || !pack->speaker || !pack->catalog || !pack->entry ||
        !pack->assetname || !pack->asset || !pack->gallery || !pack->blob ||
        !pack->ops || !pack->meta || !pack->alias) {
        return false;
    }
    if (pack->catalog_size < 8) return false;
    pack->scene_count = clannad_rd16(pack->catalog);
    pack->block_count = clannad_rd16(pack->catalog + 2);
    pack->route_count = clannad_rd16(pack->catalog + 4);
    pack->chapter_count = clannad_rd16(pack->catalog + 6);
    if (pack->asset_count != pack->assetname_count) return false;
    if (pack->asset_count == 0 || pack->scene_count == 0 || pack->block_count == 0) {
        return false;
    }
    return true;
}

static uint32_t catalog_scene_off(const clannad_pack_t *pack)
{
    (void)pack;
    return 8;
}

static uint32_t catalog_block_off(const clannad_pack_t *pack)
{
    return 8u + (uint32_t)pack->scene_count * CLAN_SCENE_BYTES;
}

static uint32_t catalog_route_off(const clannad_pack_t *pack)
{
    return catalog_block_off(pack) + (uint32_t)pack->block_count * CLAN_BLOCK_REC_BYTES;
}

static uint32_t catalog_chapter_off(const clannad_pack_t *pack)
{
    return catalog_route_off(pack) + (uint32_t)pack->route_count * 8u;
}

static bool catalog_fits(const clannad_pack_t *pack)
{
    return catalog_chapter_off(pack) + (uint32_t)pack->chapter_count * 8u <=
           pack->catalog_size;
}

bool clannad_pack_scene(const clannad_pack_t *pack, uint16_t index, clannad_scene_t *out)
{
    if (!pack || !out || index >= pack->scene_count || !catalog_fits(pack)) return false;
    const uint8_t *p = pack->catalog + catalog_scene_off(pack) +
                       (size_t)index * CLAN_SCENE_BYTES;
    out->id = clannad_rd16(p);
    out->name_off = clannad_rd16(p + 2);
    out->source_off = clannad_rd16(p + 4);
    out->length = clannad_rd16(p + 6);
    out->entries_index = clannad_rd16(p + 8);
    out->entry_count = p[10];
    out->route = p[11];
    out->first_block = clannad_rd16(p + 12);
    out->block_count = clannad_rd16(p + 14);
    return out->name_off < pack->text_size && out->source_off < pack->text_size;
}

bool clannad_pack_find_scene(const clannad_pack_t *pack, uint16_t id, uint16_t *index_out)
{
    if (!pack) return false;
    for (uint16_t i = 0; i < pack->scene_count; ++i) {
        clannad_scene_t scene;
        if (!clannad_pack_scene(pack, i, &scene)) return false;
        if (scene.id == id) {
            if (index_out) *index_out = i;
            return true;
        }
    }
    return false;
}

bool clannad_pack_block(const clannad_pack_t *pack, uint16_t block_index,
                        clannad_block_rec_t *out)
{
    if (!pack || !out || block_index >= pack->block_count || !catalog_fits(pack)) {
        return false;
    }
    const uint8_t *p = pack->catalog + catalog_block_off(pack) +
                       (size_t)block_index * CLAN_BLOCK_REC_BYTES;
    out->off = clannad_rd32(p);
    out->comp_size = clannad_rd32(p + 4);
    out->raw_size = clannad_rd32(p + 8);
    out->op_count = clannad_rd16(p + 12);
    if (out->comp_size == 0 || out->raw_size < 8) return false;
    if (out->op_count > CLAN_BLOCK_SIZE) return false;
    if (!range_ok(pack->ops_size, out->off, out->comp_size)) return false;
    return true;
}

bool clannad_pack_chapter(const clannad_pack_t *pack, uint16_t index, clannad_chapter_t *out)
{
    if (!pack || !out || index >= pack->chapter_count || !catalog_fits(pack)) return false;
    const uint8_t *p = pack->catalog + catalog_chapter_off(pack) + (size_t)index * 8u;
    out->scene_id = clannad_rd16(p);
    out->name_off = clannad_rd16(p + 2);
    out->route = p[4];
    out->flags = p[5];
    return out->name_off < pack->text_size;
}

bool clannad_pack_route(const clannad_pack_t *pack, uint16_t index, clannad_route_t *out)
{
    if (!pack || !out || index >= pack->route_count || !catalog_fits(pack)) return false;
    const uint8_t *p = pack->catalog + catalog_route_off(pack) + (size_t)index * 8u;
    out->name_off = clannad_rd16(p);
    out->first_chapter = clannad_rd16(p + 2);
    out->chapter_count = clannad_rd16(p + 4);
    return out->name_off < pack->text_size &&
           (uint32_t)out->first_chapter + out->chapter_count <= pack->chapter_count;
}

bool clannad_pack_asset(const clannad_pack_t *pack, uint16_t index, clannad_asset_t *out)
{
    if (!pack || !out || index >= pack->asset_count) return false;
    const uint8_t *p = pack->asset + (size_t)index * CLAN_ASSET_BYTES;
    out->jpeg_off = clannad_rd32(p);
    out->jpeg_len = clannad_rd32(p + 4);
    out->w = clannad_rd16(p + 8);
    out->h = clannad_rd16(p + 10);
    out->mask_off = clannad_rd32(p + 12);
    out->mask_len = clannad_rd32(p + 16);
    out->ow = clannad_rd16(p + 20);
    out->oh = clannad_rd16(p + 22);
    out->ax = (int16_t)clannad_rd16(p + 24);
    out->ay = (int16_t)clannad_rd16(p + 26);
    out->kind = p[28];
    if (!range_ok(pack->blob_size, out->jpeg_off, out->jpeg_len)) return false;
    if (out->mask_len && !range_ok(pack->blob_size, out->mask_off, out->mask_len)) {
        return false;
    }
    return out->jpeg_len > 0 && out->w > 0 && out->h > 0;
}

bool clannad_pack_gallery_group(const clannad_pack_t *pack, uint16_t index,
                                clannad_gallery_group_t *out)
{
    if (!pack || !out || index >= pack->gallery_group_count) return false;
    const uint8_t *p = pack->gallery + (size_t)index * CLAN_GALLERY_GROUP_BYTES;
    out->label_off = clannad_rd16(p);
    out->first_entry = clannad_rd16(p + 2);
    out->count = clannad_rd16(p + 4);
    out->cover = clannad_rd16(p + 6);
    if (out->label_off >= pack->text_size) return false;
    if ((uint32_t)out->first_entry + out->count > pack->gallery_entry_count) return false;
    if (out->cover >= pack->asset_count) return false;
    return true;
}

bool clannad_pack_gallery_entry(const clannad_pack_t *pack, uint16_t index,
                                clannad_gallery_entry_t *out)
{
    if (!pack || !out || index >= pack->gallery_entry_count) return false;
    const uint8_t *p = pack->gallery + pack->gallery_group_count * CLAN_GALLERY_GROUP_BYTES +
                       (size_t)index * 2u;
    out->asset = clannad_rd16(p);
    return out->asset < pack->asset_count;
}

const char *clannad_pack_text(const clannad_pack_t *pack, uint32_t off)
{
    if (!pack || off >= pack->text_size) return "";
    return (const char *)(pack->text + off);
}

const char *clannad_pack_speaker(const clannad_pack_t *pack, uint16_t index)
{
    if (!pack || index >= pack->speaker_count) return "";
    const uint16_t off = clannad_rd16(pack->speaker + (size_t)index * 2u);
    return clannad_pack_text(pack, off);
}

const char *clannad_pack_scene_name(const clannad_pack_t *pack, uint16_t scene_index)
{
    clannad_scene_t scene;
    if (!clannad_pack_scene(pack, scene_index, &scene)) return "";
    return clannad_pack_text(pack, scene.name_off);
}

const char *clannad_pack_scene_source(const clannad_pack_t *pack, uint16_t scene_index)
{
    clannad_scene_t scene;
    if (!clannad_pack_scene(pack, scene_index, &scene)) return "";
    return clannad_pack_text(pack, scene.source_off);
}

const char *clannad_pack_chapter_name(const clannad_pack_t *pack, uint16_t chapter_index)
{
    clannad_chapter_t chapter;
    if (!clannad_pack_chapter(pack, chapter_index, &chapter)) return "";
    return clannad_pack_text(pack, chapter.name_off);
}

const char *clannad_pack_route_name(const clannad_pack_t *pack, uint16_t route_index)
{
    clannad_route_t route;
    if (!clannad_pack_route(pack, route_index, &route)) return "";
    return clannad_pack_text(pack, route.name_off);
}

const char *clannad_pack_gallery_label(const clannad_pack_t *pack, uint16_t group)
{
    clannad_gallery_group_t info;
    if (!clannad_pack_gallery_group(pack, group, &info)) return "";
    return clannad_pack_text(pack, info.label_off);
}

const char *clannad_pack_asset_name(const clannad_pack_t *pack, uint16_t asset)
{
    if (!pack || asset >= pack->assetname_count) return "";
    const uint16_t off = clannad_rd16(pack->assetname + (size_t)asset * 2u);
    return clannad_pack_text(pack, off);
}

int clannad_pack_meta_int(const clannad_pack_t *pack, const char *key, int fallback)
{
    if (!pack || !key) return fallback;
    const size_t key_len = strlen(key);
    const char *p = (const char *)pack->meta;
    const char *end = p + pack->meta_size;
    while (p < end) {
        const char *line_end = memchr(p, '\n', (size_t)(end - p));
        if (!line_end) line_end = end;
        if ((size_t)(line_end - p) > key_len && p[key_len] == '=' &&
            memcmp(p, key, key_len) == 0) {
            int value = 0;
            bool any = false;
            for (const char *q = p + key_len + 1; q < line_end; ++q) {
                if (*q < '0' || *q > '9') break;
                value = value * 10 + (*q - '0');
                any = true;
            }
            return any ? value : fallback;
        }
        p = line_end + 1;
    }
    return fallback;
}

const char *clannad_pack_meta(const clannad_pack_t *pack, const char *key)
{
    // 仅用于调试：把值复制进静态缓冲不必要，调用方只读一行；返回指向
    // 值起点并依赖调用方限制长度。测试只比较前缀。
    if (!pack || !key) return "";
    const size_t key_len = strlen(key);
    const char *p = (const char *)pack->meta;
    const char *end = p + pack->meta_size;
    while (p < end) {
        const char *line_end = memchr(p, '\n', (size_t)(end - p));
        if (!line_end) line_end = end;
        if ((size_t)(line_end - p) > key_len && p[key_len] == '=' &&
            memcmp(p, key, key_len) == 0) {
            return p + key_len + 1;
        }
        p = line_end + 1;
    }
    return "";
}

bool clannad_pack_copy_text(const clannad_pack_t *pack, uint32_t off, char *out, size_t cap)
{
    if (!pack || !out || cap == 0 || off >= pack->text_size) return false;
    const char *src = (const char *)pack->text + off;
    size_t n = 0;
    while (off + n < pack->text_size && src[n] && n + 1 < cap) {
        out[n] = src[n];
        ++n;
    }
    out[n] = '\0';
    return true;
}

// ------------------------------------------------------------------- blocks
bool clannad_pack_load_block(const clannad_pack_t *pack, uint16_t block_index,
                             uint8_t *raw, uint32_t cap, clannad_block_rec_t *rec_out)
{
    clannad_block_rec_t rec;
    if (!clannad_pack_block(pack, block_index, &rec)) return false;
    if (rec.raw_size > cap) return false;
    uint32_t out_len = 0;
    if (!clannad_inflate_raw(pack->ops + rec.off, rec.comp_size, raw, cap, &out_len)) {
        return false;
    }
    if (out_len != rec.raw_size) return false;
    const uint16_t op_count = clannad_rd16(raw);
    const uint16_t text_size = clannad_rd16(raw + 2);
    const uint16_t extra_size = clannad_rd16(raw + 4);
    if (op_count != rec.op_count) return false;
    if (8u + (uint32_t)op_count * CLAN_OP_BYTES + extra_size + text_size != out_len) {
        return false;
    }
    if (rec_out) *rec_out = rec;
    return true;
}

bool clannad_pack_block_op(const uint8_t *raw, uint32_t raw_size, uint16_t op_index,
                           clannad_op_t *out)
{
    if (!raw || !out || raw_size < 8) return false;
    const uint16_t op_count = clannad_rd16(raw);
    if (op_index >= op_count) return false;
    const uint32_t off = 8u + (uint32_t)op_index * CLAN_OP_BYTES;
    if (off + CLAN_OP_BYTES > raw_size) return false;
    const uint8_t *p = raw + off;
    out->type = p[0];
    out->n = p[1];
    out->x = clannad_rd16(p + 2);
    out->y = clannad_rd16(p + 4);
    out->z = clannad_rd16(p + 6);
    out->text_off = clannad_rd16(p + 8);
    out->text_len = clannad_rd16(p + 10);
    out->extra = clannad_rd16(p + 12);
    return true;
}

const uint8_t *clannad_pack_block_extra(const uint8_t *raw, uint32_t raw_size,
                                        uint16_t op_count, uint16_t *size_out)
{
    if (!raw || raw_size < 8) return NULL;
    const uint16_t text_size = clannad_rd16(raw + 2);
    const uint16_t extra_size = clannad_rd16(raw + 4);
    const uint32_t off = 8u + (uint32_t)op_count * CLAN_OP_BYTES;
    if ((uint64_t)off + extra_size + text_size != raw_size) return NULL;
    if (size_out) *size_out = extra_size;
    return raw + off;
}

const char *clannad_pack_block_text(const uint8_t *raw, uint32_t raw_size,
                                    uint16_t op_count, uint16_t extra_size,
                                    uint16_t *size_out)
{
    if (!raw || raw_size < 8) return NULL;
    const uint16_t text_size = clannad_rd16(raw + 2);
    const uint32_t off = 8u + (uint32_t)op_count * CLAN_OP_BYTES + extra_size;
    if ((uint64_t)off + text_size != raw_size) return NULL;
    if (size_out) *size_out = text_size;
    return (const char *)raw + off;
}

// ---------------------------------------------------------------- validate
bool clannad_pack_validate(const clannad_pack_t *pack, char *why, size_t cap)
{
    if (!pack) return false;
#define FAIL(msg) do { if (why && cap) snprintf(why, cap, "%s", msg); return false; } while (0)
    if (!catalog_fits(pack)) FAIL("catalog table overflow");
    if (pack->entry_count < (uint32_t)pack->scene_count * CLAN_ENTRY_KEEP) {
        FAIL("entry table too short");
    }
    for (uint16_t i = 0; i < pack->scene_count; ++i) {
        clannad_scene_t scene;
        if (!clannad_pack_scene(pack, i, &scene)) FAIL("bad scene record");
        if (scene.entry_count != CLAN_ENTRY_KEEP) FAIL("bad scene entry count");
        if (scene.entry_count == 0) FAIL("scene without entries");
        if ((uint32_t)scene.first_block + scene.block_count > pack->block_count) {
            FAIL("scene block range");
        }
        if ((uint32_t)scene.entries_index + scene.entry_count > pack->entry_count) {
            FAIL("scene entry range");
        }
        if (scene.length == 0) FAIL("empty scene");
        if (scene.block_count != (scene.length + CLAN_BLOCK_SIZE - 1) / CLAN_BLOCK_SIZE) {
            FAIL("scene block count mismatch");
        }
    }
    for (uint16_t i = 0; i < pack->block_count; ++i) {
        clannad_block_rec_t rec;
        if (!clannad_pack_block(pack, i, &rec)) FAIL("bad block record");
        if (rec.raw_size > 65535u) FAIL("block raw too large");
        if ((uint32_t)rec.op_count * CLAN_OP_BYTES + 8u > rec.raw_size) {
            FAIL("block op bytes");
        }
    }
    for (uint16_t i = 0; i < pack->asset_count; ++i) {
        clannad_asset_t asset;
        if (!clannad_pack_asset(pack, i, &asset)) FAIL("bad asset record");
        if (asset.w > 640 || asset.h > 480) FAIL("implausible asset size");
        if (asset.mask_len) {
            const uint32_t stride = ((uint32_t)asset.w + 7u) / 8u;
            if (asset.mask_len != stride * asset.h) FAIL("mask size mismatch");
        }
    }
    for (uint16_t i = 0; i < pack->route_count; ++i) {
        clannad_route_t route;
        if (!clannad_pack_route(pack, i, &route)) FAIL("bad route record");
    }
    for (uint16_t i = 0; i < pack->chapter_count; ++i) {
        clannad_chapter_t chapter;
        uint16_t scene_index;
        if (!clannad_pack_chapter(pack, i, &chapter)) FAIL("bad chapter record");
        if (!clannad_pack_find_scene(pack, chapter.scene_id, &scene_index)) {
            FAIL("chapter scene missing");
        }
    }
    for (uint16_t i = 0; i < pack->gallery_group_count; ++i) {
        clannad_gallery_group_t group;
        if (!clannad_pack_gallery_group(pack, i, &group)) FAIL("bad gallery group");
        for (uint16_t k = 0; k < group.count; ++k) {
            clannad_gallery_entry_t entry;
            if (!clannad_pack_gallery_entry(pack, group.first_entry + k, &entry)) {
                FAIL("bad gallery entry");
            }
        }
    }
    return true;
#undef FAIL
}

// ---------------------------------------------------------------- deflate
// 仅用于存档（约 1 KB）的固定 Huffman 压缩器：greedy LZ77 + RFC1951 fixed codes。
    // 压缩失败/不可压缩时回退到 stored block。解压统一走上面同一份 inflate。
typedef struct {
    uint8_t *out;
    uint32_t cap;
    uint32_t len;
    uint32_t bitbuf;
    int bitcnt;
    bool ok;
} bitw_t;

static void bw_put(bitw_t *w, uint32_t bits, int n)
{
    while (n > 0) {
        const int take = n > 16 ? 16 : n;
        const uint32_t mask = (1u << take) - 1u;
        w->bitbuf |= (bits & mask) << w->bitcnt;
        w->bitcnt += take;
        bits >>= take;
        n -= take;
        while (w->bitcnt >= 8) {
            if (w->len < w->cap) w->out[w->len++] = (uint8_t)w->bitbuf;
            else w->ok = false;
            w->bitbuf >>= 8;
            w->bitcnt -= 8;
        }
    }
}

static uint32_t reverse_bits(uint32_t v, int n)
{
    uint32_t r = 0;
    for (int i = 0; i < n; ++i) {
        r = (r << 1) | (v & 1u);
        v >>= 1;
    }
    return r;
}

static void bw_code(bitw_t *w, uint32_t code, int n)
{
    bw_put(w, reverse_bits(code, n), n);
}

static void bw_lit(bitw_t *w, uint32_t v)
{
    if (v <= 143u) bw_code(w, 0x30u + v, 8);
    else if (v <= 255u) bw_code(w, 0x190u + (v - 144u), 9);
    else if (v <= 279u) bw_code(w, v - 256u, 7);
    else bw_code(w, 0xC0u + (v - 280u), 8);
}

static void bw_flush(bitw_t *w)
{
    if (w->bitcnt > 0) {
        if (w->len < w->cap) w->out[w->len++] = (uint8_t)w->bitbuf;
        else w->ok = false;
        w->bitbuf = 0;
        w->bitcnt = 0;
    }
}

static void bw_align(bitw_t *w)
{
    if (w->bitcnt > 0) bw_put(w, 0, 8 - w->bitcnt);
}

static bool deflate_stored(const uint8_t *src, uint32_t len, uint8_t *dst,
                           uint32_t cap, uint32_t *out_len)
{
    bitw_t w = {.out = dst, .cap = cap, .len = 0, .bitbuf = 0, .bitcnt = 0, .ok = true};
    uint32_t done = 0;
    do {
        const uint32_t take = len - done > 65535u ? 65535u : len - done;
        const bool final = done + take == len;
        bw_put(&w, final ? 1u : 0u, 1);
        bw_put(&w, 0u, 2);
        bw_align(&w);
        if (w.len + 4u + take > cap) return false;
        w.out[w.len++] = (uint8_t)take;
        w.out[w.len++] = (uint8_t)(take >> 8);
        w.out[w.len++] = (uint8_t)~take;
        w.out[w.len++] = (uint8_t)(~take >> 8);
        memcpy(w.out + w.len, src + done, take);
        w.len += take;
        done += take;
    } while (done < len);
    if (out_len) *out_len = w.len;
    return true;
}

static const uint16_t LEN_BASE_ENC[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t LEN_EXTRA_ENC[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t DIST_BASE_ENC[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193,
    12289, 16385, 24577};
static const uint8_t DIST_EXTRA_ENC[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static void emit_match(bitw_t *w, uint32_t length, uint32_t distance)
{
    int li = 0;
    while (li < 28 && length >= LEN_BASE_ENC[li + 1]) ++li;
    bw_lit(w, (uint32_t)(257 + li));
    if (LEN_EXTRA_ENC[li]) bw_put(w, length - LEN_BASE_ENC[li], LEN_EXTRA_ENC[li]);
    int di = 0;
    while (di < 29 && distance >= DIST_BASE_ENC[di + 1]) ++di;
    bw_code(w, (uint32_t)di, 5);
    if (DIST_EXTRA_ENC[di]) bw_put(w, distance - DIST_BASE_ENC[di], DIST_EXTRA_ENC[di]);
}

bool clannad_deflate_raw(const uint8_t *src, uint32_t len, uint8_t *dst,
                         uint32_t cap, uint32_t *out_len)
{
    if (!src || !dst || len == 0) return false;
    if (len > 60000u) return false;   // 存档远小于此；超出直接走 stored
    static int16_t head[1024];
    static int16_t prev[2048];
    if (len > sizeof(prev) / sizeof(prev[0])) return false;
    for (int i = 0; i < 1024; ++i) head[i] = -1;
    bitw_t w = {.out = dst, .cap = cap, .len = 0, .bitbuf = 0, .bitcnt = 0, .ok = true};
    bw_put(&w, 1u, 1);   // BFINAL
    bw_put(&w, 1u, 2);   // fixed Huffman
    uint32_t pos = 0;
    while (pos < len) {
        uint32_t best_len = 0;
        uint32_t best_dist = 0;
        if (pos + 2 < len) {
            const uint32_t h = ((uint32_t)src[pos] * 251u + src[pos + 1]) * 251u +
                               src[pos + 2];
            const uint32_t slot = (h ^ (h >> 12)) & 1023u;
            int32_t cand = head[slot];
            int probes = 0;
            while (cand >= 0 && probes < 64) {
                const uint32_t dist = pos - (uint32_t)cand;
                if (dist == 0 || dist > 32768u) break;
                if (src[cand] == src[pos] && src[cand + 1] == src[pos + 1]) {
                    uint32_t m = 2;
                    const uint32_t max = len - pos > 258u ? 258u : len - pos;
                    while (m < max && src[cand + m] == src[pos + m]) ++m;
                    if (m > best_len) {
                        best_len = m;
                        best_dist = dist;
                        if (m == 258u) break;
                    }
                }
                cand = prev[cand];
                ++probes;
            }
            prev[pos] = head[slot];
            head[slot] = (int16_t)pos;
        }
        if (best_len >= 3u) {
            emit_match(&w, best_len, best_dist);
            for (uint32_t i = 1; i < best_len; ++i) {
                const uint32_t p = pos + i;
                if (p + 2 >= len) break;
                const uint32_t h = ((uint32_t)src[p] * 251u + src[p + 1]) * 251u +
                                   src[p + 2];
                const uint32_t slot = (h ^ (h >> 12)) & 1023u;
                prev[p] = head[slot];
                head[slot] = (int16_t)p;
            }
            pos += best_len;
        } else {
            bw_lit(&w, src[pos]);
            ++pos;
        }
        if (!w.ok) break;
    }
    bw_lit(&w, 256u);
    bw_flush(&w);
    if (!w.ok || w.len >= len + 5u) {
        return deflate_stored(src, len, dst, cap, out_len);
    }
    if (out_len) *out_len = w.len;
    return true;
}
