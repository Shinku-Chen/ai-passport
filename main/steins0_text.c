// main/steins0_text.c —— 纯逻辑实现。
#include "steins0_text.h"

#include "steins0_widths.h"

#include <string.h>

#define PUA_BASE 0xE000u
#define PUA_END 0xF8FFu

uint32_t sg0_utf8_next(const char *s, size_t len, size_t *pos)
{
    const uint8_t *p = (const uint8_t *)s;
    size_t i = *pos;
    if (!s || i >= len) return 0;
    const uint8_t lead = p[i];
    uint32_t cp = lead;
    size_t step = 1;
    if ((lead & 0xE0u) == 0xC0u) {
        cp = lead & 0x1Fu;
        step = 2;
    } else if ((lead & 0xF0u) == 0xE0u) {
        cp = lead & 0x0Fu;
        step = 3;
    } else if ((lead & 0xF8u) == 0xF0u) {
        cp = lead & 0x07u;
        step = 4;
    } else if (lead >= 0x80u) {
        *pos = i + 1;
        return 0xFFFDu;
    }
    if (i + step > len) {
        *pos = len;
        return 0xFFFDu;
    }
    for (size_t k = 1; k < step; ++k) {
        if ((p[i + k] & 0xC0u) != 0x80u) {
            *pos = i + 1;
            return 0xFFFDu;
        }
        cp = (cp << 6) | (p[i + k] & 0x3Fu);
    }
    *pos = i + step;
    return cp;
}

size_t sg0_utf8_prev(const char *s, size_t pos)
{
    if (!s || pos == 0) return 0;
    size_t i = pos - 1;
    while (i > 0 && (((uint8_t)s[i]) & 0xC0u) == 0x80u) --i;
    return i;
}

size_t sg0_utf8_encode(uint32_t cp, char *out)
{
    if (cp < 0x80u) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    out[0] = (char)(0xF0u | (cp >> 18));
    out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

uint16_t sg0_advance(uint32_t cp)
{
    if (cp < 0x80u) {
        if (cp >= 0x20u && cp <= 0x7Eu) return sg0_ascii_adv[cp - 0x20u];
        return cp == 0x20u ? 85u : SG0_ADV_HALF;
    }
    if (cp >= PUA_BASE && cp <= PUA_END) {
        const uint32_t id = cp - PUA_BASE;
        if (id < sizeof(sg0_pua_adv) / sizeof(sg0_pua_adv[0])) return sg0_pua_adv[id];
        return SG0_ADV_FULL;
    }
    if (cp == 0x3000u) return 85u;
    return SG0_ADV_FULL;
}

bool sg0_no_line_start(uint32_t cp)
{
    // 源 PUA 标点:SC3 字形 id 190..231(、。，．：；？！‘’“”（〔［｛〈《「『【＜＞・…～ー♪─)
    if (cp >= PUA_BASE && cp <= PUA_BASE + 0xE7u) {
        const uint32_t id = cp - PUA_BASE;
        if (id >= 190u && id <= 231u) return true;
    }
    if (cp == PUA_BASE + 702u || cp == PUA_BASE + 703u) return true;  // 。、
    switch (cp) {
    case 0x3001: case 0x3002: case 0xFF0C: case 0xFF0E: case 0xFF01: case 0xFF1F:
    case 0xFF1A: case 0xFF1B: case 0xFF09: case 0x300D: case 0x300F: case 0x3011:
    case 0x300B: case 0x3009: case 0x2026: case 0x2015: case 0x2014: case 0x30FB:
    case 0xFF5D: case 0x3015: case 0x2019: case 0x201D: case 0x0029: case 0x005D:
    case 0x003A: case 0x003B: case 0x0025: case 0x0021: case 0x003F: case 0x002C:
    case 0x002E:
        return true;
    default:
        return false;
    }
}

static bool opening_bracket(uint32_t cp)
{
    switch (cp) {
    case 0x0028: case 0x005B: case 0x007B: case 0x3008: case 0x300A: case 0x300C:
    case 0x300E: case 0x3010: case 0x3014: case 0xFF08: case 0xFF3B: case 0xFF5B:
    case 0x2018: case 0x201C:
        return true;
    default:
        break;
    }
    if (cp >= PUA_BASE && cp <= PUA_BASE + 0xE7u) {
        const uint32_t id = cp - PUA_BASE;
        return id >= 200u && id <= 231u && (id == 200u || id == 202u || id == 204u ||
                                             id == 206u || id == 208u || id == 210u ||
                                             id == 212u || id == 214u || id == 216u ||
                                             id == 218u || id == 220u || id == 222u);
    }
    return false;
}

static bool is_space(uint32_t cp)
{
    return cp == 0x20u || cp == 0x3000u || cp == PUA_BASE || cp == PUA_BASE + 0x3Fu;
}

static bool is_cjk(uint32_t cp)
{
    if (cp >= 0x3040u && cp <= 0x9FFFu) return true;
    if (cp >= PUA_BASE + 0xE8u && cp <= 0xF1FFu) return true;
    return false;
}

size_t sg0_text_clean(const char *src, size_t len, char *dst, size_t cap)
{
    if (!dst || cap == 0) return 0;
    if (!src) src = "";
    uint32_t cps[384];
    size_t count = 0;
    size_t pos = 0;
    while (pos < len && count < sizeof(cps) / sizeof(cps[0])) {
        uint32_t cp = sg0_utf8_next(src, len, &pos);
        if (cp == 0) break;
        cps[count++] = cp;
    }
    // 去外层对话括号(源 dialogueText:去掉首「与尾」)。
    if (count >= 1 && (cps[0] == PUA_BASE + 216u || cps[0] == 0x300Cu)) {
        memmove(cps, cps + 1, (count - 1) * sizeof(cps[0]));
        --count;
    }
    if (count >= 1 && (cps[count - 1] == PUA_BASE + 217u || cps[count - 1] == 0x300Du)) {
        --count;
    }
    // 去首尾空白。
    size_t begin = 0;
    while (begin < count && is_space(cps[begin])) ++begin;
    while (count > begin && is_space(cps[count - 1])) --count;
    size_t out = 0;
    bool prev_space = false;
    for (size_t i = begin; i < count; ++i) {
        uint32_t cp = is_space(cps[i]) ? 0x20u : cps[i];
        if (cp == 0x20u) {
            if (prev_space) continue;
            const bool prev_cjk = i > begin && is_cjk(cps[i - 1]);
            const bool next_cjk = i + 1 < count && is_cjk(cps[i + 1]);
            const bool next_close = i + 1 < count && sg0_no_line_start(cps[i + 1]);
            if (prev_cjk || next_cjk || next_close) continue;
            prev_space = true;
        } else {
            prev_space = false;
        }
        char tmp[4];
        const size_t n = sg0_utf8_encode(cp, tmp);
        if (out + n + 1 > cap) break;
        memcpy(dst + out, tmp, n);
        out += n;
    }
    dst[out < cap ? out : cap - 1] = '\0';
    return out;
}

static uint32_t measure(const char *text, size_t from, size_t to)
{
    uint32_t units = 0;
    size_t pos = from;
    while (pos < to) {
        const uint32_t cp = sg0_utf8_next(text, to, &pos);
        units += sg0_advance(cp);
    }
    return units;
}

bool sg0_text_layout(const char *text, uint16_t width_adv, uint8_t lines_per_page,
                     sg0_pages_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!text) text = "";
    if (!lines_per_page) lines_per_page = 1;
    if (lines_per_page > SG0_TEXT_LINES) lines_per_page = SG0_TEXT_LINES;
    const size_t len = strlen(text);
    uint16_t line_offs[128];
    uint8_t line_count = 0;
    uint32_t units = 0;
    size_t line_start = 0;
    size_t pos = 0;
    while (pos < len && line_count < sizeof(line_offs) / sizeof(line_offs[0])) {
        const size_t cp_start = pos;
        const uint32_t cp = sg0_utf8_next(text, len, &pos);
        if (cp == 0) break;
        if (cp == '\n') {
            line_offs[line_count++] = (uint16_t)line_start;
            line_start = pos;
            units = 0;
            continue;
        }
        const uint32_t adv = sg0_advance(cp);
        if (units > 0 && units + adv > width_adv) {
            size_t break_at = cp_start;
            if (sg0_no_line_start(cp) && break_at > line_start) {
                // 连续闭标点:一直退到前面第一个非禁则/非开括号码点之前,
                // 否则第二个句号仍会落到下一行行首。回退受 line_start 边界约束。
                break_at = sg0_utf8_prev(text, break_at);
                while (break_at > line_start) {
                    const size_t at = break_at;
                    size_t next_pos = at;
                    const uint32_t at_cp = sg0_utf8_next(text, len, &next_pos);
                    if (!sg0_no_line_start(at_cp) && !opening_bracket(at_cp)) break;
                    break_at = sg0_utf8_prev(text, break_at);
                }
                if (break_at <= line_start) break_at = cp_start;
            } else if (break_at > line_start) {
                // 普通字符溢出时,若前一个字符是开括号,不要把开括号留在行尾。
                size_t prev = sg0_utf8_prev(text, break_at);
                size_t p2 = prev;
                const uint32_t prev_cp = sg0_utf8_next(text, break_at, &p2);
                if (opening_bracket(prev_cp)) break_at = prev;
            }
            if (break_at <= line_start) break_at = cp_start;
            line_offs[line_count++] = (uint16_t)line_start;
            line_start = break_at;
            units = measure(text, break_at, cp_start) + adv;
        } else {
            units += adv;
        }
    }
    line_offs[line_count++] = (uint16_t)line_start;
    if (line_count == 0) line_offs[line_count++] = 0;
    uint32_t page = 0;
    uint32_t index = 0;
    out->page_start[0] = (uint16_t)line_offs[0];
    while (index < line_count && page < SG0_TEXT_MAX_PAGES) {
        const uint8_t take = (uint8_t)((line_count - index) > lines_per_page
                                           ? lines_per_page
                                           : (line_count - index));
        out->line_count[page] = take;
        for (uint8_t k = 0; k < take; ++k) {
            out->line_start[page][k] = line_offs[index + k];
        }
        const uint32_t next_index = index + take;
        out->line_start[page][take] =
            next_index < line_count ? line_offs[next_index] : (uint16_t)len;
        ++page;
        index = next_index;
        if (page < SG0_TEXT_MAX_PAGES) out->page_start[page] = out->line_start[page - 1][take];
    }
    if (page == 0) {
        out->line_count[0] = 1;
        out->line_start[0][0] = 0;
        out->line_start[0][1] = 0;
        page = 1;
    }
    out->pages = (uint8_t)page;
    out->page_start[page] = (uint16_t)len;
    return true;
}

// ---------------------------------------------------------------- CRC / 存档
uint16_t sg0_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

void sg0_save_init(sg0_save_t *save, uint8_t node, uint16_t cursor, uint8_t mode)
{
    if (!save) return;
    memset(save, 0, sizeof(*save));
    save->meta.node = node;
    save->meta.cursor = cursor;
    save->meta.mode = mode;
    save->last_card = 0xFFu;
}

void sg0_save_meta_from(const sg0_save_t *save, sg0_save_meta_t *meta)
{
    if (!save || !meta) return;
    *meta = save->meta;
}

static bool save_bit(const uint8_t *bits, uint16_t ref)
{
    if (ref >= SG0_SAVE_RINE_BITS) return false;
    return (bits[ref >> 3] >> (ref & 7u)) & 1u;
}

static void save_set_bit(uint8_t *bits, uint16_t ref, bool value)
{
    if (ref >= SG0_SAVE_RINE_BITS) return;
    if (value) bits[ref >> 3] |= (uint8_t)(1u << (ref & 7u));
    else bits[ref >> 3] &= (uint8_t)~(1u << (ref & 7u));
}

bool sg0_save_rine_active(const sg0_save_t *save, uint16_t ref)
{
    return save && save_bit(save->rine_active, ref);
}

bool sg0_save_rine_read(const sg0_save_t *save, uint16_t ref)
{
    return save && save_bit(save->rine_read, ref);
}

void sg0_save_rine_set(sg0_save_t *save, uint16_t ref, bool read)
{
    if (!save) return;
    save_set_bit(save->rine_active, ref, true);
    save_set_bit(save->rine_read, ref, read);
}

size_t sg0_save_encode(const sg0_save_t *save, uint8_t *out, size_t cap)
{
    if (!save || !out || cap < SG0_SAVE_BYTES) return 0;
    memset(out, 0, SG0_SAVE_BYTES);
    const sg0_save_meta_t *meta = &save->meta;
    out[0] = 'S';
    out[1] = '0';
    out[2] = SG0_SAVE_VERSION;
    out[3] = meta->node;
    out[4] = (uint8_t)(meta->cursor & 0xFFu);
    out[5] = (uint8_t)(meta->cursor >> 8);
    out[6] = meta->page;
    out[7] = meta->mode;
    out[8] = meta->endings;
    out[9] = meta->flags;
    memcpy(out + 10, meta->chapters, SG0_CHAPTER_BYTES);
    memcpy(out + 10 + SG0_CHAPTER_BYTES, meta->gallery, SG0_GALLERY_BYTES);
    out[37] = meta->group;
    out[38] = (uint8_t)(meta->date & 0xFFu);
    out[39] = (uint8_t)((meta->date >> 8) & 0xFFu);
    out[40] = (uint8_t)((meta->date >> 16) & 0xFFu);
    out[41] = (uint8_t)((meta->date >> 24) & 0xFFu);
    const uint8_t preview_len = meta->preview_len > SG0_SAVE_PREVIEW - 1
                                    ? SG0_SAVE_PREVIEW - 1 : meta->preview_len;
    out[42] = preview_len;
    memcpy(out + 43, meta->preview, preview_len);
    const uint16_t core_crc = sg0_crc16(out, 62);
    out[62] = (uint8_t)(core_crc & 0xFFu);
    out[63] = (uint8_t)(core_crc >> 8);
    memcpy(out + 64, save->rine_active, SG0_SAVE_RINE_BYTES);
    memcpy(out + 64 + SG0_SAVE_RINE_BYTES, save->rine_read, SG0_SAVE_RINE_BYTES);
    const uint16_t off = 64 + SG0_SAVE_RINE_BYTES * 2;
    out[off] = (uint8_t)(save->rine_replied & 0xFFu);
    out[off + 1] = (uint8_t)(save->rine_replied >> 8);
    out[off + 2] = (uint8_t)(save->rine_windows & 0xFFu);
    out[off + 3] = (uint8_t)(save->rine_windows >> 8);
    out[off + 4] = save->pending_valid;
    out[off + 5] = save->pending_node;
    out[off + 6] = save->last_card;
    out[off + 7] = save->event_node;
    out[off + 8] = (uint8_t)(save->event_cursor & 0xFFu);
    out[off + 9] = (uint8_t)(save->event_cursor >> 8);
    const uint16_t ext_crc = sg0_crc16(out + 64, off + 10u - 64u);
    out[SG0_SAVE_BYTES - 2] = (uint8_t)(ext_crc & 0xFFu);
    out[SG0_SAVE_BYTES - 1] = (uint8_t)(ext_crc >> 8);
    return SG0_SAVE_BYTES;
}

bool sg0_save_decode(sg0_save_t *save, const uint8_t *data, size_t len)
{
    if (!save || !data) return false;
    if (len != SG0_SAVE_CORE_BYTES && len != SG0_SAVE_V2_BYTES &&
        len != SG0_SAVE_BYTES) return false;
    if (data[0] != 'S' || data[1] != '0') return false;
    const uint8_t version = data[2];
    if (version < 1 || version > SG0_SAVE_VERSION) return false;
    if (version == 1 && len != SG0_SAVE_CORE_BYTES) return false;
    if (version == 2 && len != SG0_SAVE_V2_BYTES) return false;
    if (version == 3 && len != SG0_SAVE_BYTES) return false;
    const uint16_t core_crc = (uint16_t)data[62] | ((uint16_t)data[63] << 8);
    if (core_crc != sg0_crc16(data, 62)) return false;
    memset(save, 0, sizeof(*save));
    sg0_save_meta_t *meta = &save->meta;
    meta->node = data[3];
    meta->cursor = (uint16_t)data[4] | ((uint16_t)data[5] << 8);
    meta->page = data[6];
    meta->mode = data[7] > 1 ? 1 : data[7];
    meta->endings = data[8] & 0x3Fu;
    meta->flags = data[9];
    memcpy(meta->chapters, data + 10, SG0_CHAPTER_BYTES);
    memcpy(meta->gallery, data + 10 + SG0_CHAPTER_BYTES, SG0_GALLERY_BYTES);
    meta->group = data[37];
    meta->date = (uint32_t)data[38] | ((uint32_t)data[39] << 8) |
                 ((uint32_t)data[40] << 16) | ((uint32_t)data[41] << 24);
    meta->preview_len = data[42] >= SG0_SAVE_PREVIEW ? SG0_SAVE_PREVIEW - 1 : data[42];
    memcpy(meta->preview, data + 43, meta->preview_len);
    meta->preview[meta->preview_len] = 0;
    if (meta->page > SG0_TEXT_MAX_PAGES) return false;
    if (version == 1) {
        save->last_card = 0xFFu;
        return true;
    }
    const uint16_t off = 64 + SG0_SAVE_RINE_BYTES * 2;
    const bool v3 = version == 3;
    const uint16_t ext_len = v3 ? 10u : 7u;
    const uint16_t crc_at = v3 ? (uint16_t)(SG0_SAVE_BYTES - 2u)
                               : (uint16_t)(SG0_SAVE_V2_BYTES - 2u);
    const uint16_t ext_crc = (uint16_t)data[crc_at] | ((uint16_t)data[crc_at + 1] << 8);
    if (ext_crc != sg0_crc16(data + 64, off + ext_len - 64u)) return false;
    memcpy(save->rine_active, data + 64, SG0_SAVE_RINE_BYTES);
    memcpy(save->rine_read, data + 64 + SG0_SAVE_RINE_BYTES, SG0_SAVE_RINE_BYTES);
    save->rine_replied = (uint16_t)data[off] | ((uint16_t)data[off + 1] << 8);
    save->rine_windows = (uint16_t)data[off + 2] | ((uint16_t)data[off + 3] << 8);
    save->pending_valid = data[off + 4] ? 1 : 0;
    save->pending_node = data[off + 5];
    save->last_card = data[off + 6];
    if (v3) {
        save->event_node = data[off + 7];
        save->event_cursor = (uint16_t)data[off + 8] | ((uint16_t)data[off + 9] << 8);
        save->event_valid = 1;
    }
    return true;
}

// ---------------------------------------------------------------- 解锁
static bool bit_test(const uint8_t *bits, uint16_t index)
{
    return (bits[index >> 3] >> (index & 7u)) & 1u;
}

static void bit_set(uint8_t *bits, uint16_t index)
{
    bits[index >> 3] |= (uint8_t)(1u << (index & 7u));
}

void sg0_unlock_init(sg0_unlock_t *unlock)
{
    if (unlock) memset(unlock, 0, sizeof(*unlock));
}

bool sg0_unlock_has_chapter(const sg0_unlock_t *unlock, uint16_t index)
{
    return unlock && index < SG0_CHAPTER_BITS && bit_test(unlock->chapters, index);
}

void sg0_unlock_set_chapter(sg0_unlock_t *unlock, uint16_t index)
{
    if (unlock && index < SG0_CHAPTER_BITS) bit_set(unlock->chapters, index);
}

bool sg0_unlock_has_gallery(const sg0_unlock_t *unlock, uint8_t group)
{
    return unlock && group < SG0_GALLERY_BITS && bit_test(unlock->gallery, group);
}

void sg0_unlock_set_gallery(sg0_unlock_t *unlock, uint8_t group)
{
    if (unlock && group < SG0_GALLERY_BITS) bit_set(unlock->gallery, group);
}

bool sg0_unlock_has_ending(const sg0_unlock_t *unlock, uint8_t ending)
{
    return unlock && ending < 6u && ((unlock->endings >> ending) & 1u);
}

void sg0_unlock_set_ending(sg0_unlock_t *unlock, uint8_t ending)
{
    if (unlock && ending < 6u) unlock->endings |= (uint8_t)(1u << ending);
}

bool sg0_unlock_has_flag(const sg0_unlock_t *unlock, uint8_t flag)
{
    return unlock && ((unlock->flags >> flag) & 1u);
}

void sg0_unlock_set_flag(sg0_unlock_t *unlock, uint8_t flag)
{
    if (unlock && flag < 8u) unlock->flags |= (uint8_t)(1u << flag);
}

bool sg0_unlock_merge(sg0_unlock_t *dst, const sg0_unlock_t *src)
{
    if (!dst || !src) return false;
    bool changed = false;
    const uint8_t endings = (uint8_t)(dst->endings | src->endings);
    if (endings != dst->endings) { dst->endings = endings; changed = true; }
    const uint8_t flags = (uint8_t)(dst->flags | src->flags);
    if (flags != dst->flags) { dst->flags = flags; changed = true; }
    for (size_t i = 0; i < SG0_CHAPTER_BYTES; ++i) {
        const uint8_t value = (uint8_t)(dst->chapters[i] | src->chapters[i]);
        if (value != dst->chapters[i]) { dst->chapters[i] = value; changed = true; }
    }
    for (size_t i = 0; i < SG0_GALLERY_BYTES; ++i) {
        const uint8_t value = (uint8_t)(dst->gallery[i] | src->gallery[i]);
        if (value != dst->gallery[i]) { dst->gallery[i] = value; changed = true; }
    }
    return changed;
}

bool sg0_unlock_merge_save(sg0_unlock_t *unlock, const sg0_save_t *save)
{
    if (!unlock || !save) return false;
    sg0_unlock_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.endings = save->meta.endings;
    tmp.flags = save->meta.flags;
    memcpy(tmp.chapters, save->meta.chapters, SG0_CHAPTER_BYTES);
    memcpy(tmp.gallery, save->meta.gallery, SG0_GALLERY_BYTES);
    return sg0_unlock_merge(unlock, &tmp);
}

size_t sg0_unlock_encode(const sg0_unlock_t *unlock, uint8_t *out, size_t cap)
{
    if (!unlock || !out || cap < SG0_UNLOCK_BYTES) return 0;
    memset(out, 0, SG0_UNLOCK_BYTES);
    out[0] = 'U';
    out[1] = '0';
    out[2] = SG0_UNLOCK_VERSION;
    out[3] = unlock->endings;
    out[4] = unlock->flags;
    memcpy(out + 5, unlock->chapters, SG0_CHAPTER_BYTES);
    memcpy(out + 5 + SG0_CHAPTER_BYTES, unlock->gallery, SG0_GALLERY_BYTES);
    const uint16_t crc = sg0_crc16(out, 5 + SG0_CHAPTER_BYTES + SG0_GALLERY_BYTES);
    (void)crc;
    out[SG0_UNLOCK_BYTES - 2] = (uint8_t)(crc & 0xFFu);
    out[SG0_UNLOCK_BYTES - 1] = (uint8_t)(crc >> 8);
    return SG0_UNLOCK_BYTES;
}

bool sg0_unlock_decode(sg0_unlock_t *unlock, const uint8_t *data, size_t len)
{
    if (!unlock || !data || len != SG0_UNLOCK_BYTES) return false;
    if (data[0] != 'U' || data[1] != '0' || data[2] != SG0_UNLOCK_VERSION) return false;
    const uint16_t crc = (uint16_t)data[SG0_UNLOCK_BYTES - 2] |
                         ((uint16_t)data[SG0_UNLOCK_BYTES - 1] << 8);
    if (crc != sg0_crc16(data, 5 + SG0_CHAPTER_BYTES + SG0_GALLERY_BYTES)) return false;
    memset(unlock, 0, sizeof(*unlock));
    unlock->endings = data[3] & 0x3Fu;
    unlock->flags = data[4];
    memcpy(unlock->chapters, data + 5, SG0_CHAPTER_BYTES);
    memcpy(unlock->gallery, data + 5 + SG0_CHAPTER_BYTES, SG0_GALLERY_BYTES);
    return true;
}
