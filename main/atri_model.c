// main/atri_model.c —— 阅读器纯逻辑实现。
#include "atri_model.h"

#include <string.h>

// ---------------------------------------------------------------- 文本宽度模型
// 判断 UTF-8 字符是否为"半角":ASCII 与半角片假名(U+FF61..U+FF9F)。其余按全角算。
static bool codepoint_half_width(uint32_t cp)
{
    if (cp < 0x80u) return true;
    if (cp >= 0xFF61u && cp <= 0xFF9Fu) return true;
    return false;
}

// 行首禁则:这些标点不允许出现在行首,遇到时把它拉回上一行。
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
        step = 1;   // 非法首字节:按单字节跳过,保证不会死循环
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

int atri_text_pages(const char *utf8, int units_per_line, int lines_per_page,
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
                // 禁则标点:挂在本行行尾(允许轻微超宽),再换行。
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

// ---------------------------------------------------------------- 播放状态
static int pages_of(const char *text, const atri_layout_t *layout)
{
    uint32_t offsets[ATRI_MAX_PAGES + 1];
    const int pages = atri_text_pages(text, layout->units_per_line, layout->lines_per_page,
                                      offsets, ATRI_MAX_PAGES + 1);
    return pages < 1 ? 1 : pages;
}

static void refresh_pages(atri_player_t *player, const atri_pack_t *pack,
                          const atri_layout_t *layout){
    char text[ATRI_TEXT_BUFFER];
    atri_player_text(player, pack, text, sizeof(text));
    int pages = pages_of(text, layout);
    if (pages > ATRI_MAX_PAGES) pages = ATRI_MAX_PAGES;
    player->page_count = (uint16_t)pages;
    if (player->page >= player->page_count) player->page = (uint16_t)(player->page_count - 1);
}

// 角色立绘:记录里写 ATRI_CHAR_KEEP 就沿用当前这张(粘性)。
static void apply_dialogue_char(atri_player_t *player, const atri_pack_t *pack,
                                uint16_t dialogue_index)
{
    atri_dialogue_t dlg;
    atri_pack_dialogue(pack, dialogue_index, &dlg);
    if (dlg.chr != ATRI_CHAR_KEEP) player->chr = dlg.chr;
}

static bool load_scene(atri_player_t *player, const atri_pack_t *pack, uint16_t chapter,
                       uint16_t scene, const atri_layout_t *layout)
{
    if (!pack || chapter >= pack->chapter_count) return false;
    atri_chapter_t ch;
    atri_pack_chapter(pack, chapter, &ch);
    if (scene >= ch.scene_count) return false;

    atri_scene_t sc;
    atri_pack_scene(pack, (uint16_t)(ch.first_scene + scene), &sc);

    player->chapter = chapter;
    player->scene = scene;
    player->dialogue = 0;
    player->page = 0;
    player->page_count = 1;
    player->at_choice = 0;
    player->ended = 0;
    player->end_name = ATRI_NONE;
    // 角色立绘是粘性的:换幕不自动清,由对白里的 chr 字段决定何时换人。

    if (sc.choice_count > 0) {
        player->at_choice = 1;
        return true;
    }
    if (sc.dlg_count == 0) return false;

    apply_dialogue_char(player, pack, sc.first_dlg);
    refresh_pages(player, pack, layout);
    return true;
}

void atri_player_reset(atri_player_t *player)
{
    if (!player) return;
    memset(player, 0, sizeof(*player));
    player->end_name = ATRI_NONE;
    player->chr = ATRI_CHAR_NONE;
    player->page_count = 1;
}

bool atri_player_start(atri_player_t *player, const atri_pack_t *pack, uint16_t chapter,
                       const atri_layout_t *layout)
{
    if (!player || !pack || !layout) return false;
    atri_player_reset(player);
    return load_scene(player, pack, chapter, 0, layout);
}

// 选择历史是否命中分流条件(pack 里每项 2 bit,低位在前)。
static bool choice_history_matches(const atri_player_t *player, const atri_chapter_t *ch)
{
    if (ch->branch_count == 0) return true;
    if (player->choice_len < ch->branch_count) return false;
    for (uint8_t i = 0; i < ch->branch_count; ++i) {
        const uint8_t want = (uint8_t)((ch->branch_pick >> (2 * i)) & 0x3u);
        if ((player->choice_pick[i] & 0x3u) != want) return false;
    }
    return true;
}

atri_step_t atri_player_advance(atri_player_t *player, const atri_pack_t *pack,
                                const atri_layout_t *layout)
{
    if (!player || !pack) return ATRI_STEP_STUCK;
    if (player->ended) return ATRI_STEP_ENDING;
    if (player->at_choice) return ATRI_STEP_CHOICE;
    if (player->chapter >= pack->chapter_count) return ATRI_STEP_STUCK;

    atri_chapter_t ch;
    atri_pack_chapter(pack, player->chapter, &ch);
    if (player->scene >= ch.scene_count) return ATRI_STEP_STUCK;
    atri_scene_t sc;
    atri_pack_scene(pack, (uint16_t)(ch.first_scene + player->scene), &sc);
    if (sc.dlg_count == 0) return ATRI_STEP_STUCK;

    if (player->page + 1 < player->page_count) {
        player->page++;
        return ATRI_STEP_TEXT;
    }

    if (player->dialogue + 1 < sc.dlg_count) {
        player->dialogue++;
        player->page = 0;
        apply_dialogue_char(player, pack, (uint16_t)(sc.first_dlg + player->dialogue));
        refresh_pages(player, pack, layout);
        return ATRI_STEP_TEXT;
    }

    // 走到本场景末句:先看跳转,再看结局/分流,最后才是下一幕/下一章。
    atri_dialogue_t last;
    atri_pack_dialogue(pack, (uint16_t)(sc.first_dlg + sc.dlg_count - 1), &last);

    if (player->scene + 1 < ch.scene_count) {
        const uint16_t target =
            (last.flags & ATRI_DLG_TO_SCENE) ? (uint16_t)(player->scene + last.jump)
                                             : (uint16_t)(player->scene + 1);
        if (load_scene(player, pack, player->chapter, target, layout)) return ATRI_STEP_SCENE;
        player->ended = 1;
        player->end_name = ATRI_NONE;
        return ATRI_STEP_ENDING;
    }

    if (last.flags & ATRI_DLG_END) {
        player->ended = 1;
        player->end_name = last.arg;
        return ATRI_STEP_ENDING;
    }

    uint16_t target = ATRI_NONE;
    if (last.flags & ATRI_DLG_BRANCH) {
        target = choice_history_matches(player, &ch) ? ch.next : ch.branch_bad;
    } else {
        target = ch.next;
    }
    if (target != ATRI_NONE && target < pack->chapter_count) {
        if (load_scene(player, pack, target, 0, layout)) return ATRI_STEP_CHAPTER;
    }

    player->ended = 1;
    player->end_name = ATRI_NONE;
    return ATRI_STEP_ENDING;
}

bool atri_player_choose(atri_player_t *player, const atri_pack_t *pack, uint8_t index,
                        const atri_layout_t *layout)
{
    if (!layout) return false;
    if (!player || !pack || !player->at_choice) return false;
    if (player->chapter >= pack->chapter_count) return false;
    atri_chapter_t ch;
    atri_pack_chapter(pack, player->chapter, &ch);
    if (player->scene >= ch.scene_count) return false;
    atri_scene_t sc;
    atri_pack_scene(pack, (uint16_t)(ch.first_scene + player->scene), &sc);
    if (index >= sc.choice_count) return false;
    const uint16_t target = (uint16_t)(player->scene + sc.choice_jump[index]);
    if (target >= ch.scene_count) return false;
    if (player->choice_len < ATRI_CHOICE_HISTORY) {
        player->choice_pick[player->choice_len++] = index;
    }
    return load_scene(player, pack, player->chapter, target, layout);
}

bool atri_player_skip_scene(atri_player_t *player, const atri_pack_t *pack,
                            const atri_layout_t *layout)
{
    if (!player || !pack || !layout || player->at_choice || player->ended) return false;
    if (player->chapter >= pack->chapter_count) return false;
    atri_chapter_t ch;
    atri_pack_chapter(pack, player->chapter, &ch);
    if (player->scene + 1 >= ch.scene_count) return false;
    atri_scene_t sc;
    atri_pack_scene(pack, (uint16_t)(ch.first_scene + player->scene), &sc);
    if (sc.dlg_count == 0) return false;
    atri_dialogue_t last;
    atri_pack_dialogue(pack, (uint16_t)(sc.first_dlg + sc.dlg_count - 1), &last);
    if (last.flags & (ATRI_DLG_TO_SCENE | ATRI_DLG_END | ATRI_DLG_BRANCH)) return false;
    return load_scene(player, pack, player->chapter, (uint16_t)(player->scene + 1), layout);
}

atri_step_t atri_player_skip_chapter(atri_player_t *player, const atri_pack_t *pack,
                                     const atri_layout_t *layout)
{
    if (!player || !pack || !layout) return ATRI_STEP_STUCK;
    const uint16_t start_chapter = player->chapter;
    for (int i = 0; i < ATRI_SKIP_CHAPTER_MAX_STEPS; ++i) {
        if (player->at_choice) return ATRI_STEP_CHOICE;
        if (player->ended) return ATRI_STEP_ENDING;
        const atri_step_t step = atri_player_advance(player, pack, layout);
        switch (step) {
        case ATRI_STEP_TEXT:
        case ATRI_STEP_SCENE:
            // 还在本章里,继续往后推。换章由 advance() 返回 ATRI_STEP_CHAPTER。
            if (player->chapter != start_chapter) return ATRI_STEP_CHAPTER;
            continue;
        case ATRI_STEP_CHAPTER:
            return ATRI_STEP_CHAPTER;
        case ATRI_STEP_CHOICE:
            return ATRI_STEP_CHOICE;   // 遇到选项一定停下,交回玩家
        case ATRI_STEP_ENDING:
            return ATRI_STEP_ENDING;
        case ATRI_STEP_STUCK:
        default:
            return ATRI_STEP_STUCK;
        }
    }
    return ATRI_STEP_STUCK;   // 数据异常(循环超过上限)
}

bool atri_player_load(atri_player_t *player, const atri_pack_t *pack, const atri_save_t *save,
                      const atri_layout_t *layout)
{    if (!player || !pack || !save) return false;
    if (!load_scene(player, pack, save->chapter, save->scene, layout)) return false;

    player->choice_len =
        save->choice_len > ATRI_CHOICE_HISTORY ? ATRI_CHOICE_HISTORY : save->choice_len;
    memcpy(player->choice_pick, save->choice_pick, sizeof(player->choice_pick));
    if (player->at_choice) return true;

    atri_chapter_t ch;
    atri_pack_chapter(pack, player->chapter, &ch);
    atri_scene_t sc;
    atri_pack_scene(pack, (uint16_t)(ch.first_scene + player->scene), &sc);
    if (save->dialogue >= sc.dlg_count) return true;   // 越界的对白下标:停在场景开头

    player->dialogue = save->dialogue;
    player->page = 0;
    // 粘性立绘以存档为准(存档早于 v2 时该字段为“沿用”)。
    if (save->chr != ATRI_CHAR_KEEP) player->chr = save->chr;
    refresh_pages(player, pack, layout);
    return true;
}

size_t atri_player_text(const atri_player_t *player, const atri_pack_t *pack, char *out,
                        size_t capacity)
{
    if (!player || !pack || player->chapter >= pack->chapter_count) {
        return atri_pack_text(pack, 0, 0, out, capacity);
    }
    atri_chapter_t ch;
    atri_pack_chapter(pack, player->chapter, &ch);
    if (player->scene >= ch.scene_count) return atri_pack_text(pack, 0, 0, out, capacity);
    atri_scene_t sc;
    atri_pack_scene(pack, (uint16_t)(ch.first_scene + player->scene), &sc);
    if (player->dialogue >= sc.dlg_count) return atri_pack_text(pack, 0, 0, out, capacity);
    atri_dialogue_t dlg;
    atri_pack_dialogue(pack, (uint16_t)(sc.first_dlg + player->dialogue), &dlg);
    return atri_pack_text(pack, dlg.text_off, dlg.text_len, out, capacity);
}

size_t atri_player_page_text(const atri_player_t *player, const atri_pack_t *pack,
                             const atri_layout_t *layout, char *out, size_t capacity)
{
    char full[ATRI_TEXT_BUFFER];
    atri_player_text(player, pack, full, sizeof(full));
    if (!out || capacity == 0) return 0;
    if (!layout) {
        const size_t len = strlen(full);
        const size_t take = len < capacity - 1 ? len : capacity - 1;
        memcpy(out, full, take);
        out[take] = '\0';
        return take;
    }
    uint32_t offsets[ATRI_MAX_PAGES + 1];
    int pages = atri_text_pages(full, layout->units_per_line, layout->lines_per_page,
                                offsets, ATRI_MAX_PAGES + 1);
    if (pages < 1) pages = 1;
    int page = player->page;
    if (page >= pages) page = pages - 1;
    const uint32_t start = offsets[page];
    const uint32_t end = offsets[page + 1];
    size_t len = (size_t)(end - start);
    if (len > capacity - 1) len = capacity - 1;
    memcpy(out, full + start, len);
    out[len] = '\0';
    return len;
}

size_t atri_player_speaker(const atri_player_t *player, const atri_pack_t *pack, char *out,
                           size_t capacity)
{
    if (!player || !pack || player->chapter >= pack->chapter_count) {
        return atri_pack_text(pack, 0, 0, out, capacity);
    }
    atri_chapter_t ch;
    atri_pack_chapter(pack, player->chapter, &ch);
    if (player->scene >= ch.scene_count) return atri_pack_text(pack, 0, 0, out, capacity);
    atri_scene_t sc;
    atri_pack_scene(pack, (uint16_t)(ch.first_scene + player->scene), &sc);
    if (player->dialogue >= sc.dlg_count) return atri_pack_text(pack, 0, 0, out, capacity);
    atri_dialogue_t dlg;
    atri_pack_dialogue(pack, (uint16_t)(sc.first_dlg + player->dialogue), &dlg);
    if (dlg.name == ATRI_NONE) return atri_pack_text(pack, 0, 0, out, capacity);
    return atri_pack_name(pack, dlg.name, out, capacity);
}

uint16_t atri_player_char(const atri_player_t *player)
{
    return player ? player->chr : ATRI_CHAR_KEEP;
}

bool atri_player_scene_view(const atri_player_t *player, const atri_pack_t *pack,
                            atri_scene_t *out)
{
    if (!player || !pack || !out || player->chapter >= pack->chapter_count) return false;
    atri_chapter_t ch;
    atri_pack_chapter(pack, player->chapter, &ch);
    if (player->scene >= ch.scene_count) return false;
    atri_pack_scene(pack, (uint16_t)(ch.first_scene + player->scene), out);
    return true;
}

uint8_t atri_player_chapter_flags(const atri_player_t *player, const atri_pack_t *pack)
{
    if (!player || !pack || player->chapter >= pack->chapter_count) return 0;
    atri_chapter_t ch;
    atri_pack_chapter(pack, player->chapter, &ch);
    return ch.flags;
}

// ---------------------------------------------------------------- 存档
#define ATRI_SAVE_MAGIC 0xA7u
#define ATRI_SAVE_VERSION 2u
// v1(旧固件写的存档,没有立绘字段):仍能读,立绘按“不换”处理。
#define ATRI_SAVE_VERSION_LEGACY 1u
#define ATRI_SAVE_SIZE_LEGACY (10u + ATRI_CHOICE_HISTORY)
// v2: chapter, scene, dialogue, chr, choice_len, pad + 选择历史
#define ATRI_SAVE_SIZE (12u + ATRI_CHOICE_HISTORY)

size_t atri_save_encode(const atri_save_t *save, uint8_t *out, size_t capacity)
{
    if (!save || !out || capacity < ATRI_SAVE_SIZE) return 0;
    out[0] = ATRI_SAVE_MAGIC;
    out[1] = ATRI_SAVE_VERSION;
    out[2] = (uint8_t)(save->chapter & 0xFFu);
    out[3] = (uint8_t)(save->chapter >> 8);
    out[4] = (uint8_t)(save->scene & 0xFFu);
    out[5] = (uint8_t)(save->scene >> 8);
    out[6] = (uint8_t)(save->dialogue & 0xFFu);
    out[7] = (uint8_t)(save->dialogue >> 8);
    out[8] = (uint8_t)(save->chr & 0xFFu);
    out[9] = (uint8_t)(save->chr >> 8);
    out[10] = save->choice_len;
    out[11] = 0;   // 保留
    for (size_t i = 0; i < ATRI_CHOICE_HISTORY; ++i) {
        out[12 + i] = i < save->choice_len ? save->choice_pick[i] : 0;
    }
    return ATRI_SAVE_SIZE;
}

bool atri_save_decode(atri_save_t *save, const uint8_t *data, size_t len)
{
    if (!save || !data || len < ATRI_SAVE_SIZE_LEGACY) return false;
    if (data[0] != ATRI_SAVE_MAGIC) return false;
    memset(save, 0, sizeof(*save));
    save->chr = ATRI_CHAR_KEEP;
    if (data[1] == ATRI_SAVE_VERSION_LEGACY) {
        if (len < ATRI_SAVE_SIZE_LEGACY) return false;
        save->chapter = (uint16_t)(data[2] | ((uint16_t)data[3] << 8));
        save->scene = (uint16_t)(data[4] | ((uint16_t)data[5] << 8));
        save->dialogue = (uint16_t)(data[6] | ((uint16_t)data[7] << 8));
        save->choice_len = data[8] > ATRI_CHOICE_HISTORY ? ATRI_CHOICE_HISTORY : data[8];
        for (size_t i = 0; i < ATRI_CHOICE_HISTORY; ++i) save->choice_pick[i] = data[10 + i];
        return true;
    }
    if (data[1] != ATRI_SAVE_VERSION || len < ATRI_SAVE_SIZE) return false;
    save->chapter = (uint16_t)(data[2] | ((uint16_t)data[3] << 8));
    save->scene = (uint16_t)(data[4] | ((uint16_t)data[5] << 8));
    save->dialogue = (uint16_t)(data[6] | ((uint16_t)data[7] << 8));
    save->chr = (uint16_t)(data[8] | ((uint16_t)data[9] << 8));
    save->choice_len = data[10] > ATRI_CHOICE_HISTORY ? ATRI_CHOICE_HISTORY : data[10];
    for (size_t i = 0; i < ATRI_CHOICE_HISTORY; ++i) save->choice_pick[i] = data[12 + i];
    return true;
}
