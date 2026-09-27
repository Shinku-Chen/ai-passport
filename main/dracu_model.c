// main/dracu_model.c —— 阅读状态机与排版实现。
//
// 本文件不接触 ESP-IDF(只用标准 C),宿主机的单元测试可以直接用真实剧本包跑完整条
// 线路。剧本包的字节解析在 dracu_scn.c,这里只做「推进 / 回退 / 选择 / 跳过」。
#include "dracu_model.h"

#include <stdio.h>
#include <string.h>

static const char *s_error = "";

static void set_error(const char *message)
{
    s_error = message;
}

const char *dracu_get_error(void)
{
    return s_error;
}

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

size_t dracu_utf8_next_boundary(const char *utf8, size_t len, size_t pos)
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

uint32_t dracu_utf8_decode(const char *utf8, size_t len, size_t pos, size_t *next_out)
{
    if (utf8 == NULL || pos >= len) {
        if (next_out != NULL) {
            *next_out = len;
        }
        return 0;
    }
    const uint8_t *bytes = (const uint8_t *)utf8;
    uint8_t lead = bytes[pos];
    size_t next = dracu_utf8_next_boundary(utf8, len, pos);
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

int dracu_char_units(uint32_t codepoint)
{
    if (codepoint < 0x1100) {
        return 1;
    }
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

int dracu_text_lines(const char *utf8, int units_per_line, uint32_t *offsets, int max_lines)
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
            uint32_t codepoint = dracu_utf8_decode(utf8, len, scan, &next);
            int width = dracu_char_units(codepoint);
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
        uint32_t head = dracu_utf8_decode(utf8, len, scan, &next);
        size_t break_at = scan;
        if (no_line_start(head) && last_fit > pos) {
            break_at = last_fit;
        }
        if (break_at <= pos) {
            break_at = next > pos ? next : len;   // 一个字符都放不下时至少前进一格
        }
        pos = break_at;
    }
    return line;
}

int dracu_text_pages(const char *utf8, int units_per_line, int lines_per_page, uint32_t *offsets,
                     int max_offsets)
{
    if (utf8 == NULL || offsets == NULL || max_offsets <= 0 || lines_per_page <= 0) {
        return 0;
    }
    size_t len = strlen(utf8);
    uint32_t line_offsets[64];
    int lines = dracu_text_lines(utf8, units_per_line, line_offsets, 64);
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
// 状态机
// --------------------------------------------------------------------------

void dracu_player_reset(dracu_player_t *player)
{
    if (player == NULL) {
        return;
    }
    memset(player, 0, sizeof(*player));
    player->page = 0;
    player->chapter = DRACU_CHAPTER_NONE;
    player->end_name = DRACU_SCN_NONE16;
    player->error = "";
}

uint32_t dracu_chapter_of_page(const dracu_scn_t *scn, uint32_t page)
{
    uint32_t found = DRACU_CHAPTER_NONE;
    uint32_t count = dracu_scn_chapter_count(scn);
    for (uint32_t index = 0; index < count; index++) {
        dracu_chapter_t chapter;
        if (!dracu_scn_chapter(scn, index, &chapter)) {
            break;
        }
        if (chapter.page <= page) {
            found = index;
        } else {
            break;
        }
    }
    return found;
}

// 读出某一页的正文并按 layout 分屏;不碰选择历史。
static bool load_page(dracu_player_t *player, dracu_scn_t *scn, uint32_t page,
                      const dracu_layout_t *layout)
{
    if (!dracu_scn_page_valid(scn, page)) {
        set_error("页码越界");
        player->error = s_error;
        return false;
    }
    player->page = page;
    player->page_index = 0;
    player->page_count = 0;
    player->at_choice = false;
    player->ended = false;
    player->end_name = DRACU_SCN_NONE16;
    player->text[0] = '\0';
    if (!dracu_scn_page(scn, page, &player->record)) {
        set_error("读页记录失败(剧本包解块失败)");
        player->error = s_error;
        return false;
    }
    if (player->record.text_off != DRACU_SCN_NO_TEXT && player->record.text_len > 0) {
        if (dracu_scn_text(scn, player->record.text_off, player->record.text_len, player->text,
                           sizeof(player->text)) == 0) {
            set_error("读正文失败(正文块解压失败)");
            player->error = s_error;
            return false;
        }
        int pages = dracu_text_pages(player->text, layout->units_per_line, layout->lines_per_page,
                                     player->page_offsets, DRACU_MAX_PAGES);
        player->page_count = (uint16_t)(pages > 0 ? pages : 0);
    }
    if ((player->record.flags & DRACU_PAGE_CHOOSE) != 0) {
        if (dracu_scn_choice(scn, page, &player->choice_view) && player->choice_view.count > 0) {
            player->at_choice = true;
        }
    }
    uint16_t end_name = 0;
    if (dracu_scn_end_name(scn, page, &end_name)) {
        player->ended = true;
        player->end_name = end_name;
    }
    player->chapter = dracu_chapter_of_page(scn, page);
    player->error = "";
    return true;
}

bool dracu_player_start(dracu_player_t *player, dracu_scn_t *scn, uint32_t page,
                        const dracu_layout_t *layout)
{
    if (player == NULL || scn == NULL || layout == NULL) {
        return false;
    }
    memset(player->choice, 0, sizeof(player->choice));
    return load_page(player, scn, page, layout);
}

bool dracu_player_resume(dracu_player_t *player, dracu_scn_t *scn, uint32_t page,
                         const uint8_t *choice, const dracu_layout_t *layout)
{
    if (player == NULL || scn == NULL || layout == NULL) {
        return false;
    }
    memset(player->choice, 0, sizeof(player->choice));
    if (choice != NULL) {
        memcpy(player->choice, choice, DRACU_COND_MAX);
    }
    return load_page(player, scn, page, layout);
}

bool dracu_player_goto(dracu_player_t *player, dracu_scn_t *scn, uint32_t page,
                       const dracu_layout_t *layout)
{
    return dracu_player_resume(player, scn, page, player->choice, layout);
}

// 换页:结局 -> 条件路由 -> 前进特判 -> 下一页
static dracu_step_t step_forward(dracu_player_t *player, dracu_scn_t *scn,
                                 const dracu_layout_t *layout)
{
    if (player->ended) {
        return DRACU_STEP_ENDING;
    }
    uint32_t target = 0;
    if (dracu_scn_hidden_page(scn, player->page, player->choice, &target)) {
        if (!load_page(player, scn, target, layout)) {
            return DRACU_STEP_STUCK;
        }
    } else {
        uint32_t jump = dracu_scn_next_page(scn, player->page);
        target = jump != 0 ? jump : player->page + 1;
        if (!load_page(player, scn, target, layout)) {
            return DRACU_STEP_STUCK;
        }
    }
    if (player->ended) {
        return DRACU_STEP_ENDING;
    }
    if (player->at_choice) {
        return DRACU_STEP_CHOICE;
    }
    return DRACU_STEP_PAGE;
}

dracu_step_t dracu_player_advance(dracu_player_t *player, dracu_scn_t *scn,
                                  const dracu_layout_t *layout)
{
    if (player == NULL || scn == NULL || layout == NULL) {
        return DRACU_STEP_STUCK;
    }
    if (player->page_index + 1 < player->page_count) {
        player->page_index++;
        return DRACU_STEP_TEXT;
    }
    return step_forward(player, scn, layout);
}

dracu_step_t dracu_player_back(dracu_player_t *player, dracu_scn_t *scn,
                               const dracu_layout_t *layout)
{
    if (player == NULL || scn == NULL || layout == NULL) {
        return DRACU_STEP_STUCK;
    }
    if (player->page_index > 0) {
        player->page_index--;
        return DRACU_STEP_TEXT;
    }
    // 回退堵死:某些页(合流点、选项分支入口)只能自环或回到指定页
    uint32_t blocked = dracu_scn_back_page(scn, player->page);
    uint32_t target;
    if (blocked != 0) {
        target = blocked;
    } else if (player->page > 1) {
        target = player->page - 1;
    } else {
        return DRACU_STEP_STUCK;
    }
    uint32_t keep_choice[DRACU_COND_MAX];
    memcpy(keep_choice, player->choice, sizeof(keep_choice));
    if (!load_page(player, scn, target, layout)) {
        return DRACU_STEP_STUCK;
    }
    // 回退不该改写选择历史:选项在回退后要能重选,但别的页的历史保持不变
    memcpy(player->choice, keep_choice, sizeof(keep_choice));
    if (player->ended) {
        return DRACU_STEP_ENDING;
    }
    if (player->at_choice) {
        return DRACU_STEP_CHOICE;
    }
    return DRACU_STEP_PAGE;
}

bool dracu_player_choose(dracu_player_t *player, dracu_scn_t *scn, uint8_t index,
                         const dracu_layout_t *layout)
{
    if (player == NULL || scn == NULL || layout == NULL || !player->at_choice) {
        return false;
    }
    if (index >= player->choice_view.count) {
        return false;
    }
    uint32_t target = player->choice_view.target[index];
    if (target == 0 || target == DRACU_SCN_NO_TEXT) {
        set_error("选项没有目标页");
        player->error = s_error;
        return false;
    }
    // 记录选择历史(条件路由要读它)
    int cond = dracu_scn_cond_index(scn, player->page);
    if (cond >= 0 && cond < DRACU_COND_MAX) {
        player->choice[cond] = (uint8_t)(index + 1);
    }
    player->at_choice = false;
    return load_page(player, scn, target, layout);
}

dracu_step_t dracu_player_skip_chapter(dracu_player_t *player, dracu_scn_t *scn,
                                       const dracu_layout_t *layout)
{
    if (player == NULL || scn == NULL || layout == NULL) {
        return DRACU_STEP_STUCK;
    }
    uint32_t count = dracu_scn_chapter_count(scn);
    uint32_t target = 0;
    for (uint32_t index = 0; index < count; index++) {
        dracu_chapter_t chapter;
        if (!dracu_scn_chapter(scn, index, &chapter)) {
            break;
        }
        if (chapter.page > player->page) {
            target = chapter.page;
            break;
        }
    }
    if (target == 0) {
        return DRACU_STEP_STUCK;   // 已经是最后一章:交给调用方提示
    }
    if (!load_page(player, scn, target, layout)) {
        return DRACU_STEP_STUCK;
    }
    if (player->ended) {
        return DRACU_STEP_ENDING;
    }
    if (player->at_choice) {
        return DRACU_STEP_CHOICE;
    }
    return DRACU_STEP_PAGE;
}

// --------------------------------------------------------------------------
// 取文本
// --------------------------------------------------------------------------

size_t dracu_player_page_text(const dracu_player_t *player, char *out, size_t capacity)
{
    if (player == NULL || out == NULL || capacity == 0) {
        return 0;
    }
    out[0] = '\0';
    if (player->page_count == 0 || player->page_index >= player->page_count) {
        return 0;
    }
    uint32_t start = player->page_offsets[player->page_index];
    uint32_t end = player->page_index + 1 < player->page_count
                       ? player->page_offsets[player->page_index + 1]
                       : (uint32_t)strlen(player->text);
    size_t length = end > start ? end - start : 0;
    if (length + 1 > capacity) {
        length = capacity - 1;
    }
    memcpy(out, player->text + start, length);
    out[length] = '\0';
    return length;
}

size_t dracu_player_text(const dracu_player_t *player, char *out, size_t capacity)
{
    if (player == NULL || out == NULL || capacity == 0) {
        return 0;
    }
    size_t length = strlen(player->text);
    if (length + 1 > capacity) {
        length = capacity - 1;
    }
    memcpy(out, player->text, length);
    out[length] = '\0';
    return length;
}

size_t dracu_player_speaker(const dracu_player_t *player, dracu_scn_t *scn, char *out,
                            size_t capacity)
{
    if (player == NULL || scn == NULL || out == NULL || capacity == 0) {
        return 0;
    }
    out[0] = '\0';
    if (player->record.name == DRACU_SCN_NONE8) {
        return 0;
    }
    return dracu_scn_string(scn, player->record.name, out, capacity);
}

// --------------------------------------------------------------------------
// 存档
// --------------------------------------------------------------------------

void dracu_save_from_player(const dracu_player_t *player, dracu_save_t *out)
{
    if (player == NULL || out == NULL) {
        return;
    }
    out->page = player->page;
    out->chapter = player->chapter;
    memcpy(out->choice, player->choice, DRACU_COND_MAX);
}

size_t dracu_save_encode(const dracu_save_t *save, uint8_t *out, size_t capacity)
{
    if (save == NULL || out == NULL || capacity < 4 + 4 + 4 + DRACU_COND_MAX) {
        return 0;
    }
    size_t offset = 0;
    out[offset++] = (uint8_t)(DRACU_SAVE_VERSION & 0xFF);
    out[offset++] = (uint8_t)(DRACU_SAVE_VERSION >> 8);
    out[offset++] = (uint8_t)(save->page & 0xFF);
    out[offset++] = (uint8_t)((save->page >> 8) & 0xFF);
    out[offset++] = (uint8_t)((save->page >> 16) & 0xFF);
    out[offset++] = (uint8_t)((save->page >> 24) & 0xFF);
    out[offset++] = (uint8_t)(save->chapter & 0xFF);
    out[offset++] = (uint8_t)((save->chapter >> 8) & 0xFF);
    out[offset++] = (uint8_t)((save->chapter >> 16) & 0xFF);
    out[offset++] = (uint8_t)((save->chapter >> 24) & 0xFF);
    memcpy(out + offset, save->choice, DRACU_COND_MAX);
    offset += DRACU_COND_MAX;
    return offset;
}

bool dracu_save_decode(dracu_save_t *save, const uint8_t *data, size_t len)
{
    if (save == NULL || data == NULL || len < 10u + DRACU_COND_MAX) {
        return false;
    }
    if (rd16(data) != DRACU_SAVE_VERSION) {
        return false;
    }
    memset(save, 0, sizeof(*save));
    save->page = rd32(data + 2);
    save->chapter = rd32(data + 6);
    memcpy(save->choice, data + 10, DRACU_COND_MAX);
    return save->page >= 1;
}
