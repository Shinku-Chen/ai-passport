// main/summer_text.c —— UTF-8 分页与行首禁则。
#include "summer_text.h"

#include <string.h>

const summer_layout_t SUMMER_LAYOUT_16 = { .units_per_line = 36, .lines_per_page = 3 };
const summer_layout_t SUMMER_LAYOUT_20 = { .units_per_line = 28, .lines_per_page = 2 };

static uint32_t decode_cp(const char *s, size_t len, size_t pos, size_t *next)
{
    const unsigned char *p = (const unsigned char *)s + pos;
    const size_t left = len - pos;
    uint32_t cp = 0;
    size_t n = 1;
    if (p[0] < 0x80) {
        cp = p[0];
    } else if ((p[0] & 0xE0u) == 0xC0u && left >= 2) {
        cp = ((uint32_t)(p[0] & 0x1Fu) << 6) | (p[1] & 0x3Fu);
        n = 2;
    } else if ((p[0] & 0xF0u) == 0xE0u && left >= 3) {
        cp = ((uint32_t)(p[0] & 0x0Fu) << 12) | ((uint32_t)(p[1] & 0x3Fu) << 6) |
             (p[2] & 0x3Fu);
        n = 3;
    } else if ((p[0] & 0xF8u) == 0xF0u && left >= 4) {
        cp = ((uint32_t)(p[0] & 0x07u) << 18) | ((uint32_t)(p[1] & 0x3Fu) << 12) |
             ((uint32_t)(p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu);
        n = 4;
    } else {
        cp = '?';
    }
    if (next) *next = pos + n;
    return cp;
}

size_t summer_utf8_next(const char *utf8, size_t len, size_t pos)
{
    if (!utf8 || pos >= len) return len;
    size_t next = pos;
    (void)decode_cp(utf8, len, pos, &next);
    return next;
}

size_t summer_utf8_char_len(const char *utf8, size_t len, size_t pos)
{
    if (!utf8 || pos >= len) return 0;
    const size_t next = summer_utf8_next(utf8, len, pos);
    return next - pos;
}

static int cp_units(uint32_t cp)
{
    return cp < 0x80 ? 1 : 2;
}

static bool closing_punct(uint32_t cp)
{
    switch (cp) {
    case 0x3001: // 、
    case 0x3002: // 。
    case 0xFF0C: // ，
    case 0xFF0E: // ．
    case 0xFF01: // ！
    case 0xFF1F: // ？
    case 0xFF1A: // ：
    case 0xFF1B: // ；
    case 0x300D: // 」
    case 0x300F: // 』
    case 0xFF09: // ）
    case 0x3011: // 】
    case 0x300B: // 》
    case 0x3009: // 〉
    case 0x2026: // …
    case 0x30FC: // ー
    case 0x301C: // 〜
    case 0xFF5E: // ～
        return true;
    default:
        return false;
    }
}

// 计算一页：返回页结束字节偏移，行数/换行位置可选输出。
static size_t layout_page(const char *utf8, size_t len, const summer_layout_t *layout,
                          uint32_t *line_breaks, int *line_count, int max_breaks)
{
    if (!utf8 || len == 0) {
        if (line_count) *line_count = 0;
        return 0;
    }
    const int per_line = layout->units_per_line > 0 ? layout->units_per_line : 36;
    const int per_page = layout->lines_per_page > 0 ? layout->lines_per_page : 3;
    int lines = 1;
    int units = 0;
    int breaks = 0;
    size_t pos = 0;
    size_t last_break = 0;
    while (pos < len) {
        size_t next = pos;
        const uint32_t cp = decode_cp(utf8, len, pos, &next);
        const int u = cp_units(cp);
        if (cp == '\n') {
            if (breaks < max_breaks && line_breaks) line_breaks[breaks] = (uint32_t)next;
            breaks++;
            lines++;
            units = 0;
            last_break = next;
            pos = next;
            if (lines > per_page) {
                // The newline belongs to the previous page.
                if (line_count) *line_count = per_page;
                return pos;
            }
            continue;
        }
        if (units + u > per_line && units > 0) {
            if (closing_punct(cp)) {
                // Keep the closing punctuation on the current line.
                units += u;
                pos = next;
                if (breaks < max_breaks && line_breaks) line_breaks[breaks] = (uint32_t)pos;
                breaks++;
                lines++;
                units = 0;
                last_break = pos;
                if (lines > per_page) {
                    if (line_count) *line_count = per_page;
                    return pos;
                }
                continue;
            }
            if (breaks < max_breaks && line_breaks) line_breaks[breaks] = (uint32_t)pos;
            breaks++;
            lines++;
            units = 0;
            last_break = pos;
            if (lines > per_page) {
                if (line_count) *line_count = per_page;
                return pos;
            }
        }
        units += u;
        pos = next;
    }
    if (line_count) *line_count = lines > per_page ? per_page : lines;
    (void)last_break;
    return pos;
}

int summer_text_pages(const char *utf8, const summer_layout_t *layout,
                      uint32_t *offsets, int max_offsets)
{
    if (!offsets || max_offsets <= 0) return 0;
    if (!utf8) utf8 = "";
    const size_t len = strlen(utf8);
    offsets[0] = 0;
    int pages = 0;
    size_t pos = 0;
    while (pos < len && pages + 1 < max_offsets) {
        const size_t end = layout_page(utf8 + pos, len - pos, layout, NULL, NULL, 0);
        if (end == 0) break;
        pos += end;
        pages++;
        offsets[pages] = (uint32_t)pos;
    }
    if (pages == 0) {
        pages = 1;
        offsets[1] = (uint32_t)len;
    }
    return pages;
}

bool summer_text_page_text(const char *utf8, const summer_layout_t *layout, int page,
                           uint32_t *out_start, uint32_t *out_end, char *out, size_t cap)
{
    if (!utf8 || !out || cap == 0 || page < 0) return false;
    const size_t len = strlen(utf8);
    size_t pos = 0;
    for (int p = 0; p <= page; ++p) {
        if (pos >= len && p > 0) return false;
        uint32_t breaks[8];
        int lines = 0;
        const size_t end = layout_page(utf8 + pos, len - pos, layout, breaks, &lines, 8);
        if (end == 0) return false;
        if (p == page) {
            if (out_start) *out_start = (uint32_t)pos;
            if (out_end) *out_end = (uint32_t)(pos + end);
            size_t used = 0;
            int bi = 0;
            for (size_t i = 0; i < end; ++i) {
                if (bi < lines - 1 && bi < 8 && breaks[bi] == (uint32_t)i) {
                    if (!(i > 0 && utf8[pos + i - 1] == '\n')) {
                        if (used + 1 < cap) out[used++] = '\n';
                    }
                    bi++;
                }
                if (used + 1 < cap) out[used++] = utf8[pos + i];
                else break;
            }
            // Trim a trailing newline (it ends the page; the label does not need it).
            if (used > 0 && out[used - 1] == '\n') used--;
            out[used < cap ? used : cap - 1] = 0;
            return true;
        }
        pos += end;
    }
    return false;
}
