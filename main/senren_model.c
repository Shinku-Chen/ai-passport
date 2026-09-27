// main/senren_model.c —— 剧本包解析 + 阅读状态机实现。
//
// 本文件不接触 ESP-IDF(只用标准 C 与 zlib 的 raw inflate),宿主机的单元测试
// 可以直接用真实剧本包跑完整条线路。
#include "senren_model.h"

#include "senren_inflate.h"

#include <string.h>

#define SENREN_SCN_HEADER_SIZE 20u
#define SENREN_SCN_SECTION_SIZE 16u
#define SENREN_SCN_CHUNK_RECORD 16u
// 一页最多切几段(单句最长 60 字 -> 每行 13 字 5 行,最多 2 页;留余量)
#define SENREN_PAGE_MAX 8
// 单个 NEXT 里最多几个条件(实测最多 4 个)
#define SENREN_COND_MAX 8

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// --------------------------------------------------------------------------
// UTF-8 与排版
// --------------------------------------------------------------------------

size_t senren_utf8_next_boundary(const char *utf8, size_t len, size_t pos)
{
    if (utf8 == NULL || pos >= len) {
        return len;
    }
    size_t next = pos + 1;
    while (next < len && ((uint8_t)utf8[next] & 0xC0) == 0x80) {
        next++;
    }
    return next;
}

uint32_t senren_utf8_decode(const char *utf8, size_t len, size_t pos, size_t *next_out)
{
    if (utf8 == NULL || pos >= len) {
        if (next_out != NULL) {
            *next_out = len;
        }
        return 0;
    }
    const uint8_t *bytes = (const uint8_t *)utf8;
    uint8_t lead = bytes[pos];
    size_t next = senren_utf8_next_boundary(utf8, len, pos);
    uint32_t codepoint = lead;
    if ((lead & 0xE0) == 0xC0 && next - pos == 2) {
        codepoint = ((uint32_t)(lead & 0x1F) << 6) | (bytes[pos + 1] & 0x3F);
    } else if ((lead & 0xF0) == 0xE0 && next - pos == 3) {
        codepoint = ((uint32_t)(lead & 0x0F) << 12) | ((uint32_t)(bytes[pos + 1] & 0x3F) << 6) |
                    (bytes[pos + 2] & 0x3F);
    } else if ((lead & 0xF8) == 0xF0 && next - pos == 4) {
        codepoint = ((uint32_t)(lead & 0x07) << 18) | ((uint32_t)(bytes[pos + 1] & 0x3F) << 12) |
                    ((uint32_t)(bytes[pos + 2] & 0x3F) << 6) | (bytes[pos + 3] & 0x3F);
    }
    if (next_out != NULL) {
        *next_out = next;
    }
    return codepoint;
}

int senren_char_units(uint32_t codepoint)
{
    if (codepoint < 0x0080) {
        return 1;
    }
    if (codepoint < 0x1100) {
        return 1;
    }
    // 全角形式、CJK 标点、假名、汉字、通用标点都按两格宽算
    if (codepoint >= 0xFF01 && codepoint <= 0xFF60) {
        return 2;
    }
    if (codepoint >= 0xFFE0 && codepoint <= 0xFFE6) {
        return 2;
    }
    if (codepoint >= 0x2000 && codepoint <= 0x206F) {
        return 2;
    }
    if (codepoint >= 0x2190 && codepoint <= 0x21FF) {
        return 2;
    }
    if (codepoint >= 0x2460 && codepoint <= 0x24FF) {
        return 2;
    }
    if (codepoint >= 0x25A0 && codepoint <= 0x25FF) {
        return 2;
    }
    if (codepoint >= 0x3000 && codepoint <= 0x303F) {
        return 2;
    }
    if (codepoint >= 0x3040 && codepoint <= 0x30FF) {
        return 2;
    }
    if (codepoint >= 0x3400 && codepoint <= 0x4DBF) {
        return 2;
    }
    if (codepoint >= 0x4E00 && codepoint <= 0x9FFF) {
        return 2;
    }
    if (codepoint >= 0xF900 && codepoint <= 0xFAFF) {
        return 2;
    }
    return 1;
}

// 不能出现在行首的字符(标点与右括号);遇到时把断点往前挪一格
static bool no_line_start(uint32_t codepoint)
{
    switch (codepoint) {
    case 0x3001:  // 、
    case 0x3002:  // 。
    case 0xFF0C:  // ，
    case 0xFF0E:  // ．
    case 0xFF01:  // !
    case 0xFF1F:  // ?
    case 0x300D:  // 」
    case 0x300F:  // 』
    case 0x3011:  // 】
    case 0x3015:  // 〗
    case 0xFF09:  // )
    case 0x3009:  // 〉
    case 0x300B:  // 》
    case 0x2026:  // …
    case 0x301C:  // 〜
    case 0xFF5E:  // ～
    case 0x30FC:  // ー(长音,避免整段跳行)
        return true;
    default:
        return false;
    }
}

int senren_text_lines(const char *utf8, int units_per_line, uint32_t *offsets, int max_lines)
{
    if (utf8 == NULL || offsets == NULL || max_lines <= 0 || units_per_line <= 0) {
        return 0;
    }
    size_t len = strlen(utf8);
    size_t pos = 0;
    int line = 0;
    while (pos < len && line < max_lines) {
        offsets[line++] = (uint32_t)pos;
        int units = 0;
        size_t scan = pos;
        size_t last_fit = pos;   // 上一个放得下的字符边界
        while (scan < len) {
            size_t next = 0;
            uint32_t codepoint = senren_utf8_decode(utf8, len, scan, &next);
            int width = senren_char_units(codepoint);
            if (units + width > units_per_line) {
                break;
            }
            units += width;
            last_fit = scan;
            scan = next;
        }
        if (scan >= len) {
            pos = len;
            break;
        }
        // 断点恰好压在禁则行首字符上,而本行还有别的字符 -> 少放一个
        size_t next = 0;
        uint32_t head = senren_utf8_decode(utf8, len, scan, &next);
        size_t break_at = scan;
        if (no_line_start(head) && last_fit > pos) {
            break_at = last_fit;
        }
        if (break_at <= pos) {
            break_at = next > pos ? next : len;   // 一个字符都放不下时至少前进一格,避免死循环
        }
        pos = break_at;
    }
    return line;
}

int senren_text_pages(const char *utf8, int units_per_line, int lines_per_page, uint32_t *offsets,
                      int max_offsets)
{
    if (utf8 == NULL || offsets == NULL || max_offsets <= 0 || lines_per_page <= 0) {
        return 0;
    }
    size_t len = strlen(utf8);
    uint32_t line_offsets[64];
    int lines = senren_text_lines(utf8, units_per_line, line_offsets, 64);
    int pages = 0;
    for (int line = 0; line < lines && pages < max_offsets; line += lines_per_page) {
        offsets[pages++] = line_offsets[line];
    }
    if (pages == 0) {
        offsets[0] = 0;
        pages = len > 0 ? 1 : 0;
    }
    return pages;
}

// --------------------------------------------------------------------------
// 剧本包解析
// --------------------------------------------------------------------------

bool senren_scn_open(senren_scn_t *scn, const uint8_t *data, uint32_t size)
{
    if (scn == NULL || data == NULL || size < SENREN_SCN_HEADER_SIZE) {
        return false;
    }
    if (memcmp(data, SENREN_SCN_MAGIC, 8) != 0) {
        return false;
    }
    uint16_t version = rd16(data + 8);
    uint16_t header_size = rd16(data + 10);
    uint16_t section_count = rd16(data + 12);
    uint32_t total = rd32(data + 16);
    if (version != SENREN_SCN_VERSION || total != size) {
        return false;
    }
    if (header_size < SENREN_SCN_HEADER_SIZE ||
        (uint64_t)header_size + (uint64_t)section_count * SENREN_SCN_SECTION_SIZE > size) {
        return false;
    }

    senren_scn_t view;
    memset(&view, 0, sizeof(view));
    view.blob = data;
    view.blob_size = size;
    for (uint16_t index = 0; index < section_count; index++) {
        const uint8_t *section = data + header_size + (uint32_t)index * SENREN_SCN_SECTION_SIZE;
        uint32_t type = rd32(section);
        uint32_t offset = rd32(section + 4);
        uint32_t count = rd32(section + 8);
        uint32_t length = rd32(section + 12);
        if (offset > size || length > size - offset) {
            return false;
        }
        switch (type) {
        case SENREN_SCN_SEC_CHAR:
            view.chars = data + offset;
            view.char_count = count;
            break;
        case SENREN_SCN_SEC_NAME:
            view.names = data + offset;
            view.names_size = length;
            break;
        case SENREN_SCN_SEC_FLAG:
            view.flag_pages = data + offset;
            view.flag_count = count;
            break;
        case SENREN_SCN_SEC_CHUNK:
            view.chunks = data + offset;
            view.chunks_size = length;
            view.chunk_count = count;
            break;
        case SENREN_SCN_SEC_BLOB:
            view.payload = data + offset;
            view.payload_size = length;
            break;
        case SENREN_SCN_SEC_META:
            view.meta = data + offset;
            view.meta_size = length;
            break;
        default:
            break;
        }
    }
    if (view.chars == NULL || view.names == NULL || view.chunks == NULL || view.payload == NULL) {
        return false;
    }
    if (view.char_count == 0 || view.chunk_count == 0 ||
        (uint64_t)view.chunk_count * SENREN_SCN_CHUNK_RECORD > view.chunks_size) {
        return false;
    }

    // 字典块连排:说话人 / 立绘键 / 事件图 / 背景 / 结局(最后一块可缺)
    const uint8_t *cursor = view.names;
    const uint8_t *names_end = view.names + view.names_size;
    const uint8_t **tables[5] = { &view.speakers, &view.sprite_keys, &view.event_names,
                                  &view.bg_names, &view.endings };
    uint32_t *counts[5] = { &view.speaker_count, &view.sprite_count, &view.event_count,
                            &view.bg_count, &view.ending_count };
    for (int block = 0; block < 5; block++) {
        if (cursor + 2 > names_end) {
            break;
        }
        uint16_t entries = rd16(cursor);
        cursor += 2;
        *tables[block] = cursor;
        *counts[block] = entries;
        for (uint16_t entry = 0; entry < entries; entry++) {
            if (cursor + 2 > names_end) {
                return false;
            }
            uint16_t length = rd16(cursor);
            cursor += 2 + (uint32_t)length * 2;
            if (cursor > names_end) {
                return false;
            }
        }
    }

    *scn = view;
    return true;
}

uint32_t senren_scn_chunk(const senren_scn_t *scn, uint16_t index, uint8_t *raw, uint32_t capacity)
{
    if (scn == NULL || raw == NULL || index >= scn->chunk_count) {
        return 0;
    }
    const uint8_t *record = scn->chunks + (uint32_t)index * SENREN_SCN_CHUNK_RECORD;
    uint32_t offset = rd32(record);
    uint32_t length = rd32(record + 4);
    uint32_t raw_len = rd32(record + 8);
    if (offset > scn->payload_size || length > scn->payload_size - offset || raw_len > capacity) {
        return 0;
    }
    // ROM 里的 tinfl 解 zlib 流(见 senren_inflate.c)
    uint32_t produced = senren_inflate(scn->payload + offset, length, raw, capacity);
    return produced == raw_len ? produced : 0;
}

// 字典项里的 u16 是字符表下标,要借字符表才能还原成 UTF-8
static size_t name_from_chars(const senren_scn_t *scn, const uint8_t *table, uint32_t count,
                              uint16_t index, char *out, size_t capacity)
{
    if (out == NULL || capacity == 0) {
        return 0;
    }
    out[0] = '\0';
    if (scn == NULL || table == NULL || index >= count) {
        return 0;
    }
    const uint8_t *cursor = table;
    for (uint16_t entry = 0; entry < index; entry++) {
        uint16_t length = rd16(cursor);
        cursor += 2 + (uint32_t)length * 2;
    }
    uint16_t length = rd16(cursor);
    cursor += 2;
    size_t written = 0;
    for (uint16_t position = 0; position < length; position++) {
        uint16_t code = rd16(cursor + (uint32_t)position * 2);
        if (code >= scn->char_count) {
            continue;
        }
        uint32_t codepoint = rd16(scn->chars + (uint32_t)code * 2);
        uint8_t encoded[4];
        size_t encoded_len;
        if (codepoint < 0x80) {
            encoded[0] = (uint8_t)codepoint;
            encoded_len = 1;
        } else if (codepoint < 0x800) {
            encoded[0] = (uint8_t)(0xC0 | (codepoint >> 6));
            encoded[1] = (uint8_t)(0x80 | (codepoint & 0x3F));
            encoded_len = 2;
        } else if (codepoint < 0x10000) {
            encoded[0] = (uint8_t)(0xE0 | (codepoint >> 12));
            encoded[1] = (uint8_t)(0x80 | ((codepoint >> 6) & 0x3F));
            encoded[2] = (uint8_t)(0x80 | (codepoint & 0x3F));
            encoded_len = 3;
        } else {
            encoded[0] = (uint8_t)(0xF0 | (codepoint >> 18));
            encoded[1] = (uint8_t)(0x80 | ((codepoint >> 12) & 0x3F));
            encoded[2] = (uint8_t)(0x80 | ((codepoint >> 6) & 0x3F));
            encoded[3] = (uint8_t)(0x80 | (codepoint & 0x3F));
            encoded_len = 4;
        }
        if (written + encoded_len + 1 > capacity) {
            break;
        }
        memcpy(out + written, encoded, encoded_len);
        written += encoded_len;
    }
    out[written] = '\0';
    return written;
}

size_t senren_scn_speaker(const senren_scn_t *scn, uint16_t index, char *out, size_t capacity)
{
    return name_from_chars(scn, scn ? scn->speakers : NULL, scn ? scn->speaker_count : 0, index, out,
                           capacity);
}

size_t senren_scn_sprite_key(const senren_scn_t *scn, uint16_t index, char *out, size_t capacity)
{
    return name_from_chars(scn, scn ? scn->sprite_keys : NULL, scn ? scn->sprite_count : 0, index, out,
                           capacity);
}

size_t senren_scn_event(const senren_scn_t *scn, uint16_t index, char *out, size_t capacity)
{
    return name_from_chars(scn, scn ? scn->event_names : NULL, scn ? scn->event_count : 0, index, out,
                           capacity);
}

size_t senren_scn_background(const senren_scn_t *scn, uint16_t index, char *out, size_t capacity)
{
    return name_from_chars(scn, scn ? scn->bg_names : NULL, scn ? scn->bg_count : 0, index, out,
                           capacity);
}

size_t senren_scn_ending(const senren_scn_t *scn, uint16_t index, char *out, size_t capacity)
{
    return name_from_chars(scn, scn ? scn->endings : NULL, scn ? scn->ending_count : 0, index, out,
                           capacity);
}

// --------------------------------------------------------------------------
// 节点读写
// --------------------------------------------------------------------------

typedef struct {
    const uint8_t *p;
    const uint8_t *end;
} cursor_t;

static bool cursor_u8(cursor_t *cursor, uint8_t *value)
{
    if (cursor->p + 1 > cursor->end) {
        return false;
    }
    *value = *cursor->p++;
    return true;
}

static bool cursor_u16(cursor_t *cursor, uint16_t *value)
{
    if (cursor->p + 2 > cursor->end) {
        return false;
    }
    *value = rd16(cursor->p);
    cursor->p += 2;
    return true;
}

static bool cursor_u32(cursor_t *cursor, uint32_t *value)
{
    if (cursor->p + 4 > cursor->end) {
        return false;
    }
    *value = rd32(cursor->p);
    cursor->p += 4;
    return true;
}

static bool cursor_skip_text(cursor_t *cursor)
{
    uint16_t length = 0;
    if (!cursor_u16(cursor, &length)) {
        return false;
    }
    if (cursor->p + (uint32_t)length * 2 > cursor->end) {
        return false;
    }
    cursor->p += (uint32_t)length * 2;
    return true;
}

// 文本(字符表下标序列)-> UTF-8
static bool cursor_text(cursor_t *cursor, const senren_scn_t *scn, char *out, size_t capacity,
                        size_t *written_out)
{
    uint16_t length = 0;
    if (!cursor_u16(cursor, &length)) {
        return false;
    }
    if (cursor->p + (uint32_t)length * 2 > cursor->end) {
        return false;
    }
    size_t written = 0;
    if (out != NULL && capacity > 0) {
        out[0] = '\0';
    }
    for (uint16_t position = 0; position < length; position++) {
        uint16_t code = rd16(cursor->p + (uint32_t)position * 2);
        if (scn == NULL || code >= scn->char_count) {
            continue;
        }
        uint32_t codepoint = rd16(scn->chars + (uint32_t)code * 2);
        uint8_t encoded[4];
        size_t encoded_len;
        if (codepoint < 0x80) {
            encoded[0] = (uint8_t)codepoint;
            encoded_len = 1;
        } else if (codepoint < 0x800) {
            encoded[0] = (uint8_t)(0xC0 | (codepoint >> 6));
            encoded[1] = (uint8_t)(0x80 | (codepoint & 0x3F));
            encoded_len = 2;
        } else if (codepoint < 0x10000) {
            encoded[0] = (uint8_t)(0xE0 | (codepoint >> 12));
            encoded[1] = (uint8_t)(0x80 | ((codepoint >> 6) & 0x3F));
            encoded[2] = (uint8_t)(0x80 | (codepoint & 0x3F));
            encoded_len = 3;
        } else {
            encoded[0] = (uint8_t)(0xF0 | (codepoint >> 18));
            encoded[1] = (uint8_t)(0x80 | ((codepoint >> 12) & 0x3F));
            encoded[2] = (uint8_t)(0x80 | ((codepoint >> 6) & 0x3F));
            encoded[3] = (uint8_t)(0x80 | (codepoint & 0x3F));
            encoded_len = 4;
        }
        if (out != NULL && written + encoded_len + 1 <= capacity) {
            memcpy(out + written, encoded, encoded_len);
            written += encoded_len;
        }
    }
    if (out != NULL && capacity > 0) {
        out[written < capacity ? written : capacity - 1] = '\0';
    }
    if (written_out != NULL) {
        *written_out = written;
    }
    cursor->p += (uint32_t)length * 2;
    return true;
}

// 跳过一条记录(不解码);解析失败返回 false
static bool node_skip(cursor_t *cursor)
{
    uint8_t kind = 0;
    if (!cursor_u8(cursor, &kind)) {
        return false;
    }
    switch (kind) {
    case SENREN_NODE_LABEL: {
        uint32_t page = 0;
        return cursor_u32(cursor, &page);
    }
    case SENREN_NODE_CHAPTER:
        return cursor_skip_text(cursor);
    case SENREN_NODE_BG: {
        uint8_t index = 0;
        return cursor_u8(cursor, &index);
    }
    case SENREN_NODE_DIALOGUE: {
        uint8_t speaker = 0;
        uint8_t action = 0;
        uint16_t sprite = 0;
        if (!cursor_u8(cursor, &speaker) || !cursor_skip_text(cursor) ||
            !cursor_u8(cursor, &action) || !cursor_u16(cursor, &sprite)) {
            return false;
        }
        return true;
    }
    case SENREN_NODE_EV: {
        uint16_t index = 0;
        return cursor_u16(cursor, &index);
    }
    case SENREN_NODE_SPRITE_OFF:
        return true;
    case SENREN_NODE_SELECT: {
        uint8_t options = 0;
        if (!cursor_u8(cursor, &options)) {
            return false;
        }
        for (uint8_t option = 0; option < options; option++) {
            uint8_t target_kind = 0;
            uint16_t target_chunk = 0;
            uint32_t target_page = 0;
            uint8_t flag = 0;
            uint8_t value = 0;
            if (!cursor_skip_text(cursor) || !cursor_u8(cursor, &target_kind) ||
                !cursor_u16(cursor, &target_chunk) || !cursor_u32(cursor, &target_page) ||
                !cursor_u8(cursor, &flag) || !cursor_u8(cursor, &value)) {
                return false;
            }
        }
        return true;
    }
    case SENREN_NODE_NEXT: {
        uint8_t target_kind = 0;
        uint16_t target_chunk = 0;
        uint32_t target_page = 0;
        uint8_t conditions = 0;
        if (!cursor_u8(cursor, &target_kind) || !cursor_u16(cursor, &target_chunk) ||
            !cursor_u32(cursor, &target_page) || !cursor_u8(cursor, &conditions)) {
            return false;
        }
        for (uint8_t condition = 0; condition < conditions; condition++) {
            uint8_t flag = 0;
            uint8_t value = 0;
            if (!cursor_u8(cursor, &flag) || !cursor_u8(cursor, &value)) {
                return false;
            }
        }
        return true;
    }
    default:
        return false;
    }
}

// 把游标推到第 index 条记录处
static bool seek_node(cursor_t *cursor, uint32_t index)
{
    for (uint32_t step = 0; step < index; step++) {
        if (!node_skip(cursor)) {
            return false;
        }
    }
    return true;
}

// --------------------------------------------------------------------------
// 状态机内部
// --------------------------------------------------------------------------

static bool player_load_chunk(senren_player_t *player, const senren_scn_t *scn, uint16_t chunk)
{
    if (player->loaded_chunk == chunk && player->loaded_len > 0) {
        return true;
    }
    uint32_t length = senren_scn_chunk(scn, chunk, player->raw, SENREN_CHUNK_RAW_MAX);
    if (length == 0) {
        return false;
    }
    player->loaded_chunk = chunk;
    player->loaded_len = length;
    return true;
}

static uint32_t chunk_node_count(const senren_player_t *player)
{
    return player->loaded_len >= 2 ? rd16(player->raw) : 0;
}

static void player_clear_sentence(senren_player_t *player)
{
    player->speaker[0] = '\0';
    player->text[0] = '\0';
    player->page[0] = '\0';
    player->page_index = 0;
    player->page_count = 0;
}

// 按排版参数把 player->text 切页,并把第 0 页放进 player->page
static void player_paginate(senren_player_t *player, const senren_layout_t *layout)
{
    uint16_t units = layout != NULL ? layout->units_per_line : 26;
    uint16_t lines = layout != NULL ? layout->lines_per_page : 5;
    int pages = senren_text_pages(player->text, units, lines, player->page_offsets, SENREN_PAGE_MAX);
    player->page_count = pages > 0 ? (uint16_t)pages : 0;
    player->page_index = 0;
    if (pages <= 0) {
        player->page[0] = '\0';
        return;
    }
    size_t start = player->page_offsets[0];
    size_t end = pages > 1 ? player->page_offsets[1] : strlen(player->text);
    if (end - start + 1 > sizeof(player->page)) {
        end = start + sizeof(player->page) - 1;
    }
    memcpy(player->page, player->text + start, end - start);
    player->page[end - start] = '\0';
}

bool senren_player_next_page(senren_player_t *player)
{
    if (player == NULL || player->page_index + 1 >= player->page_count) {
        return false;
    }
    player->page_index++;
    size_t start = player->page_offsets[player->page_index];
    size_t end = player->page_index + 1 < player->page_count ? player->page_offsets[player->page_index + 1]
                                                             : strlen(player->text);
    if (end - start + 1 > sizeof(player->page)) {
        end = start + sizeof(player->page) - 1;
    }
    memcpy(player->page, player->text + start, end - start);
    player->page[end - start] = '\0';
    return true;
}

// 在当前块里找页号对应的 LABEL 记录
static bool find_label(senren_player_t *player, const senren_scn_t *scn, uint16_t chunk, uint32_t page,
                       uint32_t *node_out)
{
    if (!player_load_chunk(player, scn, chunk)) {
        return false;
    }
    uint32_t count = chunk_node_count(player);
    cursor_t cursor = { player->raw + 2, player->raw + player->loaded_len };
    for (uint32_t index = 0; index < count; index++) {
        if (cursor.p >= cursor.end) {
            return false;
        }
        if (*cursor.p == SENREN_NODE_LABEL) {
            cursor_t probe = cursor;
            probe.p++;
            uint32_t candidate = 0;
            if (cursor_u32(&probe, &candidate) && candidate == page) {
                *node_out = index;
                return true;
            }
        }
        if (!node_skip(&cursor)) {
            return false;
        }
    }
    return false;
}

static void player_goto(senren_player_t *player, uint16_t chunk, uint32_t node)
{
    player->chunk = chunk;
    player->node = node;
}

// 执行一条记录;返回 true 表示产生了可见步骤(step_out 有效)
typedef struct {
    uint8_t kind;
    uint32_t page;
    uint8_t speaker;
    uint16_t sprite;
    uint8_t action;
    uint8_t bg;
    uint16_t event;
    uint8_t option_count;
    struct {
        uint8_t target_kind;
        uint16_t target_chunk;
        uint32_t target_page;
        uint8_t flag;
        uint8_t value;
    } option[SENREN_CHOICE_MAX];
    char option_text[SENREN_CHOICE_MAX][SENREN_NAME_MAX];
    uint8_t target_kind;
    uint16_t target_chunk;
    uint32_t target_page;
    uint8_t condition_count;
    struct {
        uint8_t flag;
        uint8_t value;
    } condition[SENREN_COND_MAX];
} record_t;

static bool record_read(cursor_t *cursor, const senren_scn_t *scn, record_t *record, char *text,
                        size_t text_capacity)
{
    memset(record, 0, sizeof(*record));
    if (!cursor_u8(cursor, &record->kind)) {
        return false;
    }
    switch (record->kind) {
    case SENREN_NODE_LABEL:
        return cursor_u32(cursor, &record->page);
    case SENREN_NODE_CHAPTER:
        return cursor_text(cursor, scn, text, text_capacity, NULL);
    case SENREN_NODE_BG:
        return cursor_u8(cursor, &record->bg);
    case SENREN_NODE_DIALOGUE:
        return cursor_u8(cursor, &record->speaker) && cursor_text(cursor, scn, text, text_capacity, NULL) &&
               cursor_u8(cursor, &record->action) && cursor_u16(cursor, &record->sprite);
    case SENREN_NODE_EV:
        return cursor_u16(cursor, &record->event);
    case SENREN_NODE_SPRITE_OFF:
        return true;
    case SENREN_NODE_SELECT: {
        if (!cursor_u8(cursor, &record->option_count)) {
            return false;
        }
        if (record->option_count > SENREN_CHOICE_MAX) {
            return false;
        }
        for (uint8_t option = 0; option < record->option_count; option++) {
            if (!cursor_text(cursor, scn, record->option_text[option], sizeof(record->option_text[option]),
                             NULL) ||
                !cursor_u8(cursor, &record->option[option].target_kind) ||
                !cursor_u16(cursor, &record->option[option].target_chunk) ||
                !cursor_u32(cursor, &record->option[option].target_page) ||
                !cursor_u8(cursor, &record->option[option].flag) ||
                !cursor_u8(cursor, &record->option[option].value)) {
                return false;
            }
        }
        return true;
    }
    case SENREN_NODE_NEXT: {
        if (!cursor_u8(cursor, &record->target_kind) || !cursor_u16(cursor, &record->target_chunk) ||
            !cursor_u32(cursor, &record->target_page) || !cursor_u8(cursor, &record->condition_count)) {
            return false;
        }
        if (record->condition_count > SENREN_COND_MAX) {
            return false;
        }
        for (uint8_t condition = 0; condition < record->condition_count; condition++) {
            if (!cursor_u8(cursor, &record->condition[condition].flag) ||
                !cursor_u8(cursor, &record->condition[condition].value)) {
                return false;
            }
        }
        return true;
    }
    default:
        return false;
    }
}

void senren_player_reset(senren_player_t *player)
{
    if (player == NULL) {
        return;
    }
    memset(player, 0, sizeof(*player));
    player->loaded_chunk = 0xFFFF;
}

bool senren_player_start(senren_player_t *player, const senren_scn_t *scn, uint16_t chapter,
                         uint16_t chunk, uint32_t node, const senren_layout_t *layout)
{
    (void)layout;
    if (player == NULL || scn == NULL) {
        return false;
    }
    senren_player_reset(player);
    player->chapter = chapter;
    player->chunk = chunk;
    player->node = node;
    player->resume_node = node;
    return true;
}

senren_step_t senren_player_advance(senren_player_t *player, const senren_scn_t *scn,
                                    const senren_layout_t *layout)
{
    if (player == NULL || scn == NULL) {
        return SENREN_STEP_STUCK;
    }
    for (uint32_t guard = 0; guard < 200000; guard++) {
        if (!player_load_chunk(player, scn, player->chunk)) {
            return SENREN_STEP_STUCK;
        }
        uint32_t count = chunk_node_count(player);
        if (player->node >= count) {
            if ((uint32_t)player->chunk + 1 >= scn->chunk_count) {
                return SENREN_STEP_ENDING;
            }
            player->chunk++;
            player->node = 0;
            continue;
        }
        cursor_t cursor = { player->raw + 2, player->raw + player->loaded_len };
        if (!seek_node(&cursor, player->node)) {
            return SENREN_STEP_STUCK;
        }
        uint32_t executed = player->node;
        player->node = executed + 1;
        record_t record;
        if (!record_read(&cursor, scn, &record, player->text, sizeof(player->text))) {
            return SENREN_STEP_STUCK;
        }
        player->resume_node = executed;
        switch (record.kind) {
        case SENREN_NODE_LABEL:
            break;
        case SENREN_NODE_CHAPTER:
            player->chapter_title[0] = '\0';
            memcpy(player->chapter_title, player->text, sizeof(player->chapter_title));
            player->chapter_title[sizeof(player->chapter_title) - 1] = '\0';
            return SENREN_STEP_CHAPTER;
        case SENREN_NODE_BG:
            senren_scn_background(scn, record.bg, player->bg, sizeof(player->bg));
            break;
        case SENREN_NODE_EV:
            if (record.event == 0xFFFF) {
                player->ev[0] = '\0';
            } else {
                senren_scn_event(scn, record.event, player->ev, sizeof(player->ev));
            }
            break;
        case SENREN_NODE_SPRITE_OFF:
            player->sprite[0] = '\0';
            player->sprite_visible = false;
            player->sprite_action = SENREN_ACTION_FADE_OUT;
            break;
        case SENREN_NODE_DIALOGUE:
            senren_scn_speaker(scn, record.speaker, player->speaker, sizeof(player->speaker));
            if (record.sprite != 0xFFFF) {
                senren_scn_sprite_key(scn, record.sprite, player->sprite, sizeof(player->sprite));
                player->sprite_visible = true;
                player->sprite_action = record.action;
            }
            player_paginate(player, layout);
            return SENREN_STEP_TEXT;
        case SENREN_NODE_SELECT: {
            player->choice_count = record.option_count;
            for (uint8_t option = 0; option < record.option_count; option++) {
                memcpy(player->choice[option], record.option_text[option], sizeof(player->choice[option]));
                player->choice[option][sizeof(player->choice[option]) - 1] = '\0';
                player->choice_target_kind[option] = record.option[option].target_kind;
                player->choice_chunk[option] = record.option[option].target_chunk;
                player->choice_page[option] = record.option[option].target_page;
                player->choice_flag[option] = record.option[option].flag;
                player->choice_value[option] = record.option[option].value;
            }
            return SENREN_STEP_CHOICE;
        }
        case SENREN_NODE_NEXT: {
            bool matched = true;
            for (uint8_t condition = 0; condition < record.condition_count; condition++) {
                uint8_t flag = record.condition[condition].flag;
                if (flag >= SENREN_FLAG_MAX || player->flags[flag] != record.condition[condition].value) {
                    matched = false;
                    break;
                }
            }
            if (!matched) {
                break;
            }
            if (record.target_kind == SENREN_TARGET_ENDING) {
                player->ending_index = (uint16_t)record.target_page;
                return SENREN_STEP_ENDING;
            }
            uint16_t target_chunk = record.target_kind == SENREN_TARGET_CROSS ? record.target_chunk
                                                                              : player->chunk;
            uint32_t target_node = 0;
            if (!find_label(player, scn, target_chunk, record.target_page, &target_node)) {
                return SENREN_STEP_STUCK;
            }
            player_goto(player, target_chunk, target_node);
            break;
        }
        default:
            return SENREN_STEP_STUCK;
        }
    }
    return SENREN_STEP_STUCK;
}

bool senren_player_choose(senren_player_t *player, const senren_scn_t *scn, uint8_t index,
                          const senren_layout_t *layout)
{
    (void)layout;
    if (player == NULL || scn == NULL || index >= player->choice_count) {
        return false;
    }
    if (player->choice_flag[index] < SENREN_FLAG_MAX) {
        player->flags[player->choice_flag[index]] = player->choice_value[index];
    }
    uint16_t target_chunk = player->choice_target_kind[index] == SENREN_TARGET_CROSS
                                ? player->choice_chunk[index]
                                : player->chunk;
    uint32_t target_node = 0;
    if (!find_label(player, scn, target_chunk, player->choice_page[index], &target_node)) {
        return false;
    }
    player_goto(player, target_chunk, target_node);
    player->choice_count = 0;
    return true;
}

static bool skip_to(senren_player_t *player, const senren_scn_t *scn, bool chapter_break)
{
    if (player == NULL || scn == NULL) {
        return false;
    }
    for (uint32_t guard = 0; guard < 200000; guard++) {
        if (!player_load_chunk(player, scn, player->chunk)) {
            return false;
        }
        uint32_t count = chunk_node_count(player);
        if (player->node >= count) {
            if ((uint32_t)player->chunk + 1 >= scn->chunk_count) {
                return false;
            }
            player->chunk++;
            player->node = 0;
            continue;
        }
        cursor_t cursor = { player->raw + 2, player->raw + player->loaded_len };
        if (!seek_node(&cursor, player->node)) {
            return false;
        }
        uint8_t kind = cursor.p < cursor.end ? *cursor.p : 0xFF;
        if (kind == SENREN_NODE_CHAPTER) {
            return true;
        }
        if (!chapter_break && (kind == SENREN_NODE_BG || kind == SENREN_NODE_EV)) {
            return true;
        }
        player->node++;
    }
    return false;
}

bool senren_player_skip_chapter(senren_player_t *player, const senren_scn_t *scn,
                                const senren_layout_t *layout)
{
    (void)layout;
    if (!skip_to(player, scn, true)) {
        return false;
    }
    player->chapter++;
    return true;
}

bool senren_player_skip_scene(senren_player_t *player, const senren_scn_t *scn,
                              const senren_layout_t *layout)
{
    (void)layout;
    return skip_to(player, scn, false);
}

bool senren_player_load(senren_player_t *player, const senren_scn_t *scn, const senren_save_t *save,
                        const senren_layout_t *layout)
{
    (void)layout;
    if (player == NULL || scn == NULL || save == NULL) {
        return false;
    }
    senren_player_reset(player);
    player->chapter = save->chapter;
    player->chunk = save->chunk;
    player->node = save->node;
    player->resume_node = save->node;
    memcpy(player->flags, save->flags, sizeof(player->flags));
    memcpy(player->bg, save->bg, sizeof(player->bg));
    memcpy(player->ev, save->ev, sizeof(player->ev));
    memcpy(player->sprite, save->sprite, sizeof(player->sprite));
    player->sprite_action = save->sprite_action;
    player->sprite_visible = save->sprite_visible;
    return true;
}

size_t senren_player_page_text(const senren_player_t *player, char *out, size_t capacity)
{
    if (player == NULL || out == NULL || capacity == 0) {
        return 0;
    }
    size_t length = strlen(player->page);
    if (length >= capacity) {
        length = capacity - 1;
    }
    memcpy(out, player->page, length);
    out[length] = '\0';
    return length;
}

size_t senren_player_text(const senren_player_t *player, char *out, size_t capacity)
{
    if (player == NULL || out == NULL || capacity == 0) {
        return 0;
    }
    size_t length = strlen(player->text);
    if (length >= capacity) {
        length = capacity - 1;
    }
    memcpy(out, player->text, length);
    out[length] = '\0';
    return length;
}

size_t senren_player_speaker(const senren_player_t *player, char *out, size_t capacity)
{
    if (player == NULL || out == NULL || capacity == 0) {
        return 0;
    }
    size_t length = strlen(player->speaker);
    if (length >= capacity) {
        length = capacity - 1;
    }
    memcpy(out, player->speaker, length);
    out[length] = '\0';
    return length;
}

// --------------------------------------------------------------------------
// 存档
// --------------------------------------------------------------------------

#define SENREN_SAVE_VERSION 1u

static size_t put_name(uint8_t *out, size_t capacity, const char *value)
{
    size_t length = strlen(value);
    if (length + 1 > capacity) {
        length = capacity - 1;
    }
    if (length > 0xFF) {
        length = 0xFF;
    }
    out[0] = (uint8_t)length;
    memcpy(out + 1, value, length);
    return length + 1;
}

static size_t take_name(const uint8_t *data, size_t len, size_t offset, char *out, size_t capacity)
{
    if (offset >= len) {
        return 0;
    }
    size_t length = data[offset];
    if (offset + 1 + length > len) {
        return 0;
    }
    size_t copy = length < capacity - 1 ? length : capacity - 1;
    memcpy(out, data + offset + 1, copy);
    out[copy] = '\0';
    return length + 1;
}

void senren_save_from_player(const senren_player_t *player, senren_save_t *out)
{
    if (player == NULL || out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->chunk = player->chunk;
    out->node = player->resume_node;
    out->chapter = player->chapter;
    memcpy(out->flags, player->flags, sizeof(out->flags));
    memcpy(out->bg, player->bg, sizeof(out->bg));
    memcpy(out->ev, player->ev, sizeof(out->ev));
    memcpy(out->sprite, player->sprite, sizeof(out->sprite));
    out->sprite_action = player->sprite_action;
    out->sprite_visible = player->sprite_visible;
}

size_t senren_save_encode(const senren_save_t *save, uint8_t *out, size_t capacity)
{
    if (save == NULL || out == NULL || capacity < 4 + SENREN_FLAG_MAX + 3) {
        return 0;
    }
    size_t offset = 0;
    out[offset++] = (uint8_t)(SENREN_SAVE_VERSION & 0xFF);
    out[offset++] = (uint8_t)(SENREN_SAVE_VERSION >> 8);
    out[offset++] = (uint8_t)(save->chapter & 0xFF);
    out[offset++] = (uint8_t)(save->chapter >> 8);
    out[offset++] = (uint8_t)(save->chunk & 0xFF);
    out[offset++] = (uint8_t)(save->chunk >> 8);
    out[offset++] = (uint8_t)(save->node & 0xFF);
    out[offset++] = (uint8_t)((save->node >> 8) & 0xFF);
    out[offset++] = (uint8_t)((save->node >> 16) & 0xFF);
    out[offset++] = (uint8_t)((save->node >> 24) & 0xFF);
    memcpy(out + offset, save->flags, SENREN_FLAG_MAX);
    offset += SENREN_FLAG_MAX;
    out[offset++] = save->sprite_action;
    out[offset++] = save->sprite_visible ? 1 : 0;
    offset += put_name(out + offset, capacity - offset, save->bg);
    offset += put_name(out + offset, capacity - offset, save->ev);
    offset += put_name(out + offset, capacity - offset, save->sprite);
    return offset;
}

bool senren_save_decode(senren_save_t *save, const uint8_t *data, size_t len)
{
    if (save == NULL || data == NULL || len < 4 + SENREN_FLAG_MAX + 3) {
        return false;
    }
    if (rd16(data) != SENREN_SAVE_VERSION) {
        return false;
    }
    memset(save, 0, sizeof(*save));
    save->chapter = rd16(data + 2);
    save->chunk = rd16(data + 4);
    save->node = rd32(data + 6);
    memcpy(save->flags, data + 10, SENREN_FLAG_MAX);
    size_t offset = 10 + SENREN_FLAG_MAX;
    save->sprite_action = data[offset++];
    save->sprite_visible = data[offset++] != 0;
    offset += take_name(data, len, offset, save->bg, sizeof(save->bg));
    offset += take_name(data, len, offset, save->ev, sizeof(save->ev));
    offset += take_name(data, len, offset, save->sprite, sizeof(save->sprite));
    return true;
}
