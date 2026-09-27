// main/tsxx_model.c —— 阅读器纯逻辑。不依赖 ESP-IDF / LVGL,可在宿主机上测试。
#include "tsxx_model.h"

#include <string.h>

static void decode_utf8(const uint8_t *s, uint32_t *codepoint, uint32_t *length)
{
    const uint8_t first = s[0];
    if (first < 0x80u) {
        *codepoint = first;
        *length = 1;
    } else if ((first & 0xE0u) == 0xC0u) {
        *codepoint = ((uint32_t)(first & 0x1Fu) << 6) | (s[1] & 0x3Fu);
        *length = 2;
    } else if ((first & 0xF0u) == 0xE0u) {
        *codepoint = ((uint32_t)(first & 0x0Fu) << 12) | ((uint32_t)(s[1] & 0x3Fu) << 6) |
                     (s[2] & 0x3Fu);
        *length = 3;
    } else {
        *codepoint = ((uint32_t)(first & 0x07u) << 18) | ((uint32_t)(s[1] & 0x3Fu) << 12) |
                     ((uint32_t)(s[2] & 0x3Fu) << 6) | (s[3] & 0x3Fu);
        *length = 4;
    }
}

int tsxx_char_units(uint32_t codepoint)
{
    if (codepoint < 0x80u) {
        return 1;                          // ASCII
    }
    if (codepoint >= 0x20000u) {
        return 2;                          // CJK 扩展 B 及以后
    }
    // 中日韩统一表意文字、假名、谚文、CJK 符号与全角形式
    if ((codepoint >= 0x1100u && codepoint <= 0x115Fu) ||
        (codepoint >= 0x2E80u && codepoint <= 0xA4CFu) ||
        (codepoint >= 0xAC00u && codepoint <= 0xD7A3u) ||
        (codepoint >= 0xF900u && codepoint <= 0xFAFFu) ||
        (codepoint >= 0xFE30u && codepoint <= 0xFE6Fu) ||
        (codepoint >= 0xFF00u && codepoint <= 0xFF60u) ||
        (codepoint >= 0xFFE0u && codepoint <= 0xFFE6u)) {
        return 2;
    }
    // 中文排印里当全角用的几个标点
    switch (codepoint) {
    case 0x2014u:   // —
    case 0x2018u:   // '
    case 0x2019u:   // '
    case 0x201Cu:   // "
    case 0x201Du:   // "
    case 0x2026u:   // …
        return 2;
    default:
        return 1;
    }
}

// 不能出现在行首的标点(行尾标点/右括号)。
static bool is_no_start(uint32_t cp)
{
    switch (cp) {
    case 0x3001u:   // 、
    case 0x3002u:   // 。
    case 0xFF0Cu:   // ,
    case 0xFF0Eu:   // .
    case 0xFF01u:   // !
    case 0xFF1Fu:   // ?
    case 0xFF1Au:   // :
    case 0xFF1Bu:   // ;
    case 0xFF09u:   // )
    case 0xFF3Du:   // ]
    case 0xFF5Du:   // }
    case 0x300Du:   // 」
    case 0x300Fu:   // 』
    case 0x3011u:   // 】
    case 0x2026u:   // …
    case 0x2014u:   // —
    case 0x301Cu:   // 〜
    case 0xFF5Eu:   // ～
        return true;
    default:
        return false;
    }
}

// 不能出现在行尾的标点(左括号)。
static bool is_no_end(uint32_t cp)
{
    switch (cp) {
    case 0xFF08u:   // (
    case 0xFF3Bu:   // [
    case 0xFF5Bu:   // {
    case 0x300Cu:   // 「
    case 0x300Eu:   // 『
    case 0x3010u:   // 【
        return true;
    default:
        return false;
    }
}

int tsxx_text_screens(const char *utf8, const tsxx_layout_t *layout, uint32_t *offsets,
                      int max_screens)
{
    if (utf8 == NULL || offsets == NULL || max_screens <= 0 || utf8[0] == '\0') {
        return 0;
    }
    const int per_line = (layout != NULL && layout->units_per_line > 0)
                             ? layout->units_per_line : 20;
    const int per_screen = (layout != NULL && layout->lines_per_page > 0)
                               ? layout->lines_per_page : 5;
    const uint8_t *s = (const uint8_t *)utf8;
    uint32_t pos = 0;
    uint32_t line_start = 0;
    uint32_t prev_start = 0;
    int prev_units = 0;
    int used = 0;
    int lines = 1;
    int screens = 1;
    offsets[0] = 0;

    while (s[pos] != '\0') {
        if (screens >= max_screens) {
            pos = (uint32_t)strlen(utf8);   // 超出一页的屏数上限:剩下的都并进最后一屏
            break;
        }
        uint32_t codepoint = 0;
        uint32_t length = 1;
        decode_utf8(s + pos, &codepoint, &length);

        if (codepoint == '\n') {
            pos += length;
            line_start = pos;
            prev_start = pos;
            prev_units = 0;
            used = 0;
            ++lines;
        } else {
            const int units = tsxx_char_units(codepoint);
            if (used + units > per_line && pos > line_start) {
                // 禁则:标点不能落在行首 / 行尾时,把断点往前挪一个字符。
                uint32_t break_at = pos;
                int next_used = 0;
                if ((is_no_start(codepoint) || is_no_end(codepoint)) &&
                    prev_start > line_start) {
                    break_at = prev_start;
                    next_used = prev_units;
                }
                line_start = break_at;
                used = next_used;
                ++lines;
            }
            used += units;
            prev_start = pos;
            prev_units = units;
            pos += length;
        }

        if (lines > per_screen && screens < max_screens) {
            offsets[screens] = line_start;
            ++screens;
            lines = 1;
        }
    }
    offsets[screens] = pos;
    // 文本正好以断行结尾时不要留一个空屏。
    if (screens > 1 && offsets[screens - 1] == pos) {
        --screens;
    }
    return screens;
}

static int page_screens(const tsxx_pack_t *pack, const tsxx_page_t *page,
                        const tsxx_layout_t *layout, char *scratch, size_t scratch_size)
{
    if (page->text_len == 0) {
        return 0;
    }
    if (tsxx_pack_text(pack, page, 0, 0, scratch, scratch_size) == 0) {
        return 0;
    }
    uint32_t offsets[TSXX_MAX_SCREENS + 1];
    return tsxx_text_screens(scratch, layout, offsets, TSXX_MAX_SCREENS);
}

static void enter_page(tsxx_player_t *player, const tsxx_pack_t *pack,
                       const tsxx_layout_t *layout, uint32_t page)
{
    char scratch[TSXX_TEXT_BUFFER];
    tsxx_page_t view;
    player->page = page;
    player->screen = 0;
    if (!tsxx_pack_page(pack, page, &view)) {
        player->screens = 0;
        player->choice_count = 0;
        player->at_choice = 0;
        player->ended = 1;
        return;
    }
    player->screens = (uint8_t)page_screens(pack, &view, layout, scratch, sizeof(scratch));
    player->choice_count = tsxx_pack_choice_count(pack, page);
    player->at_choice = player->choice_count > 0 ? 1u : 0u;
    player->ended = 0;
}

void tsxx_player_reset(tsxx_player_t *player)
{
    memset(player, 0, sizeof(*player));
}

bool tsxx_player_start(tsxx_player_t *player, const tsxx_pack_t *pack, uint32_t page,
                       const tsxx_layout_t *layout)
{
    const uint32_t pages = tsxx_pack_pages(pack);
    if (pages == 0) {
        return false;
    }
    tsxx_player_reset(player);
    if (page >= pages) {
        page = pages - 1u;
    }
    enter_page(player, pack, layout, page);
    return true;
}

tsxx_step_t tsxx_player_advance(tsxx_player_t *player, const tsxx_pack_t *pack,
                                const tsxx_layout_t *layout)
{
    if (player->at_choice) {
        return TSXX_STEP_STUCK;
    }
    if (player->ended) {
        return TSXX_STEP_STUCK;
    }
    if (player->screen + 1u < player->screens) {
        ++player->screen;
        return TSXX_STEP_SCREEN;
    }
    const uint32_t next = player->page + 1u;
    if (next >= tsxx_pack_pages(pack)) {
        player->ended = 1;
        player->at_choice = 0;
        return TSXX_STEP_END;
    }
    char scratch[TSXX_TEXT_BUFFER];
    tsxx_page_t view;
    player->page = next;
    player->screen = 0;
    tsxx_pack_page(pack, next, &view);
    player->screens = (uint8_t)page_screens(pack, &view, layout, scratch, sizeof(scratch));
    player->choice_count = tsxx_pack_choice_count(pack, next);
    player->at_choice = player->choice_count > 0 ? 1u : 0u;
    return player->at_choice ? TSXX_STEP_CHOICE : TSXX_STEP_PAGE;
}

bool tsxx_player_choose(tsxx_player_t *player, const tsxx_pack_t *pack, uint8_t index,
                        const tsxx_layout_t *layout)
{
    if (!player->at_choice || index >= player->choice_count) {
        return false;
    }
    uint32_t target = 0;
    if (!tsxx_pack_choice_option(pack, player->page, index, NULL, 0, &target)) {
        return false;
    }
    if (target >= tsxx_pack_pages(pack)) {
        return false;
    }
    enter_page(player, pack, layout, target);
    return true;
}

bool tsxx_player_jump(tsxx_player_t *player, const tsxx_pack_t *pack, uint32_t page,
                      const tsxx_layout_t *layout)
{
    if (page >= tsxx_pack_pages(pack)) {
        return false;
    }
    enter_page(player, pack, layout, page);
    return true;
}

tsxx_step_t tsxx_player_skip_chapter(tsxx_player_t *player, const tsxx_pack_t *pack,
                                     const tsxx_layout_t *layout)
{
    tsxx_chapter_t chapters[TSXX_MAX_CHAPTERS];
    const int count = tsxx_chapters_scan(pack, chapters, TSXX_MAX_CHAPTERS);
    uint32_t target = tsxx_pack_pages(pack);
    for (int i = 0; i < count; ++i) {
        if (chapters[i].page > player->page) {
            target = chapters[i].page;
            break;
        }
    }
    if (target >= tsxx_pack_pages(pack)) {
        return TSXX_STEP_END;
    }
    // 一直推进到目标页,但遇到选项立刻停下(玩家必须自己做选择)。
    for (uint32_t guard = 0; guard < TSXX_SKIP_MAX_STEPS; ++guard) {
        if (player->page >= target) {
            return TSXX_STEP_PAGE;
        }
        const tsxx_step_t step = tsxx_player_advance(player, pack, layout);
        if (step == TSXX_STEP_CHOICE || step == TSXX_STEP_END || step == TSXX_STEP_STUCK) {
            return step;
        }
    }
    return TSXX_STEP_STUCK;
}

size_t tsxx_player_text(const tsxx_player_t *player, const tsxx_pack_t *pack, char *out,
                        size_t capacity)
{
    tsxx_page_t view;
    if (!tsxx_pack_page(pack, player->page, &view)) {
        if (capacity > 0) {
            out[0] = '\0';
        }
        return 0;
    }
    return tsxx_pack_text(pack, &view, 0, 0, out, capacity);
}

size_t tsxx_player_screen_text(const tsxx_player_t *player, const tsxx_pack_t *pack,
                               const tsxx_layout_t *layout, char *out, size_t capacity)
{
    if (out == NULL || capacity == 0) {
        return 0;
    }
    out[0] = '\0';
    tsxx_page_t view;
    if (!tsxx_pack_page(pack, player->page, &view) || view.text_len == 0) {
        return 0;
    }
    char whole[TSXX_TEXT_BUFFER];
    if (tsxx_pack_text(pack, &view, 0, 0, whole, sizeof(whole)) == 0) {
        return 0;
    }
    uint32_t offsets[TSXX_MAX_SCREENS + 1];
    const int screens = tsxx_text_screens(whole, layout, offsets, TSXX_MAX_SCREENS);
    if (screens == 0) {
        return 0;
    }
    uint32_t index = player->screen;
    if (index >= (uint32_t)screens) {
        index = (uint32_t)screens - 1u;
    }
    const uint32_t start = offsets[index];
    const uint32_t end = offsets[index + 1u];
    uint32_t length = end - start;
    if (length >= capacity) {
        length = (uint32_t)capacity - 1u;
        // 按 UTF-8 字符边界回退,避免截出半个字。
        while (length > 0 && ((uint8_t)whole[start + length] & 0xC0u) == 0x80u) {
            --length;
        }
    }
    memcpy(out, whole + start, length);
    out[length] = '\0';
    return length;
}

size_t tsxx_player_speaker(const tsxx_player_t *player, const tsxx_pack_t *pack, char *out,
                           size_t capacity)
{
    if (out == NULL || capacity == 0) {
        return 0;
    }
    out[0] = '\0';
    tsxx_page_t view;
    if (!tsxx_pack_page(pack, player->page, &view) || view.speaker == TSXX_NONE8) {
        return 0;
    }
    return tsxx_pack_name(pack, TSXX_TABLE_SPEAKER, view.speaker, out, capacity);
}

bool tsxx_player_cg(const tsxx_player_t *player, const tsxx_pack_t *pack, tsxx_cg_t *out)
{
    return tsxx_pack_cg_of_page(pack, player->page, out);
}

bool tsxx_pack_cg_of_page(const tsxx_pack_t *pack, uint32_t page, tsxx_cg_t *out)
{
    tsxx_page_t view;
    if (!tsxx_pack_page(pack, page, &view) || !view.has_cg) {
        return false;
    }
    const uint32_t rank = tsxx_pack_cg_rank(pack, page);
    const uint16_t id = tsxx_pack_cg_at(pack, rank);
    if (id == TSXX_NONE16) {
        return false;
    }
    return tsxx_pack_cg(pack, id, out);
}

int tsxx_chapters_scan(const tsxx_pack_t *pack, tsxx_chapter_t *out, int max)
{
    if (out == NULL || max <= 0) {
        return 0;
    }
    uint8_t is_mark[256];
    memset(is_mark, 0, sizeof(is_mark));
    char label[64];
    for (uint16_t id = 0; id < 256u; ++id) {
        if (tsxx_pack_name(pack, TSXX_TABLE_SPEAKER, id, label, sizeof(label)) == 0) {
            continue;
        }
        if (label[0] == '[') {
            is_mark[id] = 1;
        }
    }
    int found = 0;
    for (uint32_t page = 0; page < pack->page_count && found < max; ++page) {
        const uint8_t speaker = pack->pspk[page];
        if (speaker == TSXX_NONE8 || !is_mark[speaker]) {
            continue;
        }
        bool seen = false;
        for (int i = 0; i < found; ++i) {
            if (out[i].name_id == speaker) {
                seen = true;
                break;
            }
        }
        if (seen) {
            continue;
        }
        out[found].name_id = speaker;
        out[found].page = page;
        ++found;
    }
    return found;
}

size_t tsxx_save_encode(const tsxx_save_t *save, uint8_t *out, size_t capacity)
{
    if (out == NULL || capacity < 8) {
        return 0;
    }
    memcpy(out, &save->page, 4);
    memcpy(out + 4, &save->screen, 4);
    return 8;
}

bool tsxx_save_decode(tsxx_save_t *save, const uint8_t *data, size_t len)
{
    if (data == NULL || len < 8) {
        return false;
    }
    memcpy(&save->page, data, 4);
    memcpy(&save->screen, data + 4, 4);
    return true;
}
