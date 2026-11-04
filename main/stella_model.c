// main/stella_model.c —— 纯逻辑实现。
#include "stella_model.h"

#include <string.h>

const stella_layout_t STELLA_LAYOUT_16 = { .units_per_line = 38, .lines_per_page = 4 };
const stella_layout_t STELLA_LAYOUT_20 = { .units_per_line = 30, .lines_per_page = 3 };

static bool codepoint_half_width(uint32_t cp)
{
    if (cp < 0x80u) return true;
    if (cp >= 0xFF61u && cp <= 0xFF9Fu) return true;
    return false;
}

static bool no_line_start(uint32_t cp)
{
    switch (cp) {
    case 0x3001: case 0x3002: case 0xFF0C: case 0xFF0E: case 0xFF01: case 0xFF1F:
    case 0xFF1A: case 0xFF1B: case 0xFF09: case 0x300D: case 0x300F: case 0x3011:
    case 0x300B: case 0x3009: case 0x2026: case 0x2015: case 0x2014: case 0x30FB:
    case 0xFF5D: case 0x3015:
        return true;
    default:
        return false;
    }
}

size_t stella_utf8_next(const char *utf8, size_t len, size_t pos)
{
    if (!utf8 || pos >= len) return len;
    const uint8_t *p = (const uint8_t *)utf8;
    const uint8_t lead = p[pos];
    size_t step = 1;
    if ((lead & 0xE0u) == 0xC0u) step = 2;
    else if ((lead & 0xF0u) == 0xE0u) step = 3;
    else if ((lead & 0xF8u) == 0xF0u) step = 4;
    if (pos + step > len) return len;
    for (size_t k = 1; k < step; ++k) {
        if ((p[pos + k] & 0xC0u) != 0x80u) return pos + 1;
    }
    return pos + step;
}

static uint32_t utf8_decode(const char *utf8, size_t len, size_t *pos)
{
    const uint8_t *p = (const uint8_t *)utf8;
    const size_t i = *pos;
    if (i >= len) return 0;
    const uint8_t lead = p[i];
    uint32_t cp = lead;
    size_t step = 1;
    if ((lead & 0xE0u) == 0xC0u) { cp = lead & 0x1Fu; step = 2; }
    else if ((lead & 0xF0u) == 0xE0u) { cp = lead & 0x0Fu; step = 3; }
    else if ((lead & 0xF8u) == 0xF0u) { cp = lead & 0x07u; step = 4; }
    else if (lead >= 0x80u) { *pos = i + 1; return 0xFFFDu; }
    if (i + step > len) { *pos = len; return 0xFFFDu; }
    for (size_t k = 1; k < step; ++k) {
        if ((p[i + k] & 0xC0u) != 0x80u) { *pos = i + 1; return 0xFFFDu; }
        cp = (cp << 6) | (p[i + k] & 0x3Fu);
    }
    *pos = i + step;
    return cp;
}

int stella_text_pages(const char *utf8, const stella_layout_t *layout,
                      uint32_t *offsets, int max_offsets)
{
    if (!utf8) utf8 = "";
    if (!layout) layout = &STELLA_LAYOUT_16;
    int units_per_line = layout->units_per_line < 2 ? 2 : layout->units_per_line;
    int lines_per_page = layout->lines_per_page < 1 ? 1 : layout->lines_per_page;
    const size_t len = strlen(utf8);
    int pages = 1;
    if (offsets && max_offsets > 0) offsets[0] = 0;
    int line_units = 0;
    int line_index = 0;
    size_t pos = 0;
    while (pos < len) {
        const size_t before = pos;
        const uint32_t cp = utf8_decode(utf8, len, &pos);
        if (cp == '\n') {
            line_units = 0;
            line_index++;
            if (line_index >= lines_per_page) {
                line_index = 0;
                pages++;
                if (offsets && pages - 1 < max_offsets) offsets[pages - 1] = (uint32_t)pos;
            }
            continue;
        }
        const int units = codepoint_half_width(cp) ? 1 : 2;
        if (line_units + units > units_per_line && line_units > 0) {
            if (no_line_start(cp)) {
                line_units = 0;
                line_index++;
                if (line_index >= lines_per_page) {
                    line_index = 0;
                    pages++;
                    if (offsets && pages - 1 < max_offsets) offsets[pages - 1] = (uint32_t)pos;
                }
                continue;
            }
            line_units = units;
            line_index++;
            if (line_index >= lines_per_page) {
                line_index = 0;
                pages++;
                if (offsets && pages - 1 < max_offsets) offsets[pages - 1] = (uint32_t)before;
            }
            continue;
        }
        line_units += units;
    }
    // 行尾禁则可能把最后一页的最后一个字“挂”到上一行，从而在文末多记一个空页；
    // 这里把末尾的空页合并掉，保证 offsets 严格递增。
    while (pages > 1 && offsets && pages - 1 < max_offsets && offsets[pages - 1] == (uint32_t)len) {
        pages--;
    }
    if (offsets && max_offsets > 0) {
        const int last = pages < max_offsets ? pages : max_offsets - 1;
        offsets[last] = (uint32_t)len;
    }
    return pages;
}

static uint16_t crc16_ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

size_t stella_save_encode(const stella_save_t *save, uint8_t *out, size_t cap)
{
    if (!save || !out || cap < STELLA_SAVE_BYTES) return 0;
    if (save->mode != STELLA_MODE_STORY && save->mode != STELLA_MODE_CHAPTER) return 0;
    if (save->scene == 0 || save->op >= 0x10000u) return 0;
    out[0] = 'S'; out[1] = 'T'; out[2] = 'L'; out[3] = 'S';
    out[4] = STELLA_SAVE_VERSION;
    out[5] = save->mode;
    out[6] = (uint8_t)(save->flags & 0xFFu);
    out[7] = (uint8_t)(save->flags >> 8);
    out[8] = (uint8_t)(save->scene & 0xFFu);
    out[9] = (uint8_t)(save->scene >> 8);
    out[10] = (uint8_t)(save->page & 0xFFu);
    out[11] = (uint8_t)(save->page >> 8);
    out[12] = (uint8_t)(save->op & 0xFFu);
    out[13] = (uint8_t)((save->op >> 8) & 0xFFu);
    out[14] = (uint8_t)((save->op >> 16) & 0xFFu);
    out[15] = (uint8_t)((save->op >> 24) & 0xFFu);
    out[16] = 0;
    out[17] = 0;
    const uint16_t crc = crc16_ccitt(out, 18);
    out[18] = (uint8_t)(crc & 0xFFu);
    out[19] = (uint8_t)(crc >> 8);
    return STELLA_SAVE_BYTES;
}

bool stella_save_decode(stella_save_t *save, const uint8_t *data, size_t len)
{
    if (!save || !data || len != STELLA_SAVE_BYTES) return false;
    if (data[0] != 'S' || data[1] != 'T' || data[2] != 'L' || data[3] != 'S') return false;
    // v1 没有 mode，无法可靠恢复 story/chapter；明确拒绝，不猜测。
    if (data[4] != STELLA_SAVE_VERSION) return false;
    const uint16_t crc = (uint16_t)(data[18] | ((uint16_t)data[19] << 8));
    if (crc != crc16_ccitt(data, 18)) return false;
    const uint8_t mode = data[5];
    if (mode != STELLA_MODE_STORY && mode != STELLA_MODE_CHAPTER) return false;
    const uint16_t flags = (uint16_t)(data[6] | ((uint16_t)data[7] << 8));
    const uint16_t scene = (uint16_t)(data[8] | ((uint16_t)data[9] << 8));
    const uint16_t page = (uint16_t)(data[10] | ((uint16_t)data[11] << 8));
    const uint32_t op = (uint32_t)data[12] | ((uint32_t)data[13] << 8) |
                        ((uint32_t)data[14] << 16) | ((uint32_t)data[15] << 24);
    if (scene == 0 || op >= 0x10000u || data[16] != 0 || data[17] != 0) return false;
    save->flags = flags;
    save->scene = scene;
    save->op = op;
    save->page = page;
    save->mode = mode;
    return true;
}

void stella_unlock_init(stella_unlock_t *unlock)
{
    if (unlock) memset(unlock, 0, sizeof(*unlock));
}

static bool bit_get(const uint8_t *bits, uint16_t index, uint16_t bytes)
{
    if (index >= bytes * 8u) return false;
    return (bits[index >> 3] >> (index & 7u)) & 1u;
}

static void bit_set(uint8_t *bits, uint16_t index, uint16_t bytes)
{
    if (index >= bytes * 8u) return;
    bits[index >> 3] |= (uint8_t)(1u << (index & 7u));
}

static int scene_bit(uint16_t scene_id)
{
    if (scene_id >= 1 && scene_id <= 21) return scene_id - 1;
    if (scene_id >= 101 && scene_id <= 109) return 21 + (scene_id - 101);
    return -1;
}

bool stella_unlock_has_scene(const stella_unlock_t *unlock, uint16_t scene_id)
{
    const int bit = scene_bit(scene_id);
    return unlock && bit >= 0 && bit_get(unlock->scenes, (uint16_t)bit, STELLA_UNLOCK_SCENE_BYTES);
}

void stella_unlock_set_scene(stella_unlock_t *unlock, uint16_t scene_id)
{
    const int bit = scene_bit(scene_id);
    if (unlock && bit >= 0) bit_set(unlock->scenes, (uint16_t)bit, STELLA_UNLOCK_SCENE_BYTES);
}

bool stella_unlock_has_gallery(const stella_unlock_t *unlock, uint16_t group)
{
    return unlock && bit_get(unlock->gallery, group, STELLA_UNLOCK_GALLERY_BYTES);
}

void stella_unlock_set_gallery(stella_unlock_t *unlock, uint16_t group)
{
    if (unlock) bit_set(unlock->gallery, group, STELLA_UNLOCK_GALLERY_BYTES);
}

bool stella_unlock_has_flag(const stella_unlock_t *unlock, uint8_t flag)
{
    return unlock && (unlock->flags & flag) != 0;
}

void stella_unlock_set_flag(stella_unlock_t *unlock, uint8_t flag)
{
    if (unlock) unlock->flags |= flag;
}
