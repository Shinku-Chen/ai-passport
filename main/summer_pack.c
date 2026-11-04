// main/summer_pack.c —— 资源包解析、12 位正文解码、指令解码。
#include "summer_pack.h"

#include <string.h>

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static bool section_range(const uint8_t *blob, uint32_t size, uint32_t off, uint32_t len)
{
    (void)blob;
    return off <= size && len <= size - off;
}

// uvarint，最多 5 字节；失败返回 false。
static bool read_uvar(const uint8_t **pp, const uint8_t *end, uint32_t *out)
{
    const uint8_t *p = *pp;
    uint32_t value = 0;
    uint32_t shift = 0;
    for (int i = 0; i < 5; ++i) {
        if (p >= end) return false;
        const uint8_t b = *p++;
        value |= (uint32_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) {
            *pp = p;
            *out = value;
            return true;
        }
        shift += 7;
    }
    return false;
}

static bool read_svar(const uint8_t **pp, const uint8_t *end, int32_t *out)
{
    uint32_t u = 0;
    if (!read_uvar(pp, end, &u)) return false;
    *out = (int32_t)((u >> 1) ^ (uint32_t)(-(int32_t)(u & 1u)));
    return true;
}

static const uint8_t *section_ptr(const uint8_t *blob, uint32_t size,
                                  uint32_t off, uint32_t len)
{
    return section_range(blob, size, off, len) ? blob + off : NULL;
}

static bool check_count(uint32_t count, uint32_t size, uint32_t elem)
{
    if (elem == 0) return size == 0;
    // Strict multiplication, not size / elem: an ignored remainder would let a
    // section with trailing junk look valid.
    return count <= UINT32_MAX / elem && count * elem == size;
}

bool summer_pack_open(summer_pack_t *pack, const uint8_t *blob, uint32_t size,
                      const char **err)
{
    const char *dummy = NULL;
    if (!err) err = &dummy;
    *err = NULL;
    if (!pack || !blob) {
        *err = "pack is null";
        return false;
    }
    memset(pack, 0, sizeof(*pack));
    if (size < SUMMER_PACK_HEADER_SIZE) {
        *err = "pack truncated";
        return false;
    }
    if (((uintptr_t)blob & 3u) != 0) {
        *err = "pack base is not 4-byte aligned";
        return false;
    }
    if (memcmp(blob, SUMMER_PACK_MAGIC, 8) != 0) {
        *err = "bad magic";
        return false;
    }
    const uint16_t version = rd16(blob + 8);
    const uint16_t header_size = rd16(blob + 10);
    const uint16_t section_count = rd16(blob + 12);
    const uint16_t flags = rd16(blob + 14);
    const uint32_t total = rd32(blob + 16);
    if (version != SUMMER_PACK_VERSION) {
        *err = "bad version";
        return false;
    }
    if (section_count != SUMMER_PACK_SECTION_COUNT) {
        *err = "bad section count";
        return false;
    }
    if (header_size != SUMMER_PACK_HEADER_SIZE + section_count * SUMMER_PACK_SECTION_SIZE) {
        *err = "bad header size";
        return false;
    }
    // The 20-byte raw header may be readable while the section table itself is
    // truncated.  Reject before touching any section record.
    if ((uint32_t)header_size > size) {
        *err = "header exceeds pack size";
        return false;
    }
    if (total != size) {
        *err = "bad total size";
        return false;
    }
    if (flags != 0) {
        *err = "bad flags";
        return false;
    }

    struct {
        uint32_t off;
        uint32_t count;
        uint32_t len;
    } sec[SUMMER_PACK_SECTION_COUNT];
    bool seen[SUMMER_PACK_SECTION_COUNT] = { false };
    memset(sec, 0, sizeof(sec));
    for (uint32_t i = 0; i < section_count; ++i) {
        const uint8_t *e = blob + SUMMER_PACK_HEADER_SIZE + i * SUMMER_PACK_SECTION_SIZE;
        const uint16_t type = rd16(e);
        if (type >= SUMMER_PACK_SECTION_COUNT) {
            *err = "bad section type";
            return false;
        }
        if (seen[type]) {
            *err = "duplicate section type";
            return false;
        }
        seen[type] = true;
        sec[type].off = rd32(e + 4);
        sec[type].count = rd32(e + 8);
        sec[type].len = rd32(e + 12);
        if (sec[type].off < header_size) {
            *err = "section starts inside header";
            return false;
        }
        if (!section_range(blob, size, sec[type].off, sec[type].len) ||
            (sec[type].off & 3u) != 0) {
            *err = "section out of range or misaligned";
            return false;
        }
    }
    // Payload spans must not overlap.  Zero-length sections are allowed; they
    // have no bytes to collide with and are used by the format's optional slots.
    for (uint32_t i = 0; i < SUMMER_PACK_SECTION_COUNT; ++i) {
        if (sec[i].len == 0) continue;
        for (uint32_t j = i + 1; j < SUMMER_PACK_SECTION_COUNT; ++j) {
            if (sec[j].len == 0) continue;
            if (sec[i].off < sec[j].off + sec[j].len &&
                sec[j].off < sec[i].off + sec[i].len) {
                *err = "section spans overlap";
                return false;
            }
        }
    }

    pack->blob = blob;
    pack->size = size;
    pack->text = section_ptr(blob, size, sec[SUMMER_SEC_TEXT].off, sec[SUMMER_SEC_TEXT].len);
    pack->text_codes = sec[SUMMER_SEC_TEXT].count;
    pack->chartab = section_ptr(blob, size, sec[SUMMER_SEC_CHARTAB].off,
                                sec[SUMMER_SEC_CHARTAB].len);
    pack->chartab_count = sec[SUMMER_SEC_CHARTAB].count;
    pack->ops = section_ptr(blob, size, sec[SUMMER_SEC_OPS].off, sec[SUMMER_SEC_OPS].len);
    pack->ops_size = sec[SUMMER_SEC_OPS].len;
    pack->opidx = (const uint32_t *)section_ptr(blob, size, sec[SUMMER_SEC_OPIDX].off,
                                                sec[SUMMER_SEC_OPIDX].len);
    pack->opidx_count = sec[SUMMER_SEC_OPIDX].count;
    pack->scenes = (const summer_scene_t *)section_ptr(blob, size, sec[SUMMER_SEC_SCENE].off,
                                                       sec[SUMMER_SEC_SCENE].len);
    pack->scene_count = sec[SUMMER_SEC_SCENE].count;
    pack->chapters = (const summer_chapter_t *)section_ptr(blob, size, sec[SUMMER_SEC_CHAPTER].off,
                                                           sec[SUMMER_SEC_CHAPTER].len);
    pack->chapter_count = sec[SUMMER_SEC_CHAPTER].count;
    pack->routes = (const summer_route_t *)section_ptr(blob, size, sec[SUMMER_SEC_ROUTE].off,
                                                       sec[SUMMER_SEC_ROUTE].len);
    pack->route_count = sec[SUMMER_SEC_ROUTE].count;
    pack->gallery = (const summer_gallery_t *)section_ptr(blob, size, sec[SUMMER_SEC_GALLERY].off,
                                                          sec[SUMMER_SEC_GALLERY].len);
    pack->gallery_count = sec[SUMMER_SEC_GALLERY].count;
    pack->gallery_items = (const uint16_t *)section_ptr(blob, size, sec[SUMMER_SEC_GALLERY_ITEM].off,
                                                        sec[SUMMER_SEC_GALLERY_ITEM].len);
    pack->gallery_item_count = sec[SUMMER_SEC_GALLERY_ITEM].count;
    pack->images = (const summer_image_t *)section_ptr(blob, size, sec[SUMMER_SEC_IMAGE].off,
                                                       sec[SUMMER_SEC_IMAGE].len);
    pack->image_count = sec[SUMMER_SEC_IMAGE].count;
    pack->imap = (const uint16_t *)section_ptr(blob, size, sec[SUMMER_SEC_IMAP].off,
                                               sec[SUMMER_SEC_IMAP].len);
    pack->imap_count = sec[SUMMER_SEC_IMAP].count;
    pack->expr = section_ptr(blob, size, sec[SUMMER_SEC_EXPR].off, sec[SUMMER_SEC_EXPR].len);
    pack->expr_size = sec[SUMMER_SEC_EXPR].len;
    pack->expridx = (const uint32_t *)section_ptr(blob, size, sec[SUMMER_SEC_EXPRIDX].off,
                                                  sec[SUMMER_SEC_EXPRIDX].len);
    pack->expr_count = sec[SUMMER_SEC_EXPRIDX].count;
    pack->vars = (const uint32_t *)section_ptr(blob, size, sec[SUMMER_SEC_VARS].off,
                                               sec[SUMMER_SEC_VARS].len);
    pack->var_count = sec[SUMMER_SEC_VARS].count;
    pack->choices = (const summer_choice_group_t *)section_ptr(blob, size, sec[SUMMER_SEC_CHOICE].off,
                                                               sec[SUMMER_SEC_CHOICE].len);
    pack->choice_count = sec[SUMMER_SEC_CHOICE].count;
    pack->choice_items = (const summer_choice_t *)section_ptr(blob, size, sec[SUMMER_SEC_CHOICE_ITEM].off,
                                                              sec[SUMMER_SEC_CHOICE_ITEM].len);
    pack->choice_item_count = sec[SUMMER_SEC_CHOICE_ITEM].count;
    pack->maps = (const summer_map_group_t *)section_ptr(blob, size, sec[SUMMER_SEC_MAP].off,
                                                         sec[SUMMER_SEC_MAP].len);
    pack->map_count = sec[SUMMER_SEC_MAP].count;
    pack->map_items = (const summer_map_item_t *)section_ptr(blob, size, sec[SUMMER_SEC_MAP_ITEM].off,
                                                             sec[SUMMER_SEC_MAP_ITEM].len);
    pack->map_item_count = sec[SUMMER_SEC_MAP_ITEM].count;
    pack->map_labels = (const summer_maplabel_t *)section_ptr(blob, size, sec[SUMMER_SEC_MAPLABEL].off,
                                                              sec[SUMMER_SEC_MAPLABEL].len);
    pack->map_label_count = sec[SUMMER_SEC_MAPLABEL].count;
    pack->flow_labels = (const summer_flowlabel_t *)section_ptr(blob, size, sec[SUMMER_SEC_FLOWLABEL].off,
                                                                sec[SUMMER_SEC_FLOWLABEL].len);
    pack->flow_label_count = sec[SUMMER_SEC_FLOWLABEL].count;
    pack->scene_index = (const uint16_t *)section_ptr(blob, size, sec[SUMMER_SEC_SCENE_INDEX].off,
                                                      sec[SUMMER_SEC_SCENE_INDEX].len);
    pack->scene_index_count = sec[SUMMER_SEC_SCENE_INDEX].count;
    pack->speakers = (const uint32_t *)section_ptr(blob, size, sec[SUMMER_SEC_SPEAKER].off,
                                                   sec[SUMMER_SEC_SPEAKER].len);
    pack->speaker_count = sec[SUMMER_SEC_SPEAKER].count;
    pack->imgdata = section_ptr(blob, size, sec[SUMMER_SEC_IMGDATA].off, sec[SUMMER_SEC_IMGDATA].len);
    pack->imgdata_size = sec[SUMMER_SEC_IMGDATA].len;

    if (!pack->text || !pack->chartab || !pack->ops || !pack->opidx || !pack->scenes ||
        !pack->chapters || !pack->routes || !pack->gallery || !pack->gallery_items ||
        !pack->images || !pack->imap || !pack->expr || !pack->expridx || !pack->vars ||
        !pack->choices || !pack->choice_items || !pack->maps || !pack->map_items ||
        !pack->map_labels || !pack->flow_labels || !pack->scene_index || !pack->speakers ||
        !pack->imgdata) {
        *err = "missing section";
        return false;
    }

    // ---- 计数与长度
    if (pack->text_codes == 0 || pack->text_codes == UINT32_MAX) {
        *err = "bad text section";
        return false;
    }
    {
        const uint32_t words = (pack->text_codes + 1u) / 2u;
        if (words > UINT32_MAX / 3u || words * 3u != sec[SUMMER_SEC_TEXT].len) {
            *err = "bad text section";
            return false;
        }
    }
    if (!check_count(pack->chartab_count, sec[SUMMER_SEC_CHARTAB].len, 2) ||
        pack->chartab_count == 0) {
        *err = "bad chartab";
        return false;
    }
    if (!check_count(pack->scene_count, sec[SUMMER_SEC_SCENE].len, sizeof(summer_scene_t)) ||
        pack->scene_count == 0 || !check_count(pack->chapter_count, sec[SUMMER_SEC_CHAPTER].len,
                                                sizeof(summer_chapter_t)) ||
        !check_count(pack->route_count, sec[SUMMER_SEC_ROUTE].len, sizeof(summer_route_t)) ||
        !check_count(pack->gallery_count, sec[SUMMER_SEC_GALLERY].len, sizeof(summer_gallery_t)) ||
        !check_count(pack->gallery_item_count, sec[SUMMER_SEC_GALLERY_ITEM].len, 2) ||
        !check_count(pack->image_count, sec[SUMMER_SEC_IMAGE].len, sizeof(summer_image_t)) ||
        pack->image_count == 0 || !check_count(pack->imap_count, sec[SUMMER_SEC_IMAP].len, 2) ||
        pack->imap_count < 16046 || !check_count(pack->opidx_count, sec[SUMMER_SEC_OPIDX].len, 4) ||
        !check_count(pack->var_count, sec[SUMMER_SEC_VARS].len, 4) ||
        !check_count(pack->choice_count, sec[SUMMER_SEC_CHOICE].len, sizeof(summer_choice_group_t)) ||
        !check_count(pack->choice_item_count, sec[SUMMER_SEC_CHOICE_ITEM].len,
                     sizeof(summer_choice_t)) ||
        !check_count(pack->map_count, sec[SUMMER_SEC_MAP].len, sizeof(summer_map_group_t)) ||
        !check_count(pack->map_item_count, sec[SUMMER_SEC_MAP_ITEM].len, sizeof(summer_map_item_t)) ||
        !check_count(pack->map_label_count, sec[SUMMER_SEC_MAPLABEL].len, sizeof(summer_maplabel_t)) ||
        !check_count(pack->flow_label_count, sec[SUMMER_SEC_FLOWLABEL].len,
                     sizeof(summer_flowlabel_t)) ||
        !check_count(pack->scene_index_count, sec[SUMMER_SEC_SCENE_INDEX].len, 2) ||
        pack->scene_index_count < 423 ||
        !check_count(pack->speaker_count, sec[SUMMER_SEC_SPEAKER].len, 4) ||
        sec[SUMMER_SEC_PROGRESS].len != 154 || sec[SUMMER_SEC_META].len != 76 ||
        sec[SUMMER_SEC_EXPRIDX].count < 2 ||
        !check_count(sec[SUMMER_SEC_EXPRIDX].count, sec[SUMMER_SEC_EXPRIDX].len, 4)) {
        *err = "bad section counts";
        return false;
    }
    for (uint32_t i = 0; i < pack->image_count; ++i) {
        const summer_image_t *img = &pack->images[i];
        if (img->kind > SUMMER_IMG_SOLID) {
            *err = "bad image kind";
            return false;
        }
        if (img->kind == SUMMER_IMG_SOLID) {
            if (img->width == 0 || img->height == 0) {
                *err = "bad solid image";
                return false;
            }
        } else {
            if (img->width == 0 || img->height == 0 || img->len < 4 ||
                !section_range(pack->imgdata, pack->imgdata_size, img->off, img->len)) {
                *err = "bad image data";
                return false;
            }
            if (img->kind == SUMMER_IMG_PORTRAIT &&
                (img->mask_off == 0 || img->mask_len == 0 ||
                 !section_range(pack->imgdata, pack->imgdata_size, img->mask_off, img->mask_len))) {
                *err = "bad portrait mask";
                return false;
            }
        }
    }
    for (uint32_t i = 0; i < pack->imap_count; ++i) {
        if (pack->imap[i] >= pack->image_count) {
            *err = "bad image map";
            return false;
        }
    }
    for (uint32_t i = 0; i < pack->scene_count; ++i) {
        const summer_scene_t *sc = &pack->scenes[i];
        if (sc->id == 0 || sc->id >= pack->scene_index_count ||
            pack->scene_index[sc->id] != i) {
            *err = "bad scene index";
            return false;
        }
        if (sc->first_block > pack->opidx_count ||
            sc->block_count > pack->opidx_count - sc->first_block ||
            sc->block_count != (sc->op_count + 31u) / 32u) {
            *err = "bad scene blocks";
            return false;
        }
        for (uint32_t b = 0; b < sc->block_count; ++b) {
            if (pack->opidx[sc->first_block + b] >= pack->ops_size) {
                *err = "bad op block";
                return false;
            }
        }
        if (sc->name >= pack->text_codes || sc->map_name >= pack->text_codes ||
            sc->date >= pack->text_codes || sc->date_label >= pack->text_codes ||
            sc->route_name >= pack->text_codes) {
            *err = "bad scene text";
            return false;
        }
        if (sc->phase > 2 || (sc->flags & ~3u) || sc->route < -1 || sc->route > 13) {
            *err = "bad scene fields";
            return false;
        }
        if (sc->next != 0 && !summer_scene_by_id(pack, sc->next)) {
            *err = "bad scene next";
            return false;
        }
        // The whole op stream must decode with the same block table the VM
        // will use.  This rejects a bad first op kind, a broken varint, or a
        // block offset that disagrees with sequential decoding before startup.
        if (sc->op_count > 0) {
            uint32_t off = pack->opidx[sc->first_block];
            for (uint32_t n = 0; n < sc->op_count; ++n) {
                if (n % 32u == 0 &&
                    pack->opidx[sc->first_block + n / 32u] != off) {
                    *err = "bad op block offset";
                    return false;
                }
                const uint32_t size = summer_op_size(pack, off);
                if (size == 0 || size > pack->ops_size - off) {
                    *err = "bad op stream";
                    return false;
                }
                off += size;
            }
            if (off > pack->ops_size) {
                *err = "bad op stream end";
                return false;
            }
        }
    }
    for (uint32_t i = 0; i < pack->chapter_count; ++i) {
        const summer_chapter_t *ch = &pack->chapters[i];
        if (!summer_scene_by_id(pack, ch->scene) || ch->name >= pack->text_codes) {
            *err = "bad chapter";
            return false;
        }
    }
    for (uint32_t i = 0; i < pack->gallery_count; ++i) {
        const summer_gallery_t *g = &pack->gallery[i];
        if (g->first > pack->gallery_item_count ||
            g->count > pack->gallery_item_count - g->first || g->label >= pack->text_codes) {
            *err = "bad gallery";
            return false;
        }
        for (uint32_t j = 0; j < g->count; ++j) {
            if (pack->gallery_items[g->first + j] >= pack->image_count) {
                *err = "bad gallery item";
                return false;
            }
        }
    }
    if (!check_count(pack->expr_count, sec[SUMMER_SEC_EXPRIDX].len, 4) ||
        pack->expr_count < 2 || pack->expridx[0] != 0 ||
        pack->expridx[pack->expr_count - 1] != pack->expr_size) {
        *err = "bad expression index";
        return false;
    }
    for (uint32_t i = 0; i + 1 < pack->expr_count; ++i) {
        if (pack->expridx[i] > pack->expridx[i + 1] || pack->expridx[i + 1] > pack->expr_size) {
            *err = "bad expression bounds";
            return false;
        }
    }
    for (uint32_t i = 0; i < pack->choice_count; ++i) {
        const summer_choice_group_t *g = &pack->choices[i];
        if (g->first > pack->choice_item_count ||
            g->count > pack->choice_item_count - g->first) {
            *err = "bad choice group";
            return false;
        }
        for (uint32_t j = 0; j < g->count; ++j) {
            const summer_choice_t *it = &pack->choice_items[g->first + j];
            if (it->text >= pack->text_codes ||
                (it->cond != 0 && it->cond >= pack->expr_count - 1u)) {
                *err = "bad choice item";
                return false;
            }
        }
    }
    for (uint32_t i = 0; i < pack->map_count; ++i) {
        const summer_map_group_t *g = &pack->maps[i];
        if (g->first > pack->map_item_count || g->count > pack->map_item_count - g->first) {
            *err = "bad map group";
            return false;
        }
    }
    for (uint32_t i = 0; i < pack->scene_index_count; ++i) {
        if (pack->scene_index[i] != 0xFFFFu && pack->scene_index[i] >= pack->scene_count) {
            *err = "bad scene index value";
            return false;
        }
    }
    for (uint32_t i = 0; i < pack->speaker_count; ++i) {
        if (pack->speakers[i] >= pack->text_codes) {
            *err = "bad speaker";
            return false;
        }
    }
    for (uint32_t i = 1; i < pack->map_label_count; ++i) {
        if (pack->map_labels[i - 1].key > pack->map_labels[i].key) {
            *err = "map labels not sorted";
            return false;
        }
    }
    for (uint32_t i = 1; i < pack->flow_label_count; ++i) {
        const summer_flowlabel_t *a = &pack->flow_labels[i - 1];
        const summer_flowlabel_t *b = &pack->flow_labels[i];
        if (a->key > b->key || (a->key == b->key && a->kind >= b->kind)) {
            *err = "flow labels not sorted";
            return false;
        }
    }

    // ---- META / PROGRESS 解析（避免未对齐指针）
    {
        const uint8_t *m = blob + sec[SUMMER_SEC_META].off;
        summer_meta_t *o = &pack->meta;
        o->special_var = rd32(m + 0);
        o->var_count = rd32(m + 4);
        o->image_count = rd32(m + 8);
        o->gallery_count = rd32(m + 12);
        o->flow_scene = rd16(m + 16);
        o->initial_scene = rd16(m + 18);
        o->map_scene = rd16(m + 20);
        o->scene_count = rd16(m + 22);
        o->chapter_count = rd16(m + 24);
        o->route_count = rd16(m + 26);
        o->choice_count = rd32(m + 28);
        o->choice_item_count = rd32(m + 32);
        o->map_count = rd32(m + 36);
        o->map_item_count = rd32(m + 40);
        o->block_count = rd32(m + 44);
        o->op_bytes = rd32(m + 48);
        o->title = rd32(m + 52);
        o->subtitle = rd32(m + 56);
        o->version = rd32(m + 60);
        o->tagline = rd32(m + 64);
        o->logo_image = rd16(m + 68);
        o->title_bg_image = rd16(m + 70);
        o->_pad3 = rd16(m + 72);
        o->_pad4 = rd16(m + 74);
        if (o->scene_count != pack->scene_count || o->chapter_count != pack->chapter_count ||
            o->route_count != pack->route_count || o->gallery_count != pack->gallery_count ||
            o->choice_count != pack->choice_count ||
            o->choice_item_count != pack->choice_item_count ||
            o->map_count != pack->map_count || o->map_item_count != pack->map_item_count ||
            o->image_count != pack->image_count || o->block_count != pack->opidx_count ||
            o->op_bytes != pack->ops_size || o->var_count != pack->var_count ||
            o->title >= pack->text_codes || o->subtitle >= pack->text_codes ||
            o->version >= pack->text_codes || o->tagline >= pack->text_codes ||
            (o->logo_image != SUMMER_IMAGE_NONE && o->logo_image >= pack->image_count) ||
            (o->title_bg_image != SUMMER_IMAGE_NONE && o->title_bg_image >= pack->image_count)) {
            *err = "bad meta";
            return false;
        }
        const summer_scene_t *flow = summer_scene_by_id(pack, o->flow_scene);
        if (!flow || !summer_scene_by_id(pack, o->initial_scene)) {
            *err = "bad entry scene";
            return false;
        }
    }
    {
        const uint8_t *p = blob + sec[SUMMER_SEC_PROGRESS].off;
        summer_progress_meta_t *o = &pack->progress;
        o->max_ending = rd16(p + 0);
        o->heroine_count = p[2];
        o->flow_ending_count = p[3];
        o->globals_count = p[4];
        o->_pad = p[5];
        p += 6;
        if (o->max_ending != 14 || o->heroine_count > 14 || o->flow_ending_count > 14 ||
            o->globals_count > 6) {
            *err = "bad progress header";
            return false;
        }
        for (int i = 0; i < 14; ++i) {
            o->clear_var[i + 1] = rd32(p);
            o->heroine[i + 1] = p[4];
            p += 5;
        }
        for (int i = 0; i < o->flow_ending_count; ++i) {
            o->flow_cursor[i] = rd16(p);
            o->flow_ending[i] = p[2];
            p += 3;
        }
        for (int i = 0; i < o->globals_count; ++i) {
            o->globals[i] = rd32(p);
            p += 4;
        }
        o->restart_var = rd32(p);
        o->special_var = rd32(p + 4);
        o->clear_count_var = rd32(p + 8);
        if (o->special_var != 24006 || o->clear_count_var != 24006) {
            *err = "bad progress vars";
            return false;
        }
        for (int i = 0; i < o->flow_ending_count; ++i) {
            if (o->flow_ending[i] < 1 || o->flow_ending[i] > 14) {
                *err = "bad flow ending";
                return false;
            }
        }
    }
    return true;
}

const summer_scene_t *summer_scene_by_id(const summer_pack_t *pack, uint16_t id)
{
    if (!pack || !pack->scene_index || id >= pack->scene_index_count) return NULL;
    const uint16_t index = pack->scene_index[id];
    if (index == 0xFFFFu || index >= pack->scene_count) return NULL;
    return &pack->scenes[index];
}

const summer_scene_t *summer_scene_at(const summer_pack_t *pack, uint16_t index)
{
    if (!pack || index >= pack->scene_count) return NULL;
    return &pack->scenes[index];
}

const summer_image_t *summer_image(const summer_pack_t *pack, uint16_t source_id)
{
    if (!pack || source_id >= pack->imap_count) return NULL;
    const uint16_t index = pack->imap[source_id];
    if (index >= pack->image_count) return NULL;
    return &pack->images[index];
}

const summer_image_t *summer_image_at(const summer_pack_t *pack, uint32_t index)
{
    if (!pack || index >= pack->image_count) return NULL;
    return &pack->images[index];
}

static bool text_code(const summer_pack_t *pack, uint32_t index, uint32_t *out)
{
    if (index >= pack->text_codes) return false;
    const uint8_t *blob = pack->text + (index / 2u) * 3u;
    uint32_t value = (uint32_t)blob[0] | ((uint32_t)blob[1] << 8) | ((uint32_t)blob[2] << 16);
    *out = (index & 1u) ? ((value >> 12) & 0xFFFu) : (value & 0xFFFu);
    return true;
}

static int utf8_put(uint32_t cp, char *dst, size_t cap, size_t *pos)
{
    if (cp == 0 || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) cp = '?';
    unsigned char buf[4];
    size_t n;
    if (cp < 0x80) {
        buf[0] = (unsigned char)cp;
        n = 1;
    } else if (cp < 0x800) {
        buf[0] = (unsigned char)(0xC0u | (cp >> 6));
        buf[1] = (unsigned char)(0x80u | (cp & 0x3Fu));
        n = 2;
    } else if (cp < 0x10000u) {
        buf[0] = (unsigned char)(0xE0u | (cp >> 12));
        buf[1] = (unsigned char)(0x80u | ((cp >> 6) & 0x3Fu));
        buf[2] = (unsigned char)(0x80u | (cp & 0x3Fu));
        n = 3;
    } else {
        buf[0] = (unsigned char)(0xF0u | (cp >> 18));
        buf[1] = (unsigned char)(0x80u | ((cp >> 12) & 0x3Fu));
        buf[2] = (unsigned char)(0x80u | ((cp >> 6) & 0x3Fu));
        buf[3] = (unsigned char)(0x80u | (cp & 0x3Fu));
        n = 4;
    }
    if (*pos + n + 1 > cap) return 0;
    memcpy(dst + *pos, buf, n);
    *pos += n;
    dst[*pos] = 0;
    return 1;
}

bool summer_pack_text_into(summer_pack_t *pack, uint32_t off, char *dst, size_t cap)
{
    if (!pack || !dst || cap == 0) return false;
    dst[0] = 0;
    if (off >= pack->text_codes) return false;
    size_t pos = 0;
    for (uint32_t index = off; index < pack->text_codes; ++index) {
        uint32_t code = 0;
        if (!text_code(pack, index, &code)) return false;
        if (code == 0) return true;
        if (code > pack->chartab_count) return false;
        const uint8_t *cp = pack->chartab + (code - 1u) * 2u;
        const uint32_t point = rd16(cp);
        if (!utf8_put(point, dst, cap, &pos)) return false;
    }
    return false;
}

const char *summer_pack_text(summer_pack_t *pack, uint32_t off)
{
    if (!pack) return "";
    if (!summer_pack_text_into(pack, off, pack->text_buf, sizeof(pack->text_buf))) {
        pack->text_buf[0] = 0;
    }
    return pack->text_buf;
}

uint32_t summer_speaker_text(const summer_pack_t *pack, uint32_t speaker)
{
    if (!pack || speaker >= pack->speaker_count) return 0;
    return pack->speakers[speaker];
}

static uint32_t op_parse(const summer_pack_t *pack, uint32_t offset, summer_op_t *op)
{
    if (offset >= pack->ops_size) return 0;
    const uint8_t *p = pack->ops + offset;
    const uint8_t *end = pack->ops + pack->ops_size;
    const uint8_t kind = *p++;
    uint32_t a = 0, b = 0;
    int32_t value = 0;
    uint8_t tag = 0, truth = 0;
    switch (kind) {
    case SUMMER_OP_TEXT:
        if (!read_uvar(&p, end, &a) || !read_uvar(&p, end, &b)) return 0;
        break;
    case SUMMER_OP_IMAGE:
        if (!read_uvar(&p, end, &a) || !read_uvar(&p, end, &b)) return 0;
        break;
    case SUMMER_OP_SET:
    case SUMMER_OP_ADD:
    case SUMMER_OP_SUB:
        if (!read_uvar(&p, end, &a) || p >= end) return 0;
        tag = *p++;
        if (tag == 0) {
            if (!read_svar(&p, end, &value)) return 0;
        } else {
            if (!read_uvar(&p, end, &b)) return 0;
        }
        break;
    case SUMMER_OP_IF:
        if (!read_uvar(&p, end, &a) || !read_uvar(&p, end, &b) || p >= end) return 0;
        truth = *p++ ? 1 : 0;
        break;
    case SUMMER_OP_GOTO:
    case SUMMER_OP_CALL:
        if (!read_uvar(&p, end, &a)) return 0;
        break;
    case SUMMER_OP_CALLSCENE:
    case SUMMER_OP_JUMPSCENE:
        if (!read_uvar(&p, end, &a) || !read_uvar(&p, end, &b)) return 0;
        break;
    case SUMMER_OP_RETURN:
    case SUMMER_OP_END:
    case SUMMER_OP_NOP:
        break;
    case SUMMER_OP_CHOICE:
    case SUMMER_OP_MAP:
        if (!read_uvar(&p, end, &a)) return 0;
        break;
    case SUMMER_OP_CLEAR:
        if (!read_uvar(&p, end, &a)) return 0;
        break;
    default:
        return 0;
    }
    if (op) {
        op->kind = kind;
        op->tag = tag;
        op->truth = truth;
        op->_pad = 0;
        op->a = a;
        op->b = b;
        op->value = value;
    }
    return (uint32_t)(p - pack->ops);
}

uint32_t summer_op_size(const summer_pack_t *pack, uint32_t offset)
{
    const uint32_t next = op_parse(pack, offset, NULL);
    return next > offset ? next - offset : 0;
}

uint32_t summer_op_decode(const summer_pack_t *pack, uint32_t offset, summer_op_t *op)
{
    return op_parse(pack, offset, op);
}

bool summer_op_seek(const summer_pack_t *pack, const summer_scene_t *scene,
                    uint32_t index, uint32_t *offset)
{
    if (!pack || !scene || !offset || index >= scene->op_count) return false;
    const uint32_t block = index / 32u;
    if (block >= scene->block_count) return false;
    uint32_t off = pack->opidx[scene->first_block + block];
    if (off >= pack->ops_size) return false;
    for (uint32_t i = 0; i < index % 32u; ++i) {
        const uint32_t size = summer_op_size(pack, off);
        if (size == 0 || size > pack->ops_size - off) return false;
        off += size;
    }
    *offset = off;
    return true;
}

const summer_choice_group_t *summer_choice_group(const summer_pack_t *pack, uint32_t group)
{
    if (!pack || group >= pack->choice_count) return NULL;
    return &pack->choices[group];
}

const summer_choice_t *summer_choice_item(const summer_pack_t *pack, uint32_t group, int index)
{
    const summer_choice_group_t *g = summer_choice_group(pack, group);
    if (!g || index < 0 || index >= g->count) return NULL;
    return &pack->choice_items[g->first + (uint32_t)index];
}

const summer_map_group_t *summer_map_group(const summer_pack_t *pack, uint32_t group)
{
    if (!pack || group >= pack->map_count) return NULL;
    return &pack->maps[group];
}

const char *summer_map_label(const summer_pack_t *pack, uint16_t scene, uint32_t cursor)
{
    if (!pack || pack->map_label_count == 0) return NULL;
    const uint32_t key = ((uint32_t)scene << 16) | (cursor & 0xFFFFu);
    uint32_t lo = 0, hi = pack->map_label_count;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        if (pack->map_labels[mid].key < key) lo = mid + 1;
        else hi = mid;
    }
    if (lo < pack->map_label_count && pack->map_labels[lo].key == key) {
        return summer_pack_text((summer_pack_t *)pack, pack->map_labels[lo].text);
    }
    return NULL;
}

const char *summer_flow_text(const summer_pack_t *pack, uint32_t cursor, int kind)
{
    if (!pack) return NULL;
    uint32_t lo = 0, hi = pack->flow_label_count;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        const summer_flowlabel_t *e = &pack->flow_labels[mid];
        if (e->key < cursor || (e->key == cursor && e->kind < kind)) lo = mid + 1;
        else hi = mid;
    }
    if (lo < pack->flow_label_count) {
        const summer_flowlabel_t *e = &pack->flow_labels[lo];
        if (e->key == cursor && e->kind == kind) {
            return summer_pack_text((summer_pack_t *)pack, e->text);
        }
    }
    return NULL;
}

const summer_gallery_t *summer_gallery_group(const summer_pack_t *pack, uint32_t group)
{
    if (!pack || group >= pack->gallery_count) return NULL;
    return &pack->gallery[group];
}

uint16_t summer_gallery_image(const summer_pack_t *pack, uint32_t group, int variant)
{
    const summer_gallery_t *g = summer_gallery_group(pack, group);
    if (!g || variant < 0 || variant >= g->count) return SUMMER_IMAGE_NONE;
    const uint16_t index = pack->gallery_items[g->first + (uint32_t)variant];
    if (index >= pack->image_count) return SUMMER_IMAGE_NONE;
    // gallery_items store image indices; convert back to the source id by
    // scanning the image map is not possible.  The UI needs the source id for
    // the image layer, so store the mapping via image index -> first source id
    // at open time (below).
    return index;
}

int summer_gallery_group_for_image(const summer_pack_t *pack, uint16_t source_id)
{
    if (!pack) return -1;
    const uint16_t image_index = (source_id < pack->imap_count) ? pack->imap[source_id] : 0xFFFFu;
    if (image_index >= pack->image_count) return -1;
    for (uint32_t g = 0; g < pack->gallery_count; ++g) {
        const summer_gallery_t *grp = &pack->gallery[g];
        for (uint32_t j = 0; j < grp->count; ++j) {
            if (pack->gallery_items[grp->first + j] == image_index) return (int)g;
        }
    }
    return -1;
}

int summer_ending_for_cursor(const summer_pack_t *pack, uint16_t scene, uint32_t cursor)
{
    if (!pack || scene != pack->meta.flow_scene) return 0;
    for (int i = 0; i < pack->progress.flow_ending_count; ++i) {
        if (pack->progress.flow_cursor[i] == cursor) return pack->progress.flow_ending[i];
    }
    return 0;
}

uint32_t summer_clear_var(const summer_pack_t *pack, int ending)
{
    if (!pack || ending < 1 || ending > 14) return 0;
    return pack->progress.clear_var[ending];
}

bool summer_ending_is_heroine(const summer_pack_t *pack, int ending)
{
    if (!pack || ending < 1 || ending > 14) return false;
    return pack->progress.heroine[ending] != 0;
}
