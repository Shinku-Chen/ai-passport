// main/steins_model.c —— 折行/分页、存档、解锁、世界线的纯逻辑实现。
#include "steins_model.h"

#include <stdio.h>
#include <string.h>

#include "steins_text_metrics.h"

uint16_t steins_adv_of(uint32_t cp)
{
    unsigned lo = 0, hi = steins_adv_count;
    while (lo < hi) {
        const unsigned mid = lo + (hi - lo) / 2u;
        if (steins_adv_table[mid].cp < cp) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    if (lo < steins_adv_count && steins_adv_table[lo].cp == cp) {
        return steins_adv_table[lo].adv16;
    }
    // 未知码位按全角算:宁可算宽(多折一行)也不能算窄导致溢出。
    return cp >= 0x80u ? 16u : 8u;
}

uint16_t steins_crc16(const uint8_t *data, size_t len)
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

size_t steins_utf8_next(const char *s, size_t len, size_t pos)
{
    if (!s || pos >= len) return len;
    const unsigned char c = (unsigned char)s[pos];
    size_t step = 1;
    if ((c & 0x80u) == 0u) step = 1;
    else if ((c & 0xE0u) == 0xC0u) step = 2;
    else if ((c & 0xF0u) == 0xE0u) step = 3;
    else if ((c & 0xF8u) == 0xF0u) step = 4;
    if (pos + step > len) step = 1;
    for (size_t i = 1; i < step; ++i) {
        if (((unsigned char)s[pos + i] & 0xC0u) != 0x80u) return pos + 1;
    }
    return pos + step;
}

uint32_t steins_utf8_decode(const char *s, size_t len, size_t *pos)
{
    if (!s || !pos || *pos >= len) return 0u;
    const size_t start = *pos;
    const size_t next = steins_utf8_next(s, len, start);
    const unsigned char c0 = (unsigned char)s[start];
    uint32_t cp = c0;
    const size_t n = next - start;
    if (n == 2) {
        cp = ((uint32_t)(c0 & 0x1Fu) << 6) | ((unsigned char)s[start + 1] & 0x3Fu);
    } else if (n == 3) {
        cp = ((uint32_t)(c0 & 0x0Fu) << 12) | (((unsigned char)s[start + 1] & 0x3Fu) << 6) |
             ((unsigned char)s[start + 2] & 0x3Fu);
    } else if (n == 4) {
        cp = ((uint32_t)(c0 & 0x07u) << 18) | (((unsigned char)s[start + 1] & 0x3Fu) << 12) |
             (((unsigned char)s[start + 2] & 0x3Fu) << 6) |
             ((unsigned char)s[start + 3] & 0x3Fu);
    }
    *pos = next;
    return cp;
}

bool steins_text_blank(const char *utf8)
{
    if (!utf8) return true;
    const size_t len = strlen(utf8);
    size_t pos = 0;
    while (pos < len) {
        const uint32_t cp = steins_utf8_decode(utf8, len, &pos);
        if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r') continue;
        if (cp == 0x3000u) continue;                    // 全角空格
        if (cp == 0xE000u || cp == 0xE03Fu) continue;    // 源的空格字形
        return false;
    }
    return true;
}

bool steins_text_prefix(const char *utf8, int chars, char *out, size_t cap)
{
    if (!utf8 || !out || cap == 0u) return false;
    const size_t len = strlen(utf8);
    size_t pos = 0;
    int taken = 0;
    while (pos < len && taken < chars) {
        const size_t next = steins_utf8_next(utf8, len, pos);
        if (next > pos) ++taken;
        pos = next;
    }
    if (pos >= cap) pos = cap - 1u;
    while (pos > 0u && pos < len && ((unsigned char)utf8[pos] & 0xC0u) == 0x80u) --pos;
    memcpy(out, utf8, pos);
    out[pos] = '\0';
    return true;
}

bool steins_text_has_pua(const char *utf8)
{
    if (!utf8) return false;
    const size_t len = strlen(utf8);
    size_t pos = 0;
    while (pos < len) {
        const uint32_t cp = steins_utf8_decode(utf8, len, &pos);
        if (cp >= 0xE000u && cp <= 0xF8FFu) return true;
    }
    return false;
}

// ---------------------------------------------------------------- 折行

static bool is_opening(uint32_t cp)
{
    switch (cp) {
    case 0xFF08: case 0x3014: case 0xFF3B: case 0xFF5B: case 0x3008: case 0x300A:
    case 0x300C: case 0x300E: case 0x3010: case 0xFF1C: case 0x2018: case 0x201C:
    case '(': case '[': case '{':
        return true;
    default:
        return false;
    }
}

static bool is_closing(uint32_t cp)
{
    switch (cp) {
    case 0x3001: case 0x3002: case 0xFF0C: case 0xFF0E: case 0xFF1A: case 0xFF1B:
    case 0xFF1F: case 0xFF01: case 0xFF09: case 0x3009: case 0x300B: case 0x300D:
    case 0x300F: case 0x3011: case 0xFF1E: case 0x2019: case 0x201D: case 0x3015:
    case 0xFF3D: case 0xFF5D: case ',': case '.': case '!': case '?': case ';':
    case ':': case '%': case ')': case ']': case '}':
        return true;
    default:
        return false;
    }
}

typedef struct {
    uint32_t cps[STEINS_TEXT_CHARS];
    uint16_t boff[STEINS_TEXT_CHARS + 1];
    uint16_t adv[STEINS_TEXT_CHARS];
    int count;
    bool truncated;   // 输入超过 STEINS_TEXT_CHARS:调用方必须当成错误处理
} wrap_input_t;

static wrap_input_t s_wrap;   // 只在持锁的界面任务里用;6.4KB 静态,不占任务栈

static void wrap_prepare(const char *utf8)
{
    const size_t len = strlen(utf8);
    size_t pos = 0;
    int count = 0;
    int newlines = 0;
    s_wrap.truncated = false;
    while (pos < len && count < STEINS_TEXT_CHARS) {
        const uint32_t cp = steins_utf8_decode(utf8, len, &pos);
        if (cp == 0x0Au) {
            // 源 cleanText 会把三个以上连续换行压成两个;这里同样收紧,
            // 免得单帧里出现整页空行(整页空白的帧由 skipBlank 跳过)。
            if (newlines >= 2) continue;
            ++newlines;
        } else {
            newlines = 0;
        }
        s_wrap.boff[count] = (uint16_t)(pos - (cp < 0x80u ? 1 : (cp < 0x800u ? 2 : (cp < 0x10000u ? 3 : 4))));
        s_wrap.cps[count] = cp;
        ++count;
    }
    s_wrap.boff[count] = (uint16_t)pos;
    s_wrap.count = count;
    if (pos < len) s_wrap.truncated = true;   // 超出工作区:宁可报错也不能悄悄吞字
}

// 一行能放多少码点:返回结束下标(不含)。budget_q4 = 宽度(px) * 16。
static int fit_line(int start, int budget_q4, int size_px)
{
    int used = 0;
    int i = start;
    while (i < s_wrap.count) {
        if (s_wrap.cps[i] == '\n') break;
        const int w = (int)steins_adv_of(s_wrap.cps[i]) * size_px / 16;
        if (used + w > budget_q4) break;
        used += w;
        ++i;
    }
    return i;
}

// 折出第 index 行(0 起);*end_out 返回该行结束下标(不含)。没有该行返回 false。
static bool wrap_line(int index, int budget_q4, int size_px, int *start_out, int *end_out)
{
    int line = 0;
    int i = 0;
    while (i < s_wrap.count) {
        int end;
        if (s_wrap.cps[i] == '\n') {
            end = i + 1;                       // 源里的硬换行:本行到此为止
            if (line == index) {
                *start_out = i;
                *end_out = i;
                return true;
            }
            ++line;
            i = end;
            continue;
        }
        end = fit_line(i, budget_q4, size_px);
        if (end <= i) end = i + 1;             // 至少放一个字,避免死循环
        // 标点禁则:行尾不留开括号;下一行不以闭标点开头(把上一个字一起带下去)。
        if (end < s_wrap.count && s_wrap.cps[end] != '\n') {
            int cut = end;
            while (cut > i + 1 && is_opening(s_wrap.cps[cut - 1])) --cut;
            if (is_closing(s_wrap.cps[end])) {
                while (cut > i + 1 && is_closing(s_wrap.cps[cut - 1])) --cut;
                if (cut > i + 1) --cut;
            }
            if (cut > i) end = cut;
        }
        if (line == index) {
            *start_out = i;
            *end_out = end;
            return true;
        }
        ++line;
        i = end;
    }
    return false;
}

static int wrap_line_count(int budget_q4, int size_px)
{
    int line = 0;
    while (true) {
        int s, e;
        if (!wrap_line(line, budget_q4, size_px, &s, &e)) break;
        ++line;
        if (line > STEINS_TEXT_CHARS + 2) break;
    }
    return line;
}

int steins_page_build(const char *utf8, int size_px, int width_px, int lines_per_page,
                      int page_index, steins_text_page_t *out)
{
    if (!utf8 || !out || size_px <= 0 || width_px <= 0 || lines_per_page <= 0) return 0;
    memset(out, 0, sizeof(*out));
    wrap_prepare(utf8);
    if (s_wrap.truncated) return 0;      // 结果不可信:让调用方显式处理
    const int budget_q4 = width_px * 16;
    if (s_wrap.count <= 0) {
        out->pages = 1;
        out->page = 0;
        out->offsets[0] = 0;
        return 1;
    }
    const int lines = wrap_line_count(budget_q4, size_px);
    if (lines <= 0) return 0;
    const int pages = (lines + lines_per_page - 1) / lines_per_page;
    int page = page_index;
    if (page < 0) page = 0;
    if (page >= pages) page = pages - 1;

    out->pages = (int16_t)pages;
    out->page = (int16_t)page;
    const int first = page * lines_per_page;
    const int last = first + lines_per_page < lines ? first + lines_per_page : lines;
    for (int l = first; l < last; ++l) {
        int s = 0, e = 0;
        if (!wrap_line(l, budget_q4, size_px, &s, &e)) break;
        if (l > first) {
            if (out->bytes + 1 >= (int)sizeof(out->text)) break;
            out->text[out->bytes++] = '\n';
        }
        const int line_out = out->bytes;
        const int b0 = s_wrap.boff[s];
        const int b1 = s_wrap.boff[e];
        for (int b = b0; b < b1; ++b) {
            if (out->bytes + 1 >= (int)sizeof(out->text)) break;
            out->text[out->bytes++] = utf8[b];
        }
        // 逐字符记录起点,供打字机按字符推进(绝不按字节截断多字节字)。
        for (int k = s; k < e; ++k) {
            if (out->chars >= STEINS_PAGE_CHARS) break;
            out->offsets[out->chars] =
                (uint16_t)(line_out + (s_wrap.boff[k] - s_wrap.boff[s]));
            ++out->chars;
        }
    }
    out->text[out->bytes] = '\0';
    out->offsets[out->chars] = (uint16_t)out->bytes;
    return pages;
}

// ---------------------------------------------------------------- 存档

size_t steins_save_encode(const steins_pos_t *pos, uint8_t *out, size_t cap)
{
    if (!pos || !out || cap < STEINS_SAVE_BYTES) return 0u;
    memset(out, 0, STEINS_SAVE_BYTES);
    out[0] = 'S';
    out[1] = 'G';
    out[2] = STEINS_SAVE_VERSION;
    out[3] = pos->mode;
    out[4] = (uint8_t)(pos->node & 0xFFu);
    out[5] = (uint8_t)(pos->node >> 8);
    out[6] = (uint8_t)(pos->cursor & 0xFFu);
    out[7] = (uint8_t)((pos->cursor >> 8) & 0xFFu);
    out[8] = (uint8_t)((pos->cursor >> 16) & 0xFFu);
    out[9] = (uint8_t)((pos->cursor >> 24) & 0xFFu);
    out[10] = pos->flags;
    const uint16_t crc = steins_crc16(out, 11u);
    out[11] = (uint8_t)(crc & 0xFFu);
    out[12] = (uint8_t)(crc >> 8);
    return STEINS_SAVE_BYTES;
}

bool steins_save_decode(steins_pos_t *pos, const uint8_t *data, size_t len)
{
    if (!pos || !data || len < STEINS_SAVE_BYTES) return false;
    if (data[0] != 'S' || data[1] != 'G') return false;
    if (data[2] != STEINS_SAVE_VERSION) return false;
    if (data[3] != STEINS_MODE_STORY && data[3] != STEINS_MODE_CHAPTER) return false;
    const uint16_t crc = steins_crc16(data, 11u);
    if (data[11] != (uint8_t)(crc & 0xFFu) || data[12] != (uint8_t)(crc >> 8)) return false;
    pos->mode = data[3];
    pos->node = (uint16_t)(data[4] | ((uint16_t)data[5] << 8));
    pos->cursor = (uint32_t)data[6] | ((uint32_t)data[7] << 8) |
                  ((uint32_t)data[8] << 16) | ((uint32_t)data[9] << 24);
    pos->flags = data[10];
    return true;
}

// ---------------------------------------------------------------- 解锁

void steins_unlock_init(steins_unlock_t *unlock)
{
    if (!unlock) return;
    memset(unlock, 0, sizeof(*unlock));
    unlock->version = STEINS_UNLOCK_VERSION;
}

static bool bit_get(const uint8_t *bits, unsigned index)
{
    return (bits[index >> 3] >> (index & 7u)) & 1u;
}

static bool bit_set(uint8_t *bits, unsigned index)
{
    const uint8_t mask = (uint8_t)(1u << (index & 7u));
    if (bits[index >> 3] & mask) return false;
    bits[index >> 3] |= mask;
    return true;
}

bool steins_unlock_has_ending(const steins_unlock_t *u, uint8_t index)
{
    return u && index < STEINS_MAX_ENDINGS && bit_get(u->endings, index);
}

bool steins_unlock_set_ending(steins_unlock_t *u, uint8_t index)
{
    return u && index < STEINS_MAX_ENDINGS && bit_set(u->endings, index);
}

bool steins_unlock_has_chapter(const steins_unlock_t *u, uint8_t index)
{
    return u && index < STEINS_MAX_CHAPTERS && bit_get(u->chapters, index);
}

bool steins_unlock_set_chapter(steins_unlock_t *u, uint8_t index)
{
    return u && index < STEINS_MAX_CHAPTERS && bit_set(u->chapters, index);
}

bool steins_unlock_has_gallery(const steins_unlock_t *u, uint8_t index)
{
    return u && index < STEINS_MAX_GALLERY && bit_get(u->gallery, index);
}

bool steins_unlock_set_gallery(steins_unlock_t *u, uint8_t index)
{
    return u && index < STEINS_MAX_GALLERY && bit_set(u->gallery, index);
}

bool steins_unlock_true_end(const steins_unlock_t *u)
{
    return u && (u->flags & 1u) != 0u;
}

void steins_unlock_set_true_end(steins_unlock_t *u)
{
    if (u) u->flags |= 1u;
}

bool steins_unlock_merge(steins_unlock_t *u, const steins_unlock_t *other)
{
    if (!u || !other) return false;
    bool changed = false;
    for (unsigned i = 0; i < STEINS_MAX_ENDINGS; ++i) {
        if (bit_get(other->endings, i) && bit_set(u->endings, i)) changed = true;
    }
    for (unsigned i = 0; i < STEINS_MAX_CHAPTERS; ++i) {
        if (bit_get(other->chapters, i) && bit_set(u->chapters, i)) changed = true;
    }
    for (unsigned i = 0; i < STEINS_MAX_GALLERY; ++i) {
        if (bit_get(other->gallery, i) && bit_set(u->gallery, i)) changed = true;
    }
    if ((other->flags & 1u) && !(u->flags & 1u)) {
        u->flags |= 1u;
        changed = true;
    }
    return changed;
}

size_t steins_unlock_encode(const steins_unlock_t *u, uint8_t *out, size_t cap)
{
    if (!u || !out || cap < STEINS_UNLOCK_BYTES) return 0u;
    memset(out, 0, STEINS_UNLOCK_BYTES);
    out[0] = 'S';
    out[1] = 'U';
    out[2] = STEINS_UNLOCK_VERSION;
    out[3] = u->endings[0];
    out[4] = u->endings[1];
    out[5] = u->chapters[0];
    out[6] = u->chapters[1];
    out[7] = u->flags;
    memcpy(out + 8, u->gallery, 16u);
    const uint16_t crc = steins_crc16(out, 24u);
    out[24] = (uint8_t)(crc & 0xFFu);
    out[25] = (uint8_t)(crc >> 8);
    return STEINS_UNLOCK_BYTES;
}

bool steins_unlock_decode(steins_unlock_t *u, const uint8_t *data, size_t len)
{
    if (!u || !data || len < STEINS_UNLOCK_BYTES) return false;
    if (data[0] != 'S' || data[1] != 'U' || data[2] != STEINS_UNLOCK_VERSION) return false;
    const uint16_t crc = steins_crc16(data, 24u);
    if (data[24] != (uint8_t)(crc & 0xFFu) || data[25] != (uint8_t)(crc >> 8)) return false;
    memset(u, 0, sizeof(*u));
    u->version = STEINS_UNLOCK_VERSION;
    u->endings[0] = data[3];
    u->endings[1] = data[4];
    u->chapters[0] = data[5];
    u->chapters[1] = data[6];
    u->flags = data[7];
    memcpy(u->gallery, data + 8, 16u);
    return true;
}

// ---------------------------------------------------------------- 世界线

int32_t steins_worldline_rate(const steins_pack_t *pack, uint16_t node, uint32_t cursor)
{
    steins_wline_t row;
    if (!steins_pack_wline(pack, node, &row)) return 1130426;   // 源初始世界线 1.130426
    int32_t rate = row.rate;
    for (uint8_t i = 0; i < row.count; ++i) {
        steins_wchange_t change;
        if (!steins_pack_wchange(pack, (uint16_t)(row.first + i), &change)) break;
        if (change.cursor > cursor) break;
        rate = change.rate;
    }
    return rate;
}

void steins_worldline_text(int32_t rate, char *out, size_t cap)
{
    if (!out || cap < 10u) return;
    const bool negative = rate < 0;
    uint32_t value = negative ? (uint32_t)(-(rate + 1)) + 1u : (uint32_t)rate;
    const uint32_t whole = value / 1000000u;
    const uint32_t frac = value % 1000000u;
    int n = 0;
    if (negative) out[n++] = '-';
    n += snprintf(out + n, cap - (size_t)n, "%u.%06u", (unsigned)whole, (unsigned)frac);
    out[n < (int)cap ? n : (int)cap - 1] = '\0';
}

// ---------------------------------------------------------------- 章节标题卡

uint16_t steins_chapter_card_asset(const steins_pack_t *pack, const char *title)
{
    if (!title) return STEINS_NONE;
    int number = 0;
    int digits = 0;
    while (digits < 3 && title[digits] >= '0' && title[digits] <= '9') {
        number = number * 10 + (title[digits] - '0');
        ++digits;
    }
    if (digits == 0 || number <= 0) return STEINS_NONE;
    char key[24];
    if (number >= 1 && number <= 9) {
        snprintf(key, sizeof(key), "asset_card_%d", 486 + number);
        return (uint16_t)steins_pack_meta_int(pack, key, (int)STEINS_NONE);
    }
    if (number == 11) {
        return (uint16_t)steins_pack_meta_int(pack, "asset_card_498", (int)STEINS_NONE);
    }
    if (number == 10) {
        // 源 chapter-cards.js:标题里含"因果关系"用 496,否则 497。
        static const char CAUSAL[] = "\xE5\x9B\xA0\xE6\x9E\x9C\xE5\x85\xB3\xE7\xB3\xBB";
        const bool causal = strstr(title, CAUSAL) != NULL;
        return (uint16_t)steins_pack_meta_int(
            pack, causal ? "asset_card_496" : "asset_card_497", (int)STEINS_NONE);
    }
    return STEINS_NONE;
}

// ---------------------------------------------------------------- 手机 checkpoint

void steins_checkpoint_init(steins_checkpoint_t *chk)
{
    if (!chk) return;
    memset(chk, 0, sizeof(*chk));
    chk->version = STEINS_CHK_VERSION;
    chk->mode = STEINS_MODE_STORY;
    chk->last_node = 0xFFFFu;
    chk->last_at = 0xFFFFFFFFu;
    chk->mail_life_version = 1;
    chk->opening_done = 0;
    memset(chk->pres_key, 0xFF, sizeof(chk->pres_key));
}

static bool bit_access(uint8_t *bits, uint16_t id, bool set, bool value)
{
    if (id >= STEINS_MAIL_IDS) return false;
    const uint8_t mask = (uint8_t)(1u << (id & 7u));
    const bool old = (bits[id >> 3] & mask) != 0u;
    if (!set) return old;
    if (value) bits[id >> 3] |= mask;
    else bits[id >> 3] &= (uint8_t)~mask;
    return old;
}

bool steins_checkpoint_set_read(steins_checkpoint_t *chk, uint16_t id, bool value)
{
    return chk && bit_access(chk->read, id, true, value);
}

bool steins_checkpoint_read(const steins_checkpoint_t *chk, uint16_t id)
{
    return chk && bit_access((uint8_t *)chk->read, id, false, false);
}

bool steins_checkpoint_set_replied(steins_checkpoint_t *chk, uint16_t id, bool value)
{
    return chk && bit_access(chk->replied, id, true, value);
}

bool steins_checkpoint_replied(const steins_checkpoint_t *chk, uint16_t id)
{
    return chk && bit_access((uint8_t *)chk->replied, id, false, false);
}

bool steins_checkpoint_set_unread(steins_checkpoint_t *chk, uint16_t id, bool value)
{
    return chk && bit_access(chk->unread, id, true, value);
}

bool steins_checkpoint_unread(const steins_checkpoint_t *chk, uint16_t id)
{
    return chk && bit_access((uint8_t *)chk->unread, id, false, false);
}

bool steins_checkpoint_push_inbox(steins_checkpoint_t *chk, uint16_t id)
{
    if (!chk || id >= STEINS_MAIL_IDS) return false;
    if (chk->inbox_count >= STEINS_CHK_MAX_INBOX) return false;
    chk->inbox[chk->inbox_count++] = id;
    return true;
}

bool steins_checkpoint_push_sent(steins_checkpoint_t *chk, uint16_t id)
{
    if (!chk || id >= STEINS_MAIL_IDS) return false;
    if (chk->sent_count >= STEINS_CHK_MAX_SENT) return false;
    chk->sent[chk->sent_count++] = id;
    return true;
}

// ---- 紧凑编码:LEB128 varint + 定长位图,CRC16 收尾
static size_t put_u32(uint8_t *out, size_t cap, size_t at, uint32_t value)
{
    while (value >= 0x80u) {
        if (at >= cap) return 0;
        out[at++] = (uint8_t)((value & 0x7Fu) | 0x80u);
        value >>= 7;
    }
    if (at >= cap) return 0;
    out[at++] = (uint8_t)value;
    return at;
}

static bool get_u32(const uint8_t *data, size_t len, size_t *at, uint32_t *value)
{
    uint32_t result = 0;
    int shift = 0;
    while (*at < len && shift <= 28) {
        const uint8_t byte = data[(*at)++];
        result |= (uint32_t)(byte & 0x7Fu) << shift;
        if ((byte & 0x80u) == 0u) {
            *value = result;
            return true;
        }
        shift += 7;
    }
    return false;
}

// 有序 id 列表:每个 id 10 bit(id 空间 646),保序、无损、比 varint 更省。
static size_t put_ids(uint8_t *out, size_t cap, size_t at, const uint16_t *ids, uint16_t count)
{
    const size_t bytes = ((size_t)count * 10u + 7u) / 8u;
    if (at + bytes > cap) return 0;
    uint32_t acc = 0;
    int bits = 0;
    size_t pos = at;
    for (uint16_t i = 0; i < count; ++i) {
        acc |= (uint32_t)(ids[i] & 0x3FFu) << bits;
        bits += 10;
        while (bits >= 8) {
            out[pos++] = (uint8_t)(acc & 0xFFu);
            acc >>= 8;
            bits -= 8;
        }
    }
    if (bits > 0) out[pos++] = (uint8_t)(acc & 0xFFu);
    return pos;
}

static bool get_ids(const uint8_t *data, size_t len, size_t *at, uint16_t *ids, uint16_t count)
{
    const size_t bytes = ((size_t)count * 10u + 7u) / 8u;
    if (*at + bytes > len) return false;
    uint32_t acc = 0;
    int bits = 0;
    size_t pos = *at;
    for (uint16_t i = 0; i < count; ++i) {
        while (bits < 10) {
            acc |= (uint32_t)data[pos++] << bits;
            bits += 8;
        }
        const uint32_t id = acc & 0x3FFu;
        if (id >= STEINS_MAIL_IDS) return false;
        ids[i] = (uint16_t)id;
        acc >>= 10;
        bits -= 10;
    }
    *at = *at + bytes;
    return true;
}

size_t steins_checkpoint_encode(const steins_checkpoint_t *chk, uint8_t *out, size_t cap)
{
    if (!chk || !out || cap < 8u) return 0;
    if (chk->inbox_count > STEINS_CHK_MAX_INBOX || chk->sent_count > STEINS_CHK_MAX_SENT ||
        chk->owner_count > STEINS_CHK_MAX_OWNERS || chk->open_count > STEINS_CHK_MAX_OPEN ||
        chk->pres_count > STEINS_CHK_MAX_PRES) {
        return 0;
    }
    size_t at = 0;
    out[at++] = 'S';
    out[at++] = 'M';
    out[at++] = STEINS_CHK_VERSION;
    out[at++] = chk->mode;
    at = put_u32(out, cap, at, chk->node);
    if (at == 0) return 0;
    at = put_u32(out, cap, at, chk->cursor);
    if (at == 0) return 0;
    at = put_u32(out, cap, at, chk->last_node);
    if (at == 0) return 0;
    at = put_u32(out, cap, at, chk->last_at);
    if (at == 0) return 0;
    at = put_u32(out, cap, at, chk->inbox_count);
    if (at == 0) return 0;
    at = put_ids(out, cap, at, chk->inbox, chk->inbox_count);
    if (at == 0) return 0;
    at = put_u32(out, cap, at, chk->sent_count);
    if (at == 0) return 0;
    at = put_ids(out, cap, at, chk->sent, chk->sent_count);
    if (at == 0) return 0;
    const size_t bitmap = STEINS_MOBILE_BITMAP_BYTES;
    if (at + bitmap * 3u + 40u > cap) return 0;
    for (size_t i = 0; i < bitmap; ++i) {
        out[at++] = chk->read[i];
        out[at++] = chk->replied[i];
        out[at++] = chk->unread[i];
    }
    at = put_u32(out, cap, at, chk->owner_count);
    if (at == 0) return 0;
    for (uint8_t i = 0; i < chk->owner_count; ++i) {
        at = put_u32(out, cap, at, chk->owner_id[i]);
        if (at == 0) return 0;
        at = put_u32(out, cap, at, chk->owner_val[i]);
        if (at == 0) return 0;
    }
    at = put_u32(out, cap, at, chk->open_count);
    if (at == 0) return 0;
    for (uint8_t i = 0; i < chk->open_count; ++i) {
        at = put_u32(out, cap, at, chk->open_id[i]);
        if (at == 0) return 0;
        at = put_u32(out, cap, at, chk->open_val[i]);
        if (at == 0) return 0;
    }
    for (int i = 0; i < 5; ++i) {
        at = put_u32(out, cap, at, chk->channels[i]);
        if (at == 0) return 0;
    }
    at = put_u32(out, cap, at, chk->call_present);
    if (at == 0) return 0;
    if (chk->call_present) {
        at = put_u32(out, cap, at, chk->call_site);
        if (at == 0) return 0;
        at = put_u32(out, cap, at, chk->call_owner);
        if (at == 0) return 0;
        at = put_u32(out, cap, at, chk->call_kind);
        if (at == 0) return 0;
        at = put_u32(out, cap, at, chk->call_waiting);
        if (at == 0) return 0;
        at = put_u32(out, cap, at, chk->call_node);
        if (at == 0) return 0;
        at = put_u32(out, cap, at, chk->call_at);
        if (at == 0) return 0;
    }
    at = put_u32(out, cap, at, chk->call_return_present);
    if (at == 0) return 0;
    if (chk->call_return_present) {
        at = put_u32(out, cap, at, chk->call_return_node);
        if (at == 0) return 0;
        at = put_u32(out, cap, at, chk->call_return_cursor);
        if (at == 0) return 0;
    }
    at = put_u32(out, cap, at, chk->pres_count);
    if (at == 0) return 0;
    for (uint8_t i = 0; i < chk->pres_count; ++i) {
        at = put_u32(out, cap, at, chk->pres_id[i]);
        if (at == 0) return 0;
        at = put_u32(out, cap, at, chk->pres_kind[i]);
        if (at == 0) return 0;
        if (chk->pres_key[i] != 0xFFu && chk->pres_key[i] >= STEINS_CHK_MAX_PRES_KEYS) return 0;
        at = put_u32(out, cap, at, chk->pres_owner[i]);
        if (at == 0) return 0;
    }
    // pending key：6 bit/项（64 项=48 B），0x3F 为无 key；struct 仍用 0xFF。
    {
        uint32_t acc = 0;
        int bits = 0;
        for (uint8_t i = 0; i < chk->pres_count; ++i) {
            const uint8_t key = (chk->pres_key[i] == 0xFFu) ? 0x3Fu : chk->pres_key[i];
            acc |= (uint32_t)key << bits;
            bits += 6;
            while (bits >= 8) {
                if (at >= cap) return 0;
                out[at++] = (uint8_t)(acc & 0xFFu);
                acc >>= 8;
                bits -= 8;
            }
        }
        if (bits > 0) {
            if (at >= cap) return 0;
            out[at++] = (uint8_t)(acc & 0xFFu);
        }
    }
    for (int i = 0; i < 8; ++i) {
        if (at >= cap) return 0;
        out[at++] = chk->pres_read_bits[i];
    }
    at = put_u32(out, cap, at, chk->source_site);
    if (at == 0) return 0;
    at = put_u32(out, cap, at, chk->mail_life_version);
    if (at == 0) return 0;
    at = put_u32(out, cap, at, chk->opening_done);
    if (at == 0) return 0;
    at = put_u32(out, cap, at, chk->deferred_present);
    if (at == 0) return 0;
    if (chk->deferred_present) {
        at = put_u32(out, cap, at, chk->deferred_root);
        if (at == 0) return 0;
        at = put_u32(out, cap, at, chk->deferred_id);
        if (at == 0) return 0;
        at = put_u32(out, cap, at, chk->deferred_read);
        if (at == 0) return 0;
    }
    at = put_u32(out, cap, at, chk->keyphone_present);
    if (at == 0) return 0;
    if (chk->keyphone_present) {
        at = put_u32(out, cap, at, chk->keyphone_root);
        if (at == 0) return 0;
        at = put_u32(out, cap, at, chk->keyphone_owner);
        if (at == 0) return 0;
    }
    at = put_u32(out, cap, at, chk->endings);
    if (at == 0) return 0;
    if (at + 2u > cap) return 0;
    const uint16_t crc = steins_crc16(out, at);
    out[at++] = (uint8_t)(crc & 0xFFu);
    out[at++] = (uint8_t)(crc >> 8);
    return at;
}

bool steins_checkpoint_decode(steins_checkpoint_t *chk, const uint8_t *data, size_t len)
{
    if (!chk || !data || len < 6u) return false;
    if (data[0] != 'S' || data[1] != 'M' || data[2] != STEINS_CHK_VERSION) return false;
    const uint16_t crc = steins_crc16(data, len - 2u);
    if (data[len - 2u] != (uint8_t)(crc & 0xFFu) || data[len - 1u] != (uint8_t)(crc >> 8)) {
        return false;
    }
    steins_checkpoint_init(chk);
    size_t at = 3;
    uint32_t v = 0;
    chk->mode = data[at++];
    if (chk->mode != STEINS_MODE_STORY && chk->mode != STEINS_MODE_CHAPTER) return false;
    const size_t body = len - 2u;
    if (!get_u32(data, body, &at, &v) || v > 0xFFFFu) return false;
    chk->node = (uint16_t)v;
    if (!get_u32(data, body, &at, &v)) return false;
    chk->cursor = v;
    if (!get_u32(data, body, &at, &v) || v > 0xFFFFu) return false;
    chk->last_node = (uint16_t)v;
    if (!get_u32(data, body, &at, &v)) return false;
    chk->last_at = v;
    if (!get_u32(data, body, &at, &v) || v > STEINS_CHK_MAX_INBOX) return false;
    chk->inbox_count = (uint16_t)v;
    if (!get_ids(data, body, &at, chk->inbox, chk->inbox_count)) return false;
    if (!get_u32(data, body, &at, &v) || v > STEINS_CHK_MAX_SENT) return false;
    chk->sent_count = (uint16_t)v;
    if (!get_ids(data, body, &at, chk->sent, chk->sent_count)) return false;
    const size_t bitmap = STEINS_MOBILE_BITMAP_BYTES;
    if (at + bitmap * 3u > body) return false;
    for (size_t i = 0; i < bitmap; ++i) {
        chk->read[i] = data[at++];
        chk->replied[i] = data[at++];
        chk->unread[i] = data[at++];
    }
    if (!get_u32(data, body, &at, &v) || v > STEINS_CHK_MAX_OWNERS) return false;
    chk->owner_count = (uint8_t)v;
    for (uint8_t i = 0; i < chk->owner_count; ++i) {
        if (!get_u32(data, body, &at, &v) || v >= STEINS_MAIL_IDS) return false;
        chk->owner_id[i] = (uint16_t)v;
        if (!get_u32(data, body, &at, &v) || v > 255u) return false;
        chk->owner_val[i] = (uint8_t)v;
    }
    if (!get_u32(data, body, &at, &v) || v > STEINS_CHK_MAX_OPEN) return false;
    chk->open_count = (uint8_t)v;
    for (uint8_t i = 0; i < chk->open_count; ++i) {
        if (!get_u32(data, body, &at, &v) || v >= STEINS_MAIL_IDS) return false;
        chk->open_id[i] = (uint16_t)v;
        if (!get_u32(data, body, &at, &v) || v > 255u) return false;
        chk->open_val[i] = (uint8_t)v;
    }
    for (int i = 0; i < 5; ++i) {
        if (!get_u32(data, body, &at, &v) || v > 0xFFFFu) return false;
        chk->channels[i] = (uint16_t)v;
    }
    if (!get_u32(data, body, &at, &v) || v > 1u) return false;
    chk->call_present = (uint8_t)v;
    if (chk->call_present) {
        if (!get_u32(data, body, &at, &v) || v > 0xFFFFu) return false;
        chk->call_site = (uint16_t)v;
        if (!get_u32(data, body, &at, &v) || v > 255u) return false;
        chk->call_owner = (uint8_t)v;
        if (!get_u32(data, body, &at, &v) || v > 255u) return false;
        chk->call_kind = (uint8_t)v;
        if (!get_u32(data, body, &at, &v) || v > 255u) return false;
        chk->call_waiting = (uint8_t)v;
        if (!get_u32(data, body, &at, &v) || v > 0xFFFFu) return false;
        chk->call_node = (uint16_t)v;
        if (!get_u32(data, body, &at, &v)) return false;
        chk->call_at = v;
    }
    if (!get_u32(data, body, &at, &v) || v > 1u) return false;
    chk->call_return_present = (uint8_t)v;
    if (chk->call_return_present) {
        if (!get_u32(data, body, &at, &v) || v > 0xFFFFu) return false;
        chk->call_return_node = (uint16_t)v;
        if (!get_u32(data, body, &at, &v)) return false;
        chk->call_return_cursor = v;
    }
    if (!get_u32(data, body, &at, &v) || v > STEINS_CHK_MAX_PRES) return false;
    chk->pres_count = (uint8_t)v;
    for (uint8_t i = 0; i < chk->pres_count; ++i) {
        if (!get_u32(data, body, &at, &v) || v >= STEINS_MAIL_IDS) return false;
        chk->pres_id[i] = (uint16_t)v;
        if (!get_u32(data, body, &at, &v) || v > 255u) return false;
        chk->pres_kind[i] = (uint8_t)v;
        if (!get_u32(data, body, &at, &v) || v > 255u) return false;
        chk->pres_owner[i] = (uint8_t)v;
    }
    {
        const size_t key_bytes = ((size_t)chk->pres_count * 6u + 7u) / 8u;
        if (at + key_bytes > body) return false;
        uint32_t acc = 0;
        int bits = 0;
        size_t kp = at;
        for (uint8_t i = 0; i < chk->pres_count; ++i) {
            while (bits < 6) {
                acc |= (uint32_t)data[kp++] << bits;
                bits += 8;
            }
            const uint8_t key = (uint8_t)(acc & 0x3Fu);
            acc >>= 6;
            bits -= 6;
            if (key == 0x3Fu) {
                chk->pres_key[i] = 0xFFu;   // wire 63 = NONE
            } else if (key >= STEINS_CHK_MAX_PRES_KEYS) {
                // wire 56..62 不是合法 key；不能当 NONE 或未知放过。
                return false;
            } else {
                chk->pres_key[i] = key;
            }
        }
        at += key_bytes;
    }
    if (at + 8u > body) return false;
    for (int i = 0; i < 8; ++i) chk->pres_read_bits[i] = data[at++];
    if (chk->pres_read_bits[7] != 0u) return false;   // byte7 保留，56..63 不合法
    if (!get_u32(data, body, &at, &v) || v > 0xFFFFu) return false;
    chk->source_site = (uint16_t)v;
    if (!get_u32(data, body, &at, &v) || v > 255u) return false;
    chk->mail_life_version = (uint8_t)v;
    if (!get_u32(data, body, &at, &v) || v > 1u) return false;
    chk->opening_done = (uint8_t)v;
    if (!get_u32(data, body, &at, &v) || v > 1u) return false;
    chk->deferred_present = (uint8_t)v;
    if (chk->deferred_present) {
        if (!get_u32(data, body, &at, &v) || v > 0xFFFFu) return false;
        chk->deferred_root = (uint16_t)v;
        if (!get_u32(data, body, &at, &v) || v >= STEINS_MAIL_IDS) return false;
        chk->deferred_id = (uint16_t)v;
        if (!get_u32(data, body, &at, &v) || v > 1u) return false;
        chk->deferred_read = (uint8_t)v;
    }
    if (!get_u32(data, body, &at, &v) || v > 1u) return false;
    chk->keyphone_present = (uint8_t)v;
    if (chk->keyphone_present) {
        if (!get_u32(data, body, &at, &v) || v > 0xFFFFu) return false;
        chk->keyphone_root = (uint16_t)v;
        if (!get_u32(data, body, &at, &v) || v > 255u) return false;
        chk->keyphone_owner = (uint8_t)v;
    }
    if (!get_u32(data, body, &at, &v) || v > 0xFFFFu) return false;
    chk->endings = (uint16_t)v;
    return at == body;
}

bool steins_checkpoint_equal(const steins_checkpoint_t *a, const steins_checkpoint_t *b)
{
    if (!a || !b) return false;
    if (a->version != b->version || a->mode != b->mode || a->node != b->node ||
        a->cursor != b->cursor || a->last_node != b->last_node ||
        a->last_at != b->last_at || a->inbox_count != b->inbox_count ||
        a->sent_count != b->sent_count || a->owner_count != b->owner_count ||
        a->open_count != b->open_count || a->pres_count != b->pres_count ||
        a->call_present != b->call_present ||
        a->call_return_present != b->call_return_present ||
        a->deferred_present != b->deferred_present ||
        a->keyphone_present != b->keyphone_present || a->endings != b->endings ||
        a->source_site != b->source_site ||
        a->mail_life_version != b->mail_life_version ||
        a->opening_done != b->opening_done ||
        memcmp(a->pres_read_bits, b->pres_read_bits, sizeof(a->pres_read_bits)) != 0) {
        return false;
    }
    return memcmp(a->inbox, b->inbox, sizeof(uint16_t) * a->inbox_count) == 0 &&
           memcmp(a->sent, b->sent, sizeof(uint16_t) * a->sent_count) == 0 &&
           memcmp(a->read, b->read, sizeof(a->read)) == 0 &&
           memcmp(a->replied, b->replied, sizeof(a->replied)) == 0 &&
           memcmp(a->unread, b->unread, sizeof(a->unread)) == 0 &&
           memcmp(a->channels, b->channels, sizeof(a->channels)) == 0 &&
           memcmp(a->owner_id, b->owner_id, sizeof(uint16_t) * a->owner_count) == 0 &&
           memcmp(a->owner_val, b->owner_val, a->owner_count) == 0 &&
           memcmp(a->open_id, b->open_id, sizeof(uint16_t) * a->open_count) == 0 &&
           memcmp(a->open_val, b->open_val, a->open_count) == 0 &&
           memcmp(a->pres_id, b->pres_id, sizeof(uint16_t) * a->pres_count) == 0 &&
           memcmp(a->pres_kind, b->pres_kind, a->pres_count) == 0 &&
           memcmp(a->pres_owner, b->pres_owner, a->pres_count) == 0 &&
           memcmp(a->pres_key, b->pres_key, a->pres_count) == 0 &&
           a->call_site == b->call_site && a->call_owner == b->call_owner &&
           a->call_kind == b->call_kind && a->call_waiting == b->call_waiting &&
           a->call_node == b->call_node && a->call_at == b->call_at &&
           a->call_return_node == b->call_return_node &&
           a->call_return_cursor == b->call_return_cursor &&
           a->deferred_root == b->deferred_root &&
           a->deferred_id == b->deferred_id &&
           a->deferred_read == b->deferred_read &&
           a->keyphone_root == b->keyphone_root &&
           a->keyphone_owner == b->keyphone_owner;
}

// ---------------------------------------------------------------- v2 存档迁移
size_t steins_checkpoint_save_encode(const steins_checkpoint_t *chk, uint8_t *out, size_t cap)
{
    return steins_checkpoint_encode(chk, out, cap);
}

bool steins_checkpoint_save_decode(steins_checkpoint_t *chk, const uint8_t *data, size_t len,
                                   bool *migrated_from_v1)
{
    if (migrated_from_v1) *migrated_from_v1 = false;
    if (!chk || !data || len < 3u) return false;
    if (data[0] == 'S' && data[1] == 'M') {
        return steins_checkpoint_decode(chk, data, len);
    }
    if (data[0] == 'S' && data[1] == 'G') {
        steins_pos_t pos;
        if (!steins_save_decode(&pos, data, len)) return false;
        steins_checkpoint_init(chk);
        chk->mode = pos.mode;
        chk->node = pos.node;
        chk->cursor = pos.cursor;
        chk->last_node = 0xFFFFu;
        chk->last_at = 0xFFFFFFFFu;
        chk->opening_done = pos.mode == STEINS_MODE_CHAPTER ? 1u : 0u;
        if (migrated_from_v1) *migrated_from_v1 = true;
        return true;
    }
    return false;
}

