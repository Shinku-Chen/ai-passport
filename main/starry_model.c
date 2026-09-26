// main/starry_model.c —— 阅读器纯逻辑实现。
#include "starry_model.h"

#include <string.h>

// ---------------------------------------------------------------- 文本宽度模型
int starry_char_units(uint32_t cp)
{
    // 与 tools/starry_font.py 的 char_units() 同一张 East Asian Width 表:
    // 全角 2 单位、半角 1 单位。字形包的单元格宽度也按它生成。
    if (cp == 0x00B7u) return 2;                       // 间隔号 ·:中文语境下按全角排
    if (cp < 0x1100u) return 1;                       // ASCII / 拉丁
    if (cp >= 0xFF61u && cp <= 0xFF9Fu) return 1;      // 半角片假名
    static const uint32_t wide[][2] = {
        { 0x1100u, 0x115Fu }, { 0x2E80u, 0x303Eu }, { 0x3041u, 0x33FFu },
        { 0x3400u, 0x4DBFu }, { 0x4E00u, 0x9FFFu }, { 0xA000u, 0xA4CFu },
        { 0xAC00u, 0xD7A3u }, { 0xF900u, 0xFAFFu }, { 0xFE30u, 0xFE6Fu },
        { 0xFF00u, 0xFF60u }, { 0xFFE0u, 0xFFE6u }, { 0x20000u, 0x3FFFDu },
        // 这些在 East Asian Width 里是 "Ambiguous",在中文语境下被排成全角
        // (Noto Sans SC 的实际推进也是 14~16px)。漏掉它们会让"——"这种长标点串
        // 被算成半角,一行塞进二十几个,画出 300px 以上的超长行。
        { 0x2010u, 0x2027u }, { 0x2030u, 0x205Eu }, { 0x2100u, 0x21FFu },
        { 0x2600u, 0x27BFu },
    };
    for (size_t i = 0; i < sizeof(wide) / sizeof(wide[0]); ++i) {
        if (cp >= wide[i][0] && cp <= wide[i][1]) return 2;
    }
    return 1;
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

// 换行核心:把一段 UTF-8 按禁则切行,再按 lines_per_group 行一组记录组起点。
// lines_per_group = 1 时就是"逐行"(starry_text_lines),= N 时就是"每页 N 行"。
static int wrap_text(const char *utf8, int units_per_line, int lines_per_group,
                     uint32_t *offsets, int max_offsets)
{
    if (!utf8) utf8 = "";
    if (units_per_line < 2) units_per_line = 2;
    if (lines_per_group < 1) lines_per_group = 1;

    const size_t len = strlen(utf8);
    int groups = 1;
    int line_units = 0;
    int line_index = 0;   // 本组里已经排满的行数
    size_t pos = 0;
    if (offsets && max_offsets > 0) offsets[0] = 0;

    while (pos < len) {
        const size_t before = pos;
        const uint32_t cp = utf8_next(utf8, len, &pos);
        size_t brk_at = 0;   // 非 0 表示本字符产生了断点
        if (cp == '\n') {
            line_units = 0;
            line_index++;
            brk_at = pos;    // 断在换行符之后
        } else {
            const int units = starry_char_units(cp);
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
        // 断点落在文本末尾时会多出一个空行(禁则标点挂行尾、或结尾换行),去掉它,
        // 否则分页与逐行都会比实际多一行(设备上表现为多出一个空页/空行)。
        if (groups > 1 && offsets[groups - 1] == (uint32_t)len) groups--;
        const int last = groups < max_offsets ? groups : max_offsets - 1;
        offsets[last] = (uint32_t)len;
    }
    return groups;
}

int starry_text_pages(const char *utf8, int units_per_line, int lines_per_page,
                      uint32_t *offsets, int max_offsets)
{
    return wrap_text(utf8, units_per_line, lines_per_page, offsets, max_offsets);
}

int starry_text_lines(const char *utf8, int units_per_line, uint32_t *offsets, int max_lines)
{
    return wrap_text(utf8, units_per_line, 1, offsets, max_lines);
}

// 打字机逐字显示用:返回 pos 处字符的下一个字节边界(到末尾返回 len,非法字节跳过 1)。
size_t starry_utf8_next_boundary(const char *utf8, size_t len, size_t pos)
{
    if (!utf8 || pos >= len) return len;
    size_t next = pos;
    (void)utf8_next(utf8, len, &next);
    return next > len ? len : next;
}

uint32_t starry_utf8_decode(const char *utf8, size_t len, size_t pos, size_t *next_out)
{
    size_t next = pos;
    const uint32_t cp = utf8 ? utf8_next(utf8, len, &next) : 0;
    if (next_out) *next_out = next > len ? len : next;
    return cp;
}

// ---------------------------------------------------------------- 播放状态
static int pages_of(const char *text, const starry_layout_t *layout)
{
    uint32_t offsets[STARRY_MAX_PAGES + 1];
    const int pages = starry_text_pages(text, layout->units_per_line, layout->lines_per_page,
                                        offsets, STARRY_MAX_PAGES + 1);
    return pages < 1 ? 1 : pages;
}

static void refresh_pages(starry_player_t *player, const starry_pack_t *pack,
                          const starry_layout_t *layout)
{
    char text[STARRY_TEXT_BUFFER];
    starry_player_text(player, pack, text, sizeof(text));
    int pages = pages_of(text, layout);
    if (pages > STARRY_MAX_PAGES) pages = STARRY_MAX_PAGES;
    player->page_count = (uint16_t)pages;
    if (player->page >= player->page_count) player->page = (uint16_t)(player->page_count - 1);
}

static bool load_scene(starry_player_t *player, const starry_pack_t *pack, uint16_t chapter,
                       uint16_t scene, const starry_layout_t *layout)
{
    if (chapter >= pack->chapter_count) return false;
    starry_chapter_t ch;
    starry_pack_chapter(pack, chapter, &ch);
    if (scene >= ch.scene_count) return false;

    starry_scene_t sc;
    starry_pack_scene(pack, (uint16_t)(ch.first_scene + scene), &sc);

    player->chapter = chapter;
    player->scene = scene;
    player->dialogue = 0;
    player->page = 0;
    player->page_count = 1;
    player->at_choice = 0;
    player->ended = 0;
    player->end_name = STARRY_NONE;
    player->sprite = STARRY_NONE;
    if (sc.bg != STARRY_BG_KEEP) player->bg = sc.bg;

    if (sc.choice_count > 0) {
        player->at_choice = 1;
        return true;
    }
    if (sc.dlg_count == 0) return false;

    starry_dialogue_t first;
    starry_pack_dialogue(pack, sc.first_dlg, &first);
    if (first.sprite != STARRY_FG_KEEP && first.sprite != STARRY_NONE) {
        player->sprite = first.sprite;
    }
    refresh_pages(player, pack, layout);
    return true;
}

void starry_player_reset(starry_player_t *player)
{
    if (!player) return;
    memset(player, 0, sizeof(*player));
    player->bg = STARRY_NONE;
    player->sprite = STARRY_NONE;
    player->end_name = STARRY_NONE;
    player->page_count = 1;
}

uint16_t starry_player_visible_sprite(const starry_player_t *player, const starry_pack_t *pack)
{
    if (!player || !pack) return STARRY_NONE;
    const uint16_t sprite = player->sprite;
    if (sprite == STARRY_NONE || player->ended) return STARRY_NONE;

    starry_bg_t bg;
    if (starry_pack_bg(pack, player->bg, &bg) && bg.no_sprite) return STARRY_NONE;

    starry_fg_t fg;
    if (!starry_pack_fg(pack, sprite, &fg)) return STARRY_NONE;
    if (fg.owner == STARRY_NONE) return STARRY_NONE;
    if (player->at_choice) return sprite;   // 选项句没有说话人,保留提问者

    char owner[64] = { 0 };
    char speaker[64] = { 0 };
    starry_pack_name(pack, fg.owner, owner, sizeof(owner));
    starry_player_speaker(player, pack, speaker, sizeof(speaker));
    return (owner[0] != '\0' && strcmp(owner, speaker) == 0) ? sprite : (uint16_t)STARRY_NONE;
}

bool starry_player_start(starry_player_t *player, const starry_pack_t *pack, uint16_t chapter,
                         const starry_layout_t *layout)
{
    if (!player || !pack || !layout) return false;
    starry_player_reset(player);
    return load_scene(player, pack, chapter, 0, layout);
}

starry_step_t starry_player_advance(starry_player_t *player, const starry_pack_t *pack,
                                    const starry_layout_t *layout)
{
    if (!player || !pack) return STARRY_STEP_STUCK;
    if (player->ended) return STARRY_STEP_ENDING;
    if (player->at_choice) return STARRY_STEP_CHOICE;
    if (player->chapter >= pack->chapter_count) return STARRY_STEP_STUCK;

    starry_chapter_t ch;
    starry_pack_chapter(pack, player->chapter, &ch);
    if (player->scene >= ch.scene_count) return STARRY_STEP_STUCK;
    starry_scene_t sc;
    starry_pack_scene(pack, (uint16_t)(ch.first_scene + player->scene), &sc);
    if (sc.dlg_count == 0) return STARRY_STEP_STUCK;

    if (player->page + 1 < player->page_count) {
        player->page++;
        return STARRY_STEP_TEXT;
    }

    if (player->dialogue + 1 < sc.dlg_count) {
        player->dialogue++;
        starry_dialogue_t dlg;
        starry_pack_dialogue(pack, (uint16_t)(sc.first_dlg + player->dialogue), &dlg);
        if (dlg.sprite != STARRY_FG_KEEP && dlg.sprite != STARRY_NONE) player->sprite = dlg.sprite;
        player->page = 0;
        refresh_pages(player, pack, layout);
        return STARRY_STEP_TEXT;
    }

    starry_dialogue_t last;
    starry_pack_dialogue(pack, (uint16_t)(sc.first_dlg + sc.dlg_count - 1), &last);

    if (last.flags & STARRY_DLG_TO_SCENE) {
        const uint16_t target = (uint16_t)(player->scene + last.jump);
        if (load_scene(player, pack, player->chapter, target, layout)) return STARRY_STEP_SCENE;
        player->ended = 1;
        player->end_name = STARRY_NONE;
        return STARRY_STEP_ENDING;
    }

    if (last.flags & STARRY_DLG_END) {
        player->ended = 1;
        player->end_name = last.arg;
        return STARRY_STEP_ENDING;
    }

    // STARRY_DLG_BRANCH:源数据里没有用到,按普通推进处理(直接进入下一幕/下一章)。
    if (player->scene + 1 < ch.scene_count) {
        if (load_scene(player, pack, player->chapter, (uint16_t)(player->scene + 1), layout)) {
            return STARRY_STEP_SCENE;
        }
    }
    if (ch.next != STARRY_NONE && ch.next < pack->chapter_count) {
        if (load_scene(player, pack, ch.next, 0, layout)) return STARRY_STEP_CHAPTER;
    }

    player->ended = 1;
    player->end_name = STARRY_NONE;
    return STARRY_STEP_ENDING;
}

bool starry_player_choose(starry_player_t *player, const starry_pack_t *pack, uint8_t index,
                          const starry_layout_t *layout)
{
    if (!layout) return false;
    if (!player || !pack || !player->at_choice) return false;
    if (player->chapter >= pack->chapter_count) return false;
    starry_chapter_t ch;
    starry_pack_chapter(pack, player->chapter, &ch);
    if (player->scene >= ch.scene_count) return false;
    starry_scene_t sc;
    starry_pack_scene(pack, (uint16_t)(ch.first_scene + player->scene), &sc);
    if (index >= sc.choice_count) return false;

    // 目标存的是全局场景下标,换算成"哪一章 + 章内第几幕"。
    uint16_t local_scene = 0;
    const int target_chapter = starry_pack_chapter_of_scene(pack, sc.choice_target[index],
                                                           &local_scene);
    if (target_chapter < 0) return false;
    if (player->choice_len < STARRY_CHOICE_HISTORY) {
        player->choice_pick[player->choice_len++] = index;
    }
    return load_scene(player, pack, (uint16_t)target_chapter, local_scene, layout);
}

bool starry_player_skip_chapter(starry_player_t *player, const starry_pack_t *pack,
                                const starry_layout_t *layout)
{
    if (!player || !pack || !layout || player->ended) return false;
    if (player->chapter >= pack->chapter_count) return false;
    starry_chapter_t ch;
    starry_pack_chapter(pack, player->chapter, &ch);
    if (ch.next == STARRY_NONE || ch.next >= pack->chapter_count) return false;
    return load_scene(player, pack, ch.next, 0, layout);
}

bool starry_player_skip_scene(starry_player_t *player, const starry_pack_t *pack,
                              const starry_layout_t *layout)
{
    if (!player || !pack || !layout || player->at_choice || player->ended) return false;
    if (player->chapter >= pack->chapter_count) return false;
    starry_chapter_t ch;
    starry_pack_chapter(pack, player->chapter, &ch);
    if (player->scene + 1 >= ch.scene_count) return false;
    starry_scene_t sc;
    starry_pack_scene(pack, (uint16_t)(ch.first_scene + player->scene), &sc);
    if (sc.dlg_count == 0) return false;
    starry_dialogue_t last;
    starry_pack_dialogue(pack, (uint16_t)(sc.first_dlg + sc.dlg_count - 1), &last);
    if (last.flags & (STARRY_DLG_TO_SCENE | STARRY_DLG_END | STARRY_DLG_BRANCH)) return false;
    return load_scene(player, pack, player->chapter, (uint16_t)(player->scene + 1), layout);
}

bool starry_player_load(starry_player_t *player, const starry_pack_t *pack,
                        const starry_save_t *save, const starry_layout_t *layout)
{
    if (!player || !pack || !save) return false;
    if (!load_scene(player, pack, save->chapter, save->scene, layout)) return false;

    player->choice_len =
        save->choice_len > STARRY_CHOICE_HISTORY ? STARRY_CHOICE_HISTORY : save->choice_len;
    memcpy(player->choice_pick, save->choice_pick, sizeof(player->choice_pick));
    // 存档里的背景/立绘是当时实际显示的那一张(背景在场景间是粘性的)。
    if (save->bg != STARRY_NONE) player->bg = save->bg;
    if (player->at_choice) return true;

    starry_chapter_t ch;
    starry_pack_chapter(pack, player->chapter, &ch);
    starry_scene_t sc;
    starry_pack_scene(pack, (uint16_t)(ch.first_scene + player->scene), &sc);
    if (save->dialogue >= sc.dlg_count) return true;   // 越界的对白下标:停在场景开头

    player->dialogue = save->dialogue;
    // 立绘在场景内是粘性的,存档直接记录了当时的立绘,优先采用它。
    if (save->sprite != STARRY_NONE) player->sprite = save->sprite;
    player->page = 0;
    refresh_pages(player, pack, layout);
    return true;
}

size_t starry_player_text(const starry_player_t *player, const starry_pack_t *pack, char *out,
                          size_t capacity)
{
    if (!player || !pack || player->chapter >= pack->chapter_count) {
        return starry_pack_text(pack, 0, 0, out, capacity);
    }
    starry_chapter_t ch;
    starry_pack_chapter(pack, player->chapter, &ch);
    if (player->scene >= ch.scene_count) return starry_pack_text(pack, 0, 0, out, capacity);
    starry_scene_t sc;
    starry_pack_scene(pack, (uint16_t)(ch.first_scene + player->scene), &sc);
    if (player->dialogue >= sc.dlg_count) return starry_pack_text(pack, 0, 0, out, capacity);
    starry_dialogue_t dlg;
    starry_pack_dialogue(pack, (uint16_t)(sc.first_dlg + player->dialogue), &dlg);
    return starry_pack_text(pack, dlg.text_off, dlg.text_len, out, capacity);
}

size_t starry_player_page_text(const starry_player_t *player, const starry_pack_t *pack,
                               const starry_layout_t *layout, char *out, size_t capacity)
{
    char full[STARRY_TEXT_BUFFER];
    starry_player_text(player, pack, full, sizeof(full));
    uint32_t offsets[STARRY_MAX_PAGES + 1];
    int pages = starry_text_pages(full, layout->units_per_line, layout->lines_per_page,
                                  offsets, STARRY_MAX_PAGES + 1);
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

size_t starry_player_speaker(const starry_player_t *player, const starry_pack_t *pack, char *out,
                             size_t capacity)
{
    if (!player || !pack || player->chapter >= pack->chapter_count) {
        return starry_pack_text(pack, 0, 0, out, capacity);
    }
    starry_chapter_t ch;
    starry_pack_chapter(pack, player->chapter, &ch);
    if (player->scene >= ch.scene_count) return starry_pack_text(pack, 0, 0, out, capacity);
    starry_scene_t sc;
    starry_pack_scene(pack, (uint16_t)(ch.first_scene + player->scene), &sc);
    if (player->dialogue >= sc.dlg_count) return starry_pack_text(pack, 0, 0, out, capacity);
    starry_dialogue_t dlg;
    starry_pack_dialogue(pack, (uint16_t)(sc.first_dlg + player->dialogue), &dlg);
    if (dlg.name == STARRY_NONE) return starry_pack_text(pack, 0, 0, out, capacity);
    return starry_pack_name(pack, dlg.name, out, capacity);
}

void starry_save_from_player(const starry_player_t *player, starry_save_t *out)
{
    if (!player || !out) return;
    memset(out, 0, sizeof(*out));
    out->chapter = player->chapter;
    out->scene = player->scene;
    out->dialogue = player->dialogue;
    out->bg = player->bg;
    out->sprite = player->sprite;
    out->choice_len = player->choice_len;
    memcpy(out->choice_pick, player->choice_pick, sizeof(out->choice_pick));
}

// ---------------------------------------------------------------- 存档
#define STARRY_SAVE_MAGIC 0x53u
#define STARRY_SAVE_VERSION 1u
#define STARRY_SAVE_FIXED 13u    // magic/version/chapter/scene/dialogue/bg/sprite/choice_len

size_t starry_save_encode(const starry_save_t *save, uint8_t *out, size_t capacity)
{
    if (!save || !out) return 0;
    const size_t need = STARRY_SAVE_FIXED + STARRY_CHOICE_HISTORY;
    if (capacity < need) return 0;
    out[0] = STARRY_SAVE_MAGIC;
    out[1] = STARRY_SAVE_VERSION;
    out[2] = (uint8_t)(save->chapter & 0xFFu);
    out[3] = (uint8_t)(save->chapter >> 8);
    out[4] = (uint8_t)(save->scene & 0xFFu);
    out[5] = (uint8_t)(save->scene >> 8);
    out[6] = (uint8_t)(save->dialogue & 0xFFu);
    out[7] = (uint8_t)(save->dialogue >> 8);
    out[8] = (uint8_t)(save->bg & 0xFFu);
    out[9] = (uint8_t)(save->bg >> 8);
    out[10] = (uint8_t)(save->sprite & 0xFFu);
    out[11] = (uint8_t)(save->sprite >> 8);
    out[12] = save->choice_len;
    for (size_t i = 0; i < STARRY_CHOICE_HISTORY; ++i) {
        out[STARRY_SAVE_FIXED + i] = i < save->choice_len ? save->choice_pick[i] : 0;
    }
    return need;
}

bool starry_save_decode(starry_save_t *save, const uint8_t *data, size_t len)
{
    if (!save || !data || len < STARRY_SAVE_FIXED + STARRY_CHOICE_HISTORY) return false;
    if (data[0] != STARRY_SAVE_MAGIC || data[1] != STARRY_SAVE_VERSION) return false;
    memset(save, 0, sizeof(*save));
    save->chapter = (uint16_t)(data[2] | ((uint16_t)data[3] << 8));
    save->scene = (uint16_t)(data[4] | ((uint16_t)data[5] << 8));
    save->dialogue = (uint16_t)(data[6] | ((uint16_t)data[7] << 8));
    save->bg = (uint16_t)(data[8] | ((uint16_t)data[9] << 8));
    save->sprite = (uint16_t)(data[10] | ((uint16_t)data[11] << 8));
    save->choice_len = data[12] > STARRY_CHOICE_HISTORY ? STARRY_CHOICE_HISTORY : data[12];
    for (size_t i = 0; i < STARRY_CHOICE_HISTORY; ++i) {
        save->choice_pick[i] = data[STARRY_SAVE_FIXED + i];
    }
    return true;
}
