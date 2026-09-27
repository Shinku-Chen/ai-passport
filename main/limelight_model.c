// main/limelight_model.c —— 阅读器纯逻辑(不依赖 ESP-IDF / LVGL)。
//
// 分页与禁则的算法沿用《星空列车》版 main/starry_model.c(那一版已在真机验收过),
// 只把"章节/场景/对白"三级游标换成平铺 id。
#include "limelight_model.h"

#include <stdio.h>
#include <string.h>

#define LIME_SAVE_MAGIC 0x4Cu        // 'L'
#define LIME_SAVE_VERSION 1u
#define LIME_SAVE_FIXED 10u          // magic/version/id(4)/page(2)/choice_len

// 诊断:只在选项步打印一行(调试选项页用,后续可删)。
#include <stdio.h>
#define LOG_CHOICE(id, flags, found, count)                                                do {                                                                                       printf("[lime] choice id=%u flags=0x%02X found=%d count=%u\n", (unsigned)(id),                (unsigned)(flags), (int)(found), (unsigned)(count));                        } while (0)

int lime_char_units(uint32_t cp)
{
    // East Asian Width:全角 2 单位、半角 1 单位。字形按同一张表算推进量。
    if (cp == 0x00B7u) return 2;                       // 间隔号 ·:中文语境按全角
    if (cp < 0x1100u) return 1;                        // ASCII / 拉丁
    if (cp >= 0xFF61u && cp <= 0xFF9Fu) return 1;      // 半角片假名
    static const uint32_t wide[][2] = {
        { 0x1100u, 0x115Fu }, { 0x2E80u, 0x303Eu }, { 0x3041u, 0x33FFu },
        { 0x3400u, 0x4DBFu }, { 0x4E00u, 0x9FFFu }, { 0xA000u, 0xA4CFu },
        { 0xAC00u, 0xD7A3u }, { 0xF900u, 0xFAFFu }, { 0xFE30u, 0xFE6Fu },
        { 0xFF00u, 0xFF60u }, { 0xFFE0u, 0xFFE6u }, { 0x20000u, 0x3FFFDu },
        // Ambiguous 但在中文语境下排成全角:漏掉会让"——"这种串被算成半角,
        // 一行塞进二十几个,画出超过文本框宽度的长行。
        { 0x2010u, 0x2027u }, { 0x2030u, 0x205Eu }, { 0x2100u, 0x21FFu },
        { 0x2600u, 0x27BFu },
    };
    for (size_t i = 0; i < sizeof(wide) / sizeof(wide[0]); ++i) {
        if (cp >= wide[i][0] && cp <= wide[i][1]) return 2;
    }
    return 1;
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
        *pos = i + 1;               // 非法首字节:跳 1 字节,保证不死循环
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

static bool no_line_start(uint32_t cp)
{
    switch (cp) {
    case 0x3001: case 0x3002: case 0xFF0C: case 0xFF0E: case 0xFF01:
    case 0xFF1F: case 0xFF1A: case 0xFF1B: case 0xFF09: case 0x300D:
    case 0x300F: case 0x3011: case 0x300B: case 0x3009: case 0x2026:
    case 0x2015: case 0x2014: case 0x30FB: case 0xFF5D: case 0x3015:
        return true;
    default:
        return false;
    }
}

// 换行核心:按禁则切行,再按 lines_per_group 行一组记录组起点。
// lines_per_group = 1 即逐行(界面绘制用),= N 即每页 N 行(分页用)。
static int wrap_text(const char *utf8, int units_per_line, int lines_per_group,
                     uint32_t *offsets, int max_offsets)
{
    if (!utf8) utf8 = "";
    if (units_per_line < 2) units_per_line = 2;
    if (lines_per_group < 1) lines_per_group = 1;

    const size_t len = strlen(utf8);
    int groups = 1;
    int line_units = 0;
    int line_index = 0;
    size_t pos = 0;
    if (offsets && max_offsets > 0) offsets[0] = 0;

    while (pos < len) {
        const size_t before = pos;
        const uint32_t cp = utf8_next(utf8, len, &pos);
        size_t brk_at = 0;
        if (cp == '\n') {
            line_units = 0;
            line_index++;
            brk_at = pos;                    // 断在换行符之后
        } else {
            const int units = lime_char_units(cp);
            if (line_units + units > units_per_line && line_units > 0) {
                if (no_line_start(cp)) {
                    // 禁则标点挂在本行行尾(允许轻微超宽),断点落在它之后。
                    line_units = 0;
                    line_index++;
                    brk_at = pos;
                } else {
                    line_units = units;
                    line_index++;
                    brk_at = before;
                }
            } else {
                line_units += units;
            }
        }
        if (brk_at != 0 && line_index >= lines_per_group) {
            line_index = 0;
            groups++;
            if (offsets && groups - 1 < max_offsets) offsets[groups - 1] = (uint32_t)brk_at;
        }
    }

    if (offsets && max_offsets > 0) {
        // 断点落在末尾时会多出一个空行(行尾挂标点或结尾换行),去掉它。
        if (groups > 1 && offsets[groups - 1] == (uint32_t)len) groups--;
        const int last = groups < max_offsets ? groups : max_offsets - 1;
        offsets[last] = (uint32_t)len;
    }
    return groups;
}

int lime_text_pages(const char *utf8, int units_per_line, int lines_per_page,
                    uint32_t *offsets, int max_offsets)
{
    return wrap_text(utf8, units_per_line, lines_per_page, offsets, max_offsets);
}

int lime_text_lines(const char *utf8, int units_per_line, uint32_t *offsets, int max_lines)
{
    return wrap_text(utf8, units_per_line, 1, offsets, max_lines);
}

size_t lime_utf8_next_boundary(const char *utf8, size_t len, size_t pos)
{
    if (!utf8 || pos >= len) return len;
    size_t next = pos;
    (void)utf8_next(utf8, len, &next);
    return next > len ? len : next;
}

uint32_t lime_utf8_decode(const char *utf8, size_t len, size_t pos, size_t *next_out)
{
    size_t next = pos;
    const uint32_t cp = utf8 ? utf8_next(utf8, len, &next) : 0;
    if (next_out) *next_out = next > len ? len : next;
    return cp;
}

// ---------------------------------------------------------------- 播放状态
static size_t dialogue_text(lime_script_t *script, uint32_t id, char *out, size_t capacity)
{
    lime_dialogue_t dialogue;
    if (!lime_script_dialogue(script, id, &dialogue) || !dialogue.text) {
        if (out && capacity) out[0] = '\0';
        return 0;
    }
    size_t length = dialogue.text_len;
    if (length >= capacity) length = capacity ? capacity - 1 : 0;
    if (out) {
        memcpy(out, dialogue.text, length);
        out[length] = '\0';
    }
    return length;
}

static void refresh(lime_player_t *player, lime_script_t *script, const lime_layout_t *layout)
{
    lime_dialogue_t dialogue;
    if (!lime_script_dialogue(script, player->id, &dialogue)) {
        player->at_choice = 0;
        player->choice_count = 0;
        player->page_count = 1;
        return;
    }
    player->bg = dialogue.bg;
    player->sprite = dialogue.sprite;
    player->cg = dialogue.cg;
    player->speaker = dialogue.speaker;
    player->z = (dialogue.flags & LIME_DLG_Z1) ? 1 : ((dialogue.flags & LIME_DLG_Z2) ? 2 : 0);
    player->chapter = lime_script_chapter_of(script, player->id);

    char text[LIME_TEXT_BUFFER];
    dialogue_text(script, player->id, text, sizeof(text));
    uint32_t offsets[LIME_MAX_PAGES + 1];
    int pages = lime_text_pages(text, layout->units_per_line, layout->lines_per_page,
                                offsets, LIME_MAX_PAGES + 1);
    if (pages < 1) pages = 1;
    if (pages > LIME_MAX_PAGES) pages = LIME_MAX_PAGES;
    player->page_count = (uint16_t)pages;
    if (player->page >= player->page_count) player->page = (uint16_t)(player->page_count - 1);

    player->at_choice = 0;
    player->choice_count = 0;
    if (dialogue.flags & LIME_DLG_IS_CHOICE) {
        lime_choice_option_t options[4];
        uint8_t count = 0;
        const bool found = lime_script_choice_for(script, player->id, &count, options, 4);
        if (found && count > 0) {
            player->at_choice = 1;
            player->choice_count = count;
        }
    }
    player->ended = player->id >= lime_script_entries(script) ? 1 : 0;
}

void lime_player_reset(lime_player_t *player)
{
    if (!player) return;
    memset(player, 0, sizeof(*player));
    player->bg = player->sprite = player->cg = player->speaker = LIME_NAME_NONE;
    player->page_count = 1;
}

bool lime_player_start(lime_player_t *player, lime_script_t *script, uint32_t id,
                       const lime_layout_t *layout)
{
    if (!player || !script || !layout) return false;
    lime_player_reset(player);
    if (id < 1) id = 1;
    if (id > script->entries) id = script->entries;
    player->id = id;
    refresh(player, script, layout);
    return true;
}

lime_step_t lime_player_advance(lime_player_t *player, lime_script_t *script,
                                const lime_layout_t *layout)
{
    if (!player || !script || !layout) return LIME_STEP_STUCK;
    if (player->at_choice) return LIME_STEP_CHOICE;      // 停在选项上,必须先选
    if (player->page + 1 < player->page_count) {         // 同一句还有下一页
        player->page++;
        return LIME_STEP_TEXT;
    }
    if (player->id >= script->entries) {
        player->ended = 1;
        return LIME_STEP_ENDING;
    }

    const uint32_t before_chapter = player->chapter;
    player->id++;
    player->page = 0;
    refresh(player, script, layout);
    if (player->chapter != before_chapter) return LIME_STEP_CHAPTER;
    if (player->at_choice) return LIME_STEP_CHOICE;
    if (player->ended) return LIME_STEP_ENDING;
    return LIME_STEP_TEXT;
}

bool lime_player_back(lime_player_t *player, lime_script_t *script, const lime_layout_t *layout)
{
    if (!player || !script || !layout) return false;
    if (player->id <= 1) {
        player->page = 0;
        return false;
    }
    player->id--;
    refresh(player, script, layout);
    // 回退后停在最后一句的最后一页,而不是第一页。
    if (player->page_count > 0) player->page = (uint16_t)(player->page_count - 1);
    return true;
}

bool lime_player_choose(lime_player_t *player, lime_script_t *script, uint8_t index,
                        const lime_layout_t *layout)
{
    if (!player || !script || !layout || !player->at_choice) return false;
    lime_choice_option_t options[4];
    uint8_t count = 0;
    if (!lime_script_choice_for(script, player->id, &count, options, 4)) return false;
    if (index >= count) return false;
    if (player->choice_len < LIME_CHOICE_HISTORY) {
        player->choice_pick[player->choice_len++] = index;
    }
    uint32_t target = options[index].target;
    if (target < 1 || target > script->entries) return false;
    player->id = target;
    player->page = 0;
    refresh(player, script, layout);
    return true;
}

bool lime_player_jump(lime_player_t *player, lime_script_t *script, uint32_t id,
                      const lime_layout_t *layout)
{
    if (!player || !script || !layout) return false;
    if (id < 1 || id > script->entries) return false;
    player->id = id;
    player->page = 0;
    refresh(player, script, layout);
    return true;
}

size_t lime_player_text(const lime_player_t *player, lime_script_t *script, char *out,
                        size_t capacity)
{
    if (!player || !script) return 0;
    return dialogue_text(script, player->id, out, capacity);
}

size_t lime_player_page_text(const lime_player_t *player, lime_script_t *script,
                             const lime_layout_t *layout, char *out, size_t capacity)
{
    if (!player || !script || !layout) return 0;
    char text[LIME_TEXT_BUFFER];
    const size_t length = dialogue_text(script, player->id, text, sizeof(text));
    if (length == 0) {
        if (out && capacity) out[0] = '\0';
        return 0;
    }
    uint32_t offsets[LIME_MAX_PAGES + 1];
    int pages = lime_text_pages(text, layout->units_per_line, layout->lines_per_page,
                                offsets, LIME_MAX_PAGES + 1);
    if (pages < 1) pages = 1;
    uint16_t page = player->page;
    if (page >= pages) page = (uint16_t)(pages - 1);
    const uint32_t begin = offsets[page];
    const uint32_t end = offsets[page + 1];
    size_t span = end > begin ? end - begin : 0;
    if (span >= capacity) span = capacity ? capacity - 1 : 0;
    if (out) {
        memcpy(out, text + begin, span);
        out[span] = '\0';
    }
    return span;
}

size_t lime_player_speaker(const lime_player_t *player, lime_script_t *script, char *out,
                           size_t capacity)
{
    if (out && capacity) out[0] = '\0';
    if (!player || !script || !out || capacity == 0) return 0;
    if (player->speaker == LIME_NAME_NONE) return 0;
    const char *text = NULL;
    uint16_t length = 0;
    if (!lime_script_name(script, player->speaker, &text, &length)) return 0;
    size_t span = length;
    if (span >= capacity) span = capacity - 1;
    memcpy(out, text, span);
    out[span] = '\0';
    return span;
}

bool lime_player_show_sprite(const lime_player_t *player)
{
    return player && player->sprite != LIME_NAME_NONE;
}

int lime_chapter_number(const char *label, size_t length)
{
    int major = 0, minor = 0;
    return lime_chapter_pair(label, length, &major, &minor) ? major : 0;
}

// 章节标记形如 "[CHAPTER10-3]" / "CHAPTER10-3":解析出 "章-节" 两个数。
// UI 上显示为 X-X(如 10-3),所以这里要两个数都拿出来。
bool lime_chapter_pair(const char *label, size_t length, int *major, int *minor)
{
    if (!label) return false;
    int values[2] = { 0, 0 };
    int found = 0;
    size_t at = 0;
    while (at < length && found < 2) {
        if (label[at] < '0' || label[at] > '9') {
            at++;
            continue;
        }
        int value = 0;
        bool overflow = false;
        while (at < length && label[at] >= '0' && label[at] <= '9') {
            value = value * 10 + (label[at] - '0');
            if (value > 9999) overflow = true;   // 明显不是章号
            at++;
        }
        if (overflow) return false;
        values[found++] = value;
    }
    if (found == 0) return false;
    if (major) *major = values[0];
    if (minor) *minor = values[1];
    return true;
}

// ---------------------------------------------------------------- 存档
size_t lime_save_encode(const lime_save_t *save, uint8_t *out, size_t capacity)
{
    if (!save || !out) return 0;
    const size_t need = LIME_SAVE_FIXED + LIME_CHOICE_HISTORY;
    if (capacity < need) return 0;
    out[0] = LIME_SAVE_MAGIC;
    out[1] = LIME_SAVE_VERSION;
    out[2] = (uint8_t)(save->id & 0xFFu);
    out[3] = (uint8_t)((save->id >> 8) & 0xFFu);
    out[4] = (uint8_t)((save->id >> 16) & 0xFFu);
    out[5] = (uint8_t)((save->id >> 24) & 0xFFu);
    out[6] = (uint8_t)(save->page & 0xFFu);
    out[7] = (uint8_t)(save->page >> 8);
    out[8] = save->choice_len;
    out[9] = 0;                                  // 预留
    for (size_t i = 0; i < LIME_CHOICE_HISTORY; ++i) {
        out[LIME_SAVE_FIXED + i] = i < save->choice_len ? save->choice_pick[i] : 0;
    }
    return need;
}

bool lime_save_decode(lime_save_t *save, const uint8_t *data, size_t len)
{
    if (!save || !data || len < LIME_SAVE_FIXED + LIME_CHOICE_HISTORY) return false;
    if (data[0] != LIME_SAVE_MAGIC || data[1] != LIME_SAVE_VERSION) return false;
    save->id = (uint32_t)data[2] | ((uint32_t)data[3] << 8) | ((uint32_t)data[4] << 16) |
               ((uint32_t)data[5] << 24);
    save->page = (uint16_t)(data[6] | ((uint16_t)data[7] << 8));
    save->choice_len = data[8];
    if (save->choice_len > LIME_CHOICE_HISTORY) return false;
    for (size_t i = 0; i < LIME_CHOICE_HISTORY; ++i) {
        save->choice_pick[i] = data[LIME_SAVE_FIXED + i];
    }
    return save->id >= 1;
}

void lime_save_from_player(const lime_player_t *player, lime_save_t *out)
{
    if (!player || !out) return;
    out->id = player->id;
    out->page = player->page;
    out->choice_len = player->choice_len;
    for (size_t i = 0; i < LIME_CHOICE_HISTORY; ++i) {
        out->choice_pick[i] = i < player->choice_len ? player->choice_pick[i] : 0;
    }
}
