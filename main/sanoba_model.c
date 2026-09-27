// main/sanoba_model.c —— 剧本包(SANOSCN1)解析 + 阅读状态机实现。
//
// 本文件不接触 ESP-IDF(只用标准 C 与 ROM 里的 tinfl),宿主机的单元测试可以直接用
// 真实剧本包跑完整条线路。字段布局与语义见 main/sanoba_model.h 与
// tools/sanoba_scn_pack.py 的模块 docstring(那里是格式的权威源)。
#include "sanoba_model.h"

#include "sanoba_inflate.h"

#include <stdlib.h>
#include <string.h>

#define SANOBA_SCN_HEADER_SIZE 20u
#define SANOBA_SCN_SECTION_SIZE 16u
#define SANOBA_SCN_CHUNK_RECORD 16u
#define SANOBA_SCN_LABEL_RECORD 12u
// 一页最多切几段(单句最长 60 字 -> 每行 13 字 5 行,最多 2 页;留余量)
#define SANOBA_PAGE_MAX 8
// 单个 NEXT 里最多几个条件(实测最多 4 个)
#define SANOBA_COND_MAX 8

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// 记录流游标(实现见下面的"节点读写"段;提前声明给场景表/标签表访问器用)
typedef struct {
    const uint8_t *p;
    const uint8_t *end;
} cursor_t;

static bool cursor_text(cursor_t *cursor, const sanoba_scn_t *scn, char *out, size_t capacity,
                        size_t *written_out);

// --------------------------------------------------------------------------
// UTF-8 与排版
// --------------------------------------------------------------------------

size_t sanoba_utf8_next_boundary(const char *utf8, size_t len, size_t pos)
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

uint32_t sanoba_utf8_decode(const char *utf8, size_t len, size_t pos, size_t *next_out)
{
    if (utf8 == NULL || pos >= len) {
        if (next_out != NULL) {
            *next_out = len;
        }
        return 0;
    }
    const uint8_t *bytes = (const uint8_t *)utf8;
    uint8_t lead = bytes[pos];
    size_t next = sanoba_utf8_next_boundary(utf8, len, pos);
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

int sanoba_char_units(uint32_t codepoint)
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

int sanoba_text_lines(const char *utf8, int units_per_line, uint32_t *offsets, int max_lines)
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
            uint32_t codepoint = sanoba_utf8_decode(utf8, len, scan, &next);
            int width = sanoba_char_units(codepoint);
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
        uint32_t head = sanoba_utf8_decode(utf8, len, scan, &next);
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

int sanoba_text_pages(const char *utf8, int units_per_line, int lines_per_page, uint32_t *offsets,
                      int max_offsets)
{
    if (utf8 == NULL || offsets == NULL || max_offsets <= 0 || lines_per_page <= 0) {
        return 0;
    }
    size_t len = strlen(utf8);
    uint32_t line_offsets[64];
    int lines = sanoba_text_lines(utf8, units_per_line, line_offsets, 64);
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

bool sanoba_scn_open(sanoba_scn_t *scn, const uint8_t *data, uint32_t size)
{
    if (scn == NULL || data == NULL || size < SANOBA_SCN_HEADER_SIZE) {
        return false;
    }
    if (memcmp(data, SANOBA_SCN_MAGIC, 8) != 0) {
        return false;
    }
    uint16_t version = rd16(data + 8);
    uint16_t header_size = rd16(data + 10);
    uint16_t section_count = rd16(data + 12);
    uint32_t total = rd32(data + 16);
    if (version != SANOBA_SCN_VERSION || total != size) {
        return false;
    }
    if (header_size < SANOBA_SCN_HEADER_SIZE + (uint32_t)section_count * SANOBA_SCN_SECTION_SIZE ||
        (uint64_t)header_size > size) {
        return false;
    }

    sanoba_scn_t view;
    memset(&view, 0, sizeof(view));
    view.blob = data;
    view.blob_size = size;
    // 段表紧跟在 20 字节头部之后;header_size 是「段体起点」(头部 + 段表)
    for (uint16_t index = 0; index < section_count; index++) {
        const uint8_t *section = data + SANOBA_SCN_HEADER_SIZE + (uint32_t)index * SANOBA_SCN_SECTION_SIZE;
        uint32_t type = rd32(section);
        uint32_t offset = rd32(section + 4);
        uint32_t count = rd32(section + 8);
        uint32_t length = rd32(section + 12);
        if (offset > size || length > size - offset) {
            return false;
        }
        switch (type) {
        case SANOBA_SCN_SEC_CHAR:
            // 段体 = u32 count + count × u16 码位
            if (length < 4 || count == 0 || length < 4u + (uint64_t)count * 2u) {
                return false;
            }
            view.chars = data + offset + 4;
            view.char_count = count;
            break;
        case SANOBA_SCN_SEC_NAME:
            // 段体 = 字典块连排(每块 u16 count + 条目),没有额外前缀
            view.names = data + offset;
            view.names_size = length;
            break;
        case SANOBA_SCN_SEC_SCENARIO:
            // 段体 = u16 count + count × { first_chunk u16, chunk_count u16, title text }
            if (length < 2 || count == 0) {
                return false;
            }
            view.scenarios = data + offset;
            view.scenarios_size = length;
            view.scenario_count = count;
            break;
        case SANOBA_SCN_SEC_CHUNK:
            // 段体 = u16 count + count × 16 字节记录
            if (length < 2 || count == 0 || length < 2u + (uint64_t)count * SANOBA_SCN_CHUNK_RECORD) {
                return false;
            }
            view.chunks = data + offset + 2;
            view.chunks_size = length - 2u;
            view.chunk_count = count;
            break;
        case SANOBA_SCN_SEC_BLOB:
            view.payload = data + offset;
            view.payload_size = length;
            break;
        case SANOBA_SCN_SEC_LABEL:
            // 段体 = u32 count + count × 12 字节 { name_id u32, scenario u16, chunk u16, node u32 }
            if (length < 4 || length < 4u + (uint64_t)count * SANOBA_SCN_LABEL_RECORD) {
                return false;
            }
            view.labels = data + offset + 4;
            view.label_count = count;
            view.labels_size = length - 4u;
            break;
        case SANOBA_SCN_SEC_ROUTE:
            // 段体 = u16 触发场景 + u16 兜底场景 + u16 目标数 + 每项 { flag_id u8, scenario u16 }
            view.route = data + offset;
            view.route_size = length;
            break;
        case SANOBA_SCN_SEC_META:
            view.meta = data + offset;
            view.meta_size = length;
            break;
        default:
            break;
        }
    }
    if (view.chars == NULL || view.names == NULL || view.scenarios == NULL || view.chunks == NULL ||
        view.payload == NULL) {
        return false;
    }
    if (view.char_count == 0 || view.chunk_count == 0 || view.scenario_count == 0 ||
        (uint64_t)view.chunk_count * SANOBA_SCN_CHUNK_RECORD > view.chunks_size) {
        return false;
    }

    // 字典块连排,顺序固定:speaker / sprite_key / sprite_position / sprite_expression /
    // sprite_outfit / background / event / flag / label。固件只用到说话人 / 立绘键 /
    // 背景 / 事件 / 旗标 / 标签名,其余只跳过。
    const uint8_t *cursor = view.names;
    const uint8_t *names_end = view.names + view.names_size;
    const uint8_t **tables[9] = { &view.speakers, &view.sprite_keys, NULL, NULL, NULL,
                                  &view.bg_names, &view.event_names, &view.flag_names,
                                  &view.label_names };
    uint32_t *counts[9] = { &view.speaker_count, &view.sprite_count, NULL, NULL, NULL,
                            &view.bg_count, &view.event_count, &view.flag_count,
                            &view.label_name_count };
    for (int block = 0; block < 9; block++) {
        if (cursor + 2 > names_end) {
            return false;
        }
        uint16_t entries = rd16(cursor);
        cursor += 2;
        if (tables[block] != NULL) {
            *tables[block] = cursor;
            *counts[block] = entries;
        }
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

// 场景表项:段体首 2 字节是计数,条目紧跟在后面
static const uint8_t *scenario_entry(const sanoba_scn_t *scn, uint16_t index)
{
    if (scn->scenarios == NULL || index >= scn->scenario_count) {
        return NULL;
    }
    const uint8_t *cursor = scn->scenarios + 2;
    const uint8_t *end = scn->scenarios + scn->scenarios_size;
    for (uint16_t item = 0; item < index; item++) {
        if (cursor + 8 > end) {
            return NULL;
        }
        uint16_t length = rd16(cursor + 6);
        cursor += 8 + (uint32_t)length * 2;
        if (cursor > end) {
            return NULL;
        }
    }
    return cursor;
}

bool sanoba_scn_scenario(const sanoba_scn_t *scn, uint16_t index, uint16_t *first_chunk,
                         uint16_t *chunk_count, char *title_out, size_t title_capacity)
{
    if (scn == NULL) {
        return false;
    }
    const uint8_t *entry = scenario_entry(scn, index);
    if (entry == NULL) {
        return false;
    }
    const uint8_t *end = scn->scenarios + scn->scenarios_size;
    if (entry + 8 > end) {
        return false;
    }
    if (first_chunk != NULL) {
        *first_chunk = rd16(entry);
    }
    if (chunk_count != NULL) {
        *chunk_count = rd16(entry + 2);
    }
    if (title_out != NULL && title_capacity > 0) {
        uint16_t length = rd16(entry + 6);
        title_out[0] = '\0';
        if (entry + 8 + (uint32_t)length * 2 <= end) {
            // 游标要包含长度字段本身:cursor_text 自己读 u16 长度再读码表下标
            cursor_t cursor = { (uint8_t *)entry + 6, (uint8_t *)entry + 8 + (uint32_t)length * 2 };
            (void)cursor_text(&cursor, scn, title_out, title_capacity, NULL);
        }
    }
    return true;
}

uint16_t sanoba_scn_scenario_next(const sanoba_scn_t *scn, uint16_t index)
{
    if (scn == NULL) {
        return SANOBA_SCENARIO_NONE;
    }
    const uint8_t *entry = scenario_entry(scn, index);
    if (entry == NULL || entry + 8 > scn->scenarios + scn->scenarios_size) {
        return SANOBA_SCENARIO_NONE;
    }
    return rd16(entry + 4);
}

bool sanoba_scn_label(const sanoba_scn_t *scn, uint32_t label_id, uint16_t *scenario, uint16_t *chunk,
                      uint32_t *node)
{
    if (scn == NULL || scn->labels == NULL || label_id >= scn->label_count) {
        return false;
    }
    const uint8_t *entry = scn->labels + (uint32_t)label_id * SANOBA_SCN_LABEL_RECORD;
    if ((uint32_t)label_id * SANOBA_SCN_LABEL_RECORD + SANOBA_SCN_LABEL_RECORD > scn->labels_size) {
        return false;
    }
    if (scenario != NULL) {
        *scenario = rd16(entry + 4);
    }
    if (chunk != NULL) {
        *chunk = rd16(entry + 6);
    }
    if (node != NULL) {
        *node = rd32(entry + 8);
    }
    return true;
}

uint16_t sanoba_scn_next_scenario(const sanoba_scn_t *scn, uint16_t scenario, const uint8_t *flags)
{
    if (scn == NULL || scenario >= scn->scenario_count) {
        return SANOBA_SCENARIO_NONE;
    }
    // 选线规则(与源引擎 _resolveNextScn 一致):命中触发场景时按旗标最高分,
    // 全 0 或并列走兜底;否则按场景表顺序推进。
    if (scn->route != NULL && scn->route_size >= 6) {
        const uint8_t *route = scn->route;
        uint16_t trigger = rd16(route);
        uint16_t fallback = rd16(route + 2);
        uint16_t targets = rd16(route + 4);
        if (scenario == trigger && targets > 0 && (uint32_t)6 + (uint32_t)targets * 3 <= scn->route_size) {
            int best = -1;
            uint16_t best_scenario = fallback;
            int best_score = 0;
            bool tie = false;
            for (uint16_t item = 0; item < targets; item++) {
                const uint8_t *target = route + 6 + (uint32_t)item * 3;
                uint8_t flag = target[0];
                uint16_t next = rd16(target + 1);
                int score = (flags != NULL && flag < SANOBA_FLAG_MAX) ? flags[flag] : 0;
                if (best < 0 || score > best_score) {
                    best = item;
                    best_scenario = next;
                    best_score = score;
                    tie = false;
                } else if (score == best_score) {
                    tie = true;
                }
            }
            if (best < 0 || best_score == 0 || tie) {
                return fallback;
            }
            return best_scenario;
        }
    }
    if ((uint32_t)scenario + 1 < scn->scenario_count) {
        uint16_t next = sanoba_scn_scenario_next(scn, scenario);
        if (next != SANOBA_SCENARIO_NONE && next < scn->scenario_count) {
            return next;
        }
    }
    return SANOBA_SCENARIO_NONE;
}

uint32_t sanoba_scn_chunk(const sanoba_scn_t *scn, uint16_t index, uint8_t *raw, uint32_t capacity)
{
    if (scn == NULL || raw == NULL || index >= scn->chunk_count) {
        return 0;
    }
    const uint8_t *record = scn->chunks + (uint32_t)index * SANOBA_SCN_CHUNK_RECORD;
    uint32_t offset = rd32(record);
    uint32_t length = rd32(record + 4);
    uint32_t raw_len = rd32(record + 8);
    if (offset > scn->payload_size || length > scn->payload_size - offset || raw_len > capacity) {
        return 0;
    }
    // ROM 里的 tinfl 解 zlib 流(见 sanoba_inflate.c)
    uint32_t produced = sanoba_inflate(scn->payload + offset, length, raw, capacity);
    return produced == raw_len ? produced : 0;
}

// 字典项里的 u16 是字符表下标,要借字符表才能还原成 UTF-8
static size_t name_from_chars(const sanoba_scn_t *scn, const uint8_t *table, uint32_t count,
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

size_t sanoba_scn_speaker(const sanoba_scn_t *scn, uint16_t index, char *out, size_t capacity)
{
    return name_from_chars(scn, scn ? scn->speakers : NULL, scn ? scn->speaker_count : 0, index, out,
                           capacity);
}

size_t sanoba_scn_sprite_key(const sanoba_scn_t *scn, uint16_t index, char *out, size_t capacity)
{
    return name_from_chars(scn, scn ? scn->sprite_keys : NULL, scn ? scn->sprite_count : 0, index, out,
                           capacity);
}

size_t sanoba_scn_event(const sanoba_scn_t *scn, uint16_t index, char *out, size_t capacity)
{
    return name_from_chars(scn, scn ? scn->event_names : NULL, scn ? scn->event_count : 0, index, out,
                           capacity);
}

size_t sanoba_scn_background(const sanoba_scn_t *scn, uint16_t index, char *out, size_t capacity)
{
    return name_from_chars(scn, scn ? scn->bg_names : NULL, scn ? scn->bg_count : 0, index, out,
                           capacity);
}

size_t sanoba_scn_ending(const sanoba_scn_t *scn, uint16_t index, char *out, size_t capacity)
{
    (void)scn;
    if (out == NULL || capacity == 0) {
        return 0;
    }
    // 源数据里的两个结局标签哪都没定义,直接用固定文案(index 只区分标题/收集)
    const char *text = index == SANOBA_ENDING_COLLECTION ? "ENDING" : "ENDING";
    size_t length = strlen(text);
    if (length + 1 > capacity) {
        length = capacity - 1;
    }
    memcpy(out, text, length);
    out[length] = '\0';
    return length;
}

size_t sanoba_scn_flag(const sanoba_scn_t *scn, uint16_t index, char *out, size_t capacity)
{
    return name_from_chars(scn, scn ? scn->flag_names : NULL, scn ? scn->flag_count : 0, index, out,
                           capacity);
}

size_t sanoba_scn_scenario_title(const sanoba_scn_t *scn, uint16_t index, char *out, size_t capacity)
{
    if (out != NULL && capacity > 0) {
        out[0] = '\0';
    }
    return sanoba_scn_scenario(scn, index, NULL, NULL, out, capacity) ? strlen(out) : 0;
}

// --------------------------------------------------------------------------
// 节点读写
// --------------------------------------------------------------------------

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
static bool cursor_text(cursor_t *cursor, const sanoba_scn_t *scn, char *out, size_t capacity,
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
    case SANOBA_NODE_LABEL: {
        uint32_t label = 0;
        return cursor_u32(cursor, &label);
    }
    case SANOBA_NODE_CHAPTER:
        return cursor_skip_text(cursor);
    case SANOBA_NODE_BG: {
        uint8_t index = 0;
        return cursor_u8(cursor, &index);
    }
    case SANOBA_NODE_DIALOGUE: {
        uint8_t speaker = 0;
        uint8_t sprites = 0;
        if (!cursor_u8(cursor, &speaker) || !cursor_skip_text(cursor) || !cursor_u8(cursor, &sprites)) {
            return false;
        }
        // 每个立绘 4 个 u8(键 / 位置 / 表情 / 服装);本作源工程没有立绘图,解出来也丢掉
        if (cursor->p + (uint32_t)sprites * 4u > cursor->end) {
            return false;
        }
        cursor->p += (uint32_t)sprites * 4u;
        return true;
    }
    case SANOBA_NODE_EV: {
        uint16_t value = 0;
        return cursor_u16(cursor, &value);
    }
    case SANOBA_NODE_SPRITE_OFF:
        return true;
    case SANOBA_NODE_SELECT: {
        uint8_t options = 0;
        if (!cursor_u8(cursor, &options)) {
            return false;
        }
        for (uint8_t option = 0; option < options; option++) {
            uint8_t target_kind = 0;
            uint8_t assigns = 0;
            uint32_t label = 0;
            if (!cursor_skip_text(cursor) || !cursor_u8(cursor, &target_kind) ||
                !cursor_u32(cursor, &label) || !cursor_u8(cursor, &assigns)) {
                return false;
            }
            if (cursor->p + (uint32_t)assigns * 4u > cursor->end) {
                return false;
            }
            cursor->p += (uint32_t)assigns * 4u;
        }
        return true;
    }
    case SANOBA_NODE_NEXT: {
        uint8_t target_kind = 0;
        uint8_t conditions = 0;
        uint32_t label = 0;
        if (!cursor_u8(cursor, &target_kind) || !cursor_u32(cursor, &label) ||
            !cursor_u8(cursor, &conditions)) {
            return false;
        }
        if (cursor->p + (uint32_t)conditions * 4u > cursor->end) {
            return false;
        }
        cursor->p += (uint32_t)conditions * 4u;
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

static bool player_load_chunk(sanoba_player_t *player, const sanoba_scn_t *scn, uint16_t chunk)
{
    if (player->loaded_chunk == chunk && player->loaded_len > 0) {
        return true;
    }
    if (player->raw == NULL) {
        // 缓冲欠着用:本板静态段被画布与 LVGL 内存池占满,堆还有余地
        player->raw = (uint8_t *)malloc(SANOBA_CHUNK_RAW_MAX);
        if (player->raw == NULL) {
            return false;
        }
        player->raw_capacity = SANOBA_CHUNK_RAW_MAX;
    }
    uint32_t length = sanoba_scn_chunk(scn, chunk, player->raw, player->raw_capacity);
    if (length == 0) {
        return false;
    }
    player->loaded_chunk = chunk;
    player->loaded_len = length;
    return true;
}

static uint32_t chunk_node_count(const sanoba_player_t *player)
{
    return player->loaded_len >= 2 ? rd16(player->raw) : 0;
}

// 按排版参数把 player->text 切页,并把第 0 页放进 player->page
static void player_paginate(sanoba_player_t *player, const sanoba_layout_t *layout)
{
    uint16_t units = layout != NULL ? layout->units_per_line : 26;
    uint16_t lines = layout != NULL ? layout->lines_per_page : 5;
    int pages = sanoba_text_pages(player->text, units, lines, player->page_offsets, SANOBA_PAGE_MAX);
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

bool sanoba_player_next_page(sanoba_player_t *player)
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
// 把玩家位置挪到标签落点;落点在别的场景时一并换场景(源数据的分支链跨文件)。
static bool player_goto_label(sanoba_player_t *player, const sanoba_scn_t *scn, uint32_t label_id)
{
    uint16_t scenario = 0;
    uint16_t chunk = 0;
    uint32_t node = 0;
    if (!sanoba_scn_label(scn, label_id, &scenario, &chunk, &node)) {
        return false;
    }
    if (chunk >= scn->chunk_count) {
        return false;
    }
    player->scenario = scenario;
    player->chunk = chunk;
    player->node = node;
    return true;
}

// 把位置往前推一格,遇到块尾 / 场景尾就换块 / 按选线规则换场景。
// 返回 false 表示剧情结束(没有下一个场景了)。
static bool player_step_position(sanoba_player_t *player, const sanoba_scn_t *scn)
{
    for (uint32_t guard = 0; guard < 200000; guard++) {
        if (!player_load_chunk(player, scn, player->chunk)) {
            return false;
        }
        if (player->node < chunk_node_count(player)) {
            return true;
        }
        uint16_t first = 0;
        uint16_t length = 0;
        if (!sanoba_scn_scenario(scn, player->scenario, &first, &length, NULL, 0)) {
            return false;
        }
        if ((uint32_t)player->chunk + 1u < (uint32_t)first + length) {
            player->chunk++;
            player->node = 0;
            continue;
        }
        uint16_t next = sanoba_scn_next_scenario(scn, player->scenario, player->flags);
        if (next == SANOBA_SCENARIO_NONE) {
            return false;
        }
        if (!sanoba_scn_scenario(scn, next, &first, &length, NULL, 0) || length == 0) {
            return false;
        }
        player->scenario = next;
        player->chunk = first;
        player->node = 0;
    }
    return false;
}

static void player_goto(sanoba_player_t *player, uint16_t chunk, uint32_t node)
{
    player->chunk = chunk;
    player->node = node;
}

// 执行一条记录;返回 true 表示产生了可见步骤(step_out 有效)
typedef struct {
    uint8_t kind;
    uint32_t label;          // LABEL / SELECT 选项目标 / NEXT 目标
    uint8_t speaker;
    uint8_t bg;
    uint8_t ev_kind;
    uint16_t event;          // 0xFFFF = 清空
    uint8_t option_count;
    struct {
        uint8_t target_kind;
        uint32_t label;
        uint8_t assign_count;
        struct {
            uint8_t flag;
            uint8_t op;
            uint16_t value;
        } assign[SANOBA_ASSIGN_MAX];
    } option[SANOBA_CHOICE_MAX];
    char option_text[SANOBA_CHOICE_MAX][SANOBA_NAME_MAX];
    uint8_t target_kind;
    uint8_t condition_count;
    struct {
        uint8_t flag;
        uint8_t op;
        uint16_t value;
    } condition[SANOBA_COND_MAX];
} record_t;

static bool record_read(cursor_t *cursor, const sanoba_scn_t *scn, record_t *record, char *text,
                        size_t text_capacity)
{
    memset(record, 0, sizeof(*record));
    if (!cursor_u8(cursor, &record->kind)) {
        return false;
    }
    switch (record->kind) {
    case SANOBA_NODE_LABEL:
        return cursor_u32(cursor, &record->label);
    case SANOBA_NODE_CHAPTER:
        return cursor_text(cursor, scn, text, text_capacity, NULL);
    case SANOBA_NODE_BG:
        return cursor_u8(cursor, &record->bg);
    case SANOBA_NODE_DIALOGUE: {
        uint8_t sprites = 0;
        if (!cursor_u8(cursor, &record->speaker) ||
            !cursor_text(cursor, scn, text, text_capacity, NULL) || !cursor_u8(cursor, &sprites)) {
            return false;
        }
        if (cursor->p + (uint32_t)sprites * 4u > cursor->end) {
            return false;
        }
        cursor->p += (uint32_t)sprites * 4u;   // 立绘列表:本作没有立绘图,丢掉
        return true;
    }
    case SANOBA_NODE_EV: {
        uint16_t value = 0;
        if (!cursor_u16(cursor, &value)) {
            return false;
        }
        if (value == 0xFFFF) {
            record->event = 0xFFFF;
            return true;
        }
        record->ev_kind = (uint8_t)(value >> 14);
        record->event = (uint16_t)(value & 0x3FFFu);
        return true;
    }
    case SANOBA_NODE_SPRITE_OFF:
        return true;
    case SANOBA_NODE_SELECT: {
        if (!cursor_u8(cursor, &record->option_count)) {
            return false;
        }
        if (record->option_count > SANOBA_CHOICE_MAX) {
            return false;
        }
        for (uint8_t option = 0; option < record->option_count; option++) {
            if (!cursor_text(cursor, scn, record->option_text[option], sizeof(record->option_text[option]),
                             NULL) ||
                !cursor_u8(cursor, &record->option[option].target_kind) ||
                !cursor_u32(cursor, &record->option[option].label) ||
                !cursor_u8(cursor, &record->option[option].assign_count)) {
                return false;
            }
            if (record->option[option].assign_count > SANOBA_ASSIGN_MAX) {
                return false;
            }
            for (uint8_t assign = 0; assign < record->option[option].assign_count; assign++) {
                if (!cursor_u8(cursor, &record->option[option].assign[assign].flag) ||
                    !cursor_u8(cursor, &record->option[option].assign[assign].op) ||
                    !cursor_u16(cursor, &record->option[option].assign[assign].value)) {
                    return false;
                }
            }
        }
        return true;
    }
    case SANOBA_NODE_NEXT: {
        if (!cursor_u8(cursor, &record->target_kind) || !cursor_u32(cursor, &record->label) ||
            !cursor_u8(cursor, &record->condition_count)) {
            return false;
        }
        if (record->condition_count > SANOBA_COND_MAX) {
            return false;
        }
        for (uint8_t condition = 0; condition < record->condition_count; condition++) {
            if (!cursor_u8(cursor, &record->condition[condition].flag) ||
                !cursor_u8(cursor, &record->condition[condition].op) ||
                !cursor_u16(cursor, &record->condition[condition].value)) {
                return false;
            }
        }
        return true;
    }
    default:
        return false;
    }
}

// 赋值 / 条件求值
static void apply_assign(sanoba_player_t *player, uint8_t flag, uint8_t op, uint16_t value)
{
    if (flag >= SANOBA_FLAG_MAX) {
        return;
    }
    if (op == SANOBA_OP_ADD) {
        int next = (int)player->flags[flag] + (int16_t)value;
        if (next < 0) {
            next = 0;
        }
        if (next > 255) {
            next = 255;
        }
        player->flags[flag] = (uint8_t)next;
    } else {
        player->flags[flag] = (uint8_t)(value & 0xFFu);
    }
}

static bool condition_matches(const sanoba_player_t *player, const record_t *record)
{
    for (uint8_t condition = 0; condition < record->condition_count; condition++) {
        uint8_t flag = record->condition[condition].flag;
        if (flag >= SANOBA_FLAG_MAX) {
            return false;
        }
        uint16_t want = record->condition[condition].value;
        uint8_t have = player->flags[flag];
        switch (record->condition[condition].op) {
        case SANOBA_OP_ADD:
            // 格式里条件只用 ==/!=;非等值条件目前按"不命中"处理并留给以后扩展
            if (have != (uint8_t)want) {
                return false;
            }
            break;
        default:
            if (have != (uint8_t)want) {
                return false;
            }
            break;
        }
    }
    return true;
}

void sanoba_player_reset(sanoba_player_t *player)
{
    if (player == NULL) {
        return;
    }
    // 保留已申请的解压缓冲(重置的是阅读进度,不是内存)
    uint8_t *raw = player->raw;
    uint32_t capacity = player->raw_capacity;
    memset(player, 0, sizeof(*player));
    player->raw = raw;
    player->raw_capacity = capacity;
    player->loaded_chunk = 0xFFFF;
}

void sanoba_player_release(sanoba_player_t *player)
{
    if (player == NULL) {
        return;
    }
    free(player->raw);
    player->raw = NULL;
    player->raw_capacity = 0;
    player->loaded_chunk = 0xFFFF;
    player->loaded_len = 0;
}

bool sanoba_player_start(sanoba_player_t *player, const sanoba_scn_t *scn, uint16_t scenario,
                         uint16_t chunk, uint32_t node, const sanoba_layout_t *layout)
{
    (void)layout;
    if (player == NULL || scn == NULL) {
        return false;
    }
    sanoba_player_reset(player);
    player->scenario = scenario;
    player->chunk = chunk;
    player->node = node;
    player->resume_node = node;
    return true;
}

sanoba_step_t sanoba_player_advance(sanoba_player_t *player, const sanoba_scn_t *scn,
                                    const sanoba_layout_t *layout)
{
    if (player == NULL || scn == NULL) {
        return SANOBA_STEP_STUCK;
    }
    if (player->ending_pending) {
        player->ending_pending = false;
        return SANOBA_STEP_ENDING;
    }
    for (uint32_t guard = 0; guard < 200000; guard++) {
        if (!player_step_position(player, scn)) {
            return SANOBA_STEP_ENDING;
        }
        if (!player_load_chunk(player, scn, player->chunk)) {
            return SANOBA_STEP_STUCK;
        }
        cursor_t cursor = { player->raw + 2, player->raw + player->loaded_len };
        if (!seek_node(&cursor, player->node)) {
            return SANOBA_STEP_STUCK;
        }
        uint32_t executed = player->node;
        player->node = executed + 1;
        record_t record;
        if (!record_read(&cursor, scn, &record, player->text, sizeof(player->text))) {
            return SANOBA_STEP_STUCK;
        }
        player->resume_node = executed;
        switch (record.kind) {
        case SANOBA_NODE_LABEL:
            break;
        case SANOBA_NODE_CHAPTER:
            player->chapter_title[0] = '\0';
            memcpy(player->chapter_title, player->text, sizeof(player->chapter_title));
            player->chapter_title[sizeof(player->chapter_title) - 1] = '\0';
            return SANOBA_STEP_CHAPTER;
        case SANOBA_NODE_BG:
            sanoba_scn_background(scn, record.bg, player->bg, sizeof(player->bg));
            break;
        case SANOBA_NODE_EV:
            if (record.event == 0xFFFF) {
                player->ev[0] = '\0';
                player->ev_kind = SANOBA_EV_KIND_UNSUPPORTED;
            } else {
                sanoba_scn_event(scn, record.event, player->ev, sizeof(player->ev));
                player->ev_kind = record.ev_kind;
            }
            break;
        case SANOBA_NODE_SPRITE_OFF:
            player->sprite[0] = '\0';
            player->sprite_visible = false;
            player->sprite_action = SANOBA_ACTION_FADE_OUT;
            break;
        case SANOBA_NODE_DIALOGUE:
            sanoba_scn_speaker(scn, record.speaker, player->speaker, sizeof(player->speaker));
            player_paginate(player, layout);
            return SANOBA_STEP_TEXT;
        case SANOBA_NODE_SELECT: {
            player->choice_count = record.option_count;
            for (uint8_t option = 0; option < record.option_count; option++) {
                memcpy(player->choice[option], record.option_text[option], sizeof(player->choice[option]));
                player->choice[option][sizeof(player->choice[option]) - 1] = '\0';
                player->choice_target_kind[option] = record.option[option].target_kind;
                player->choice_label[option] = record.option[option].label;
                player->choice_assign_count[option] = record.option[option].assign_count;
                for (uint8_t assign = 0; assign < record.option[option].assign_count; assign++) {
                    player->choice_assign_flag[option][assign] = record.option[option].assign[assign].flag;
                    player->choice_assign_op[option][assign] = record.option[option].assign[assign].op;
                    player->choice_assign_value[option][assign] = record.option[option].assign[assign].value;
                }
            }
            return SANOBA_STEP_CHOICE;
        }
        case SANOBA_NODE_NEXT: {
            if (!condition_matches(player, &record)) {
                break;
            }
            if (record.target_kind == SANOBA_TARGET_ENDING) {
                player->ending_index = (uint16_t)record.label;
                return SANOBA_STEP_ENDING;
            }
            if (record.target_kind == SANOBA_TARGET_IGNORED) {
                // 源里哪都没定义的名字(除两个结局标签外不存在):保持当前进度
                break;
            }
            if (!player_goto_label(player, scn, record.label)) {
                return SANOBA_STEP_STUCK;
            }
            break;
        }
        default:
            return SANOBA_STEP_STUCK;
        }
    }
    return SANOBA_STEP_STUCK;
}

bool sanoba_player_choose(sanoba_player_t *player, const sanoba_scn_t *scn, uint8_t index,
                          const sanoba_layout_t *layout)
{
    (void)layout;
    if (player == NULL || scn == NULL || index >= player->choice_count) {
        return false;
    }
    // 选项附带的赋值先落进旗标(如 f.sel_flag = 0 , f.nen_flag ++)
    for (uint8_t assign = 0; assign < player->choice_assign_count[index]; assign++) {
        apply_assign(player, player->choice_assign_flag[index][assign],
                     player->choice_assign_op[index][assign],
                     player->choice_assign_value[index][assign]);
    }
    uint8_t target_kind = player->choice_target_kind[index];
    uint32_t label = player->choice_label[index];
    player->choice_count = 0;
    if (target_kind == SANOBA_TARGET_ENDING) {
        player->ending_index = (uint16_t)label;
        player->ending_pending = true;
        return true;
    }
    if (target_kind == SANOBA_TARGET_IGNORED) {
        return true;   // 保持当前进度,由后续节点继续
    }
    return player_goto_label(player, scn, label);
}

static bool skip_to(sanoba_player_t *player, const sanoba_scn_t *scn, bool chapter_break)
{
    if (player == NULL || scn == NULL) {
        return false;
    }
    for (uint32_t guard = 0; guard < 200000; guard++) {
        if (!player_step_position(player, scn)) {
            return false;
        }
        if (!player_load_chunk(player, scn, player->chunk)) {
            return false;
        }
        cursor_t cursor = { player->raw + 2, player->raw + player->loaded_len };
        if (!seek_node(&cursor, player->node)) {
            return false;
        }
        uint8_t kind = cursor.p < cursor.end ? *cursor.p : 0xFF;
        if (kind == SANOBA_NODE_CHAPTER) {
            return true;
        }
        if (!chapter_break && (kind == SANOBA_NODE_BG || kind == SANOBA_NODE_EV)) {
            return true;
        }
        player->node++;
    }
    return false;
}

bool sanoba_player_skip_chapter(sanoba_player_t *player, const sanoba_scn_t *scn,
                                const sanoba_layout_t *layout)
{
    (void)layout;
    return skip_to(player, scn, true);
}

bool sanoba_player_skip_scene(sanoba_player_t *player, const sanoba_scn_t *scn,
                              const sanoba_layout_t *layout)
{
    (void)layout;
    return skip_to(player, scn, false);
}

bool sanoba_player_load(sanoba_player_t *player, const sanoba_scn_t *scn, const sanoba_save_t *save,
                        const sanoba_layout_t *layout)
{
    (void)layout;
    if (player == NULL || scn == NULL || save == NULL) {
        return false;
    }
    sanoba_player_reset(player);
    player->scenario = save->scenario;
    player->chunk = save->chunk;
    player->node = save->node;
    player->resume_node = save->node;
    memcpy(player->flags, save->flags, sizeof(player->flags));
    memcpy(player->bg, save->bg, sizeof(player->bg));
    memcpy(player->ev, save->ev, sizeof(player->ev));
    player->ev_kind = save->ev_kind;
    return true;
}

size_t sanoba_player_page_text(const sanoba_player_t *player, char *out, size_t capacity)
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

size_t sanoba_player_text(const sanoba_player_t *player, char *out, size_t capacity)
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

size_t sanoba_player_speaker(const sanoba_player_t *player, char *out, size_t capacity)
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

#define SANOBA_SAVE_VERSION 1u

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

void sanoba_save_from_player(const sanoba_player_t *player, sanoba_save_t *out)
{
    if (player == NULL || out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->scenario = player->scenario;
    out->chunk = player->chunk;
    out->node = player->resume_node;
    memcpy(out->flags, player->flags, sizeof(out->flags));
    memcpy(out->bg, player->bg, sizeof(out->bg));
    memcpy(out->ev, player->ev, sizeof(out->ev));
    out->ev_kind = player->ev_kind;
}

size_t sanoba_save_encode(const sanoba_save_t *save, uint8_t *out, size_t capacity)
{
    if (save == NULL || out == NULL || capacity < 10 + SANOBA_FLAG_MAX + 2) {
        return 0;
    }
    size_t offset = 0;
    out[offset++] = (uint8_t)(SANOBA_SAVE_VERSION & 0xFF);
    out[offset++] = (uint8_t)(SANOBA_SAVE_VERSION >> 8);
    out[offset++] = (uint8_t)(save->scenario & 0xFF);
    out[offset++] = (uint8_t)(save->scenario >> 8);
    out[offset++] = (uint8_t)(save->chunk & 0xFF);
    out[offset++] = (uint8_t)(save->chunk >> 8);
    out[offset++] = (uint8_t)(save->node & 0xFF);
    out[offset++] = (uint8_t)((save->node >> 8) & 0xFF);
    out[offset++] = (uint8_t)((save->node >> 16) & 0xFF);
    out[offset++] = (uint8_t)(save->node >> 24);
    memcpy(out + offset, save->flags, SANOBA_FLAG_MAX);
    offset += SANOBA_FLAG_MAX;
    out[offset++] = save->ev_kind;
    offset += put_name(out + offset, capacity - offset, save->bg);
    offset += put_name(out + offset, capacity - offset, save->ev);
    return offset;
}

bool sanoba_save_decode(sanoba_save_t *save, const uint8_t *data, size_t len)
{
    if (save == NULL || data == NULL || len < 10 + SANOBA_FLAG_MAX + 1) {
        return false;
    }
    if (rd16(data) != SANOBA_SAVE_VERSION) {
        return false;
    }
    memset(save, 0, sizeof(*save));
    save->scenario = rd16(data + 2);
    save->chunk = rd16(data + 4);
    save->node = rd32(data + 6);
    memcpy(save->flags, data + 10, SANOBA_FLAG_MAX);
    size_t offset = 10 + SANOBA_FLAG_MAX;
    save->ev_kind = data[offset++];
    offset += take_name(data, len, offset, save->bg, sizeof(save->bg));
    offset += take_name(data, len, offset, save->ev, sizeof(save->ev));
    return true;
}
