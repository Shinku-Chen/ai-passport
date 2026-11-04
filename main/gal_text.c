// main/gal_text.c —— UTF-8 分页与宽度计算。
#include "gal_text.h"

#include <string.h>

static uint32_t decode_cp(const char **pp)
{
    const unsigned char *p = (const unsigned char *)*pp;
    uint32_t cp;
    int n;
    if (p[0] < 0x80) { cp = p[0]; n = 1; }
    else if ((p[0] & 0xE0) == 0xC0) { cp = p[0] & 0x1F; n = 2; }
    else if ((p[0] & 0xF0) == 0xE0) { cp = p[0] & 0x0F; n = 3; }
    else if ((p[0] & 0xF8) == 0xF0) { cp = p[0] & 0x07; n = 4; }
    else { cp = p[0]; n = 1; }
    for (int i = 1; i < n; ++i) {
        if ((p[i] & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *pp += n;
    return cp;
}

static size_t cp_bytes(uint32_t cp, char *out)
{
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

int gal_text_cp_width(uint32_t cp, int font_px)
{
    if (cp == '\n' || cp == '\r' || cp == '\t') return 0;
    if (cp < 0x80) return font_px / 2;
    return font_px;
}

int gal_text_cp_count(const char *text)
{
    if (!text) return 0;
    const char *p = text;
    int n = 0;
    while (*p) {
        (void)decode_cp(&p);
        n++;
    }
    return n;
}

int gal_text_prefix(const char *text, int cps, char *out, size_t cap)
{
    if (!text || !out || cap == 0) return 0;
    const char *p = text;
    size_t at = 0;
    int n = 0;
    while (*p && n < cps) {
        const uint32_t cp = decode_cp(&p);
        char tmp[4];
        const size_t bytes = cp_bytes(cp, tmp);
        if (at + bytes + 1 > cap) break;
        memcpy(out + at, tmp, bytes);
        at += bytes;
        n++;
    }
    out[at] = 0;
    return n;
}

static void emit_bytes(int current_page, int target, char *out, size_t cap, size_t *at,
                       const char *bytes, size_t n)
{
    if (current_page != target || !out) return;
    if (*at + n + 1 > cap) return;
    memcpy(out + *at, bytes, n);
    *at += n;
}

bool gal_text_page(const char *text, int page, int width_px, int font_px, int max_lines,
                   char *out, size_t cap, int *page_count)
{
    return gal_text_page_ex(text, page, width_px, font_px, max_lines, out, cap,
                            page_count, NULL, NULL);
}

bool gal_text_page_ex(const char *text, int page, int width_px, int font_px, int max_lines,
                      char *out, size_t cap, int *page_count,
                      size_t *start_out, size_t *end_out)
{
    if (page_count) *page_count = 0;
    if (start_out) *start_out = 0;
    if (end_out) *end_out = 0;
    if (!text || !out || cap == 0 || page < 0 || width_px <= 0 || font_px <= 0 ||
        max_lines <= 0) {
        return false;
    }
    out[0] = 0;
    const char *base = text;
    int current = 0;
    int pages = 0;
    int lines = 0;
    int line_w = 0;
    bool page_started = false;
    size_t at = 0;
    size_t page_start = 0;
    size_t range_start = 0;
    size_t range_end = 0;
    const char *p = text;
    while (*p) {
        if (*p == '\r') { p++; continue; }
        if (*p == '\n') {
            p++;
            line_w = 0;
            lines++;
            page_started = true;
            if (lines >= max_lines) {
                const size_t boundary = (size_t)(p - base);
                if (current == page) {
                    range_end = boundary;
                    if (start_out) *start_out = range_start;
                    if (end_out) *end_out = range_end;
                }
                pages++;
                current++;
                lines = 0;
                page_started = false;
                page_start = boundary;
                if (current == page) {
                    range_start = page_start;
                    if (start_out) *start_out = range_start;
                }
            } else {
                emit_bytes(current, page, out, cap, &at, "\n", 1);
            }
            continue;
        }
        const char *cp_start = p;
        const uint32_t cp = decode_cp(&p);
        const int w = gal_text_cp_width(cp, font_px);
        if (line_w > 0 && line_w + w > width_px) {
            const size_t boundary = (size_t)(cp_start - base);
            if (current == page) {
                range_end = boundary;
                if (start_out) *start_out = range_start;
                if (end_out) *end_out = range_end;
            }
            line_w = 0;
            lines++;
            if (lines >= max_lines) {
                pages++;
                current++;
                lines = 0;
                page_started = false;
                page_start = boundary;
                if (current == page) {
                    range_start = page_start;
                    if (start_out) *start_out = range_start;
                }
            } else {
                emit_bytes(current, page, out, cap, &at, "\n", 1);
            }
        }
        char tmp[4];
        const size_t n = cp_bytes(cp, tmp);
        emit_bytes(current, page, out, cap, &at, tmp, n);
        page_started = true;
        line_w += w;
    }
    if (page_started || pages == 0) pages++;
    if (pages == 0) pages = 1;
    if (current == page) {
        range_start = page_start;
        range_end = (size_t)(p - base);
        if (start_out) *start_out = range_start;
        if (end_out) *end_out = range_end;
    }
    out[at < cap ? at : cap - 1] = 0;
    if (page_count) *page_count = pages;
    return page < pages;
}

int gal_text_page_count(const char *text, int width_px, int font_px, int max_lines)
{
    int pages = 0;
    char dummy[8];
    (void)gal_text_page(text, 0, width_px, font_px, max_lines, dummy, sizeof(dummy), &pages);
    return pages;
}
