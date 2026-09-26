// main/saya_model.c —— 阅读器纯逻辑实现。
#include "saya_model.h"

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

int saya_text_pages(const char *utf8, int units_per_line, int lines_per_page,
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
                line_units += units;
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
static int pages_of(const char *text, const saya_layout_t *layout)
{
    uint32_t offsets[SAYA_MAX_PAGES + 1];
    const int pages = saya_text_pages(text, layout->units_per_line, layout->lines_per_page,
                                     offsets, SAYA_MAX_PAGES + 1);
    return pages < 1 ? 1 : pages;
}

static void refresh_pages(saya_player_t *player, const saya_pack_t *pack, const saya_layout_t *layout)
{
    char text[SAYA_TEXT_BUFFER];
    saya_player_text(player, pack, text, sizeof(text));
    int pages = pages_of(text, layout);
    if (pages > SAYA_MAX_PAGES) pages = SAYA_MAX_PAGES;
    player->page_count = (uint16_t)pages;
    if (player->page >= player->page_count) player->page = player->page_count - 1;
}

static bool load_scene(saya_player_t *player, const saya_pack_t *pack, uint16_t chapter,
                       uint16_t scene, const saya_layout_t *layout)
{
    if (chapter >= pack->chapter_count) return false;
    saya_chapter_t ch;
    saya_pack_chapter(pack, chapter, &ch);
    if (scene >= ch.scene_count) return false;

    saya_scene_t sc;
    saya_pack_scene(pack, (uint16_t)(ch.first_scene + scene), &sc);

    player->chapter = chapter;
    player->scene = scene;
    player->dialogue = 0;
    player->page = 0;
    player->page_count = 1;
    player->at_choice = 0;
    player->ended = 0;
    player->end_name = SAYA_NONE;
    player->fg = SAYA_NONE;

    if (sc.choice_count > 0) {
        player->at_choice = 1;
        return true;
    }
    if (sc.dlg_count == 0) return false;

    saya_dialogue_t first;
    saya_pack_dialogue(pack, sc.first_dlg, &first);
    if (first.fg != SAYA_FG_KEEP && first.fg != SAYA_NONE) player->fg = first.fg;
    refresh_pages(player, pack, layout);
    return true;
}

void saya_player_reset(saya_player_t *player)
{
    if (!player) return;
    memset(player, 0, sizeof(*player));
    player->fg = SAYA_NONE;
    player->end_name = SAYA_NONE;
    player->page_count = 1;
}

bool saya_player_start(saya_player_t *player, const saya_pack_t *pack, uint16_t chapter,
                       const saya_layout_t *layout)
{
    if (!player || !pack || !layout) return false;
    saya_player_reset(player);
    return load_scene(player, pack, chapter, 0, layout);
}

saya_step_t saya_player_advance(saya_player_t *player, const saya_pack_t *pack,
                                const saya_layout_t *layout)
{
    if (!player || !pack) return SAYA_STEP_STUCK;
    if (player->ended) return SAYA_STEP_ENDING;
    if (player->at_choice) return SAYA_STEP_CHOICE;
    if (player->chapter >= pack->chapter_count) return SAYA_STEP_STUCK;

    saya_chapter_t ch;
    saya_pack_chapter(pack, player->chapter, &ch);
    if (player->scene >= ch.scene_count) return SAYA_STEP_STUCK;
    saya_scene_t sc;
    saya_pack_scene(pack, (uint16_t)(ch.first_scene + player->scene), &sc);
    if (sc.dlg_count == 0) return SAYA_STEP_STUCK;

    if (player->page + 1 < player->page_count) {
        player->page++;
        return SAYA_STEP_TEXT;
    }

    if (player->dialogue + 1 < sc.dlg_count) {
        player->dialogue++;
        saya_dialogue_t dlg;
        saya_pack_dialogue(pack, (uint16_t)(sc.first_dlg + player->dialogue), &dlg);
        if (dlg.fg != SAYA_FG_KEEP && dlg.fg != SAYA_NONE) player->fg = dlg.fg;
        player->page = 0;
        refresh_pages(player, pack, layout);
        return SAYA_STEP_TEXT;
    }

    saya_dialogue_t last;
    saya_pack_dialogue(pack, (uint16_t)(sc.first_dlg + sc.dlg_count - 1), &last);

    if (last.flags & SAYA_DLG_TO_SCENE) {
        const uint16_t target = (uint16_t)(player->scene + last.jump);
        if (load_scene(player, pack, player->chapter, target, layout)) return SAYA_STEP_SCENE;
        player->ended = 1;
        player->end_name = SAYA_NONE;
        return SAYA_STEP_ENDING;
    }

    if (last.flags & SAYA_DLG_END) {
        player->ended = 1;
        player->end_name = last.arg;
        return SAYA_STEP_ENDING;
    }

    // SAYA_DLG_BRANCH:源数据里没有用到,按普通推进处理(直接进入下一幕/下一章)。
    if (player->scene + 1 < ch.scene_count) {
        if (load_scene(player, pack, player->chapter, (uint16_t)(player->scene + 1), layout)) {
            return SAYA_STEP_SCENE;
        }
    }
    if (ch.next != SAYA_NONE && ch.next < pack->chapter_count) {
        if (load_scene(player, pack, ch.next, 0, layout)) return SAYA_STEP_CHAPTER;
    }

    player->ended = 1;
    player->end_name = SAYA_NONE;
    return SAYA_STEP_ENDING;
}

bool saya_player_choose(saya_player_t *player, const saya_pack_t *pack, uint8_t index,
                        const saya_layout_t *layout)
{
    if (!layout) return false;
    if (!player || !pack || !player->at_choice) return false;
    if (player->chapter >= pack->chapter_count) return false;
    saya_chapter_t ch;
    saya_pack_chapter(pack, player->chapter, &ch);
    if (player->scene >= ch.scene_count) return false;
    saya_scene_t sc;
    saya_pack_scene(pack, (uint16_t)(ch.first_scene + player->scene), &sc);
    if (index >= sc.choice_count) return false;
    const uint16_t target = sc.choice_href[index];
    if (target >= pack->chapter_count) return false;
    if (player->choice_len < SAYA_CHOICE_HISTORY) {
        player->choice_pick[player->choice_len++] = index;
    }
    return load_scene(player, pack, target, 0, layout);
}

bool saya_player_skip_scene(saya_player_t *player, const saya_pack_t *pack,
                            const saya_layout_t *layout)
{
    if (!player || !pack || !layout || player->at_choice || player->ended) return false;
    if (player->chapter >= pack->chapter_count) return false;
    saya_chapter_t ch;
    saya_pack_chapter(pack, player->chapter, &ch);
    if (player->scene + 1 >= ch.scene_count) return false;
    saya_scene_t sc;
    saya_pack_scene(pack, (uint16_t)(ch.first_scene + player->scene), &sc);
    if (sc.dlg_count == 0) return false;
    saya_dialogue_t last;
    saya_pack_dialogue(pack, (uint16_t)(sc.first_dlg + sc.dlg_count - 1), &last);
    if (last.flags & (SAYA_DLG_TO_SCENE | SAYA_DLG_END | SAYA_DLG_BRANCH)) return false;
    return load_scene(player, pack, player->chapter, (uint16_t)(player->scene + 1), layout);
}

bool saya_player_load(saya_player_t *player, const saya_pack_t *pack, const saya_save_t *save,
                      const saya_layout_t *layout)
{
    if (!player || !pack || !save) return false;
    if (!load_scene(player, pack, save->chapter, save->scene, layout)) return false;

    player->choice_len =
        save->choice_len > SAYA_CHOICE_HISTORY ? SAYA_CHOICE_HISTORY : save->choice_len;
    memcpy(player->choice_pick, save->choice_pick, sizeof(player->choice_pick));
    if (player->at_choice) return true;

    saya_chapter_t ch;
    saya_pack_chapter(pack, player->chapter, &ch);
    saya_scene_t sc;
    saya_pack_scene(pack, (uint16_t)(ch.first_scene + player->scene), &sc);
    if (save->dialogue >= sc.dlg_count) return true;   // 越界的对白下标:停在场景开头

    player->dialogue = save->dialogue;
    // 立绘在场景内是粘性的,存档直接记录了当时的立绘,优先采用它。
    if (save->fg != SAYA_NONE) player->fg = save->fg;
    player->page = 0;
    refresh_pages(player, pack, layout);
    return true;
}

size_t saya_player_text(const saya_player_t *player, const saya_pack_t *pack, char *out,
                        size_t capacity)
{
    if (!player || !pack || player->chapter >= pack->chapter_count) {
        return saya_pack_text(pack, 0, 0, out, capacity);
    }
    saya_chapter_t ch;
    saya_pack_chapter(pack, player->chapter, &ch);
    if (player->scene >= ch.scene_count) return saya_pack_text(pack, 0, 0, out, capacity);
    saya_scene_t sc;
    saya_pack_scene(pack, (uint16_t)(ch.first_scene + player->scene), &sc);
    if (player->dialogue >= sc.dlg_count) return saya_pack_text(pack, 0, 0, out, capacity);
    saya_dialogue_t dlg;
    saya_pack_dialogue(pack, (uint16_t)(sc.first_dlg + player->dialogue), &dlg);
    return saya_pack_text(pack, dlg.text_off, dlg.text_len, out, capacity);
}

size_t saya_player_page_text(const saya_player_t *player, const saya_pack_t *pack,
                             const saya_layout_t *layout, char *out, size_t capacity)
{
    char full[SAYA_TEXT_BUFFER];
    saya_player_text(player, pack, full, sizeof(full));
    uint32_t offsets[SAYA_MAX_PAGES + 1];
    int pages = saya_text_pages(full, layout->units_per_line, layout->lines_per_page,
                                offsets, SAYA_MAX_PAGES + 1);
    if (pages < 1) pages = 1;
    int page = player->page;
    if (page >= pages) page = pages - 1;
    const uint32_t start = offsets[page];
    const uint32_t end = offsets[page + 1];
    if (!out || capacity == 0) return 0;
    size_t len = (size_t)(end - start);
    if (len > capacity - 1) len = capacity - 1;
    memcpy(out, full + start, len);
    out[len] = '\0';
    return len;
}

size_t saya_player_speaker(const saya_player_t *player, const saya_pack_t *pack, char *out,
                           size_t capacity)
{
    if (!player || !pack || player->chapter >= pack->chapter_count) {
        return saya_pack_text(pack, 0, 0, out, capacity);
    }
    saya_chapter_t ch;
    saya_pack_chapter(pack, player->chapter, &ch);
    if (player->scene >= ch.scene_count) return saya_pack_text(pack, 0, 0, out, capacity);
    saya_scene_t sc;
    saya_pack_scene(pack, (uint16_t)(ch.first_scene + player->scene), &sc);
    if (player->dialogue >= sc.dlg_count) return saya_pack_text(pack, 0, 0, out, capacity);
    saya_dialogue_t dlg;
    saya_pack_dialogue(pack, (uint16_t)(sc.first_dlg + player->dialogue), &dlg);
    if (dlg.name == SAYA_NONE) return saya_pack_text(pack, 0, 0, out, capacity);
    return saya_pack_name(pack, dlg.name, out, capacity);
}

// ---------------------------------------------------------------- 存档
#define SAYA_SAVE_MAGIC 0x53u
#define SAYA_SAVE_VERSION 1u

size_t saya_save_encode(const saya_save_t *save, uint8_t *out, size_t capacity)
{
    if (!save || !out) return 0;
    const size_t need = 11u + SAYA_CHOICE_HISTORY;
    if (capacity < need) return 0;
    out[0] = SAYA_SAVE_MAGIC;
    out[1] = SAYA_SAVE_VERSION;
    out[2] = (uint8_t)(save->chapter & 0xFFu);
    out[3] = (uint8_t)(save->chapter >> 8);
    out[4] = (uint8_t)(save->scene & 0xFFu);
    out[5] = (uint8_t)(save->scene >> 8);
    out[6] = (uint8_t)(save->dialogue & 0xFFu);
    out[7] = (uint8_t)(save->dialogue >> 8);
    out[8] = (uint8_t)(save->fg & 0xFFu);
    out[9] = (uint8_t)(save->fg >> 8);
    out[10] = save->choice_len;
    for (size_t i = 0; i < SAYA_CHOICE_HISTORY; ++i) {
        out[11 + i] = i < save->choice_len ? save->choice_pick[i] : 0;
    }
    return need;
}

bool saya_save_decode(saya_save_t *save, const uint8_t *data, size_t len)
{
    if (!save || !data || len < 11u + SAYA_CHOICE_HISTORY) return false;
    if (data[0] != SAYA_SAVE_MAGIC || data[1] != SAYA_SAVE_VERSION) return false;
    memset(save, 0, sizeof(*save));
    save->chapter = (uint16_t)(data[2] | ((uint16_t)data[3] << 8));
    save->scene = (uint16_t)(data[4] | ((uint16_t)data[5] << 8));
    save->dialogue = (uint16_t)(data[6] | ((uint16_t)data[7] << 8));
    save->fg = (uint16_t)(data[8] | ((uint16_t)data[9] << 8));
    save->choice_len = data[10] > SAYA_CHOICE_HISTORY ? SAYA_CHOICE_HISTORY : data[10];
    for (size_t i = 0; i < SAYA_CHOICE_HISTORY; ++i) {
        save->choice_pick[i] = data[11 + i];
    }
    return true;
}
