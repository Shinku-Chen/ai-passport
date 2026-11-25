// main/gal/gal_text.c —— 文本排版纯逻辑实现。
#include "gal_text.h"

#include <stdbool.h>
#include <string.h>

// 判断 UTF-8 字符是否为“半角”：ASCII 与半角片假名（U+FF61..U+FF9F）。其余按全角算。
static bool codepoint_half_width(uint32_t cp)
{
    if (cp < 0x80u) return true;
    if (cp >= 0xFF61u && cp <= 0xFF9Fu) return true;
    return false;
}

// 行首禁则：这些标点不允许出现在行首，遇到时把它拉回上一行。
static bool no_line_start(uint32_t cp)
{
    switch (cp) {
    case 0x3001:   // 、
    case 0x3002:   // 。
    case 0xFF0C:   // ，
    case 0xFF0E:   // ．
    case 0xFF01:   // ！
    case 0xFF1F:   // ？
    case 0xFF1A:   // ：
    case 0xFF1B:   // ；
    case 0xFF09:   // )
    case 0x300D:   // 」
    case 0x300F:   // 』
    case 0x3011:   // 】
    case 0x300B:   // 》
    case 0x3009:   // 〉
    case 0x2026:   // …
    case 0x2015:   // ―
    case 0x2014:   // —
    case 0x30FB:   // ・
    case 0xFF5D:   // }
    case 0x3015:   // 〕
        return true;
    default:
        return false;
    }
}

static uint32_t utf8_next(const char *s, size_t len, size_t *pos)
{
    const uint8_t *p = (const uint8_t *)s;
    const size_t i = *pos;
    if (i >= len) return 0;
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
        step = 1;   // 非法首字节：按单字节跳过，保证不会死循环
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

int gal_text_pages(const char *utf8, int units_per_line, int lines_per_page,
                   uint32_t *offsets, int max_offsets)
{
    if (!utf8) utf8 = "";
    if (units_per_line < 2) units_per_line = 2;
    if (lines_per_page < 1) lines_per_page = 1;

    const size_t len = strlen(utf8);
    int pages = 1;
    if (offsets && max_offsets > 0) offsets[0] = 0;

    int line_units = 0;
    int line_index = 0;   // 当前是第几行(从 0 起)
    size_t pos = 0;

    while (pos < len) {
        const size_t before = pos;
        const uint32_t cp = utf8_next(utf8, len, &pos);
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
                // 禁则标点：挂在本行行尾（允许轻微超宽），再换行。
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

    if (offsets && max_offsets > 0) {
        const int last = pages < max_offsets ? pages : max_offsets - 1;
        offsets[last] = (uint32_t)len;
    }
    return pages;
}

// Incremental page-boundary iterator.  It mirrors gal_text_pages' wrapping
// rules but never stores more than one page boundary, so long sentences are not
// limited by a fixed 16-page array.
typedef struct {
    const char *utf8;
    size_t len;
    size_t pos;
    int units;
    int lines;
    int line_units;
    int line_index;
} gal_text_page_iter_t;

static size_t gal_text_next_page(gal_text_page_iter_t *it)
{
    while (it->pos < it->len) {
        const size_t before = it->pos;
        const uint32_t cp = utf8_next(it->utf8, it->len, &it->pos);
        if (cp == '\n') {
            it->line_units = 0;
            it->line_index++;
            if (it->line_index >= it->lines) {
                it->line_index = 0;
                return it->pos;
            }
            continue;
        }
        const int units = codepoint_half_width(cp) ? 1 : 2;
        if (it->line_units + units > it->units && it->line_units > 0) {
            if (no_line_start(cp)) {
                it->line_units = 0;
                it->line_index++;
                if (it->line_index >= it->lines) {
                    it->line_index = 0;
                    return it->pos;
                }
                continue;
            }
            it->line_units = units;
            it->line_index++;
            if (it->line_index >= it->lines) {
                it->line_index = 0;
                return before;
            }
            continue;
        }
        it->line_units += units;
    }
    return it->len;
}

static void gal_text_iter_init(gal_text_page_iter_t *it, const char *utf8,
                               size_t start, int units_per_line, int lines_per_page)
{
    it->utf8 = utf8 ? utf8 : "";
    it->len = strlen(it->utf8);
    it->pos = start;
    it->units = units_per_line < 2 ? 2 : units_per_line;
    it->lines = lines_per_page < 1 ? 1 : lines_per_page;
    it->line_units = 0;
    it->line_index = 0;
}

bool gal_text_page_bounds(const char *utf8, int units_per_line, int lines_per_page,
                          int page, uint32_t *begin, uint32_t *end)
{
    if (page < 0) return false;
    const size_t len = utf8 ? strlen(utf8) : 0;
    size_t start = 0;
    for (int p = 0;; ++p) {
        gal_text_page_iter_t it;
        gal_text_iter_init(&it, utf8, start, units_per_line, lines_per_page);
        const size_t next = gal_text_next_page(&it);
        if (p == page) {
            if (start > len || next < start) return false;
            if (begin) *begin = (uint32_t)start;
            if (end) *end = (uint32_t)next;
            return true;
        }
        if (next >= len) return false;
        start = next;
    }
}

bool gal_text_page_count(const char *utf8, int units_per_line, int lines_per_page,
                         int *pages_out)
{
    const size_t len = utf8 ? strlen(utf8) : 0;
    int pages = 1;
    size_t start = 0;
    while (start < len) {
        gal_text_page_iter_t it;
        gal_text_iter_init(&it, utf8, start, units_per_line, lines_per_page);
        const size_t next = gal_text_next_page(&it);
        if (next >= len) break;
        if (next <= start) break;   // malformed text safety
        start = next;
        pages++;
    }
    if (pages_out) *pages_out = pages;
    return true;
}
