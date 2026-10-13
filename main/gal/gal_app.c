// main/gal/gal_app.c —— 框架骨架的应用状态机实现。
//
// 剧情是一串事件(对白 / 选项 / 章节过场 / 结局),状态机负责把事件翻页
// 演出、维护历史和存档,并按框架规范解释三键操作。
#include "gal_app.h"

#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "gal_content.h"
#include "gal_music.h"
#include "gal_text.h"

#include "esp_log.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "gal";

// 快进:按住“上”期间每 180ms 推进一步。自动阅读:每句打完后再等 900ms;
// 章节过场自动停留 1600ms 后继续,不打断自动。
#define GAL_FAST_FORWARD_MS 180u
#define GAL_AUTO_ADVANCE_MS 900u
#define GAL_AUTO_TRANSITION_MS 1600u
// 自动进度最多每 2 秒落盘一次:NVS 擦写寿命有限,而且不需要更实时。
#define GAL_PROGRESS_SAVE_MS 2000u

LV_FONT_DECLARE(gal_cjk_16);
LV_FONT_DECLARE(gal_cjk_20);

static void enter_event(gal_app_t *app);
static void render_page(gal_app_t *app);
static void goto_page(gal_app_t *app, gal_page_t page);
static bool settings_from_reader(const gal_app_t *app);

// ---------------------------------------------------------------- 剧情锁
// 事件的可见性:条件台词只有满足标记才出现。
static bool event_visible(const gal_app_t *app, int event_index)
{
    if (event_index < 0 || event_index >= gal_content_event_count) return false;
    const gal_event_t *ev = &gal_content_events[event_index];
    if (ev->kind != GAL_EV_LINE) return true;
    const uint8_t need = gal_content_lines[ev->index].require_flag;
    return need == GAL_UNLOCK_NONE || gal_unlock_has_flag(&app->unlock, need);
}

static bool cg_unlocked(const gal_app_t *app, int index)
{
    if (index < 0 || index >= gal_content_cg_count) return false;
    const gal_cg_t *cg = &gal_content_cgs[index];
    if (cg->unlock_event == GAL_EVENT_NONE) return true;
    return gal_unlock_has_cg(&app->unlock, (uint8_t)index);
}

static bool chapter_unlocked(const gal_app_t *app, int index)
{
    if (index < 0 || index >= gal_content_chapter_count) return false;
    const gal_chapter_t *c = &gal_content_chapters[index];
    if (c->unlock_event != GAL_EVENT_NONE &&
        !gal_unlock_has_chapter(&app->unlock, (uint8_t)index)) {
        return false;
    }
    if (c->require_flag != GAL_UNLOCK_NONE &&
        !gal_unlock_has_flag(&app->unlock, c->require_flag)) {
        return false;
    }
    return true;
}

// 进入事件时结算解锁:CG / 章节按绑定的事件解锁,事件自带的标记写入剧情锁。
static void unlock_visit(gal_app_t *app)
{
    const int ev = app->event_index;
    const gal_event_t *event = &gal_content_events[ev];
    const gal_unlock_t before = app->unlock;

    for (int i = 0; i < gal_content_cg_count; ++i) {
        if (gal_content_cgs[i].unlock_event == ev) gal_unlock_grant_cg(&app->unlock, (uint8_t)i);
    }
    for (int i = 0; i < gal_content_chapter_count; ++i) {
        if (gal_content_chapters[i].unlock_event == ev) {
            gal_unlock_grant_chapter(&app->unlock, (uint8_t)i);
        }
    }
    if (event->flag_set != GAL_UNLOCK_NONE) {
        gal_unlock_set_flag(&app->unlock, event->flag_set);
    }
    if (event->kind == GAL_EV_ENDING) {
        gal_unlock_grant_ending(&app->unlock, event->index);
    }

    // 只在真的变了时才写 NVS(NVS 擦写寿命有限)。
    if (memcmp(&before, &app->unlock, sizeof(before)) != 0) {
        (void)gal_unlock_store(&app->unlock);
    }
}

// ---------------------------------------------------------------- 小工具
static uint32_t speed_ms(const gal_app_t *app)
{
    static const uint32_t SPEED_MS[4] = { 60, 40, 18, 0 };   // 0 = 瞬间显示整页
    const uint8_t idx = app->settings.text_speed > 3 ? 1 : app->settings.text_speed;
    return SPEED_MS[idx];
}

static const char *speed_label(const gal_app_t *app)
{
    static const char *const LABEL[4] = { "慢", "中", "快", "瞬间" };
    const uint8_t idx = app->settings.text_speed > 3 ? 1 : app->settings.text_speed;
    return LABEL[idx];
}

// 竖屏排版(框架规范):16px 档 13 字/行 × 5 行,20px 档 10 字/行 × 4 行。
static void layout_for(const gal_app_t *app, int *units_per_line, int *lines_per_page)
{
    if (app->settings.font_large) {
        *units_per_line = 20;
        *lines_per_page = GAL_TEXT_LINES_LARGE;
    } else {
        *units_per_line = 26;
        *lines_per_page = GAL_TEXT_LINES_SMALL;
    }
}

static void show_notice(gal_app_t *app, const char *text)
{
    snprintf(app->notice, sizeof(app->notice), "%s", text ? text : "");
    app->notice_ms = text && text[0] ? 2000 : 0;
    gal_ui_set_notice(&app->ui, app->notice);
}

static void move_selection(int *sel, int count, int delta)
{
    if (count <= 0) return;
    int next = *sel + delta;
    if (next < 0) next = 0;          // 列表不绕圈:到顶/到底停住
    if (next >= count) next = count - 1;
    *sel = next;
}

// ---------------------------------------------------------------- 章节与事件
// 事件所属的章节名:向后扫描最后一个章节过场。
static const char *chapter_name_at(int event_index)
{
    const char *name = "第 1 章";
    for (int i = 0; i <= event_index && i < gal_content_event_count; ++i) {
        if (gal_content_events[i].kind == GAL_EV_TRANSITION) {
            name = gal_content_transitions[gal_content_events[i].index].chapter;
        }
    }
    return name;
}

// 某章节的第一个事件(章节页从这里开始)。
static int chapter_first_event(int chapter)
{
    if (chapter <= 0) return 0;
    int seen = 0;
    for (int i = 0; i < gal_content_event_count; ++i) {
        if (gal_content_events[i].kind == GAL_EV_TRANSITION) {
            if (++seen == chapter) return i;
        }
    }
    return 0;
}

static gal_page_t event_page(const gal_event_t *ev)
{
    switch (ev->kind) {
    case GAL_EV_CHOICE: return GAL_PAGE_CHOICE;
    case GAL_EV_TRANSITION: return GAL_PAGE_TRANSITION;
    case GAL_EV_ENDING: return GAL_PAGE_ENDING;
    case GAL_EV_LINE:
    default: return GAL_PAGE_GAME;
    }
}

// 按当前事件切页并渲染。
static void enter_event(gal_app_t *app)
{
    if (app->event_index < 0 || app->event_index >= gal_content_event_count) {
        app->event_index = gal_content_event_count - 1;
    }
    unlock_visit(app);   // 到这里就算看过:解锁 CG / 章节并写入事件标记
    // 自动进度:记下当前事件与页,继续游戏就从这里接上。
    app->progress.valid = 1;
    app->progress.event_index = (uint16_t)app->event_index;
    app->progress.page_index = (uint16_t)app->page_index;
    app->progress_dirty = true;
    app->progress_accum_ms = 0;
    app->page = event_page(&gal_content_events[app->event_index]);
    // 快进碰到选项、过场或结局就停下,把操作交回给玩家(render_page 会同步指示);
    // 自动阅读不解除,过场自己会走过去,所以计时从头开始。
    if (app->page != GAL_PAGE_GAME) {
        app->fast_forward = false;
        app->auto_accum_ms = 0;
    }
    render_page(app);
}

static void advance_event(gal_app_t *app)
{
    // 跳过条件不满足的台词(分支的另一半)。
    int next = app->event_index + 1;
    while (next < gal_content_event_count && !event_visible(app, next)) next++;
    if (next >= gal_content_event_count) {
        goto_page(app, GAL_PAGE_TITLE);
        show_notice(app, "已经到最后了");
        return;
    }
    app->event_index = next;
    app->page_index = 0;
    app->shown_line = -1;
    // 注意:这里不清快进。按住“上”时快进要一直推到最后一句。
    enter_event(app);
}

static void history_add(gal_app_t *app, const gal_line_t *line)
{
    if (app->history_count >= GAL_HISTORY_MAX) return;
    app->history[app->history_count++] = line;
}

// 显示当前句的当前页(自动分页,长句不会被裁掉)。
static void show_line(gal_app_t *app, int line_index)
{
    if (line_index < 0 || line_index >= gal_content_line_count) return;
    const gal_line_t *line = &gal_content_lines[line_index];

    int units = 26;
    int lines = 5;
    layout_for(app, &units, &lines);

    uint32_t offsets[GAL_TEXT_MAX_PAGES + 1];
    int pages = gal_text_pages(line->text, units, lines, offsets, GAL_TEXT_MAX_PAGES + 1);
    if (pages < 1) pages = 1;
    if (pages > GAL_TEXT_MAX_PAGES) pages = GAL_TEXT_MAX_PAGES;
    if (app->page_index >= pages) app->page_index = pages - 1;
    app->page_count = pages;

    const uint32_t begin = offsets[app->page_index];
    uint32_t end = offsets[app->page_index + 1];
    if (end < begin) end = begin;
    size_t len = end - begin;
    if (len >= GAL_TEXT_CAPACITY) len = GAL_TEXT_CAPACITY - 1;
    char page_text[GAL_TEXT_CAPACITY];
    memcpy(page_text, line->text + begin, len);
    page_text[len] = '\0';

    // 快进时直接整页显示,不再走打字机。
    gal_ui_set_text(&app->ui, line->speaker, page_text,
                    app->fast_forward ? 0 : speed_ms(app),
                    line->image, line->body);
    gal_ui_set_chapter(&app->ui, chapter_name_at(app->event_index));
    app->shown_line = line_index;
    app->shown_page = app->page_index;
    // 翻页也算进度(恢复时回到同一页)。
    app->progress.valid = 1;
    app->progress.event_index = (uint16_t)app->event_index;
    app->progress.page_index = (uint16_t)app->page_index;
    app->progress_dirty = true;

    if (app->page_index == 0) history_add(app, line);
}

// 推进一句(打字中先全显;页内翻页优先,再前进到下一个事件)。
static void advance_reading(gal_app_t *app, bool fast)
{
    const gal_event_t *ev = &gal_content_events[app->event_index];
    if (ev->kind != GAL_EV_LINE) {
        advance_event(app);
        return;
    }
    if (!fast && gal_ui_typing(&app->ui)) {
        gal_ui_show_full_text(&app->ui);
        return;
    }
    if (app->page_index + 1 < app->page_count) {
        app->page_index++;
        show_line(app, ev->index);
        return;
    }
    advance_event(app);
}

// ---------------------------------------------------------------- 存读档
static void do_save(gal_app_t *app, int slot)
{
    if (slot < 0 || slot >= GAL_SAVE_SLOTS) return;
    gal_slot_t *s = &app->save.slots[slot];
    s->occupied = 1;
    s->event_index = (uint16_t)app->event_index;
    s->page_index = (uint16_t)app->page_index;
    snprintf(s->chapter, sizeof(s->chapter), "%s", chapter_name_at(app->event_index));
    if (!gal_save_store(&app->save)) {
        show_notice(app, "保存失败");
        return;
    }
    show_notice(app, "已保存");
    render_page(app);
}

static void do_load(gal_app_t *app, int slot)
{
    if (slot < 0 || slot >= GAL_SAVE_SLOTS) return;
    const gal_slot_t *s = &app->save.slots[slot];
    if (!s->occupied) {
        show_notice(app, "空槽位");
        return;
    }
    app->event_index = s->event_index;
    app->page_index = s->page_index;
    app->shown_line = -1;
    app->choice_sel = 0;
    enter_event(app);
    show_notice(app, "已读取");
}

// 开始新游戏:直接从第一句开始(章节选择在鉴赏室里)。
// 进入读档页时把光标放到第一个有存档的槽位。
static int first_occupied_slot(const gal_app_t *app)
{
    for (int i = 0; i < GAL_SAVE_SLOTS; ++i) {
        if (app->save.slots[i].occupied) return i;
    }
    return 0;
}

static void start_new_game(gal_app_t *app)
{
    app->history_count = 0;
    app->shown_line = -1;
    app->choice_sel = 0;
    app->event_index = 0;
    app->page_index = 0;
    gal_progress_clear(&app->progress);   // 新游戏不沿用上次进度
    enter_event(app);
}

// ---------------------------------------------------------------- 弹窗
enum {
    MODAL_ACT_NONE = 0,
    MODAL_ACT_SAVE,
};

static const char *const MODAL_CONFIRM_ACTIONS[] = { "取消", "确认覆盖" };

static void open_confirm_overwrite(gal_app_t *app)
{
    static char body[96];
    const gal_slot_t *s = &app->save.slots[app->save_sel];
    snprintf(body, sizeof(body), "%s 的进度将被新存档替换。\n此操作无法撤销。",
             s->chapter[0] ? s->chapter : "原有");
    app->modal = GAL_MODAL_CONFIRM;
    app->modal_page = GAL_PAGE_SAVE;
    app->modal_sel = 0;
    app->modal_action = MODAL_ACT_SAVE;
    gal_ui_show_modal(&app->ui, GAL_MODAL_CONFIRM, "覆盖该存档?", body,
                      MODAL_CONFIRM_ACTIONS, 2, 0);
}

static void close_modal(gal_app_t *app)
{
    gal_ui_hide_modal(&app->ui);
    app->modal = GAL_MODAL_NONE;
    render_page(app);
}

// ---------------------------------------------------------------- 渲染
static void fill_slots(gal_app_t *app, gal_list_t list, int selected)
{
    static char labels[GAL_SAVE_SLOTS][12];
    static char values[GAL_SAVE_SLOTS][GAL_SAVE_CHAPTER_LEN + 8];
    const char *label_ptrs[GAL_SAVE_SLOTS];
    const char *value_ptrs[GAL_SAVE_SLOTS];

    for (int i = 0; i < GAL_SAVE_SLOTS; ++i) {
        snprintf(labels[i], sizeof(labels[i]), "槽位 %02d", i + 1);
        label_ptrs[i] = labels[i];
        const gal_slot_t *s = &app->save.slots[i];
        if (s->occupied) {
            snprintf(values[i], sizeof(values[i]), "%s", s->chapter);
        } else {
            snprintf(values[i], sizeof(values[i]), "空槽位");
        }
        value_ptrs[i] = values[i];
    }
    gal_ui_set_rows(&app->ui, list, label_ptrs, value_ptrs, GAL_SAVE_SLOTS, selected);
    // 读档页的空槽位不可选(存档页的空槽位是可以写入的)。
    for (int i = 0; i < GAL_SAVE_SLOTS; ++i) {
        const bool usable = list == GAL_LIST_SAVE || app->save.slots[i].occupied;
        gal_ui_set_row_enabled(&app->ui, list, i, usable);
    }

    // 槽位预览 + 页脚。
    const gal_slot_t *sel = &app->save.slots[selected];
    if (sel->occupied) {
        gal_ui_set_save_preview(&app->ui, sel->chapter);
    } else {
        gal_ui_set_save_preview(&app->ui, "空槽位\n选择一个槽位后再操作");
    }
    gal_ui_set_footer(&app->ui, GAL_PAGE_SAVE, "共 3 个槽位");
    gal_ui_set_footer(&app->ui, GAL_PAGE_LOAD,
                      gal_save_any(&app->save) ? "共 3 个槽位" : "暂无存档");
}

static void fill_settings(gal_app_t *app)
{
    static const char *const LABELS_READER[7] = { "文字速度", "文字大小", "自动阅读",
                                                  "对话透明度", "恢复默认",
                                                  "返回标题", "返回" };
    static const char *const LABELS_TITLE[6] = { "文字速度", "文字大小", "自动阅读",
                                                 "对话透明度", "恢复默认", "返回" };
    const bool reader = settings_from_reader(app);
    static char transparency[8];
    snprintf(transparency, sizeof(transparency), "%u%%",
             (unsigned)app->settings.panel_transparency);
    const char *values_reader[7] = {
        speed_label(app),
        app->settings.font_large ? "大" : "小",
        app->settings.auto_play ? "开" : "关",
        transparency,
        "", "", "",
    };
    const char *values_title[6] = {
        speed_label(app),
        app->settings.font_large ? "大" : "小",
        app->settings.auto_play ? "开" : "关",
        transparency,
        "", "",
    };
    if (reader) {
        gal_ui_set_rows(&app->ui, GAL_LIST_SETTINGS, LABELS_READER, values_reader, 7,
                        app->settings_sel);
    } else {
        gal_ui_set_rows(&app->ui, GAL_LIST_SETTINGS, LABELS_TITLE, values_title, 6,
                        app->settings_sel);
    }
}

static void fill_history(gal_app_t *app)
{
    static char text[GAL_HISTORY_MAX][192];
    const char *labels[GAL_HISTORY_MAX];
    for (int i = 0; i < app->history_count; ++i) {
        const gal_line_t *line = app->history[i];
        if (line->speaker && line->speaker[0]) {
            snprintf(text[i], sizeof(text[i]), "%s\n%s", line->speaker, line->text);
        } else {
            snprintf(text[i], sizeof(text[i]), "旁白\n%s", line->text);
        }
        labels[i] = text[i];
    }
    gal_ui_set_rows(&app->ui, GAL_LIST_HISTORY, labels, NULL, app->history_count,
                    app->history_sel);
    if (app->history_count == 0) gal_ui_set_page_title(&app->ui, GAL_PAGE_HISTORY, "历史文本");
}

static void fill_chapters(gal_app_t *app)
{
    static char labels[8][32];
    const char *label_ptrs[8];
    const char *value_ptrs[8];
    const int count = gal_content_chapter_count > 8 ? 8 : gal_content_chapter_count;
    for (int i = 0; i < count; ++i) {
        snprintf(labels[i], sizeof(labels[i]), "%s", gal_content_chapters[i].name);
        label_ptrs[i] = labels[i];
        value_ptrs[i] = chapter_unlocked(app, i) ? "可选" : "锁定";
    }
    gal_ui_set_rows(&app->ui, GAL_LIST_CHAPTER, label_ptrs, value_ptrs, count,
                    app->chapter_sel);

    const gal_chapter_t *c = &gal_content_chapters[app->chapter_sel];
    static char info[192];
    snprintf(info, sizeof(info), "%s\n%s\n%s", c->name, c->summary, c->progress);
    gal_ui_set_chapter_info(&app->ui, info);
    gal_ui_set_action_hint(&app->ui, "单路线作品可省略此页");
}

static void fill_about(gal_app_t *app)
{
    static char text[1024];
    size_t used = 0;
    for (int i = 0; i < gal_content_about_row_count; ++i) {
        const int n = snprintf(text + used, sizeof(text) - used, "%s：%s\n",
                               gal_content_about_rows[i].label,
                               gal_content_about_rows[i].value);
        if (n > 0) used += (size_t)n;
        if (used >= sizeof(text)) break;
    }
    if (used < sizeof(text)) {
        snprintf(text + used, sizeof(text) - used, "\n%s", GAL_CONTENT_ABOUT);
    }
    gal_ui_set_about(&app->ui, GAL_CONTENT_TITLE, GAL_CONTENT_LOGO_SUB, text);
}

// CG 回想:只列出已经看过的场景(剧情锁),右侧显示所在章节。
static int s_recall_map[GAL_LIST_MAX_ROWS];
static int s_recall_shown;

static void fill_recalls(gal_app_t *app)
{
    static char labels[GAL_LIST_MAX_ROWS][48];
    static char footer[48];
    const char *label_ptrs[GAL_LIST_MAX_ROWS];
    const char *value_ptrs[GAL_LIST_MAX_ROWS];

    s_recall_shown = 0;
    for (int i = 0; i < gal_content_recall_count && s_recall_shown < GAL_LIST_MAX_ROWS; ++i) {
        const gal_recall_t *recall = &gal_content_recalls[i];
        if (!cg_unlocked(app, recall->cg_index)) continue;   // 没看过的场景不出现
        snprintf(labels[s_recall_shown], sizeof(labels[0]), "%s", recall->title);
        label_ptrs[s_recall_shown] = labels[s_recall_shown];
        value_ptrs[s_recall_shown] = recall->chapter;
        s_recall_map[s_recall_shown] = i;
        s_recall_shown++;
    }
    if (app->recall_sel >= s_recall_shown) {
        app->recall_sel = s_recall_shown > 0 ? s_recall_shown - 1 : 0;
    }
    gal_ui_set_rows(&app->ui, GAL_LIST_RECALL, label_ptrs, value_ptrs, s_recall_shown,
                    app->recall_sel);
    if (s_recall_shown == 0) {
        gal_ui_set_footer(&app->ui, GAL_PAGE_RECALL, "看过剧情后这里会出现回想");
    } else {
        snprintf(footer, sizeof(footer), "共 %d 段回想", s_recall_shown);
        gal_ui_set_footer(&app->ui, GAL_PAGE_RECALL, footer);
    }
}

// 音乐鉴赏:同样按剧情锁只列出听过的曲目;正在播放的显示“播放中”。
static int s_track_map[GAL_LIST_MAX_ROWS];
static int s_track_shown;

static void fill_tracks(gal_app_t *app)
{
    static char labels[GAL_LIST_MAX_ROWS][32];
    static char footer[48];
    const char *label_ptrs[GAL_LIST_MAX_ROWS];
    const char *value_ptrs[GAL_LIST_MAX_ROWS];
    const int playing = gal_music_current();

    s_track_shown = 0;
    for (int i = 0; i < gal_content_track_count && s_track_shown < GAL_LIST_MAX_ROWS; ++i) {
        const gal_track_t *track = &gal_content_tracks[i];
        if (!cg_unlocked(app, track->cg_index)) continue;
        snprintf(labels[s_track_shown], sizeof(labels[0]), "%s", track->title);
        label_ptrs[s_track_shown] = labels[s_track_shown];
        value_ptrs[s_track_shown] = (playing == i) ? "播放中" : track->duration;
        s_track_map[s_track_shown] = i;
        s_track_shown++;
    }
    if (app->music_sel >= s_track_shown) {
        app->music_sel = s_track_shown > 0 ? s_track_shown - 1 : 0;
    }
    gal_ui_set_rows(&app->ui, GAL_LIST_MUSIC, label_ptrs, value_ptrs, s_track_shown,
                    app->music_sel);
    if (s_track_shown == 0) {
        gal_ui_set_footer(&app->ui, GAL_PAGE_MUSIC, "听过剧情后这里会出现曲目");
    } else if (playing >= 0) {
        gal_ui_set_footer(&app->ui, GAL_PAGE_MUSIC, "短按确定停止播放");
    } else {
        snprintf(footer, sizeof(footer), "共 %d 曲 · 短按确定播放", s_track_shown);
        gal_ui_set_footer(&app->ui, GAL_PAGE_MUSIC, footer);
    }
}

static void fill_cg(gal_app_t *app)
{
    const gal_cg_t *cg = &gal_content_cgs[app->cg_sel];
    char index_text[24];
    snprintf(index_text, sizeof(index_text), "%s / %02d", cg->label, gal_content_cg_count);
    gal_ui_set_cg(&app->ui, index_text, cg->caption, "", cg->image);
}

// 鉴赏室缩略图:解锁状态由剧情锁的观看记录换算;未解锁只显示问号。
static void fill_gallery(gal_app_t *app)
{
    static gal_cg_t items[GAL_CG_TILES];
    static char footer[32];
    int count = gal_content_cg_count;
    if (count > GAL_CG_TILES) count = GAL_CG_TILES;
    int unlocked = 0;
    for (int i = 0; i < count; ++i) {
        items[i] = gal_content_cgs[i];
        items[i].unlocked = cg_unlocked(app, i);
        if (items[i].unlocked) unlocked++;
    }
    gal_ui_set_gallery(&app->ui, items, count, app->cg_sel);
    snprintf(footer, sizeof(footer), "已解锁 %d / %d", unlocked, count);
    gal_ui_set_footer(&app->ui, GAL_PAGE_CG_LIST, footer);
}

static void render_page(gal_app_t *app)
{
    gal_ui_show_page(&app->ui, app->page);
    // 自动阅读在对话页与章节过场都保持(过场会自动过去)。
    const bool auto_page = app->page == GAL_PAGE_GAME || app->page == GAL_PAGE_TRANSITION;
    gal_ui_set_mode(&app->ui, app->auto_mode && auto_page);

    switch (app->page) {
    case GAL_PAGE_START: {
        static const char *const rows[1] = { "开始游戏" };
        gal_ui_set_rows(&app->ui, GAL_LIST_START, rows, NULL, 1, 0);
        return;
    }
    case GAL_PAGE_TITLE: {
        gal_ui_set_rows(&app->ui, GAL_LIST_TITLE, gal_content_title_menu, NULL,
                        gal_content_title_menu_count, app->title_sel);
        // 继续游戏看自动进度;读取存档看手动槽位。
        const bool any_save = gal_save_any(&app->save);
        gal_ui_set_row_enabled(&app->ui, GAL_LIST_TITLE, GAL_TITLE_MENU_CONTINUE,
                               app->progress.valid);
        gal_ui_set_row_enabled(&app->ui, GAL_LIST_TITLE, GAL_TITLE_MENU_LOAD, any_save);
        return;
    }
    case GAL_PAGE_GAME: {
        const gal_event_t *ev = &gal_content_events[app->event_index];
        if (ev->kind != GAL_EV_LINE) {
            enter_event(app);
            return;
        }
        // 从子页面返回时保留已经显示的文字,不重新打字。
        if (app->shown_line != (int)ev->index || app->shown_page != app->page_index) {
            show_line(app, ev->index);
        }
        gal_ui_set_chapter(&app->ui, chapter_name_at(app->event_index));
        return;
    }
    case GAL_PAGE_CHOICE: {
        const gal_event_t *ev = &gal_content_events[app->event_index];
        if (ev->kind != GAL_EV_CHOICE) return;
        const gal_choice_t *choice = &gal_content_choices[ev->index];
        gal_ui_set_choice(&app->ui, choice->prompt, choice->options, choice->count,
                          app->choice_sel);
        // 条件不满足的选项置灰(剧情锁)。
        for (int i = 0; i < choice->count && i < GAL_LIST_MAX_ROWS; ++i) {
            const uint8_t need = choice->options[i].require_flag;
            const bool ok = need == GAL_UNLOCK_NONE || gal_unlock_has_flag(&app->unlock, need);
            gal_ui_set_row_enabled(&app->ui, GAL_LIST_CHOICE, i, ok);
        }
        return;
    }
    case GAL_PAGE_MENU:
        gal_ui_set_rows(&app->ui, GAL_LIST_MENU, gal_content_quick_menu, NULL,
                        gal_content_quick_menu_count, app->menu_sel);
        return;
    case GAL_PAGE_SAVE:
        fill_slots(app, GAL_LIST_SAVE, app->save_sel);
        return;
    case GAL_PAGE_LOAD:
        fill_slots(app, GAL_LIST_LOAD, app->load_sel);
        return;
    case GAL_PAGE_SETTINGS:
        fill_settings(app);
        return;
    case GAL_PAGE_HISTORY:
        fill_history(app);
        return;
    case GAL_PAGE_GALLERY: {
        // 鉴赏室:四个分类入口,数量按剧情锁实时统计。
        static char values[4][16];
        int cg_count = 0;
        for (int i = 0; i < gal_content_cg_count; ++i) {
            if (cg_unlocked(app, i)) cg_count++;
        }
        int recall_count = 0;
        for (int i = 0; i < gal_content_recall_count; ++i) {
            if (cg_unlocked(app, gal_content_recalls[i].cg_index)) recall_count++;
        }
        int track_count = 0;
        for (int i = 0; i < gal_content_track_count; ++i) {
            if (cg_unlocked(app, gal_content_tracks[i].cg_index)) track_count++;
        }
        snprintf(values[0], sizeof(values[0]), "%d/%d 张", cg_count, gal_content_cg_count);
        snprintf(values[1], sizeof(values[1]), "%d 段", recall_count);
        snprintf(values[2], sizeof(values[2]), "%d 章", gal_content_chapter_count);
        snprintf(values[3], sizeof(values[3]), "%d 曲", track_count);
        const char *value_ptrs[4] = { values[0], values[1], values[2], values[3] };
        gal_ui_set_rows(&app->ui, GAL_LIST_GALLERY, gal_content_gallery_menu, value_ptrs,
                        gal_content_gallery_menu_count, app->gallery_sel);
        return;
    }
    case GAL_PAGE_CG_LIST:
        fill_gallery(app);
        return;
    case GAL_PAGE_RECALL:
        fill_recalls(app);
        return;
    case GAL_PAGE_MUSIC:
        fill_tracks(app);
        return;
    case GAL_PAGE_CHAPTER:
        fill_chapters(app);
        return;
    case GAL_PAGE_TRANSITION: {
        const gal_event_t *ev = &gal_content_events[app->event_index];
        if (ev->kind != GAL_EV_TRANSITION) return;
        const gal_transition_t *tr = &gal_content_transitions[ev->index];
        gal_ui_set_transition(&app->ui, tr->eyebrow, tr->title, tr->subtitle);
        return;
    }
    case GAL_PAGE_ENDING: {
        const gal_event_t *ev = &gal_content_events[app->event_index];
        if (ev->kind != GAL_EV_ENDING) return;
        const gal_ending_t *ending = &gal_content_endings[ev->index];
        gal_ui_set_ending(&app->ui, ending->eyebrow, ending->title, ending->body);
        return;
    }
    case GAL_PAGE_ABOUT:
        fill_about(app);
        return;
    case GAL_PAGE_CG:
        fill_cg(app);
        return;
    default:
        return;
    }
}

static void goto_page(gal_app_t *app, gal_page_t page)
{
    // 离开音乐鉴赏就停止播放(回到列表再手动开)。
    if (page != GAL_PAGE_MUSIC) gal_music_stop();
    app->page = page;
    app->idle_ms = 0;
    app->fast_forward = false;
    app->auto_mode = false;
    render_page(app);
}

// ---------------------------------------------------------------- 按键:各页面
static void handle_start(gal_app_t *app, const gal_key_t *key)
{
    if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_OK) {
        app->title_sel = 0;
        goto_page(app, GAL_PAGE_TITLE);
    }
}

static void handle_title(gal_app_t *app, const gal_key_t *key)
{
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        move_selection(&app->title_sel, gal_content_title_menu_count,
                       key->btn == BSP_BTN_UP ? -1 : 1);
        gal_ui_select_row(&app->ui, GAL_LIST_TITLE, app->title_sel);
        return;
    }
    if (key->ev != BSP_BTN_CLICK || key->btn != BSP_BTN_OK) return;

    switch (app->title_sel) {
    case GAL_TITLE_MENU_NEW:
        start_new_game(app);
        break;
    case GAL_TITLE_MENU_CONTINUE: {
        // 继续游戏:直接从自动进度接上。
        if (!app->progress.valid) {
            show_notice(app, "没有进度");
            return;
        }
        app->event_index = app->progress.event_index;
        app->page_index = app->progress.page_index;
        app->shown_line = -1;
        enter_event(app);
        break;
    }
    case GAL_TITLE_MENU_LOAD:
        app->load_sel = first_occupied_slot(app);
        app->return_page = GAL_PAGE_TITLE;
        goto_page(app, GAL_PAGE_LOAD);
        break;
    case GAL_TITLE_MENU_SETTINGS:
        app->return_page = GAL_PAGE_TITLE;
        app->settings_sel = 0;
        goto_page(app, GAL_PAGE_SETTINGS);
        break;
    case GAL_TITLE_MENU_GALLERY:
        app->gallery_sel = 0;
        goto_page(app, GAL_PAGE_GALLERY);
        break;
    default:
        goto_page(app, GAL_PAGE_ABOUT);
        break;
    }
}

static void handle_game(gal_app_t *app, const gal_key_t *key)
{
    // 短按(确定/上/下任意一个):下一句(打字中先全显)。
    if (key->ev == BSP_BTN_CLICK &&
        (key->btn == BSP_BTN_OK || key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        app->fast_forward = false;
        advance_reading(app, false);
        return;
    }
    // 长按确定:游戏内快捷菜单(返回剧情、存读档、历史、设置、返回标题)。
    if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_LONG) {
        app->fast_forward = false;
        app->menu_sel = 0;
        goto_page(app, GAL_PAGE_MENU);
        return;
    }
    // 长按上:快进,松开停(快进不显示文字提示)。
    if (key->btn == BSP_BTN_UP) {
        if (key->ev == BSP_BTN_LONG) {
            app->fast_forward = true;
            app->ff_accum_ms = 0;
            gal_ui_show_full_text(&app->ui);
            return;
        }
        if (key->ev == BSP_BTN_RELEASE) {
            app->fast_forward = false;
            return;
        }
    }
    // 长按下:自动阅读开关(任意键停止,不另做提示)。
    if (key->btn == BSP_BTN_DOWN && key->ev == BSP_BTN_LONG) {
        app->settings.auto_play = app->settings.auto_play ? 0 : 1;
        gal_settings_store(&app->settings);
        app->auto_mode = app->settings.auto_play != 0;
        app->auto_accum_ms = 0;
        app->fast_forward = false;
        if (app->auto_mode) gal_ui_show_full_text(&app->ui);
        gal_ui_set_mode(&app->ui, app->auto_mode);
        return;
    }
}

static void handle_choice(gal_app_t *app, const gal_key_t *key)
{
    const gal_event_t *ev = &gal_content_events[app->event_index];
    if (ev->kind != GAL_EV_CHOICE) return;
    const gal_choice_t *choice = &gal_content_choices[ev->index];

    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        // 长按确定 = 返回剧情(选项本身必须选一个)。
        goto_page(app, GAL_PAGE_GAME);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        move_selection(&app->choice_sel, choice->count, key->btn == BSP_BTN_UP ? -1 : 1);
        gal_ui_select_row(&app->ui, GAL_LIST_CHOICE, app->choice_sel);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_OK) {
        const gal_option_t *opt = &choice->options[app->choice_sel];
        // 剧情锁:条件不满足的选项不能选。
        if (opt->require_flag != GAL_UNLOCK_NONE &&
            !gal_unlock_has_flag(&app->unlock, opt->require_flag)) {
            show_notice(app, "条件不足");
            return;
        }
        // 选中的分支会写剧情标记,后面的台词与解锁都看它。
        if (opt->flag_set != GAL_UNLOCK_NONE) {
            gal_unlock_set_flag(&app->unlock, opt->flag_set);
            (void)gal_unlock_store(&app->unlock);
        }
        app->choice_sel = 0;
        advance_event(app);
    }
}

static void handle_menu(gal_app_t *app, const gal_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        goto_page(app, GAL_PAGE_GAME);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        move_selection(&app->menu_sel, gal_content_quick_menu_count,
                       key->btn == BSP_BTN_UP ? -1 : 1);
        gal_ui_select_row(&app->ui, GAL_LIST_MENU, app->menu_sel);
        return;
    }
    if (key->ev != BSP_BTN_CLICK || key->btn != BSP_BTN_OK) return;

    switch (app->menu_sel) {
    case GAL_MENU_RESUME:
        goto_page(app, GAL_PAGE_GAME);
        break;
    case GAL_MENU_SAVE:
        app->save_sel = 0;
        app->return_page = GAL_PAGE_MENU;
        goto_page(app, GAL_PAGE_SAVE);
        break;
    case GAL_MENU_LOAD:
        app->load_sel = first_occupied_slot(app);
        app->return_page = GAL_PAGE_MENU;
        goto_page(app, GAL_PAGE_LOAD);
        break;
    case GAL_MENU_HISTORY:
        app->history_sel = app->history_count > 0 ? app->history_count - 1 : 0;
        app->return_page = GAL_PAGE_MENU;
        goto_page(app, GAL_PAGE_HISTORY);
        break;
    case GAL_MENU_SETTINGS:
        app->return_page = GAL_PAGE_MENU;
        app->settings_sel = 0;
        goto_page(app, GAL_PAGE_SETTINGS);
        break;
    default:
        goto_page(app, GAL_PAGE_TITLE);
        break;
    }
}

static void handle_save_load(gal_app_t *app, const gal_key_t *key)
{
    const bool saving = app->page == GAL_PAGE_SAVE;
    int *sel = saving ? &app->save_sel : &app->load_sel;
    const gal_list_t list = saving ? GAL_LIST_SAVE : GAL_LIST_LOAD;

    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        goto_page(app, app->return_page);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        move_selection(sel, GAL_SAVE_SLOTS, key->btn == BSP_BTN_UP ? -1 : 1);
        fill_slots(app, list, *sel);
        return;
    }
    if (key->ev != BSP_BTN_CLICK || key->btn != BSP_BTN_OK) return;

    if (saving) {
        if (app->save.slots[*sel].occupied) {
            open_confirm_overwrite(app);   // 覆盖已有进度前先确认
        } else {
            do_save(app, *sel);
        }
        return;
    }
    if (!app->save.slots[*sel].occupied) {
        // 空槽位没有可读的进度:空槽位本身也是不可选的。
        show_notice(app, "空槽位");
        return;
    }
    do_load(app, *sel);
}

enum {
    SACT_SPEED = 0,
    SACT_SIZE,
    SACT_AUTO,
    SACT_TRANSPARENCY,
    SACT_RESET,
    SACT_TITLE,
    SACT_BACK,
};

static bool settings_from_reader(const gal_app_t *app)
{
    return app->return_page == GAL_PAGE_GAME || app->return_page == GAL_PAGE_MENU;
}

static int settings_action(const gal_app_t *app, int index)
{
    if (settings_from_reader(app)) return index;   // 阅读中:下标即动作
    static const int TITLE_MAP[6] = { SACT_SPEED, SACT_SIZE, SACT_AUTO,
                                      SACT_TRANSPARENCY, SACT_RESET, SACT_BACK };
    return (index >= 0 && index < 6) ? TITLE_MAP[index] : SACT_BACK;
}

static void handle_settings(gal_app_t *app, const gal_key_t *key)
{
    const int count = settings_from_reader(app) ? 7 : 6;
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        goto_page(app, app->return_page);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        move_selection(&app->settings_sel, count, key->btn == BSP_BTN_UP ? -1 : 1);
        gal_ui_select_row(&app->ui, GAL_LIST_SETTINGS, app->settings_sel);
        return;
    }
    if (key->ev != BSP_BTN_CLICK || key->btn != BSP_BTN_OK) return;

    switch (settings_action(app, app->settings_sel)) {
    case SACT_SPEED:
        app->settings.text_speed = (uint8_t)((app->settings.text_speed + 1) % 4);
        break;
    case SACT_SIZE:
        app->settings.font_large = app->settings.font_large ? 0 : 1;
        gal_ui_set_font_size(&app->ui, app->settings.font_large != 0);
        break;
    case SACT_AUTO:
        app->settings.auto_play = app->settings.auto_play ? 0 : 1;
        break;
    case SACT_TRANSPARENCY:
        app->settings.panel_transparency =
            gal_transparency_next(app->settings.panel_transparency);
        gal_ui_set_panel_transparency(&app->ui, app->settings.panel_transparency);
        break;
    case SACT_RESET:
        gal_settings_reset(&app->settings);
        gal_ui_set_font_size(&app->ui, app->settings.font_large != 0);
        gal_ui_set_panel_transparency(&app->ui, app->settings.panel_transparency);
        break;
    case SACT_TITLE:
        goto_page(app, GAL_PAGE_TITLE);
        return;
    default:   // SACT_BACK
        goto_page(app, app->return_page);
        return;
    }
    gal_settings_store(&app->settings);
    fill_settings(app);   // 列表右侧的值就地刷新
}

static void handle_history(gal_app_t *app, const gal_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        goto_page(app, app->return_page);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        const int count = app->history_count > 0 ? app->history_count : 1;
        move_selection(&app->history_sel, count, key->btn == BSP_BTN_UP ? -1 : 1);
        gal_ui_select_row(&app->ui, GAL_LIST_HISTORY, app->history_sel);
    }
}

static void handle_gallery(gal_app_t *app, const gal_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        goto_page(app, GAL_PAGE_TITLE);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        move_selection(&app->gallery_sel, gal_content_gallery_menu_count,
                       key->btn == BSP_BTN_UP ? -1 : 1);
        gal_ui_select_row(&app->ui, GAL_LIST_GALLERY, app->gallery_sel);
        return;
    }
    if (key->ev != BSP_BTN_CLICK || key->btn != BSP_BTN_OK) return;

    if (app->gallery_sel == GAL_GALLERY_RECALL) {
        // CG 回想:重看已看过的场景。
        app->recall_sel = 0;
        goto_page(app, GAL_PAGE_RECALL);
        return;
    }
    if (app->gallery_sel == GAL_GALLERY_STORY) {
        // 剧情鉴赏:章节 / 路线选择。
        app->chapter_sel = 0;
        app->return_page = GAL_PAGE_GALLERY;
        goto_page(app, GAL_PAGE_CHAPTER);
        return;
    }
    if (app->gallery_sel == GAL_GALLERY_MUSIC) {
        // 音乐鉴赏。
        app->music_sel = 0;
        goto_page(app, GAL_PAGE_MUSIC);
        return;
    }
    app->cg_sel = 0;
    goto_page(app, GAL_PAGE_CG_LIST);
}

static void handle_cg_list(gal_app_t *app, const gal_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        goto_page(app, GAL_PAGE_GALLERY);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        move_selection(&app->cg_sel, gal_content_cg_count, key->btn == BSP_BTN_UP ? -1 : 1);
        gal_ui_set_gallery(&app->ui, gal_content_cgs, gal_content_cg_count, app->cg_sel);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_OK) {
        if (!cg_unlocked(app, app->cg_sel)) {
            show_notice(app, "尚未解锁");
            return;
        }
        goto_page(app, GAL_PAGE_CG);
    }
}

static void handle_chapter(gal_app_t *app, const gal_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        goto_page(app, app->return_page);   // 从鉴赏室进来的,就回鉴赏室
        return;
    }
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        move_selection(&app->chapter_sel, gal_content_chapter_count,
                       key->btn == BSP_BTN_UP ? -1 : 1);
        fill_chapters(app);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_OK) {
        if (!chapter_unlocked(app, app->chapter_sel)) {
            show_notice(app, "章节未解锁");
            return;
        }
        app->event_index = chapter_first_event(app->chapter_sel);
        app->page_index = 0;
        app->shown_line = -1;
        app->history_count = 0;
        enter_event(app);
    }
}

static void handle_transition(gal_app_t *app, const gal_key_t *key)
{
    // 过场没有多余的“继续”按钮:任意短按进入下一段剧情。
    if (key->ev == BSP_BTN_CLICK) {
        advance_event(app);
    }
}

static void handle_ending(gal_app_t *app, const gal_key_t *key)
{
    // 结局页没有后续入口按钮:短按或长按确定都回到标题页。
    if ((key->ev == BSP_BTN_CLICK || key->ev == BSP_BTN_LONG) && key->btn == BSP_BTN_OK) {
        app->title_sel = 0;
        goto_page(app, GAL_PAGE_TITLE);
    }
}

static void handle_about(gal_app_t *app, const gal_key_t *key)
{
    if ((key->ev == BSP_BTN_CLICK || key->ev == BSP_BTN_LONG) &&
        (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        const int dir = key->btn == BSP_BTN_UP ? -1 : 1;
        gal_ui_scroll(&app->ui, GAL_PAGE_ABOUT, dir * (key->ev == BSP_BTN_LONG ? 80 : 20));
        return;
    }
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        goto_page(app, GAL_PAGE_TITLE);
    }
}

static void handle_cg(gal_app_t *app, const gal_key_t *key)
{
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        move_selection(&app->cg_sel, gal_content_cg_count, key->btn == BSP_BTN_UP ? -1 : 1);
        fill_cg(app);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_OK) {
        goto_page(app, GAL_PAGE_CG_LIST);
    }
}

static void handle_recall(gal_app_t *app, const gal_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        goto_page(app, GAL_PAGE_GALLERY);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        move_selection(&app->recall_sel, s_recall_shown, key->btn == BSP_BTN_UP ? -1 : 1);
        gal_ui_select_row(&app->ui, GAL_LIST_RECALL, app->recall_sel);
        return;
    }
    if (key->ev != BSP_BTN_CLICK || key->btn != BSP_BTN_OK) return;
    if (s_recall_shown <= 0) return;

    // 回想:直接回到那一段的事件,重新演一遍。
    const int index = s_recall_map[app->recall_sel];
    app->event_index = gal_content_recalls[index].event_index;
    app->page_index = 0;
    app->shown_line = -1;
    enter_event(app);
    show_notice(app, "回想");
}

static void handle_music(gal_app_t *app, const gal_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        gal_music_stop();
        goto_page(app, GAL_PAGE_GALLERY);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        move_selection(&app->music_sel, s_track_shown, key->btn == BSP_BTN_UP ? -1 : 1);
        gal_ui_select_row(&app->ui, GAL_LIST_MUSIC, app->music_sel);
        return;
    }
    if (key->ev != BSP_BTN_CLICK || key->btn != BSP_BTN_OK) return;
    if (s_track_shown <= 0) return;

    // 短按确定:播放 / 停止当前曲目。
    const int index = s_track_map[app->music_sel];
    if (gal_music_current() == index) {
        gal_music_stop();
    } else {
        gal_music_play(index);
    }
    fill_tracks(app);
}

static void handle_modal(gal_app_t *app, const gal_key_t *key)
{
    const int count = 2;
    // 弹窗里长按确定 = 取消。
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        close_modal(app);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        move_selection(&app->modal_sel, count, key->btn == BSP_BTN_UP ? -1 : 1);
        gal_ui_select_row(&app->ui, GAL_LIST_MODAL, app->modal_sel);
        return;
    }
    if (key->ev != BSP_BTN_CLICK || key->btn != BSP_BTN_OK) return;

    const int action = app->modal_action;
    const int sel = app->modal_sel;
    const gal_page_t from = app->modal_page;

    if (action == MODAL_ACT_SAVE) {
        close_modal(app);
        if (sel == 1) do_save(app, app->save_sel);
        return;
    }
    close_modal(app);
    if (from == GAL_PAGE_TITLE) goto_page(app, GAL_PAGE_TITLE);
}

// ---------------------------------------------------------------- 入口
void gal_app_key(gal_app_t *app, const gal_key_t *key)
{
    if (!app || !key) return;
    app->idle_ms = 0;

    // iot_button 会把窗口内的两次快速点击合并成 DOUBLE,且不补发单击:
    // 阅读时连按很常见,把双击当一次单击处理。
    gal_key_t as_click;
    if (key->ev == BSP_BTN_DOUBLE) {
        as_click = *key;
        as_click.ev = BSP_BTN_CLICK;
        key = &as_click;
    }

    if (!bsp_lvgl_lock(200)) {
        ESP_LOGW(TAG, "拿不到 LVGL 锁,忽略本次按键");
        return;
    }

    // 弹窗优先:它盖在当前页面上。
    if (app->modal != GAL_MODAL_NONE) {
        handle_modal(app, key);
        bsp_lvgl_unlock();
        return;
    }

    // 自动阅读中按任意键:退出自动阅读(右上角的“自动”指示会隐藏),并吃掉这次按键。
    // PRESS/RELEASE 不算“按键”:否则下键长按进入自动后,松手就会把它关掉。
    if (app->auto_mode && key->ev != BSP_BTN_PRESS && key->ev != BSP_BTN_RELEASE) {
        app->auto_mode = false;
        app->auto_accum_ms = 0;
        app->settings.auto_play = 0;
        gal_settings_store(&app->settings);
        gal_ui_set_mode(&app->ui, false);
        bsp_lvgl_unlock();
        return;
    }

    switch (app->page) {
    case GAL_PAGE_START: handle_start(app, key); break;
    case GAL_PAGE_TITLE: handle_title(app, key); break;
    case GAL_PAGE_GAME: handle_game(app, key); break;
    case GAL_PAGE_CHOICE: handle_choice(app, key); break;
    case GAL_PAGE_MENU: handle_menu(app, key); break;
    case GAL_PAGE_SAVE:
    case GAL_PAGE_LOAD: handle_save_load(app, key); break;
    case GAL_PAGE_SETTINGS: handle_settings(app, key); break;
    case GAL_PAGE_HISTORY: handle_history(app, key); break;
    case GAL_PAGE_GALLERY: handle_gallery(app, key); break;
    case GAL_PAGE_CG_LIST: handle_cg_list(app, key); break;
    case GAL_PAGE_RECALL: handle_recall(app, key); break;
    case GAL_PAGE_MUSIC: handle_music(app, key); break;
    case GAL_PAGE_CHAPTER: handle_chapter(app, key); break;
    case GAL_PAGE_TRANSITION: handle_transition(app, key); break;
    case GAL_PAGE_ENDING: handle_ending(app, key); break;
    case GAL_PAGE_ABOUT: handle_about(app, key); break;
    case GAL_PAGE_CG: handle_cg(app, key); break;
    default: break;
    }
    bsp_lvgl_unlock();
}

void gal_app_tick(gal_app_t *app, uint32_t elapsed_ms)
{
    if (!app) return;
    app->idle_ms += elapsed_ms;

    if (app->fast_forward && app->page == GAL_PAGE_GAME) {
        app->ff_accum_ms += elapsed_ms;
        if (app->ff_accum_ms >= GAL_FAST_FORWARD_MS) {
            app->ff_accum_ms = 0;
            if (bsp_lvgl_lock(100)) {
                advance_reading(app, true);
                bsp_lvgl_unlock();
            }
        }
    }

    // 自动阅读:当前页打完之后才开始计时,每 900ms 推进一步;
    // 章节过场停留固定时长后自动继续(自动模式不解除)。
    if (app->auto_mode && app->page == GAL_PAGE_GAME) {
        if (gal_ui_typing(&app->ui)) {
            app->auto_accum_ms = 0;
        } else {
            app->auto_accum_ms += elapsed_ms;
            if (app->auto_accum_ms >= GAL_AUTO_ADVANCE_MS) {
                app->auto_accum_ms = 0;
                if (bsp_lvgl_lock(100)) {
                    advance_reading(app, false);
                    bsp_lvgl_unlock();
                }
            }
        }
        // 自动阅读期间不算空闲:不调暗、不息屏、不进 deep sleep。
        app->idle_ms = 0;
    } else if (app->auto_mode && app->page == GAL_PAGE_TRANSITION) {
        app->auto_accum_ms += elapsed_ms;
        if (app->auto_accum_ms >= GAL_AUTO_TRANSITION_MS) {
            app->auto_accum_ms = 0;
            if (bsp_lvgl_lock(100)) {
                advance_event(app);
                bsp_lvgl_unlock();
            }
        }
        app->idle_ms = 0;
    } else {
        app->auto_accum_ms = 0;
    }

    // 正在放曲子时也不进空闲:别在听音乐时熄屏。
    if (gal_music_current() >= 0) app->idle_ms = 0;

    if (app->notice_ms > 0) {
        app->notice_ms = app->notice_ms > elapsed_ms ? app->notice_ms - elapsed_ms : 0;
        if (app->notice_ms == 0 && bsp_lvgl_lock(50)) {
            gal_ui_set_notice(&app->ui, NULL);
            bsp_lvgl_unlock();
        }
    }

    // 自动进度节流落盘。
    if (app->progress_dirty) {
        app->progress_accum_ms += elapsed_ms;
        if (app->progress_accum_ms >= GAL_PROGRESS_SAVE_MS) {
            app->progress_accum_ms = 0;
            if (gal_progress_store(&app->progress)) app->progress_dirty = false;
        }
    }

    app->battery_accum_ms += elapsed_ms;
    if (app->battery_accum_ms < 10000) return;
    app->battery_accum_ms = 0;
    const int soc = bsp_battery_soc();
    if (soc == app->battery_percent) return;
    app->battery_percent = soc;
    if (!bsp_lvgl_lock(50)) return;
    gal_ui_set_battery(&app->ui, soc);
    bsp_lvgl_unlock();
}

uint32_t gal_app_idle_ms(const gal_app_t *app)
{
    return app ? app->idle_ms : 0;
}

void gal_app_flush(gal_app_t *app)
{
    if (!app || !app->progress_dirty) return;
    if (gal_progress_store(&app->progress)) app->progress_dirty = false;
}

void gal_app_show_sleeping(gal_app_t *app)
{
    if (!app) return;
    gal_ui_set_notice(&app->ui, "已休眠");
}

bool gal_app_init(gal_app_t *app)
{
    if (!app) return false;
    memset(app, 0, sizeof(*app));
    app->battery_percent = -1;
    app->shown_line = -1;
    app->return_page = GAL_PAGE_TITLE;

    gal_settings_default(&app->settings);
    if (!gal_settings_init()) {
        ESP_LOGW(TAG, "设置存储不可用,本次使用默认值");
    } else {
        (void)gal_settings_load(&app->settings);
    }

    gal_save_clear(&app->save);
    if (!gal_save_init()) {
        ESP_LOGW(TAG, "存档存储不可用,本次运行内有效");
    } else {
        (void)gal_save_load(&app->save);
    }

    gal_unlock_clear(&app->unlock);
    if (!gal_unlock_init()) {
        ESP_LOGW(TAG, "解锁记录不可用,本次运行内有效");
    } else {
        (void)gal_unlock_load(&app->unlock);
    }

    gal_progress_clear(&app->progress);
    if (!gal_progress_init()) {
        ESP_LOGW(TAG, "进度存储不可用,本次运行内有效");
    } else {
        (void)gal_progress_load(&app->progress);
        if (app->progress.valid) {
            ESP_LOGI(TAG, "找到上次进度:事件 %u 第 %u 页",
                     (unsigned)app->progress.event_index,
                     (unsigned)app->progress.page_index + 1);
        }
    }

    if (!gal_music_init()) ESP_LOGW(TAG, "音乐播放任务创建失败,音乐鉴赏不可用");

    if (!bsp_lvgl_lock(-1)) {
        ESP_LOGE(TAG, "LVGL 未就绪");
        return false;
    }
    const bool ui_ok = gal_ui_create(&app->ui, &gal_cjk_16, &gal_cjk_20);
    if (ui_ok) {
        gal_ui_set_brand(&app->ui, GAL_CONTENT_TITLE, GAL_CONTENT_SUBTITLE);
        gal_ui_set_title_image(&app->ui, gal_content_title_image);
        gal_ui_set_battery(&app->ui, app->battery_percent);
        gal_ui_set_font_size(&app->ui, app->settings.font_large != 0);
        gal_ui_set_panel_transparency(&app->ui, app->settings.panel_transparency);
        app->auto_mode = app->settings.auto_play != 0;
        goto_page(app, GAL_PAGE_START);
    }
    bsp_lvgl_unlock();
    return ui_ok;
}
