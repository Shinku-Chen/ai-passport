// main/anemoi/anemoi_ui.c —— 界面模型与 UTF-8 分页（纯逻辑）。
#include "anemoi_ui.h"

#include <stdio.h>
#include <string.h>

void anem_view_clear(anem_view_t *view)
{
    if (view == NULL) {
        return;
    }
    memset(view, 0, sizeof(*view));
    view->art_image = ANEM_NONE16;
}

void anem_view_add_row(anem_view_t *view, int y, uint8_t kind, const char *text, bool selected,
                       bool enabled)
{
    if (view == NULL || view->row_count >= ANEM_ROW_MAX) {
        return;
    }
    anem_view_row_t *row = &view->row[view->row_count++];
    snprintf(row->text, sizeof(row->text), "%s", text ? text : "");
    row->x = 8;
    row->y = (int16_t)y;
    row->w = ANEM_VIEW_W - 16;
    row->h = kind == ANEM_VIEW_TEXT_HEADER ? 22 : 20;
    row->kind = kind;
    row->selected = selected;
    row->enabled = enabled;
}

static uint16_t utf8_char_len(uint8_t lead)
{
    if (lead < 0x80) {
        return 1;
    }
    if ((lead & 0xE0) == 0xC0) {
        return 2;
    }
    if ((lead & 0xF0) == 0xE0) {
        return 3;
    }
    if ((lead & 0xF8) == 0xF0) {
        return 4;
    }
    return 1;
}

uint16_t anem_utf8_length(const char *text)
{
    if (text == NULL) {
        return 0;
    }
    uint16_t count = 0;
    const uint8_t *p = (const uint8_t *)text;
    while (*p != 0) {
        p += utf8_char_len(*p);
        count++;
    }
    return count;
}

size_t anem_utf8_slice(const char *text, uint16_t from_char, uint16_t max_chars, char *out,
                       size_t cap)
{
    if (out == NULL || cap == 0) {
        return 0;
    }
    out[0] = 0;
    if (text == NULL) {
        return 0;
    }
    const uint8_t *p = (const uint8_t *)text;
    uint16_t index = 0;
    while (*p != 0 && index < from_char) {
        p += utf8_char_len(*p);
        index++;
    }
    size_t written = 0;
    while (*p != 0 && index < from_char + max_chars) {
        const uint16_t len = utf8_char_len(*p);
        if (written + len + 1 > cap) {
            break;
        }
        memcpy(out + written, p, len);
        written += len;
        p += len;
        index++;
    }
    out[written] = 0;
    return written;
}

uint16_t anem_text_pages(const char *text, uint16_t chars_per_line, uint16_t lines)
{
    const uint16_t total = anem_utf8_length(text);
    if (chars_per_line == 0 || lines == 0) {
        return 1;
    }
    const uint32_t per_page = (uint32_t)chars_per_line * lines;
    const uint32_t pages = (total + per_page - 1) / per_page;
    return (uint16_t)(pages == 0 ? 1 : pages);
}

uint8_t anem_text_page(const char *text, uint16_t chars_per_line, uint16_t lines, uint16_t page,
                       char out[ANEM_LINE_MAX][ANEM_LINE_TEXT])
{
    if (text == NULL || out == NULL || chars_per_line == 0 || lines == 0) {
        return 0;
    }
    const uint32_t per_page = (uint32_t)chars_per_line * lines;
    uint16_t from = (uint16_t)(page * per_page);
    uint8_t row = 0;
    for (; row < lines && row < ANEM_LINE_MAX; row++) {
        char chunk[ANEM_LINE_TEXT];
        anem_utf8_slice(text, (uint16_t)(from + row * chars_per_line), chars_per_line, chunk,
                        sizeof(chunk));
        snprintf(out[row], ANEM_LINE_TEXT, "%s", chunk);
        if (chunk[0] == 0) {
            break;
        }
    }
    return row;
}
