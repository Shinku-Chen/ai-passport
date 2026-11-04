// main/clannad_text.c —— 分页/UTF-8 纯逻辑实现。
#include "clannad_text.h"

#include <stdbool.h>
#include <string.h>

static int codepoint_units(uint32_t cp)
{
    if (cp < 0x80u) return 1;
    if (cp >= 0xFF61u && cp <= 0xFF9Fu) return 1;
    return 2;
}

static bool no_line_start(uint32_t cp)
{
    switch (cp) {
    case 0x3001: case 0x3002: case 0xFF0C: case 0xFF0E: case 0xFF01:
    case 0xFF1F: case 0xFF1A: case 0xFF1B: case 0xFF09: case 0x300D:
    case 0x300F: case 0x3011: case 0x300B: case 0x3009: case 0x2026:
    case 0x2015: case 0x2014: case 0x30FB: case 0xFF5D: case 0x3015:
    case 0x3005: case 0x309D: case 0x309E: case 0xFF63:
        return true;
    default:
        return false;
    }
}

uint16_t clannad_utf8_next(const char *s, uint16_t len, uint16_t pos)
{
    if (!s || pos >= len) return len;
    const uint8_t *p = (const uint8_t *)s;
    const uint8_t lead = p[pos];
    uint16_t step = 1;
    if ((lead & 0xE0u) == 0xC0u) step = 2;
    else if ((lead & 0xF0u) == 0xE0u) step = 3;
    else if ((lead & 0xF8u) == 0xF0u) step = 4;
    if (pos + step > len) return len;
    for (uint16_t k = 1; k < step; ++k) {
        if ((p[pos + k] & 0xC0u) != 0x80u) return pos + 1;
    }
    return (uint16_t)(pos + step);
}

int clannad_utf8_count(const char *s, uint16_t len)
{
    int count = 0;
    for (uint16_t pos = 0; pos < len; count++) pos = clannad_utf8_next(s, len, pos);
    return count;
}

static uint32_t decode_at(const char *s, uint16_t len, uint16_t *pos)
{
    const uint8_t *p = (const uint8_t *)s;
    const uint16_t i = *pos;
    if (i >= len) return 0;
    const uint8_t lead = p[i];
    if (lead < 0x80u) {
        *pos = (uint16_t)(i + 1);
        return lead;
    }
    uint16_t step;
    uint32_t cp;
    if ((lead & 0xE0u) == 0xC0u) {
        step = 2;
        cp = lead & 0x1Fu;
    } else if ((lead & 0xF0u) == 0xE0u) {
        step = 3;
        cp = lead & 0x0Fu;
    } else if ((lead & 0xF8u) == 0xF0u) {
        step = 4;
        cp = lead & 0x07u;
    } else {
        *pos = (uint16_t)(i + 1);
        return 0xFFFDu;
    }
    if (i + step > len) {
        *pos = len;
        return 0xFFFDu;
    }
    for (uint16_t k = 1; k < step; ++k) {
        if ((p[i + k] & 0xC0u) != 0x80u) {
            *pos = (uint16_t)(i + 1);
            return 0xFFFDu;
        }
        cp = (cp << 6) | (p[i + k] & 0x3Fu);
    }
    *pos = (uint16_t)(i + step);
    return cp;
}

int clannad_text_pages(const char *utf8, int units_per_line, int lines_per_page,
                       uint16_t *offsets, int max_offsets)
{
    if (!utf8) utf8 = "";
    if (units_per_line < 2) units_per_line = 2;
    if (lines_per_page < 1) lines_per_page = 1;
    const uint16_t len = (uint16_t)strlen(utf8);
    int pages = 1;
    if (offsets && max_offsets > 0) offsets[0] = 0;
    int line_units = 0;
    int line_index = 0;
    uint16_t pos = 0;
    while (pos < len) {
        const uint16_t before = pos;
        const uint32_t cp = decode_at(utf8, len, &pos);
        if (cp == '\n') {
            line_units = 0;
            line_index++;
            if (line_index >= lines_per_page) {
                line_index = 0;
                pages++;
                if (offsets && pages - 1 < max_offsets) offsets[pages - 1] = pos;
            }
            continue;
        }
        const int units = codepoint_units(cp);
        if (line_units + units > units_per_line && line_units > 0) {
            if (no_line_start(cp)) {
                // 行首禁则：标点挂在上一行末尾，占位后换行。
                line_units = 0;
                line_index++;
                if (line_index >= lines_per_page) {
                    line_index = 0;
                    pages++;
                    if (offsets && pages - 1 < max_offsets) offsets[pages - 1] = pos;
                }
                continue;
            }
            line_units = units;
            line_index++;
            if (line_index >= lines_per_page) {
                line_index = 0;
                pages++;
                if (offsets && pages - 1 < max_offsets) offsets[pages - 1] = before;
            }
            continue;
        }
        line_units += units;
    }
    while (pages > 1 && offsets && pages - 1 < max_offsets &&
           offsets[pages - 1] == len) {
        pages--;
    }
    if (offsets && max_offsets > 0) {
        const int last = pages < max_offsets ? pages : max_offsets - 1;
        offsets[last] = len;
    }
    return pages;
}

void clannad_text_layout(const char *utf8, uint16_t start, uint16_t end, uint16_t reveal,
                         int units_per_line, char lines[CLAN_TEXT_LINES][160])
{
    if (!utf8) utf8 = "";
    if (units_per_line < 2) units_per_line = 2;
    if (end > (uint16_t)strlen(utf8)) end = (uint16_t)strlen(utf8);
    if (start > end) start = end;
    if (reveal > end) reveal = end;
    if (reveal < start) reveal = start;
    for (int i = 0; i < CLAN_TEXT_LINES; ++i) lines[i][0] = '\0';
    int line = 0;
    int units = 0;
    size_t out = 0;
    uint16_t pos = start;
    while (pos < reveal && line < CLAN_TEXT_LINES) {
        const uint16_t before = pos;
        const uint32_t cp = decode_at(utf8, end, &pos);
        if (pos > reveal) break;
        if (cp == '\n') {
            lines[line][out] = '\0';
            line++;
            out = 0;
            units = 0;
            continue;
        }
        const int w = codepoint_units(cp);
        if (units + w > units_per_line && units > 0) {
            if (!no_line_start(cp)) {
                lines[line][out] = '\0';
                line++;
                out = 0;
                units = 0;
            }
            if (line >= CLAN_TEXT_LINES) break;
        }
        const uint16_t n = (uint16_t)(pos - before);
        if (out + n >= 160) break;
        memcpy(lines[line] + out, utf8 + before, n);
        out += n;
        units += w;
        if (pos >= end) break;
    }
    if (line < CLAN_TEXT_LINES) lines[line][out] = '\0';
}
