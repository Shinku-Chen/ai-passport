// main/steins0_pack.c —— SG0PK01 解析与逐块 zlib 解压(纯 C,无 IDF 依赖)。
#include "steins0_pack.h"

#include <string.h>

// ---------------------------------------------------------------- little endian
static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static int16_t rd16s(const uint8_t *p)
{
    return (int16_t)rd16(p);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}


static bool pack_section_table_ok(const sg0_pack_t *pack, uint32_t table_end)
{
    const uint32_t widths[SG0_SEC_COUNT] = {
        0, 8, 24, 12, 12, 4, 12, 18, 4, 4, 2, 2, 4, 16, 12, 8, 30, 0, 8, 4,
        0, 2, 4, 2, 2, 16, 8, 8, 2, 8, 2, 2,
    };
    for (uint32_t i = 0; i < SG0_SEC_COUNT; ++i) {
        const sg0_section_t *s = &pack->sec[i];
        if (s->size == 0) continue;
        if (s->offset < table_end) return false;
        const uint32_t width = widths[i];
        if (width == 0) {
            if (i == SG0_SEC_TEXT) continue;
            if (s->count != s->size) return false;
            continue;
        }
        const uint64_t bytes = (uint64_t)s->count * width;
        if (bytes != s->size) return false;
    }
    if (pack->sec[SG0_SEC_TEXT].count != pack->sec[SG0_SEC_STRT].count) return false;
    // 非空段必须严格不重叠(允许段间对齐空洞)。
    const uint32_t n = SG0_SEC_COUNT;
    bool used[SG0_SEC_COUNT] = {false};
    uint32_t prev_end = 0;
    bool first = true;
    for (uint32_t k = 0; k < n; ++k) {
        int pick = -1;
        for (uint32_t i = 0; i < n; ++i) {
            if (used[i] || pack->sec[i].size == 0) continue;
            if (pick < 0 || pack->sec[i].offset < pack->sec[pick].offset) pick = (int)i;
        }
        if (pick < 0) break;
        used[pick] = true;
        const sg0_section_t *s = &pack->sec[pick];
        const uint64_t end = (uint64_t)s->offset + s->size;
        if (end > pack->size) return false;
        if (!first && s->offset < prev_end) return false;
        prev_end = (uint32_t)end;
        first = false;
    }
    return true;
}

static bool pack_graph_refs_ok(const sg0_pack_t *pack)
{
    for (uint32_t i = 0; i < pack->sec[SG0_SEC_NODE].count; ++i) {
        sg0_node_t node;
        if (!sg0_pack_node(pack, (uint8_t)i, &node)) return false;
    }
    for (uint32_t i = 0; i < pack->sec[SG0_SEC_VIEW].count; ++i) {
        sg0_view_t view;
        if (!sg0_pack_view(pack, (uint16_t)i, &view)) return false;
    }
    for (uint32_t i = 0; i < pack->sec[SG0_SEC_LAYERREF].count; ++i) {
        uint16_t layer = rd16(pack->data + pack->sec[SG0_SEC_LAYERREF].offset + i * 2u);
        if (layer >= pack->sec[SG0_SEC_LAYER].count) return false;
    }
    for (uint32_t i = 0; i < pack->sec[SG0_SEC_LAYER].count; ++i) {
        const uint8_t *p = pack->data + pack->sec[SG0_SEC_LAYER].offset + i * 12u;
        if (rd16(p) >= pack->sec[SG0_SEC_ASSETID].count) return false;
    }
    for (uint32_t i = 0; i < pack->sec[SG0_SEC_ASSETID].count; ++i) {
        sg0_assetid_t asset;
        if (!sg0_pack_asset(pack, (uint16_t)i, &asset)) return false;
        for (uint8_t k = 0; k < asset.part_count; ++k) {
            sg0_part_t part;
            if (!sg0_pack_part(pack, (uint16_t)(asset.part_first + k), &part)) return false;
        }
    }
    for (uint32_t i = 0; i < pack->sec[SG0_SEC_CHUNK].count; ++i) {
        const uint8_t *p = pack->data + pack->sec[SG0_SEC_CHUNK].offset + i * 12u;
        const uint32_t zoff = rd32(p);
        const uint32_t zlen = rd32(p + 4);
        if (zoff > pack->sec[SG0_SEC_BLOB].size ||
            zlen > pack->sec[SG0_SEC_BLOB].size - zoff) {
            return false;
        }
    }
    for (uint32_t i = 0; i < pack->sec[SG0_SEC_SPK].count; ++i) {
        const uint16_t str = rd16(pack->data + pack->sec[SG0_SEC_SPK].offset + i * 2u);
        if (str >= pack->sec[SG0_SEC_STRT].count) return false;
    }
    for (uint32_t i = 0; i < pack->sec[SG0_SEC_TITLE].count; ++i) {
        const uint16_t str = rd16(pack->data + pack->sec[SG0_SEC_TITLE].offset + i * 2u);
        if (str >= pack->sec[SG0_SEC_STRT].count) return false;
    }
    for (uint32_t i = 0; i < pack->sec[SG0_SEC_GG].count; ++i) {
        sg0_gallery_group_t group;
        if (!sg0_pack_gallery_group(pack, (uint8_t)i, &group)) return false;
    }
    for (uint32_t i = 0; i < pack->sec[SG0_SEC_GE].count; ++i) {
        uint16_t asset = 0;
        if (!sg0_pack_gallery_entry(pack, (uint16_t)i, &asset)) return false;
    }
    for (uint32_t i = 0; i < pack->sec[SG0_SEC_START].count; ++i) {
        sg0_start_t start;
        if (!sg0_pack_start(pack, (uint16_t)i, &start)) return false;
    }
    for (uint32_t i = 0; i < pack->sec[SG0_SEC_CHOICE].count; ++i) {
        sg0_choice_t choice;
        if (!sg0_pack_choice(pack, (uint16_t)i, &choice)) return false;
    }
    for (uint32_t i = 0; i < pack->sec[SG0_SEC_COND].count; ++i) {
        sg0_cond_t cond;
        if (!sg0_pack_cond(pack, (uint16_t)i, &cond)) return false;
    }
    for (uint32_t i = 0; i < pack->sec[SG0_SEC_ENDING].count; ++i) {
        sg0_ending_t ending;
        if (!sg0_pack_ending(pack, (uint16_t)i, &ending)) return false;
    }
    return true;
}

static bool pack_rine_refs_ok(const sg0_pack_t *pack)
{
    const uint32_t strt = pack->sec[SG0_SEC_STRT].count;
    const uint32_t date = pack->sec[SG0_SEC_DATE].count;
    const uint32_t asset = pack->sec[SG0_SEC_ASSETID].count;
    const uint32_t contacts = pack->sec[SG0_SEC_RCONTACT].count;
    (void)contacts;
    const uint32_t msgs = pack->sec[SG0_SEC_RMSG].count;
    const uint32_t replies = pack->sec[SG0_SEC_RREPLY].count;
    const uint32_t choices = pack->sec[SG0_SEC_RCHOICE].count;
    const uint32_t seq = pack->sec[SG0_SEC_RSEQ].count;
    for (uint32_t i = 0; i < msgs; ++i) {
        sg0_rine_msg_t msg;
        if (!sg0_pack_rine_msg(pack, (uint16_t)i, &msg)) return false;
        if (msg.text != 0xFFFFu && msg.text >= strt) return false;
        if (msg.stamp != 0xFFFFu && msg.stamp >= asset) return false;
        if (msg.date >= date) return false;
    }
    for (uint32_t i = 0; i < replies; ++i) {
        uint16_t base = 0, first = 0, key = 0;
        uint8_t contact = 0, count = 0;
        if (!sg0_pack_rine_reply(pack, (uint16_t)i, &base, &contact, &count, &first, &key)) {
            return false;
        }
        (void)contact;
        if (key >= strt || (uint32_t)first + count > choices) return false;
    }
    for (uint32_t i = 0; i < choices; ++i) {
        uint16_t selected = 0, first = 0;
        uint8_t count = 0, stamp = 0;
        if (!sg0_pack_rine_choice(pack, (uint16_t)i, &selected, &first, &count, &stamp)) {
            return false;
        }
        if (selected >= msgs || (uint32_t)first + count > seq) return false;
    }
    for (uint32_t i = 0; i < seq; ++i) {
        uint16_t ref = 0;
        if (!sg0_pack_rine_seq(pack, (uint16_t)i, &ref) || ref >= msgs) return false;
    }
    for (uint32_t i = 0; i < contacts; ++i) {
        const char *name = NULL, *number = NULL;
        uint16_t name_len = 0, number_len = 0;
        if (!sg0_pack_rine_contact(pack, (uint8_t)i, NULL, &name, &name_len,
                                   &number, &number_len)) {
            return false;
        }
    }
    for (uint32_t i = 0; i < pack->sec[SG0_SEC_CARD].count; ++i) {
        uint16_t card = 0;
        if (!sg0_pack_card(pack, (uint8_t)i, &card) || card >= asset) return false;
    }
    for (uint32_t i = 0; i < pack->sec[SG0_SEC_EVENT].count; ++i) {
        sg0_event_t ev;
        if (!sg0_pack_event(pack, (uint16_t)i, &ev) || ev.type > SG0_EVENT_EXPIRE_RINE) {
            return false;
        }
        if (ev.type == SG0_EVENT_MESSAGE && ev.data >= msgs) return false;
        if (ev.type == SG0_EVENT_RINE_REPLY && ev.data >= replies) return false;
        if (ev.type == SG0_EVENT_EXPIRE_RINE && ev.b) {
            if (ev.data >= msgs || (uint32_t)ev.b > msgs - ev.data) return false;
        }
    }
    return true;
}

static bool range_ok(uint32_t first, uint32_t count, uint32_t total)
{
    if (count == 0) return first <= total;
    return first < total && count <= total - first;
}

static bool section_ok(const sg0_pack_t *pack, uint32_t type, uint32_t need_size,
                       uint32_t index)
{
    if (type >= SG0_SEC_COUNT) return false;
    const sg0_section_t *s = &pack->sec[type];
    if (s->offset > pack->size || s->size > pack->size - s->offset) return false;
    if (need_size) {
        if (index >= s->count || need_size > s->size) return false;
        if ((uint64_t)index * need_size > (uint64_t)s->size - need_size) return false;
    }
    return true;
}

bool sg0_pack_open(sg0_pack_t *pack, const uint8_t *data, uint32_t size)
{
    if (!pack || !data || size < 20u) return false;
    memset(pack, 0, sizeof(*pack));
    if (memcmp(data, SG0_PACK_MAGIC, 8) != 0) return false;
    const uint16_t version = rd16(data + 8);
    const uint16_t header_size = rd16(data + 10);
    const uint16_t section_count = rd16(data + 12);
    const uint32_t total = rd32(data + 16);
    if (version != SG0_PACK_VERSION || header_size != 20 || section_count != SG0_SEC_COUNT) {
        return false;
    }
    const uint32_t table_end = 20u + (uint32_t)section_count * 16u;
    if (total != size || table_end > size) return false;
    pack->data = data;
    pack->size = size;
    for (uint32_t i = 0; i < SG0_SEC_COUNT; ++i) {
        const uint8_t *p = data + 20u + i * 16u;
        const uint32_t type = rd32(p);
        if (type != i) return false;
        pack->sec[i].offset = rd32(p + 4);
        pack->sec[i].count = rd32(p + 8);
        pack->sec[i].size = rd32(p + 12);
        if (!section_ok(pack, type, 0, 0)) return false;
    }
    if (!pack_section_table_ok(pack, table_end)) return false;
    if (!pack_graph_refs_ok(pack)) return false;
    pack->text = data + pack->sec[SG0_SEC_TEXT].offset;
    pack->text_size = pack->sec[SG0_SEC_TEXT].size;
    pack->meta = data + pack->sec[SG0_SEC_META].offset;
    pack->meta_size = pack->sec[SG0_SEC_META].size;
    if (!pack_rine_refs_ok(pack)) return false;
    return true;
}

bool sg0_pack_string(const sg0_pack_t *pack, uint16_t index, const char **out, uint16_t *len)
{
    if (!pack || !out || !len) return false;
    if (!section_ok(pack, SG0_SEC_STRT, 8, index)) return false;
    const uint8_t *p = pack->data + pack->sec[SG0_SEC_STRT].offset + (uint32_t)index * 8u;
    const uint32_t off = rd32(p);
    const uint16_t n = rd16(p + 4);
    if (off > pack->text_size || n > pack->text_size - off) return false;
    *out = (const char *)pack->text + off;
    *len = n;
    return true;
}

bool sg0_pack_speaker(const sg0_pack_t *pack, uint8_t index, const char **out, uint16_t *len)
{
    if (!pack || !section_ok(pack, SG0_SEC_SPK, 2, index)) return false;
    const uint16_t str = rd16(pack->data + pack->sec[SG0_SEC_SPK].offset + (uint32_t)index * 2u);
    return sg0_pack_string(pack, str, out, len);
}

uint32_t sg0_pack_date(const sg0_pack_t *pack, uint16_t index)
{
    if (!pack || !section_ok(pack, SG0_SEC_DATE, 4, index)) return 0;
    return rd32(pack->data + pack->sec[SG0_SEC_DATE].offset + (uint32_t)index * 4u);
}

bool sg0_pack_node(const sg0_pack_t *pack, uint8_t index, sg0_node_t *out)
{
    if (!pack || !out || !section_ok(pack, SG0_SEC_NODE, 24, index)) return false;
    const uint8_t *p = pack->data + pack->sec[SG0_SEC_NODE].offset + (uint32_t)index * 24u;
    out->chunk_first = rd16(p);
    out->chunk_count = rd16(p + 2);
    out->choice = rd16(p + 4);
    out->condition = rd16(p + 6);
    out->ending = (int8_t)p[8];
    out->flags = p[9];
    out->break_first = rd16(p + 10);
    out->break_count = p[12];
    out->chapter_first = rd16(p + 13);
    out->chapter_count = p[15];
    out->event_first = rd16(p + 16);
    out->event_count = rd16(p + 18);
    out->start_first = rd16(p + 20);
    out->start_count = p[22];
    if (!range_ok(out->chunk_first, out->chunk_count, pack->sec[SG0_SEC_CHUNK].count) ||
        !range_ok(out->break_first, out->break_count, pack->sec[SG0_SEC_BREAK].count) ||
        !range_ok(out->chapter_first, out->chapter_count, pack->sec[SG0_SEC_CHGRP].count) ||
        !range_ok(out->event_first, out->event_count, pack->sec[SG0_SEC_EVENT].count) ||
        !range_ok(out->start_first, out->start_count, pack->sec[SG0_SEC_START].count)) {
        return false;
    }
    if (out->choice != 0xFFFFu && out->choice >= pack->sec[SG0_SEC_CHOICE].count) return false;
    if (out->condition != 0xFFFFu && out->condition >= pack->sec[SG0_SEC_COND].count) return false;
    if (out->ending >= 0 && (uint32_t)out->ending >= pack->sec[SG0_SEC_ENDING].count) return false;
    return true;
}

bool sg0_pack_chunk_at(const sg0_pack_t *pack, uint16_t chunk_index, sg0_chunk_t *out)
{
    if (!pack || !out || !section_ok(pack, SG0_SEC_CHUNK, 12, chunk_index)) return false;
    const uint8_t *p = pack->data + pack->sec[SG0_SEC_CHUNK].offset +
                       (uint32_t)chunk_index * 12u;
    out->zoff = rd32(p);
    out->zlen = rd32(p + 4);
    out->raw_len = rd16(p + 8);
    out->frames = p[10];
    return true;
}

bool sg0_pack_frame(const uint8_t *chunk_raw, uint32_t raw_len, uint8_t index, sg0_frame_t *out)
{
    if (!chunk_raw || !out || (uint32_t)(index + 1u) * 12u > raw_len) return false;
    const uint8_t *p = chunk_raw + (uint32_t)index * 12u;
    out->text_off = rd16(p);
    out->text_len = rd16(p + 2);
    out->view = rd16(p + 4);
    out->speaker = p[6];
    out->group = p[7];
    out->date = rd16(p + 8);
    out->flags = p[10];
    return true;
}

const char *sg0_pack_frame_text(const uint8_t *chunk_raw, uint32_t raw_len, uint8_t frames,
                                const sg0_frame_t *frame, uint16_t *len)
{
    if (!chunk_raw || !frame || !len) return NULL;
    const uint32_t base = (uint32_t)frames * 12u;
    if (base > raw_len || frame->text_off > raw_len - base ||
        frame->text_len > raw_len - base - frame->text_off) {
        return NULL;
    }
    *len = frame->text_len;
    return (const char *)chunk_raw + base + frame->text_off;
}

bool sg0_pack_view(const sg0_pack_t *pack, uint16_t index, sg0_view_t *out)
{
    if (!pack || !out || !section_ok(pack, SG0_SEC_VIEW, 4, index)) return false;
    const uint8_t *p = pack->data + pack->sec[SG0_SEC_VIEW].offset + (uint32_t)index * 4u;
    out->layer_first = rd16(p);
    out->layer_count = p[2];
    return range_ok(out->layer_first, out->layer_count, pack->sec[SG0_SEC_LAYERREF].count);
}

bool sg0_pack_layer(const sg0_pack_t *pack, uint16_t ref_index, sg0_layer_t *out)
{
    if (!pack || !out || !section_ok(pack, SG0_SEC_LAYERREF, 2, ref_index)) return false;
    const uint16_t layer = rd16(pack->data + pack->sec[SG0_SEC_LAYERREF].offset +
                                (uint32_t)ref_index * 2u);
    if (!section_ok(pack, SG0_SEC_LAYER, 12, layer)) return false;
    const uint8_t *p = pack->data + pack->sec[SG0_SEC_LAYER].offset + (uint32_t)layer * 12u;
    out->asset = rd16(p);
    out->x = rd16s(p + 2);
    out->y = rd16s(p + 4);
    out->w = rd16(p + 6);
    out->h = rd16(p + 8);
    out->opacity = rd16(p + 10);
    return out->asset < pack->sec[SG0_SEC_ASSETID].count;
}

bool sg0_pack_choice(const sg0_pack_t *pack, uint16_t index, sg0_choice_t *out)
{
    if (!pack || !out || !section_ok(pack, SG0_SEC_CHOICE, 18, index)) return false;
    const uint8_t *p = pack->data + pack->sec[SG0_SEC_CHOICE].offset + (uint32_t)index * 18u;
    out->kind = p[0];
    out->critical = p[1];
    out->heading = rd16(p + 2);
    out->category = rd16(p + 4);
    if (p[6] > 2u) return false;
    out->count = p[6];
    out->to[0] = p[8];
    out->to[1] = p[9];
    out->action[0] = p[10];
    out->action[1] = p[11];
    out->text[0] = rd16(p + 12);
    out->text[1] = rd16(p + 14);
    if (out->heading >= pack->sec[SG0_SEC_STRT].count ||
        out->category >= pack->sec[SG0_SEC_STRT].count) return false;
    for (uint8_t i = 0; i < out->count; ++i) {
        if (out->to[i] >= pack->sec[SG0_SEC_NODE].count ||
            out->text[i] >= pack->sec[SG0_SEC_STRT].count) return false;
    }
    return true;
}

bool sg0_pack_cond(const sg0_pack_t *pack, uint16_t index, sg0_cond_t *out)
{
    if (!pack || !out || !section_ok(pack, SG0_SEC_COND, 4, index)) return false;
    const uint8_t *p = pack->data + pack->sec[SG0_SEC_COND].offset + (uint32_t)index * 4u;
    out->flag = p[0];
    out->yes = p[1];
    out->no = p[2];
    return out->yes < pack->sec[SG0_SEC_NODE].count &&
           out->no < pack->sec[SG0_SEC_NODE].count;
}

bool sg0_pack_ending(const sg0_pack_t *pack, uint16_t index, sg0_ending_t *out)
{
    if (!pack || !out || !section_ok(pack, SG0_SEC_ENDING, 4, index)) return false;
    const uint8_t *p = pack->data + pack->sec[SG0_SEC_ENDING].offset + (uint32_t)index * 4u;
    out->name = rd16(p);
    out->ending_id = p[2];
    return out->ending_id < 6u && out->name < pack->sec[SG0_SEC_STRT].count;
}

bool sg0_pack_endlist(const sg0_pack_t *pack, uint8_t index, uint16_t *string_index)
{
    if (!pack || !string_index || !section_ok(pack, SG0_SEC_ENDLIST, 2, index)) return false;
    *string_index = rd16(pack->data + pack->sec[SG0_SEC_ENDLIST].offset + (uint32_t)index * 2u);
    return true;
}

bool sg0_pack_break(const sg0_pack_t *pack, uint16_t index, uint16_t *cursor)
{
    if (!pack || !cursor || !section_ok(pack, SG0_SEC_BREAK, 2, index)) return false;
    *cursor = rd16(pack->data + pack->sec[SG0_SEC_BREAK].offset + (uint32_t)index * 2u);
    return true;
}

bool sg0_pack_chgrp(const sg0_pack_t *pack, uint16_t index, sg0_chgrp_t *out)
{
    if (!pack || !out || !section_ok(pack, SG0_SEC_CHGRP, 4, index)) return false;
    const uint8_t *p = pack->data + pack->sec[SG0_SEC_CHGRP].offset + (uint32_t)index * 4u;
    out->cursor = rd16(p);
    out->group = p[2];
    return true;
}

bool sg0_pack_event(const sg0_pack_t *pack, uint16_t index, sg0_event_t *out)
{
    if (!pack || !out || !section_ok(pack, SG0_SEC_EVENT, 16, index)) return false;
    const uint8_t *p = pack->data + pack->sec[SG0_SEC_EVENT].offset + (uint32_t)index * 16u;
    out->at = rd16(p);
    out->type = p[2];
    out->flags = p[3];
    out->a = rd16(p + 4);
    out->b = rd16(p + 6);
    out->c = rd16(p + 8);
    out->d = rd16(p + 10);
    out->data = rd16(p + 12);
    return out->type <= SG0_EVENT_EXPIRE_RINE;
}

bool sg0_pack_start(const sg0_pack_t *pack, uint16_t index, sg0_start_t *out)
{
    if (!pack || !out || !section_ok(pack, SG0_SEC_START, 12, index)) return false;
    const uint8_t *p = pack->data + pack->sec[SG0_SEC_START].offset + (uint32_t)index * 12u;
    out->node = p[0];
    out->group = p[1];
    out->script = rd16(p + 2);
    out->index = rd16(p + 4);
    out->date = rd16(p + 6);
    out->name = rd16(p + 8);
    return out->node < pack->sec[SG0_SEC_NODE].count &&
           out->date < pack->sec[SG0_SEC_DATE].count &&
           out->name < pack->sec[SG0_SEC_STRT].count;
}

bool sg0_pack_asset(const sg0_pack_t *pack, uint16_t index, sg0_assetid_t *out)
{
    if (!pack || !out || !section_ok(pack, SG0_SEC_ASSETID, 8, index)) return false;
    const uint8_t *p = pack->data + pack->sec[SG0_SEC_ASSETID].offset + (uint32_t)index * 8u;
    out->part_first = rd16(p);
    out->part_count = p[2];
    out->kind = p[3];
    out->name = rd16(p + 4);
    return range_ok(out->part_first, out->part_count, pack->sec[SG0_SEC_PART].count) &&
           out->name < pack->sec[SG0_SEC_STRT].count;
}

bool sg0_pack_part(const sg0_pack_t *pack, uint16_t index, sg0_part_t *out)
{
    if (!pack || !out || !section_ok(pack, SG0_SEC_PART, 30, index)) return false;
    const uint8_t *p = pack->data + pack->sec[SG0_SEC_PART].offset + (uint32_t)index * 30u;
    out->off = rd32(p);
    out->len = rd32(p + 4);
    out->mask_off = rd32(p + 8);
    out->mask_len = rd32(p + 12);
    out->w = rd16(p + 16);
    out->h = rd16(p + 18);
    out->ax = rd16s(p + 20);
    out->ay = rd16s(p + 22);
    out->ow = rd16(p + 24);
    out->oh = rd16(p + 26);
    out->flags = p[28];
    const sg0_section_t *blob = &pack->sec[SG0_SEC_BLOB];
    if (out->off > blob->size || out->len > blob->size - out->off) return false;
    if (out->mask_len && (out->mask_off > blob->size ||
                          out->mask_len > blob->size - out->mask_off)) {
        return false;
    }
    return true;
}

bool sg0_pack_rine_msg(const sg0_pack_t *pack, uint16_t index, sg0_rine_msg_t *out)
{
    if (!pack || !out || !section_ok(pack, SG0_SEC_RMSG, 16, index)) return false;
    const uint8_t *p = pack->data + pack->sec[SG0_SEC_RMSG].offset + (uint32_t)index * 16u;
    out->id = rd16(p);
    out->base = rd16(p + 2);
    out->date = rd16(p + 4);
    out->contact = p[6];
    out->sender = p[7];
    out->flags = p[8];
    out->kind = p[9];
    out->text = rd16(p + 10);
    out->stamp = rd16(p + 12);
    return true;
}

bool sg0_pack_rine_reply(const sg0_pack_t *pack, uint16_t index, uint16_t *base,
                         uint8_t *contact, uint8_t *choices, uint16_t *first_choice,
                         uint16_t *key)
{
    if (!pack || !section_ok(pack, SG0_SEC_RREPLY, 8, index)) return false;
    const uint8_t *p = pack->data + pack->sec[SG0_SEC_RREPLY].offset + (uint32_t)index * 8u;
    if (base) *base = rd16(p);
    if (contact) *contact = p[2];
    if (choices) *choices = p[3];
    if (first_choice) *first_choice = rd16(p + 4);
    if (key) *key = rd16(p + 6);
    return true;
}

bool sg0_pack_rine_choice(const sg0_pack_t *pack, uint16_t index, uint16_t *selected,
                          uint16_t *seq_first, uint8_t *seq_count, uint8_t *stamp)
{
    if (!pack || !section_ok(pack, SG0_SEC_RCHOICE, 8, index)) return false;
    const uint8_t *p = pack->data + pack->sec[SG0_SEC_RCHOICE].offset + (uint32_t)index * 8u;
    if (selected) *selected = rd16(p);
    if (seq_first) *seq_first = rd16(p + 2);
    if (seq_count) *seq_count = p[4];
    if (stamp) *stamp = p[5];
    return true;
}

bool sg0_pack_rine_seq(const sg0_pack_t *pack, uint16_t index, uint16_t *ref)
{
    if (!pack || !ref || !section_ok(pack, SG0_SEC_RSEQ, 2, index)) return false;
    *ref = rd16(pack->data + pack->sec[SG0_SEC_RSEQ].offset + (uint32_t)index * 2u);
    return true;
}

bool sg0_pack_rine_contact(const sg0_pack_t *pack, uint8_t index, uint8_t *id,
                           const char **name, uint16_t *name_len,
                           const char **number, uint16_t *number_len)
{
    if (!pack || !section_ok(pack, SG0_SEC_RCONTACT, 8, index)) return false;
    const uint8_t *p = pack->data + pack->sec[SG0_SEC_RCONTACT].offset + (uint32_t)index * 8u;
    if (id) *id = p[0];
    if (name) {
        if (!sg0_pack_string(pack, rd16(p + 1), name, name_len)) return false;
    }
    if (number) {
        if (!sg0_pack_string(pack, rd16(p + 3), number, number_len)) return false;
    }
    return true;
}

bool sg0_pack_rine_base(const sg0_pack_t *pack, uint8_t index, uint16_t *base)
{
    if (!pack || !base || !section_ok(pack, SG0_SEC_RBASE, 2, index)) return false;
    *base = rd16(pack->data + pack->sec[SG0_SEC_RBASE].offset + (uint32_t)index * 2u);
    return true;
}

bool sg0_pack_card(const sg0_pack_t *pack, uint8_t group, uint16_t *asset)
{
    if (!pack || !asset || !section_ok(pack, SG0_SEC_CARD, 2, group)) return false;
    *asset = rd16(pack->data + pack->sec[SG0_SEC_CARD].offset + (uint32_t)group * 2u);
    return true;
}

bool sg0_pack_title(const sg0_pack_t *pack, uint8_t group, const char **out, uint16_t *len)
{
    if (!pack || !out || !len || !section_ok(pack, SG0_SEC_TITLE, 2, group)) return false;
    const uint16_t str = rd16(pack->data + pack->sec[SG0_SEC_TITLE].offset +
                              (uint32_t)group * 2u);
    return sg0_pack_string(pack, str, out, len);
}

bool sg0_pack_gallery_group(const sg0_pack_t *pack, uint8_t index, sg0_gallery_group_t *out)
{
    if (!pack || !out || !section_ok(pack, SG0_SEC_GG, 8, index)) return false;
    const uint8_t *p = pack->data + pack->sec[SG0_SEC_GG].offset + (uint32_t)index * 8u;
    out->label = rd16(p);
    out->first = rd16(p + 2);
    out->count = p[4];
    return out->label < pack->sec[SG0_SEC_STRT].count &&
           range_ok(out->first, out->count, pack->sec[SG0_SEC_GE].count);
}

bool sg0_pack_gallery_entry(const sg0_pack_t *pack, uint16_t index, uint16_t *asset)
{
    if (!pack || !asset || !section_ok(pack, SG0_SEC_GE, 4, index)) return false;
    *asset = rd16(pack->data + pack->sec[SG0_SEC_GE].offset + (uint32_t)index * 4u);
    return *asset < pack->sec[SG0_SEC_ASSETID].count;
}

uint16_t sg0_pack_meta_u16(const sg0_pack_t *pack, const char *key, uint16_t fallback)
{
    if (!pack || !pack->meta || !key) return fallback;
    const size_t klen = strlen(key);
    const char *p = (const char *)pack->meta;
    const char *end = p + pack->meta_size;
    while (p < end) {
        const char *line_end = memchr(p, '\n', (size_t)(end - p));
        if (!line_end) line_end = end;
        if ((size_t)(line_end - p) > klen && memcmp(p, key, klen) == 0 && p[klen] == '=') {
            uint32_t value = 0;
            const char *v = p + klen + 1;
            while (v < line_end && *v >= '0' && *v <= '9') value = value * 10u + (uint32_t)(*v++ - '0');
            return value > 0xFFFFu ? fallback : (uint16_t)value;
        }
        p = line_end + 1;
    }
    return fallback;
}

uint32_t sg0_pack_chunk_count(const sg0_pack_t *pack)
{
    return pack ? pack->sec[SG0_SEC_CHUNK].count : 0;
}

uint32_t sg0_pack_asset_count(const sg0_pack_t *pack)
{
    return pack ? pack->sec[SG0_SEC_ASSETID].count : 0;
}

uint32_t sg0_pack_gallery_count(const sg0_pack_t *pack)
{
    return pack ? pack->sec[SG0_SEC_GG].count : 0;
}

// ---------------------------------------------------------------- zlib inflate
// 独立的 DEFLATE 解码器:不依赖 ROM miniz,宿主机与固件运行同一份代码。
// 每个正文块是独立的 zlib 流,解压输出 <= 8 KB,因此窗口就是输出缓冲本身。
typedef struct {
    const uint8_t *src;
    uint32_t len;
    uint32_t pos;
    uint32_t bits;
    int count;
} bit_reader_t;

typedef struct {
    uint16_t counts[16];
    uint16_t symbols[288];
} huff_t;

static uint32_t adler32(const uint8_t *data, uint32_t len)
{
    uint32_t a = 1, b = 0;
    for (uint32_t i = 0; i < len; ++i) {
        a += data[i];
        if (a >= 65521u) a -= 65521u;
        b += a;
        if (b >= 65521u) b -= 65521u;
    }
    return (b << 16) | a;
}

static bool bits_need(bit_reader_t *br, int need)
{
    while (br->count < need) {
        if (br->pos >= br->len) return false;
        br->bits |= (uint32_t)br->src[br->pos++] << br->count;
        br->count += 8;
    }
    return true;
}

static int bits_get(bit_reader_t *br, int need)
{
    if (!bits_need(br, need)) return -1;
    const uint32_t value = br->bits & ((1u << need) - 1u);
    br->bits >>= need;
    br->count -= need;
    return (int)value;
}

static void bits_align(bit_reader_t *br)
{
    const int drop = br->count & 7;
    br->bits >>= drop;
    br->count -= drop;
}

// 丢弃位缓冲中剩余的整字节,让 pos 指到下一个字节边界。
static void bits_align_bytes(bit_reader_t *br)
{
    bits_align(br);
    br->pos -= (uint32_t)(br->count / 8);
    br->bits = 0;
    br->count = 0;
}

static void huff_build(huff_t *h, const uint8_t *lengths, int n)
{
    memset(h->counts, 0, sizeof(h->counts));
    for (int i = 0; i < n; ++i) h->counts[lengths[i]]++;
    h->counts[0] = 0;
    uint16_t offsets[16];
    uint16_t sum = 0;
    for (int len = 1; len < 16; ++len) {
        offsets[len] = sum;
        sum = (uint16_t)(sum + h->counts[len]);
    }
    for (int i = 0; i < n; ++i) {
        if (lengths[i]) h->symbols[offsets[lengths[i]]++] = (uint16_t)i;
    }
}

static int huff_decode(bit_reader_t *br, const huff_t *h)
{
    int code = 0;
    int first = 0;
    int index = 0;
    for (int len = 1; len <= 15; ++len) {
        const int bit = bits_get(br, 1);
        if (bit < 0) return -1;
        code |= bit;
        const int count = h->counts[len];
        if (code - first < count) return h->symbols[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

static const uint16_t k_len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
    67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t k_len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5,
    5, 5, 5, 0};
static const uint16_t k_dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
    769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t k_dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10,
    11, 11, 12, 12, 13, 13};

static bool inflate_block(bit_reader_t *br, uint8_t *out, uint32_t cap, uint32_t *produced)
{
    const int final = bits_get(br, 1);
    const int type = bits_get(br, 2);
    if (final < 0 || type < 0) return false;
    if (type == 0) {   // stored
        bits_align_bytes(br);
        if (br->pos + 4 > br->len) return false;
        const uint32_t len = (uint32_t)br->src[br->pos] | ((uint32_t)br->src[br->pos + 1] << 8);
        const uint32_t nlen = (uint32_t)br->src[br->pos + 2] |
                              ((uint32_t)br->src[br->pos + 3] << 8);
        br->pos += 4;
        if ((len ^ 0xFFFFu) != nlen) return false;
        if (br->pos + len > br->len) return false;
        if (*produced + len > cap) return false;
        memcpy(out + *produced, br->src + br->pos, len);
        *produced += len;
        br->pos += len;
        br->bits = 0;
        br->count = 0;
        return final ? true : inflate_block(br, out, cap, produced);
    }
    huff_t lit;
    huff_t dist;
    uint8_t lengths[320];
    if (type == 1) {   // fixed
        int i = 0;
        for (; i < 144; ++i) lengths[i] = 8;
        for (; i < 256; ++i) lengths[i] = 9;
        for (; i < 280; ++i) lengths[i] = 7;
        for (; i < 288; ++i) lengths[i] = 8;
        huff_build(&lit, lengths, 288);
        for (i = 0; i < 30; ++i) lengths[i] = 5;
        huff_build(&dist, lengths, 30);
    } else if (type == 2) {   // dynamic
        const int hlit = bits_get(br, 5);
        const int hdist = bits_get(br, 5);
        const int hclen = bits_get(br, 4);
        if (hlit < 0 || hdist < 0 || hclen < 0) return false;
        const int nlit = hlit + 257;
        const int ndist = hdist + 1;
        const int nclen = hclen + 4;
        if (nlit > 286 || ndist > 30) return false;
        static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4,
                                          12, 3, 13, 2, 14, 1, 15};
        memset(lengths, 0, sizeof(lengths));
        for (int i = 0; i < nclen; ++i) {
            const int v = bits_get(br, 3);
            if (v < 0) return false;
            lengths[order[i]] = (uint8_t)v;
        }
        huff_t clen;
        huff_build(&clen, lengths, 19);
        int i = 0;
        while (i < nlit + ndist) {
            const int sym = huff_decode(br, &clen);
            if (sym < 0) return false;
            if (sym < 16) {
                lengths[i++] = (uint8_t)sym;
            } else if (sym == 16) {
                if (i == 0) return false;
                const int rep = bits_get(br, 2);
                if (rep < 0 || i + 3 + rep > nlit + ndist) return false;
                const uint8_t prev = lengths[i - 1];
                for (int k = 0; k < 3 + rep; ++k) lengths[i++] = prev;
            } else if (sym == 17) {
                const int rep = bits_get(br, 3);
                if (rep < 0 || i + 3 + rep > nlit + ndist) return false;
                for (int k = 0; k < 3 + rep; ++k) lengths[i++] = 0;
            } else {
                const int rep = bits_get(br, 7);
                if (rep < 0 || i + 11 + rep > nlit + ndist) return false;
                for (int k = 0; k < 11 + rep; ++k) lengths[i++] = 0;
            }
        }
        if (lengths[256] == 0) return false;
        huff_build(&lit, lengths, nlit);
        huff_build(&dist, lengths + nlit, ndist);
    } else {
        return false;
    }
    for (;;) {
        const int sym = huff_decode(br, &lit);
        if (sym < 0) return false;
        if (sym < 256) {
            if (*produced >= cap) return false;
            out[(*produced)++] = (uint8_t)sym;
        } else if (sym == 256) {
            break;
        } else {
            const int li = sym - 257;
            if (li >= 29) return false;
            const int extra = bits_get(br, k_len_extra[li]);
            if (extra < 0) return false;
            const uint32_t length = (uint32_t)k_len_base[li] + (uint32_t)extra;
            const int dsym = huff_decode(br, &dist);
            if (dsym < 0 || dsym >= 30) return false;
            const int dextra = bits_get(br, k_dist_extra[dsym]);
            if (dextra < 0) return false;
            const uint32_t distance = (uint32_t)k_dist_base[dsym] + (uint32_t)dextra;
            if (distance == 0 || distance > *produced) return false;
            if (*produced + length > cap) return false;
            for (uint32_t k = 0; k < length; ++k) {
                out[*produced] = out[*produced - distance];
                (*produced)++;
            }
        }
    }
    return final ? true : inflate_block(br, out, cap, produced);
}

bool sg0_pack_inflate(const uint8_t *src, uint32_t len, uint8_t *out, uint32_t cap,
                      uint32_t *out_len)
{
    if (!src || !out || !out_len || len < 6) return false;
    const uint8_t cmf = src[0];
    const uint8_t flg = src[1];
    if ((cmf & 0x0Fu) != 8u) return false;
    if (((uint32_t)cmf * 256u + flg) % 31u != 0u) return false;
    if (flg & 0x20u) return false;
    bit_reader_t br = {.src = src, .len = len, .pos = 2, .bits = 0, .count = 0};
    uint32_t produced = 0;
    if (!inflate_block(&br, out, cap, &produced)) return false;
    // 跳过位缓冲中的剩余字节,读出 adler32。
    bits_align_bytes(&br);
    const uint32_t pos = br.pos;
    if (pos + 4 > len) return false;
    const uint32_t want = ((uint32_t)src[pos] << 24) | ((uint32_t)src[pos + 1] << 16) |
                          ((uint32_t)src[pos + 2] << 8) | (uint32_t)src[pos + 3];
    if (adler32(out, produced) != want) return false;
    *out_len = produced;
    return true;
}

bool sg0_pack_chunk_load(const sg0_pack_t *pack, const sg0_chunk_t *chunk, uint8_t *out,
                         uint32_t cap, uint32_t *raw_len)
{
    if (!pack || !chunk || !out || !raw_len) return false;
    const sg0_section_t *blob = &pack->sec[SG0_SEC_BLOB];
    if (chunk->zoff > blob->size || chunk->zlen > blob->size - chunk->zoff) return false;
    if (chunk->raw_len > cap) return false;
    uint32_t produced = 0;
    if (!sg0_pack_inflate(pack->data + blob->offset + chunk->zoff, chunk->zlen, out, cap,
                          &produced)) {
        return false;
    }
    if (produced != chunk->raw_len) return false;
    *raw_len = produced;
    return true;
}
