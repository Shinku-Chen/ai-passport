// main/summer_app.c —— 阅读器应用状态机。
#include "summer_app.h"

#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"

#include "esp_log.h"

#include <stdio.h>
#include <string.h>

extern const lv_font_t gal_cjk_16;

static const summer_layout_t LAYOUT = { 36, 3 };

static void app_render_frame(summer_app_t *app);

static uint8_t s_save_buf[SUMMER_SAVE_CAP];
static summer_save_t s_save;
static summer_vm_t s_temp_vm;

static const char MANUAL_TEXT[] =
    "AI Passport 阅读器\n"
    "Summer Pockets REFLECTION BLUE\n\n"
    "UP 短按：下一句（打字中先补全整页）\n"
    "UP 长按：快进，松手即停\n"
    "DOWN 短按：历史记录\n"
    "DOWN 长按：自动阅读开关\n"
    "OK 短按：阅读菜单\n"
    "选项页：UP/DOWN 选择，OK 短按确认；长按 OK 打开菜单\n"
    "列表页：UP/DOWN 选择，OK 执行，长按 OK 返回\n"
    "CG 鉴赏：UP/DOWN 切换同场景差分，OK 返回列表\n\n"
    "本固件保留当前 RW5 源快照的 422 个场景、81,040 句正文、"
    "188 组选项与 136 组 CG；背景/CG 完整 fit 到 320x180 画面，"
    "立绘按源 267 姿势映射的实际底图与 40 份新增表情打包，"
    "不做 cover 裁切。\n\n"
    "存档：30 个手动栏位 + 独立自动档；通关记录与鉴赏进度独立保存。"
    "标题音乐与语音未收录，以把 Flash 留给剧情、CG 与 16px 完整字库。";

static const char *action_text(int action)
{
    switch (action) {
    case SUMMER_ACT_START: return "开始阅读";
    case SUMMER_ACT_RESTART: return "ReSTART";
    case SUMMER_ACT_ALKA: return "ALKA";
    case SUMMER_ACT_POCKET: return "Pocket";
    case SUMMER_ACT_CONTINUE: return "继续阅读";
    case SUMMER_ACT_LOAD: return "读取存档";
    case SUMMER_ACT_GALLERY: return "CG 鉴赏";
    case SUMMER_ACT_OPTIONS: return "设置";
    case SUMMER_ACT_MANUAL: return "操作说明";
    default: return "";
    }
}

static void app_notice(summer_app_t *app, const char *text)
{
    snprintf(app->notice, sizeof(app->notice), "%s", text ? text : "");
    app->notice_ms = 2600;
    summer_ui_set_notice(&app->ui, app->notice);
}

static void app_status(summer_app_t *app)
{
    summer_ui_set_chapter(&app->ui, app->chapter_label);
    summer_ui_set_battery(&app->ui, app->battery);
    summer_ui_set_auto(&app->ui, app->auto_mode, app->ff);
}

static void app_update_auto(summer_app_t *app)
{
    summer_ui_set_auto(&app->ui, app->auto_mode, app->ff);
}

static void app_reveal_text(summer_app_t *app)
{
    if (!app->typing) return;
    app->typing = false;
    app->type_accum = 0;
    snprintf(app->display_buf, sizeof(app->display_buf), "%s", app->page_text);
    summer_ui_set_text(&app->ui, app->display_buf);
}

static void app_unlock_cg(summer_app_t *app, uint16_t source_id)
{
    if (source_id < 1001 || source_id > 1856) return;
    const int group = summer_gallery_group_for_image(&app->pack, source_id);
    if (group < 0) return;
    if (summer_progress_set_cg(&app->store.progress, group)) {
        app->store.progress.dirty = true;
    }
}

static void app_update_images(summer_app_t *app)
{
    if (!app->vm_valid) return;
    if (app->shown_bg == app->vm.bg && app->shown_body == app->vm.body &&
        !app->image_error) {
        return;
    }
    const bool ok = summer_image_scene(&app->ui.canvas, &app->pack, app->vm.bg, app->vm.body);
    if (ok) {
        app->shown_bg = app->vm.bg;
        app->shown_body = app->vm.body;
        app->image_error = false;
        app_unlock_cg(app, app->vm.bg);
    } else {
        app->image_error = true;
        app_notice(app, "图片读取失败，稍后重试");
    }
}

static void app_history_push(summer_app_t *app)
{
    if (app->last_text_scene == app->vm.scene_id && app->last_text_op == app->vm.op_index) {
        return;
    }
    app->last_text_scene = app->vm.scene_id;
    app->last_text_op = app->vm.op_index;
    const int slot = app->history_head;
    snprintf(app->history_text[slot], sizeof(app->history_text[slot]), "%.*s",
             (int)(sizeof(app->history_text[slot]) - 1), app->full_text);
    snprintf(app->history_speaker[slot], sizeof(app->history_speaker[slot]), "%.*s",
             (int)(sizeof(app->history_speaker[slot]) - 1), app->speaker);
    app->history_head = (app->history_head + 1) % SUMMER_HISTORY_MAX;
    if (app->history_count < SUMMER_HISTORY_MAX) app->history_count++;
}

static int history_slot(const summer_app_t *app, int index)
{
    if (index < 0 || index >= app->history_count) return -1;
    return (app->history_head - app->history_count + index + SUMMER_HISTORY_MAX) %
           SUMMER_HISTORY_MAX;
}

static void app_draw_title(summer_app_t *app)
{
    if (!app->pack_ok) return;
    const uint16_t bg = app->pack.meta.title_bg_image;
    if (!summer_image_scene(&app->ui.canvas, &app->pack, bg, 0)) {
        summer_image_clear(&app->ui.canvas);
    }
    if (app->pack.meta.logo_image != SUMMER_IMAGE_NONE) {
        (void)summer_image_logo(&app->ui.canvas, &app->pack, app->pack.meta.logo_image,
                                8, 8, 200, 58);
    }
}

static void app_show_page_text(summer_app_t *app, int page, bool reset_typing)
{
    if (page < 0) page = 0;
    if (page >= app->page_count) page = app->page_count - 1;
    uint32_t start = 0, end = 0;
    if (!summer_text_page_text(app->full_text, &LAYOUT, page, &start, &end,
                               app->page_text, sizeof(app->page_text))) {
        snprintf(app->page_text, sizeof(app->page_text), "%.*s",
                 (int)(sizeof(app->page_text) - 1), app->full_text);
    }
    app->page_index = page;
    app->typing_pos = 0;
    if (reset_typing) {
        app->typing = app->store.text_speed != 0;
        app->type_accum = 0;
    }
    if (app->typing) {
        app->display_buf[0] = 0;
    } else {
        snprintf(app->display_buf, sizeof(app->display_buf), "%s", app->page_text);
        app->typing_pos = strlen(app->page_text);
    }
    summer_ui_set_game(&app->ui, app->speaker, app->display_buf, app->page_index + 1,
                       app->page_count);
    app_status(app);
    app->autosave_dirty = true;
}

static void app_enter_text(summer_app_t *app, const summer_step_t *step)
{
    if (!summer_pack_text_into(&app->pack, step->text, app->full_text,
                               sizeof(app->full_text))) {
        snprintf(app->full_text, sizeof(app->full_text), "（正文读取失败）");
    }
    const uint32_t speaker_off = summer_speaker_text(&app->pack, step->speaker);
    if (speaker_off == 0) app->speaker[0] = 0;
    else if (!summer_pack_text_into(&app->pack, speaker_off, app->speaker,
                                    sizeof(app->speaker))) {
        app->speaker[0] = 0;
    }
    snprintf(app->chapter_label, sizeof(app->chapter_label), "%s",
             summer_vm_scene_label(&app->vm, &app->pack));
    app->page_count = summer_text_pages(app->full_text, &LAYOUT, app->page_offsets,
                                        SUMMER_MAX_PAGES + 1);
    if (app->page_count <= 0) app->page_count = 1;
    app->page_index = 0;
    app->at_choice = false;
    app->at_map = false;
    app->ended = false;
    app->auto_accum = 0;
    app->ff_accum = 0;
    app->typing_pos = 0;
    app->typing = app->store.text_speed != 0;
    app->type_accum = 0;
    app->page = APP_PAGE_GAME;
    summer_ui_show_page(&app->ui, SUMMER_PAGE_GAME);
    app_show_page_text(app, 0, false);
    app_update_images(app);
    app_history_push(app);
    app_status(app);
    app->autosave_dirty = true;
    if (app->vm.mode == SUMMER_MODE_STORY) {
        (void)summer_progress_sync(&app->vm, &app->pack, &app->store.progress);
    }
}

static void app_build_choice_rows(summer_app_t *app)
{
    for (int i = 0; i < app->choice_count; ++i) {
        const summer_choice_t *item = summer_choice_item(&app->pack, (uint32_t)app->choice_group,
                                                         app->choice_items[i]);
        char text[96] = "";
        if (item) {
            if (!summer_pack_text_into(&app->pack, item->text, text, sizeof(text))) {
                snprintf(text, sizeof(text), "（选项读取失败）");
            }
            size_t n = strlen(text);
            if (n > 80) {
                n = 80;
                while (n > 0 && ((uint8_t)text[n] & 0xC0u) == 0x80u) n--;
                text[n] = 0;
            }
        }
        snprintf(app->choice_labels[i], sizeof(app->choice_labels[i]), "%s", text);
        app->choice_rows[i].text = app->choice_labels[i];
        app->choice_rows[i].meta = NULL;
        app->choice_rows[i].preview = NULL;
        app->choice_rows[i].disabled = false;
    }
}

static void app_enter_choice(summer_app_t *app, uint32_t group)
{
    app->choice_group = (int)group;
    int items[SUMMER_CHOICE_MAX];
    int count = summer_vm_visible_choices(&app->vm, &app->pack, group, items,
                                          SUMMER_CHOICE_MAX);
    if (count <= 0) {
        if (app->vm.error) {
            app->page = APP_PAGE_ERROR;
            summer_ui_show_page(&app->ui, SUMMER_PAGE_ERROR);
            summer_ui_set_error(&app->ui, "选项条件求值失败");
            return;
        }
        if (summer_vm_advance(&app->vm, &app->pack)) app_render_frame(app);
        return;
    }
    app->choice_count = count;
    for (int i = 0; i < count; ++i) app->choice_items[i] = items[i];
    app->choice_sel = app->vm.choice_sel;
    if (app->choice_sel < 0 || app->choice_sel >= count) app->choice_sel = 0;
    app->at_choice = true;
    app->at_map = false;
    app->typing = false;
    app->auto_mode = false;
    app->ff = false;
    app->page = APP_PAGE_CHOICE;
    app_build_choice_rows(app);
    app_update_images(app);
    summer_ui_show_page(&app->ui, SUMMER_PAGE_CHOICE);
    summer_ui_set_choice_rows(&app->ui, app->choice_rows, count, app->choice_sel);
    app_status(app);
    app->autosave_dirty = true;
}

static void app_build_map_rows(summer_app_t *app)
{
    for (int i = 0; i < app->map_count; ++i) {
        // Keep the source's two-line map option: the scene name on the card
        // and the mapLabels hint below it.
        const char *label = app->map_choices[i].label;
        const char *nl = strchr(label, '\n');
        const size_t name_len = nl ? (size_t)(nl - label) : strlen(label);
        const size_t keep = name_len < sizeof(app->map_labels[i])
                                ? name_len
                                : sizeof(app->map_labels[i]) - 1;
        snprintf(app->map_labels[i], sizeof(app->map_labels[i]), "%.*s", (int)keep, label);
        app->map_hints[i][0] = 0;
        if (nl && nl[1]) {
            snprintf(app->map_hints[i], sizeof(app->map_hints[i]), "%.*s",
                     (int)(sizeof(app->map_hints[i]) - 1), nl + 1);
        }
        app->map_rows[i].text = app->map_labels[i];
        app->map_rows[i].meta = NULL;
        app->map_rows[i].preview = app->map_hints[i][0] ? app->map_hints[i] : NULL;
        app->map_rows[i].disabled = false;
    }
}

static void app_enter_map(summer_app_t *app, uint32_t group)
{
    app->map_group = (int)group;
    app->map_count = summer_vm_preview_map(&app->vm, &app->pack, group, app->map_choices,
                                           SUMMER_MAP_MAX);
    if (app->map_count <= 0) {
        if (app->vm.error) {
            app->page = APP_PAGE_ERROR;
            summer_ui_show_page(&app->ui, SUMMER_PAGE_ERROR);
            summer_ui_set_error(&app->ui, "地图条件求值失败");
            return;
        }
        app->page = APP_PAGE_ERROR;
        summer_ui_show_page(&app->ui, SUMMER_PAGE_ERROR);
        summer_ui_set_error(&app->ui,
                            "此地图阶段暂无支持的剧情选项，可从标题选择其他篇章");
        return;
    }
    app->map_sel = app->vm.choice_sel;
    if (app->map_sel < 0 || app->map_sel >= app->map_count) app->map_sel = 0;
    app->at_map = true;
    app->at_choice = false;
    app->typing = false;
    app->auto_mode = false;
    app->ff = false;
    app->page = APP_PAGE_MAP;
    app_build_map_rows(app);
    app_update_images(app);
    summer_ui_show_page(&app->ui, SUMMER_PAGE_CHOICE);
    summer_ui_set_rows(&app->ui, SUMMER_LIST_CARD, "选择地点", "UP/DOWN 选择目的地",
                       "长按 OK 打开菜单", app->map_rows, app->map_count, app->map_sel);
    app_status(app);
    app->autosave_dirty = true;
}

static void app_enter_ending(summer_app_t *app)
{
    app->ended = true;
    app->auto_mode = false;
    app->ff = false;
    app->typing = false;
    const int ending = summer_vm_ending(&app->vm, &app->pack);
    if (ending > 0) {
        app->store.progress.cleared |= (uint16_t)(1u << (ending - 1));
        app->store.progress.dirty = true;
    }
    (void)summer_progress_sync(&app->vm, &app->pack, &app->store.progress);
    (void)summer_store_flush_progress(&app->store);
    snprintf(app->ending_body, sizeof(app->ending_body),
             ending > 0 ? "结局 %d / 14 · 可从标题选择其他篇章"
                        : "可从标题选择其他篇章", ending);
    app->page = APP_PAGE_ENDING;
    summer_ui_show_page(&app->ui, SUMMER_PAGE_ENDING);
    summer_ui_set_ending(&app->ui, "本篇阅读完毕", app->ending_body);
    app_status(app);
    app->autosave_dirty = true;
}

static void app_enter_error(summer_app_t *app, const char *message)
{
    app->auto_mode = false;
    app->ff = false;
    app->typing = false;
    app->page = APP_PAGE_ERROR;
    summer_ui_show_page(&app->ui, SUMMER_PAGE_ERROR);
    summer_ui_set_error(&app->ui, message);
    app_status(app);
}

static void app_render_frame(summer_app_t *app)
{
    if (!app->vm_valid) {
        app_enter_error(app, "没有可读取的剧情");
        return;
    }
    // index.ux applies withProgress() before every story frame (not in chapter
    // browsing).  Re-apply here so globals/clear flags/stage keep the source
    // overlay semantics across long scenes.
    if (app->vm.mode == SUMMER_MODE_STORY) {
        summer_progress_apply(&app->vm, &app->pack, &app->store.progress);
    }
    for (int i = 0; i < 512; ++i) {
        const summer_step_t step = summer_vm_step(&app->vm, &app->pack, 256);
        switch (step.kind) {
        case SUMMER_STEP_TEXT:
            app_enter_text(app, &step);
            return;
        case SUMMER_STEP_CHOICE:
            app_enter_choice(app, step.group);
            return;
        case SUMMER_STEP_MAP:
            app_enter_map(app, step.group);
            return;
        case SUMMER_STEP_END:
            app_enter_ending(app);
            return;
        case SUMMER_STEP_YIELD:
            break;
        case SUMMER_STEP_ERROR:
        default:
            app_enter_error(app, "剧情数据错误，已停止读取");
            return;
        }
    }
    app_enter_error(app, "此章节无法继续，可从标题选择其他篇章");
}

static void app_advance_game(summer_app_t *app)
{
    if (app->page != APP_PAGE_GAME || !app->vm_valid) return;
    if (app->typing) {
        app_reveal_text(app);
        app->auto_accum = 0;
        return;
    }
    if (app->page_index + 1 < app->page_count) {
        app_show_page_text(app, app->page_index + 1, true);
        app->auto_accum = 0;
        return;
    }
    if (!summer_vm_advance(&app->vm, &app->pack)) {
        app_enter_error(app, "剧情位置无效");
        return;
    }
    app->autosave_dirty = true;
    app->auto_accum = 0;
    app_render_frame(app);
}

static void app_build_title(summer_app_t *app)
{
    app->title_actions = (uint8_t)summer_progress_title_actions(&app->store.progress);
    int n = 0;
    if (app->title_actions & SUMMER_TITLE_RESTART) {
        app->title_action_ids[n++] = SUMMER_ACT_RESTART;
    } else if (app->title_actions & SUMMER_TITLE_START) {
        app->title_action_ids[n++] = SUMMER_ACT_START;
    }
    if (app->title_actions & SUMMER_TITLE_ALKA) app->title_action_ids[n++] = SUMMER_ACT_ALKA;
    if (app->title_actions & SUMMER_TITLE_POCKET) app->title_action_ids[n++] = SUMMER_ACT_POCKET;
    app->title_action_ids[n++] = SUMMER_ACT_CONTINUE;
    app->title_action_ids[n++] = SUMMER_ACT_LOAD;
    app->title_action_ids[n++] = SUMMER_ACT_GALLERY;
    app->title_action_ids[n++] = SUMMER_ACT_OPTIONS;
    app->title_action_ids[n++] = SUMMER_ACT_MANUAL;
    app->title_has_auto = false;
    size_t auto_len = 0;
    if (summer_store_read_auto(&app->store, s_save_buf, sizeof(s_save_buf), &auto_len) &&
        auto_len > 0) {
        app->title_has_auto = true;
    }
    for (int i = 0; i < n; ++i) {
        const int action = app->title_action_ids[i];
        snprintf(app->title_labels[i], sizeof(app->title_labels[i]), "%s", action_text(action));
        app->title_rows[i].text = app->title_labels[i];
        app->title_rows[i].meta = NULL;
        app->title_rows[i].preview = NULL;
        app->title_rows[i].disabled = action == SUMMER_ACT_CONTINUE && !app->title_has_auto;
    }
    app->title_count = n;
    if (app->title_sel >= n) app->title_sel = n - 1;
    if (app->title_sel < 0) app->title_sel = 0;
    char progress[48];
    const int heroines = summer_progress_heroine_count(&app->store.progress);
    snprintf(progress, sizeof(progress), "已通关 %d/5", heroines);
    if (app->store.progress.restart_stage) {
        char more[24];
        snprintf(more, sizeof(more), " · ReSTART %u", app->store.progress.restart_stage);
        strncat(progress, more, sizeof(progress) - strlen(progress) - 1);
    }
    summer_ui_show_page(&app->ui, SUMMER_PAGE_TITLE);
    summer_ui_set_title_rows(&app->ui, app->title_rows, n, app->title_sel, progress);
    app_status(app);
}

static bool app_autosave(summer_app_t *app, bool force);

static void app_open_title(summer_app_t *app)
{
    if (!app->pack_ok) {
        app_enter_error(app, "资源包校验失败，请重新构建固件");
        return;
    }
    if (app->vm_valid && app->page != APP_PAGE_TITLE) {
        (void)app_autosave(app, true);
        (void)summer_store_flush_progress(&app->store);
    }
    app->auto_mode = false;
    app->ff = false;
    app->typing = false;
    app->at_choice = false;
    app->at_map = false;
    app->page = APP_PAGE_TITLE;
    app_draw_title(app);
    app_build_title(app);
}

static void app_start_story(summer_app_t *app, int action)
{
    bool ok = false;
    if (action == SUMMER_ACT_ALKA || action == SUMMER_ACT_POCKET) {
        ok = summer_vm_start_flow(&app->vm, &app->pack, action);
    } else {
        ok = summer_vm_start_story(&app->vm, &app->pack);
    }
    if (!ok) {
        app_enter_error(app, "无法进入剧情（资源包入口无效）");
        return;
    }
    summer_progress_apply(&app->vm, &app->pack, &app->store.progress);
    app->vm_valid = true;
    app->shown_bg = 0xFFFF;
    app->shown_body = 0xFFFF;
    app->image_error = false;
    app->page_index = 0;
    app->typing = false;
    app->auto_mode = false;
    app->ff = false;
    app->at_choice = false;
    app->at_map = false;
    app->ended = false;
    app->choice_sel = 0;
    app->map_sel = 0;
    app->page = APP_PAGE_GAME;
    app->autosave_dirty = true;
    app_render_frame(app);
}

static void app_start_chapter(summer_app_t *app, uint16_t scene)
{
    if (!summer_vm_start_chapter(&app->vm, &app->pack, scene)) {
        app_notice(app, "章节入口无效");
        return;
    }
    app->vm_valid = true;
    app->shown_bg = 0xFFFF;
    app->shown_body = 0xFFFF;
    app->image_error = false;
    app->typing = false;
    app->auto_mode = false;
    app->ff = false;
    app->at_choice = false;
    app->at_map = false;
    app->ended = false;
    app->choice_sel = 0;
    app->map_sel = 0;
    app->page = APP_PAGE_GAME;
    app->autosave_dirty = true;
    app_render_frame(app);
}

static void app_build_menu(summer_app_t *app)
{
    snprintf(app->menu_labels[0], sizeof(app->menu_labels[0]), "返回游戏");
    snprintf(app->menu_labels[1], sizeof(app->menu_labels[1]), "自动播放：%s",
             app->auto_mode ? "开" : "关");
    snprintf(app->menu_labels[2], sizeof(app->menu_labels[2]), "保存进度");
    snprintf(app->menu_labels[3], sizeof(app->menu_labels[3]), "读取进度");
    snprintf(app->menu_labels[4], sizeof(app->menu_labels[4]), "历史记录");
    snprintf(app->menu_labels[5], sizeof(app->menu_labels[5]), "下一章");
    snprintf(app->menu_labels[6], sizeof(app->menu_labels[6]), "设置");
    snprintf(app->menu_labels[7], sizeof(app->menu_labels[7]), "返回标题");
    for (int i = 0; i < 8; ++i) {
        app->menu_rows[i].text = app->menu_labels[i];
        app->menu_rows[i].meta = NULL;
        app->menu_rows[i].preview = NULL;
        app->menu_rows[i].disabled = false;
    }
    if (app->menu_sel < 0 || app->menu_sel >= 8) app->menu_sel = 0;
    summer_ui_show_page(&app->ui, SUMMER_PAGE_MENU);
    summer_ui_set_rows(&app->ui, SUMMER_LIST_COMPACT, "阅读菜单", NULL,
                       "长按 OK 返回", app->menu_rows, 8, app->menu_sel);
    app_status(app);
}

static void app_open_menu(summer_app_t *app, summer_app_page_t source)
{
    app->menu_return = source;
    app->auto_mode = false;
    app->ff = false;
    app_reveal_text(app);
    app->page = APP_PAGE_MENU;
    app_build_menu(app);
}

static void app_redraw_page(summer_app_t *app, summer_app_page_t page);

static void app_close_menu(summer_app_t *app)
{
    app_redraw_page(app, app->menu_return);
}

static void app_build_settings(summer_app_t *app)
{
    snprintf(app->settings_labels[0], sizeof(app->settings_labels[0]), "文字速度");
    snprintf(app->settings_values[0], sizeof(app->settings_values[0]), "%s",
             app->store.text_speed == 0 ? "即时" : "逐字");
    snprintf(app->settings_labels[1], sizeof(app->settings_labels[1]), "自动速度");
    snprintf(app->settings_values[1], sizeof(app->settings_values[1]), "%s",
             app->store.auto_speed == 0 ? "慢速" : app->store.auto_speed == 1 ? "标准" : "快速");
    snprintf(app->settings_labels[2], sizeof(app->settings_labels[2]), "路线 / 章节");
    snprintf(app->settings_values[2], sizeof(app->settings_values[2]), "按章节入口");
    snprintf(app->settings_labels[3], sizeof(app->settings_labels[3]), "返回");
    app->settings_values[3][0] = 0;
    for (int i = 0; i < 4; ++i) {
        app->settings_rows[i].text = app->settings_labels[i];
        app->settings_rows[i].meta = app->settings_values[i][0] ? app->settings_values[i] : NULL;
        app->settings_rows[i].preview = NULL;
        app->settings_rows[i].disabled = false;
    }
    if (app->settings_sel < 0 || app->settings_sel >= 4) app->settings_sel = 0;
    summer_ui_show_page(&app->ui, SUMMER_PAGE_LIST);
    summer_ui_set_rows(&app->ui, SUMMER_LIST_COMPACT, "设置", NULL, "长按 OK 返回",
                       app->settings_rows, 4, app->settings_sel);
    app_status(app);
}

static void app_open_settings(summer_app_t *app, summer_app_page_t source)
{
    app->settings_return = source;
    app->page = APP_PAGE_SETTINGS;
    app_build_settings(app);
}

static void app_build_slot_window(summer_app_t *app)
{
    const int visible = 3;
    int first = app->slot_sel - 1;
    if (first < 0) first = 0;
    if (first > SUMMER_SLOT_COUNT - visible) first = SUMMER_SLOT_COUNT - visible;
    for (int i = 0; i < visible; ++i) {
        const int index = first + i;
        summer_slot_meta_t *meta = &app->slot_meta[index];
        snprintf(app->slot_labels[i], sizeof(app->slot_labels[i]), "存档 %02d", index + 1);
        if (!meta->occupied) {
            snprintf(app->slot_meta_text[i], sizeof(app->slot_meta_text[i]), "空栏位");
            app->slot_preview[i][0] = 0;
        } else if (!meta->valid) {
            snprintf(app->slot_meta_text[i], sizeof(app->slot_meta_text[i]), "存档无法读取");
            app->slot_preview[i][0] = 0;
        } else {
            snprintf(app->slot_meta_text[i], sizeof(app->slot_meta_text[i]), "%s", meta->meta);
            snprintf(app->slot_preview[i], sizeof(app->slot_preview[i]), "%s", meta->preview);
        }
        app->slot_rows[i].text = app->slot_labels[i];
        app->slot_rows[i].meta = app->slot_meta_text[i];
        app->slot_rows[i].preview = app->slot_preview[i][0] ? app->slot_preview[i] : NULL;
        app->slot_rows[i].disabled = app->slot_role == SUMMER_SLOT_LOAD &&
                                     (!meta->occupied || !meta->valid);
    }
    summer_ui_show_page(&app->ui, SUMMER_PAGE_LIST);
    summer_ui_set_window(&app->ui, SUMMER_LIST_CARD,
                         app->slot_role == SUMMER_SLOT_SAVE ? "保存进度" : "读取进度",
                         "30 个栏位", "OK 选择 · 长按 OK 返回", app->slot_rows, visible,
                         first, SUMMER_SLOT_COUNT, app->slot_sel);
    app_status(app);
}

static void app_reload_slots(summer_app_t *app)
{
    memset(app->slot_meta, 0, sizeof(app->slot_meta));
    for (int i = 0; i < SUMMER_SLOT_COUNT; ++i) {
        summer_slot_meta_t *meta = &app->slot_meta[i];
        size_t len = 0;
        if (!summer_store_read_slot(&app->store, i, s_save_buf, sizeof(s_save_buf), &len)) {
            // The store's existence cache is written by a successful save/slot
            // init.  If the blob is known to exist but the read failed, keep the
            // slot visible as an unreadable occupied slot instead of claiming it
            // is empty.
            if (summer_store_slot_exists(&app->store, i)) {
                meta->occupied = true;
                meta->valid = false;
            } else {
                meta->occupied = false;
            }
            continue;
        }
        meta->occupied = true;
        char err[64];
        if (!summer_save_decode(&app->pack, s_save_buf, len, &s_save, err, sizeof(err)) ||
            !summer_save_to_vm(&s_save, &s_temp_vm, &app->pack)) {
            meta->valid = false;
            continue;
        }
        meta->valid = true;
        snprintf(meta->meta, sizeof(meta->meta), "%s",
                 summer_vm_scene_label(&s_temp_vm, &app->pack));
        (void)summer_save_preview(&app->pack, &s_save, meta->preview, sizeof(meta->preview));
    }
}

static void app_open_slots(summer_app_t *app, summer_slot_role_t role, summer_app_page_t source)
{
    app->slot_role = role;
    app->slots_return = source;
    app_reload_slots(app);
    if (app->slot_sel < 0 || app->slot_sel >= SUMMER_SLOT_COUNT) app->slot_sel = 0;
    app->page = APP_PAGE_SLOTS;
    app_build_slot_window(app);
}

static bool app_autosave(summer_app_t *app, bool force)
{
    (void)force;
    if (!app->vm_valid || app->ended) return false;
    if (app->page != APP_PAGE_GAME && app->page != APP_PAGE_CHOICE &&
        app->page != APP_PAGE_MAP) {
        return false;
    }
    const size_t len = summer_save_encode(&app->vm, (uint8_t)app->page_index,
                                          &app->store.progress, s_save_buf,
                                          sizeof(s_save_buf));
    if (len == 0) return false;
    const bool ok = summer_store_write_auto(&app->store, s_save_buf, len);
    if (ok) app->autosave_dirty = false;
    return ok;
}

static bool app_load_from_blob(summer_app_t *app, const uint8_t *data, size_t len,
                               bool auto_save)
{
    char err[64];
    if (!summer_save_decode(&app->pack, data, len, &s_save, err, sizeof(err))) {
        app_notice(app, auto_save ? "自动存档无法读取" : "这个存档无法读取");
        return false;
    }
    if (!summer_image_probe_scene(&app->pack, s_save.bg, s_save.body)) {
        app_notice(app, "存档图片损坏，未改变当前进度");
        return false;
    }
    if (!summer_save_to_vm(&s_save, &s_temp_vm, &app->pack)) {
        app_notice(app, "存档位置无效");
        return false;
    }
    (void)summer_progress_merge(&app->store.progress, s_save.cleared, s_save.seen_cg,
                                s_save.seen_cg_count);
    summer_progress_apply(&s_temp_vm, &app->pack, &app->store.progress);
    s_temp_vm.choice_sel = s_save.choice_sel;
    app->vm = s_temp_vm;
    app->vm_valid = true;
    app->shown_bg = 0xFFFF;
    app->shown_body = 0xFFFF;
    app->image_error = false;
    app->typing = false;
    app->auto_mode = false;
    app->ff = false;
    app->at_choice = false;
    app->at_map = false;
    app->ended = false;
    app->page = APP_PAGE_GAME;
    app_render_frame(app);
    if (app->page == APP_PAGE_GAME && !app->at_choice && !app->at_map &&
        s_save.page > 0 && s_save.page < app->page_count) {
        app_show_page_text(app, s_save.page, true);
    }
    app_notice(app, auto_save ? "已继续上次进度" : "已读取存档");
    return true;
}

static void app_load_auto(summer_app_t *app)
{
    size_t len = 0;
    if (!summer_store_read_auto(&app->store, s_save_buf, sizeof(s_save_buf), &len)) {
        app_notice(app, "没有可继续的自动存档");
        return;
    }
    app_load_from_blob(app, s_save_buf, len, true);
}

static void app_save_slot(summer_app_t *app, int index)
{
    const size_t len = summer_save_encode(&app->vm, (uint8_t)app->page_index,
                                          &app->store.progress, s_save_buf,
                                          sizeof(s_save_buf));
    if (len == 0) {
        app_notice(app, "存档失败：数据过大");
        return;
    }
    if (!summer_store_write_slot(&app->store, index, s_save_buf, len)) {
        app_notice(app, "保存失败，请重试");
        return;
    }
    summer_slot_meta_t *meta = &app->slot_meta[index];
    meta->occupied = true;
    meta->valid = true;
    snprintf(meta->meta, sizeof(meta->meta), "%s",
             summer_vm_scene_label(&app->vm, &app->pack));
    (void)summer_save_preview(&app->pack, &s_save, meta->preview, sizeof(meta->preview));
    char msg[48];
    snprintf(msg, sizeof(msg), "已保存到存档 %02d", index + 1);
    app_redraw_page(app, app->slots_return);
    app_notice(app, msg);
}

static void app_load_slot(summer_app_t *app, int index)
{
    size_t len = 0;
    if (!summer_store_read_slot(&app->store, index, s_save_buf, sizeof(s_save_buf), &len)) {
        // An occupied slot whose blob cannot be read is an I/O failure, not an
        // empty slot; keep the current VM and say so.
        if (app->slot_meta[index].occupied) {
            app_notice(app, "存档读取失败，请重试");
        } else {
            app_notice(app, "这个栏位还没有存档");
        }
        return;
    }
    if (!app_load_from_blob(app, s_save_buf, len, false)) return;
}

static void app_open_confirm(summer_app_t *app, summer_confirm_kind_t kind,
                             summer_app_page_t source)
{
    app->confirm_kind = kind;
    app->confirm_return = source;
    app->confirm_sel = 0;
    app->page = APP_PAGE_CONFIRM;
    summer_ui_show_page(&app->ui, SUMMER_PAGE_CHOICE);
    summer_ui_row_t rows[2];
    char *labels[2] = { "取消", kind == SUMMER_CONFIRM_OVERWRITE ? "确认覆盖" : "返回标题" };
    for (int i = 0; i < 2; ++i) {
        rows[i].text = labels[i];
        rows[i].meta = NULL;
        rows[i].preview = NULL;
        rows[i].disabled = false;
    }
    summer_ui_set_rows(&app->ui, SUMMER_LIST_COMPACT,
                       kind == SUMMER_CONFIRM_OVERWRITE ? "覆盖存档？" : "返回标题？",
                       kind == SUMMER_CONFIRM_OVERWRITE ? "覆盖后将保存当前进度。" : NULL,
                       "OK 确认 · 长按 OK 取消", rows, 2, app->confirm_sel);
    app_status(app);
}

static void app_build_gallery_window(summer_app_t *app)
{
    const int visible = 7;
    int first = app->gallery_sel - 3;
    if (first < 0) first = 0;
    if (first > (int)app->pack.gallery_count - visible) first = (int)app->pack.gallery_count - visible;
    if (first < 0) first = 0;
    for (int i = 0; i < visible; ++i) {
        const int index = first + i;
        const summer_gallery_t *group = summer_gallery_group(&app->pack, (uint32_t)index);
        char label[40] = "";
        if (group) {
            (void)summer_pack_text_into(&app->pack, group->label, label, sizeof(label));
            snprintf(app->gallery_meta[i], sizeof(app->gallery_meta[i]), "%u 张",
                     (unsigned)group->count);
        }
        snprintf(app->gallery_labels[i], sizeof(app->gallery_labels[i]), "%s", label);
        app->gallery_rows[i].text = app->gallery_labels[i];
        app->gallery_rows[i].meta = app->gallery_meta[i];
        app->gallery_rows[i].preview = NULL;
        app->gallery_rows[i].disabled = false;
    }
    summer_ui_show_page(&app->ui, SUMMER_PAGE_LIST);
    summer_ui_set_window(&app->ui, SUMMER_LIST_COMPACT, "CG 鉴赏",
                         "按场景分组 · 全部开放", "OK 打开 · 长按 OK 返回",
                         app->gallery_rows, visible, first, (int)app->pack.gallery_count,
                         app->gallery_sel);
    app_status(app);
}

static void app_open_gallery(summer_app_t *app, summer_app_page_t source)
{
    app->gallery_return = source;
    app->page = APP_PAGE_GALLERY;
    if (app->gallery_sel < 0 || app->gallery_sel >= (int)app->pack.gallery_count) {
        app->gallery_sel = 0;
    }
    app_build_gallery_window(app);
}

static void app_render_cg(summer_app_t *app)
{
    const summer_gallery_t *group = summer_gallery_group(&app->pack, (uint32_t)app->cg_group);
    if (!group || group->count == 0) {
        app_notice(app, "CG 组为空");
        return;
    }
    if (app->cg_variant < 0 || app->cg_variant >= group->count) app->cg_variant = 0;
    const uint16_t image_index = summer_gallery_image(&app->pack, (uint32_t)app->cg_group,
                                                      app->cg_variant);
    const bool ok = summer_image_cg_index(&app->ui.canvas, &app->pack, image_index);
    if (ok) {
        if (summer_progress_set_cg(&app->store.progress, app->cg_group)) {
            app->store.progress.dirty = true;
        }
    } else {
        app_notice(app, "CG 图片读取失败，按 OK 返回列表");
    }
    char label[40] = "";
    (void)summer_pack_text_into(&app->pack, group->label, label, sizeof(label));
    char caption[96];
    snprintf(caption, sizeof(caption), "%s · 差分 %d/%u", label, app->cg_variant + 1,
             (unsigned)group->count);
    summer_ui_set_cg(&app->ui, caption, "UP/DOWN 切换差分 · OK 返回列表");
    app_status(app);
}

static void app_open_cg(summer_app_t *app, int group)
{
    app->cg_group = group;
    app->cg_variant = 0;
    app->page = APP_PAGE_CG;
    summer_ui_show_page(&app->ui, SUMMER_PAGE_CG);
    app_render_cg(app);
}

static void app_build_history_window(summer_app_t *app)
{
    const int visible = 3;
    // Keep the real history length; set_window hides rows past total.
    int total = app->history_count;
    int first = app->history_sel - 1;
    if (first < 0) first = 0;
    if (first > total - visible) first = total - visible;
    for (int i = 0; i < visible; ++i) {
        const int index = first + i;
        if (index >= app->history_count) {
            app->history_rows_text[i][0] = 0;
            app->history_rows_meta[i][0] = 0;
        } else {
            const int slot = history_slot(app, index);
            char text[96];
            snprintf(text, sizeof(text), "%.*s", (int)(sizeof(text) - 1),
                     slot >= 0 ? app->history_text[slot] : "");
            for (char *p = text; *p; ++p) if (*p == '\n') *p = ' ';
            size_t n = strlen(text);
            if (n > 80) {
                n = 80;
                while (n > 0 && ((uint8_t)text[n] & 0xC0u) == 0x80u) n--;
                text[n] = 0;
            }
            snprintf(app->history_rows_text[i], sizeof(app->history_rows_text[i]), "%s", text);
            snprintf(app->history_rows_meta[i], sizeof(app->history_rows_meta[i]), "%s",
                     slot >= 0 ? app->history_speaker[slot] : "");
        }
        app->history_rows[i].text = app->history_rows_text[i];
        app->history_rows[i].meta = app->history_rows_meta[i][0] ? app->history_rows_meta[i] : NULL;
        app->history_rows[i].preview = NULL;
        app->history_rows[i].disabled = false;
    }
    summer_ui_show_page(&app->ui, SUMMER_PAGE_LIST);
    summer_ui_set_window(&app->ui, SUMMER_LIST_HISTORY, "历史记录", "最近 12 句",
                         "长按 OK 返回", app->history_rows, visible, first, total,
                         app->history_sel);
    app_status(app);
}

static void app_open_history(summer_app_t *app, summer_app_page_t source)
{
    app->history_return = source;
    if (source == APP_PAGE_GAME) app_reveal_text(app);
    app->page = APP_PAGE_HISTORY;
    app->history_sel = app->history_count > 0 ? app->history_count - 1 : 0;
    app_build_history_window(app);
}

static void app_build_routes_window(summer_app_t *app)
{
    const unsigned actions = summer_progress_title_actions(&app->store.progress);
    const bool pocket = summer_progress_has_pocket(&app->store.progress);
    app->route_count = 0;
    for (int i = 0; i < (int)app->pack.route_count && app->route_count < 14; ++i) {
        bool show = true;
        if (i == 9 || i == 10 || i == 11) {
            show = pocket ||
                   (i == 9 && (actions & SUMMER_TITLE_ALKA)) ||
                   (i == 10 && (actions & SUMMER_TITLE_POCKET));
        }
        if (show) app->route_ids[app->route_count++] = i;
    }
    if (app->route_count == 0) {
        app->route_count = (int)app->pack.route_count;
        for (int i = 0; i < app->route_count && i < 14; ++i) app->route_ids[i] = i;
    }
    if (app->route_sel < 0 || app->route_sel >= app->route_count) app->route_sel = 0;
    const int visible = 7;
    int first = app->route_sel - 3;
    if (first < 0) first = 0;
    if (first > app->route_count - visible) first = app->route_count - visible;
    if (first < 0) first = 0;
    for (int i = 0; i < visible; ++i) {
        const int slot = first + i;
        if (slot >= app->route_count) {
            app->route_labels[i][0] = 0;
            app->route_meta[i][0] = 0;
        } else {
            const int route = app->route_ids[slot];
            (void)summer_pack_text_into(&app->pack, app->pack.routes[route].name,
                                        app->route_labels[i], sizeof(app->route_labels[i]));
            snprintf(app->route_meta[i], sizeof(app->route_meta[i]), "%u 个章节",
                     (unsigned)app->pack.routes[route].chapter_count);
        }
        app->route_rows[i].text = app->route_labels[i];
        app->route_rows[i].meta = app->route_meta[i];
        app->route_rows[i].preview = NULL;
        app->route_rows[i].disabled = false;
    }
    summer_ui_show_page(&app->ui, SUMMER_PAGE_LIST);
    summer_ui_set_window(&app->ui, SUMMER_LIST_COMPACT, "选择路线", "按章节入口阅读",
                         "长按 OK 返回", app->route_rows, visible, first, app->route_count,
                         app->route_sel);
    app_status(app);
}

static void app_open_routes(summer_app_t *app, summer_app_page_t source)
{
    app->routes_return = source;
    app->page = APP_PAGE_ROUTES;
    app_build_routes_window(app);
}

static void app_build_chapters_window(summer_app_t *app)
{
    const int visible = 7;
    int first = app->chapter_sel - 3;
    if (first < 0) first = 0;
    if (first > app->chapter_count - visible) first = app->chapter_count - visible;
    if (first < 0) first = 0;
    for (int i = 0; i < visible; ++i) {
        const int slot = first + i;
        if (slot >= app->chapter_count) {
            app->chapter_labels[i][0] = 0;
            app->chapter_meta[i][0] = 0;
        } else {
            const summer_scene_t *scene = summer_scene_by_id(&app->pack,
                                                            (uint16_t)app->chapter_scene[slot]);
            if (scene) {
                (void)summer_pack_text_into(&app->pack, scene->name, app->chapter_labels[i],
                                            sizeof(app->chapter_labels[i]));
            } else {
                app->chapter_labels[i][0] = 0;
            }
            snprintf(app->chapter_meta[i], sizeof(app->chapter_meta[i]), "开始阅读");
        }
        app->chapter_rows[i].text = app->chapter_labels[i];
        app->chapter_rows[i].meta = app->chapter_meta[i];
        app->chapter_rows[i].preview = NULL;
        app->chapter_rows[i].disabled = false;
    }
    summer_ui_show_page(&app->ui, SUMMER_PAGE_LIST);
    summer_ui_set_window(&app->ui, SUMMER_LIST_COMPACT, "选择章节", "阅读不伪造结局记录",
                         "长按 OK 返回", app->chapter_rows, visible, first, app->chapter_count,
                         app->chapter_sel);
    app_status(app);
}

static void app_open_chapters(summer_app_t *app, int route)
{
    app->chapters_return = APP_PAGE_ROUTES;
    app->chapter_count = 0;
    for (int i = 0; i < (int)app->pack.chapter_count && app->chapter_count < SUMMER_CHAPTER_MAX; ++i) {
        const summer_chapter_t *chapter = &app->pack.chapters[i];
        if ((int)(int8_t)chapter->route == route) {
            app->chapter_scene[app->chapter_count++] = chapter->scene;
        }
    }
    app->chapter_sel = 0;
    app->page = APP_PAGE_CHAPTERS;
    app_build_chapters_window(app);
}

static void app_redraw_page(summer_app_t *app, summer_app_page_t page)
{
    switch (page) {
    case APP_PAGE_TITLE:
        app_open_title(app);
        break;
    case APP_PAGE_GAME:
        app->page = APP_PAGE_GAME;
        summer_ui_show_page(&app->ui, SUMMER_PAGE_GAME);
        app_show_page_text(app, app->page_index, false);
        app_status(app);
        break;
    case APP_PAGE_CHOICE:
        app->page = APP_PAGE_CHOICE;
        summer_ui_show_page(&app->ui, SUMMER_PAGE_CHOICE);
        summer_ui_set_choice_rows(&app->ui, app->choice_rows, app->choice_count, app->choice_sel);
        app_status(app);
        break;
    case APP_PAGE_MAP:
        app->page = APP_PAGE_MAP;
        summer_ui_show_page(&app->ui, SUMMER_PAGE_CHOICE);
        summer_ui_set_rows(&app->ui, SUMMER_LIST_CARD, "选择地点", "UP/DOWN 选择目的地",
                           "长按 OK 打开菜单", app->map_rows, app->map_count, app->map_sel);
        app_status(app);
        break;
    case APP_PAGE_MENU:
        app->page = APP_PAGE_MENU;
        app_build_menu(app);
        break;
    case APP_PAGE_SLOTS:
        app->page = APP_PAGE_SLOTS;
        app_build_slot_window(app);
        break;
    case APP_PAGE_SETTINGS:
        app->page = APP_PAGE_SETTINGS;
        app_build_settings(app);
        break;
    case APP_PAGE_HISTORY:
        app->page = APP_PAGE_HISTORY;
        app_build_history_window(app);
        break;
    case APP_PAGE_GALLERY:
        app->page = APP_PAGE_GALLERY;
        app_build_gallery_window(app);
        break;
    case APP_PAGE_CG:
        app->page = APP_PAGE_CG;
        summer_ui_show_page(&app->ui, SUMMER_PAGE_CG);
        break;
    case APP_PAGE_ROUTES:
        app->page = APP_PAGE_ROUTES;
        app_build_routes_window(app);
        break;
    case APP_PAGE_CHAPTERS:
        app->page = APP_PAGE_CHAPTERS;
        app_build_chapters_window(app);
        break;
    case APP_PAGE_MANUAL:
        app->page = APP_PAGE_MANUAL;
        summer_ui_show_page(&app->ui, SUMMER_PAGE_LIST);
        summer_ui_set_help(&app->ui, MANUAL_TEXT);
        break;
    case APP_PAGE_ENDING:
        app->page = APP_PAGE_ENDING;
        summer_ui_show_page(&app->ui, SUMMER_PAGE_ENDING);
        summer_ui_set_ending(&app->ui, "本篇阅读完毕", app->ending_body);
        break;
    case APP_PAGE_ERROR:
        app->page = APP_PAGE_ERROR;
        summer_ui_show_page(&app->ui, SUMMER_PAGE_ERROR);
        break;
    case APP_PAGE_CONFIRM:
        break;
    }
}

static void app_open_manual(summer_app_t *app, summer_app_page_t source)
{
    app->manual_return = source;
    app->page = APP_PAGE_MANUAL;
    summer_ui_show_page(&app->ui, SUMMER_PAGE_LIST);
    summer_ui_set_help(&app->ui, MANUAL_TEXT);
    app_status(app);
}

static void app_next_chapter(summer_app_t *app)
{
    if (!app->vm_valid || app->at_choice || app->at_map) {
        app_notice(app, "请先作出当前选择");
        return;
    }
    const uint16_t root = app->vm.root_scene;
    for (int i = 0; i < 20000; ++i) {
        if (!summer_vm_advance(&app->vm, &app->pack)) {
            app_notice(app, "本篇已结束");
            return;
        }
        const summer_step_t step = summer_vm_step(&app->vm, &app->pack, 256);
        if (step.kind == SUMMER_STEP_TEXT) {
            if (app->vm.root_scene != root) {
                app_enter_text(app, &step);
                return;
            }
            continue;
        }
        if (step.kind == SUMMER_STEP_CHOICE) {
            app_enter_choice(app, step.group);
            return;
        }
        if (step.kind == SUMMER_STEP_MAP) {
            app_enter_map(app, step.group);
            return;
        }
        if (step.kind == SUMMER_STEP_END) {
            app_enter_ending(app);
            return;
        }
        if (step.kind == SUMMER_STEP_ERROR) {
            app_enter_error(app, "剧情数据错误");
            return;
        }
    }
    app_notice(app, "没有找到下一章");
}

static void app_menu_action(summer_app_t *app, int index)
{
    switch (index) {
    case 0:
        app_close_menu(app);
        break;
    case 1:
        app->auto_mode = !app->auto_mode;
        app->auto_accum = 0;
        app_build_menu(app);
        break;
    case 2:
        app_open_slots(app, SUMMER_SLOT_SAVE, APP_PAGE_MENU);
        break;
    case 3:
        app_open_slots(app, SUMMER_SLOT_LOAD, APP_PAGE_MENU);
        break;
    case 4:
        app_open_history(app, APP_PAGE_MENU);
        break;
    case 5:
        app_next_chapter(app);
        break;
    case 6:
        app_open_settings(app, APP_PAGE_MENU);
        break;
    case 7:
        app_open_confirm(app, SUMMER_CONFIRM_TITLE, APP_PAGE_MENU);
        break;
    default:
        break;
    }
}

static void app_settings_action(summer_app_t *app, int index)
{
    switch (index) {
    case 0:
        app->store.text_speed = app->store.text_speed == 0 ? 30 : 0;
        if (!summer_store_save_settings(&app->store)) {
            app_notice(app, "设置保存失败，仅本次有效");
        }
        app_build_settings(app);
        break;
    case 1:
        app->store.auto_speed = (uint8_t)((app->store.auto_speed + 1) % 3);
        if (!summer_store_save_settings(&app->store)) {
            app_notice(app, "设置保存失败，仅本次有效");
        }
        app_build_settings(app);
        break;
    case 2:
        app_open_routes(app, APP_PAGE_SETTINGS);
        break;
    default:
        app_redraw_page(app, app->settings_return);
        break;
    }
}

static void app_slot_ok(summer_app_t *app)
{
    if (app->slot_sel < 0 || app->slot_sel >= SUMMER_SLOT_COUNT) return;
    if (app->slot_role == SUMMER_SLOT_LOAD) {
        if (!app->slot_meta[app->slot_sel].occupied) {
            app_notice(app, "这个栏位还没有存档");
            return;
        }
        if (!app->slot_meta[app->slot_sel].valid) {
            app_notice(app, "这个存档无法读取");
            return;
        }
        app_load_slot(app, app->slot_sel);
        return;
    }
    if (app->slot_meta[app->slot_sel].occupied) {
        app->slot_pending = app->slot_sel;
        app_open_confirm(app, SUMMER_CONFIRM_OVERWRITE, APP_PAGE_SLOTS);
        return;
    }
    app_save_slot(app, app->slot_sel);
}

static void app_title_action(summer_app_t *app, int index)
{
    if (index < 0 || index >= SUMMER_UI_ROWS) return;
    const int action = app->title_action_ids[index];
    if (action == SUMMER_ACT_CONTINUE && !app->title_has_auto) {
        app_notice(app, "没有可继续的自动存档");
        return;
    }
    switch (action) {
    case SUMMER_ACT_START:
    case SUMMER_ACT_RESTART:
    case SUMMER_ACT_ALKA:
    case SUMMER_ACT_POCKET:
        app_start_story(app, action);
        break;
    case SUMMER_ACT_CONTINUE:
        app_load_auto(app);
        break;
    case SUMMER_ACT_LOAD:
        app_open_slots(app, SUMMER_SLOT_LOAD, APP_PAGE_TITLE);
        break;
    case SUMMER_ACT_GALLERY:
        app_open_gallery(app, APP_PAGE_TITLE);
        break;
    case SUMMER_ACT_OPTIONS:
        app_open_settings(app, APP_PAGE_TITLE);
        break;
    case SUMMER_ACT_MANUAL:
        app_open_manual(app, APP_PAGE_TITLE);
        break;
    default:
        break;
    }
}

static void app_key_game(summer_app_t *app, const summer_key_t *key);

bool summer_app_init(summer_app_t *app, const uint8_t *pack_blob, uint32_t pack_size)
{
    if (!app) return false;
    memset(app, 0, sizeof(*app));
    app->battery = -1;
    if (!bsp_lvgl_lock(-1)) return false;
    app->pack_ok = summer_pack_open(&app->pack, pack_blob, pack_size, NULL);
    (void)summer_store_init(&app->store);
    if (!summer_ui_create(&app->ui, app->pixels, &gal_cjk_16)) {
        bsp_lvgl_unlock();
        return false;
    }
    summer_vm_init(&app->vm);
    app->battery = bsp_battery_soc();
    app->init_ok = true;
    if (!app->pack_ok) {
        app_enter_error(app, "资源包校验失败，请重新构建固件");
    } else {
        app_open_title(app);
    }
    bsp_lvgl_unlock();
    return app->init_ok;
}

void summer_app_show_sleeping(summer_app_t *app)
{
    if (!app || !app->init_ok) return;
    if (!bsp_lvgl_lock(1000)) return;
    summer_ui_show_page(&app->ui, SUMMER_PAGE_ERROR);
    summer_ui_set_error(&app->ui, "设备即将休眠\n按任意键唤醒");
    bsp_lvgl_unlock();
}

void summer_app_flush(summer_app_t *app)
{
    if (!app || !app->init_ok) return;
    if (!bsp_lvgl_lock(2000)) return;
    (void)app_autosave(app, true);
    (void)summer_store_flush_progress(&app->store);
    bsp_lvgl_unlock();
}

uint32_t summer_app_idle_ms(const summer_app_t *app)
{
    if (app && (app->auto_mode || app->ff)) return 0;   // 自动/快进不休眠
    return app ? app->idle_ms : 0;
}

static bool app_key_press_suppressed(summer_app_t *app, uint8_t btn)
{
    const uint8_t mask = (uint8_t)(1u << btn);
    if (app->suppress_click & mask) {
        app->suppress_click &= (uint8_t)~mask;
        return true;
    }
    return false;
}

static void app_key_title(summer_app_t *app, const summer_key_t *key)
{
    if (key->ev != BSP_BTN_CLICK) return;
    if (key->btn == BSP_BTN_UP) {
        if (app->title_sel > 0) app->title_sel--;
        app_build_title(app);
    } else if (key->btn == BSP_BTN_DOWN) {
        if (app->title_sel + 1 < app->title_count) app->title_sel++;
        app_build_title(app);
    } else if (key->btn == BSP_BTN_OK) {
        app_title_action(app, app->title_sel);
    }
}

static void app_key_choice(summer_app_t *app, const summer_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        app_open_menu(app, app->page);
        return;
    }
    if (key->ev != BSP_BTN_CLICK) return;
    if (key->btn == BSP_BTN_UP && app->choice_count > 0) {
        app->choice_sel = (app->choice_sel + app->choice_count - 1) % app->choice_count;
        app->vm.choice_sel = app->choice_sel;
        summer_ui_set_choice_rows(&app->ui, app->choice_rows, app->choice_count, app->choice_sel);
    } else if (key->btn == BSP_BTN_DOWN && app->choice_count > 0) {
        app->choice_sel = (app->choice_sel + 1) % app->choice_count;
        app->vm.choice_sel = app->choice_sel;
        summer_ui_set_choice_rows(&app->ui, app->choice_rows, app->choice_count, app->choice_sel);
    } else if (key->btn == BSP_BTN_OK && app->choice_count > 0) {
        const int item = app->choice_items[app->choice_sel];
        if (!summer_vm_choose(&app->vm, &app->pack, (uint32_t)app->choice_group, item)) {
            app_enter_error(app, "选项写入失败");
            return;
        }
        app->at_choice = false;
        app->autosave_dirty = true;
        app_render_frame(app);
    }
}

static void app_key_map(summer_app_t *app, const summer_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        app_open_menu(app, app->page);
        return;
    }
    if (key->ev != BSP_BTN_CLICK) return;
    if (key->btn == BSP_BTN_UP && app->map_count > 0) {
        app->map_sel = (app->map_sel + app->map_count - 1) % app->map_count;
        app->vm.choice_sel = app->map_sel;
        summer_ui_set_rows(&app->ui, SUMMER_LIST_CARD, "选择地点", "UP/DOWN 选择目的地",
                           "长按 OK 打开菜单", app->map_rows, app->map_count, app->map_sel);
    } else if (key->btn == BSP_BTN_DOWN && app->map_count > 0) {
        app->map_sel = (app->map_sel + 1) % app->map_count;
        app->vm.choice_sel = app->map_sel;
        summer_ui_set_rows(&app->ui, SUMMER_LIST_CARD, "选择地点", "UP/DOWN 选择目的地",
                           "长按 OK 打开菜单", app->map_rows, app->map_count, app->map_sel);
    } else if (key->btn == BSP_BTN_OK && app->map_count > 0) {
        const int item = app->map_choices[app->map_sel].item_index;
        if (!summer_vm_choose_map(&app->vm, &app->pack, (uint32_t)app->map_group, item)) {
            app_enter_error(app, "地图写入失败");
            return;
        }
        app->at_map = false;
        app->autosave_dirty = true;
        app_render_frame(app);
    }
}

static void app_key_menu(summer_app_t *app, const summer_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        app_close_menu(app);
        return;
    }
    if (key->ev != BSP_BTN_CLICK) return;
    if (key->btn == BSP_BTN_UP) {
        if (app->menu_sel > 0) app->menu_sel--;
        app_build_menu(app);
    } else if (key->btn == BSP_BTN_DOWN) {
        if (app->menu_sel < 7) app->menu_sel++;
        app_build_menu(app);
    } else if (key->btn == BSP_BTN_OK) {
        app_menu_action(app, app->menu_sel);
    }
}

static void app_key_slots(summer_app_t *app, const summer_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        app_redraw_page(app, app->slots_return);
        return;
    }
    if (key->ev != BSP_BTN_CLICK) return;
    if (key->btn == BSP_BTN_UP) {
        if (app->slot_sel > 0) app->slot_sel--;
        app_build_slot_window(app);
    } else if (key->btn == BSP_BTN_DOWN) {
        if (app->slot_sel + 1 < SUMMER_SLOT_COUNT) app->slot_sel++;
        app_build_slot_window(app);
    } else if (key->btn == BSP_BTN_OK) {
        app_slot_ok(app);
    }
}

static void app_key_settings(summer_app_t *app, const summer_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        app_redraw_page(app, app->settings_return);
        return;
    }
    if (key->ev != BSP_BTN_CLICK) return;
    if (key->btn == BSP_BTN_UP) {
        if (app->settings_sel > 0) app->settings_sel--;
        app_build_settings(app);
    } else if (key->btn == BSP_BTN_DOWN) {
        if (app->settings_sel < 3) app->settings_sel++;
        app_build_settings(app);
    } else if (key->btn == BSP_BTN_OK) {
        app_settings_action(app, app->settings_sel);
    }
}

static void app_key_history(summer_app_t *app, const summer_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        app_redraw_page(app, app->history_return);
        return;
    }
    if (key->ev != BSP_BTN_CLICK) return;
    if (key->btn == BSP_BTN_UP) {
        if (app->history_sel > 0) app->history_sel--;
        app_build_history_window(app);
    } else if (key->btn == BSP_BTN_DOWN) {
        if (app->history_sel + 1 < app->history_count) app->history_sel++;
        app_build_history_window(app);
    } else if (key->btn == BSP_BTN_OK) {
        app_redraw_page(app, app->history_return);
    }
}

static void app_key_gallery(summer_app_t *app, const summer_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        app_redraw_page(app, app->gallery_return);
        return;
    }
    if (key->ev != BSP_BTN_CLICK) return;
    if (key->btn == BSP_BTN_UP) {
        if (app->gallery_sel > 0) app->gallery_sel--;
        app_build_gallery_window(app);
    } else if (key->btn == BSP_BTN_DOWN) {
        if (app->gallery_sel + 1 < (int)app->pack.gallery_count) app->gallery_sel++;
        app_build_gallery_window(app);
    } else if (key->btn == BSP_BTN_OK) {
        app_open_cg(app, app->gallery_sel);
    }
}

static void app_key_cg(summer_app_t *app, const summer_key_t *key)
{
    const summer_gallery_t *group = summer_gallery_group(&app->pack, (uint32_t)app->cg_group);
    if (!group) {
        app_redraw_page(app, APP_PAGE_GALLERY);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        if (group->count > 1) {
            if (key->btn == BSP_BTN_UP) app->cg_variant = (app->cg_variant + group->count - 1) % group->count;
            else app->cg_variant = (app->cg_variant + 1) % group->count;
            app_render_cg(app);
        }
        return;
    }
    if ((key->ev == BSP_BTN_CLICK || key->ev == BSP_BTN_LONG) && key->btn == BSP_BTN_OK) {
        app->gallery_sel = app->cg_group;
        app_redraw_page(app, APP_PAGE_GALLERY);
    }
}

static void app_key_routes(summer_app_t *app, const summer_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        app_redraw_page(app, app->routes_return);
        return;
    }
    if (key->ev != BSP_BTN_CLICK) return;
    if (key->btn == BSP_BTN_UP) {
        if (app->route_sel > 0) app->route_sel--;
        app_build_routes_window(app);
    } else if (key->btn == BSP_BTN_DOWN) {
        if (app->route_sel + 1 < app->route_count) app->route_sel++;
        app_build_routes_window(app);
    } else if (key->btn == BSP_BTN_OK) {
        app_open_chapters(app, app->route_ids[app->route_sel]);
    }
}

static void app_key_chapters(summer_app_t *app, const summer_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        app_redraw_page(app, APP_PAGE_ROUTES);
        return;
    }
    if (key->ev != BSP_BTN_CLICK) return;
    if (key->btn == BSP_BTN_UP) {
        if (app->chapter_sel > 0) app->chapter_sel--;
        app_build_chapters_window(app);
    } else if (key->btn == BSP_BTN_DOWN) {
        if (app->chapter_sel + 1 < app->chapter_count) app->chapter_sel++;
        app_build_chapters_window(app);
    } else if (key->btn == BSP_BTN_OK) {
        if (app->chapter_sel >= 0 && app->chapter_sel < app->chapter_count) {
            app_start_chapter(app, (uint16_t)app->chapter_scene[app->chapter_sel]);
        }
    }
}

static void app_key_manual(summer_app_t *app, const summer_key_t *key)
{
    if ((key->ev == BSP_BTN_CLICK || key->ev == BSP_BTN_LONG) && key->btn == BSP_BTN_OK) {
        app_redraw_page(app, app->manual_return);
        return;
    }
    if (key->ev != BSP_BTN_CLICK) return;
    if (key->btn == BSP_BTN_UP) summer_ui_scroll(&app->ui, -100);
    else if (key->btn == BSP_BTN_DOWN) summer_ui_scroll(&app->ui, 100);
}

static void app_key_confirm(summer_app_t *app, const summer_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        app_redraw_page(app, app->confirm_return);
        return;
    }
    if (key->ev != BSP_BTN_CLICK) return;
    if (key->btn == BSP_BTN_UP) {
        app->confirm_sel = 0;
        summer_ui_set_rows(&app->ui, SUMMER_LIST_COMPACT,
                           app->confirm_kind == SUMMER_CONFIRM_OVERWRITE ? "覆盖存档？" : "返回标题？",
                           app->confirm_kind == SUMMER_CONFIRM_OVERWRITE ? "覆盖后将保存当前进度。" : NULL,
                           "OK 确认 · 长按 OK 取消",
                           (const summer_ui_row_t[]){
                               { "取消", NULL, NULL, false },
                               { app->confirm_kind == SUMMER_CONFIRM_OVERWRITE ? "确认覆盖" : "返回标题",
                                 NULL, NULL, false },
                           }, 2, 0);
    } else if (key->btn == BSP_BTN_DOWN) {
        app->confirm_sel = 1;
        summer_ui_set_rows(&app->ui, SUMMER_LIST_COMPACT,
                           app->confirm_kind == SUMMER_CONFIRM_OVERWRITE ? "覆盖存档？" : "返回标题？",
                           app->confirm_kind == SUMMER_CONFIRM_OVERWRITE ? "覆盖后将保存当前进度。" : NULL,
                           "OK 确认 · 长按 OK 取消",
                           (const summer_ui_row_t[]){
                               { "取消", NULL, NULL, false },
                               { app->confirm_kind == SUMMER_CONFIRM_OVERWRITE ? "确认覆盖" : "返回标题",
                                 NULL, NULL, false },
                           }, 2, 1);
    } else if (key->btn == BSP_BTN_OK) {
        if (app->confirm_sel == 1) {
            if (app->confirm_kind == SUMMER_CONFIRM_OVERWRITE) {
                app_save_slot(app, app->slot_pending);
            } else {
                app_open_title(app);
            }
        } else {
            app_redraw_page(app, app->confirm_return);
        }
    }
}

static void app_key_game(summer_app_t *app, const summer_key_t *key)
{
    if (app->auto_mode && key->ev != BSP_BTN_RELEASE &&
        !(key->btn == BSP_BTN_DOWN && key->ev == BSP_BTN_LONG)) {
        app->auto_mode = false;
        app_update_auto(app);
    }
    if (key->ev == BSP_BTN_PRESS) {
        if (key->btn == BSP_BTN_UP) {
            app->suppress_click |= (uint8_t)(1u << BSP_BTN_UP);
            app_advance_game(app);
        }
        return;
    }
    if (key->ev == BSP_BTN_LONG) {
        if (key->btn == BSP_BTN_UP) {
            app->ff = true;
            app->ff_accum = 0;
            app_update_auto(app);
        } else if (key->btn == BSP_BTN_DOWN) {
            app->auto_mode = !app->auto_mode;
            app->auto_accum = 0;
            app_update_auto(app);
            app_notice(app, app->auto_mode ? "自动阅读：开" : "自动阅读：关");
        } else if (key->btn == BSP_BTN_OK) {
            app_open_menu(app, APP_PAGE_GAME);
        }
        return;
    }
    if (key->ev == BSP_BTN_RELEASE) {
        if (key->btn == BSP_BTN_UP) {
            app->ff = false;
            app_update_auto(app);
        }
        return;
    }
    if (key->ev != BSP_BTN_CLICK) return;
    if (key->btn == BSP_BTN_UP) {
        if (!app_key_press_suppressed(app, BSP_BTN_UP)) app_advance_game(app);
    } else if (key->btn == BSP_BTN_DOWN) {
        app_open_history(app, APP_PAGE_GAME);
    } else if (key->btn == BSP_BTN_OK) {
        // 短按 OK 与上键一致：推进下一句；进菜单改成长按 OK（见上面的 LONG 分支）。
        app_advance_game(app);
    }
}

void summer_app_key(summer_app_t *app, const summer_key_t *key)
{
    if (!app || !key || !app->init_ok) return;
    app->idle_ms = 0;
    if (!bsp_lvgl_lock(-1)) return;
    if (key->ev == BSP_BTN_PRESS) {
        app->suppress_click &= (uint8_t)~(1u << key->btn);
    }
    if (key->ev == BSP_BTN_CLICK && (app->suppress_click & (1u << key->btn))) {
        app->suppress_click &= (uint8_t)~(1u << key->btn);
        bsp_lvgl_unlock();
        return;
    }
    if (app->ff && !(key->btn == BSP_BTN_UP &&
                     (key->ev == BSP_BTN_PRESS || key->ev == BSP_BTN_LONG ||
                      key->ev == BSP_BTN_RELEASE))) {
        app->ff = false;
    }
    switch (app->page) {
    case APP_PAGE_TITLE: app_key_title(app, key); break;
    case APP_PAGE_GAME: app_key_game(app, key); break;
    case APP_PAGE_CHOICE: app_key_choice(app, key); break;
    case APP_PAGE_MAP: app_key_map(app, key); break;
    case APP_PAGE_MENU: app_key_menu(app, key); break;
    case APP_PAGE_SLOTS: app_key_slots(app, key); break;
    case APP_PAGE_SETTINGS: app_key_settings(app, key); break;
    case APP_PAGE_HISTORY: app_key_history(app, key); break;
    case APP_PAGE_GALLERY: app_key_gallery(app, key); break;
    case APP_PAGE_CG: app_key_cg(app, key); break;
    case APP_PAGE_ROUTES: app_key_routes(app, key); break;
    case APP_PAGE_CHAPTERS: app_key_chapters(app, key); break;
    case APP_PAGE_MANUAL: app_key_manual(app, key); break;
    case APP_PAGE_CONFIRM: app_key_confirm(app, key); break;
    case APP_PAGE_ENDING:
    case APP_PAGE_ERROR:
        if ((key->ev == BSP_BTN_CLICK || key->ev == BSP_BTN_LONG) && key->btn == BSP_BTN_OK) {
            app_open_title(app);
        }
        break;
    default:
        break;
    }
    // 长按 OK 一定换了页面/动作，随后驱动补发的 CLICK 必须吞掉（否则游戏页会再
    // 推进一句、菜单页会再触发一次选中行）。和 UP 的 suppress_click 同一套机制。
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        app->suppress_click |= (uint8_t)(1u << BSP_BTN_OK);
    }
    bsp_lvgl_unlock();
}

static uint32_t app_auto_wait(const summer_app_t *app)
{
    static const uint32_t base[3] = { 2600, 1500, 800 };
    const uint32_t speed = app->store.auto_speed <= 2 ? app->store.auto_speed : 1;
    const size_t len = strlen(app->page_text);
    uint32_t wait = base[speed];
    if (len > 60) wait += (uint32_t)(len - 60) * 55;
    if (wait > base[speed] + 5000) wait = base[speed] + 5000;
    return wait < 400 ? 400 : wait;
}

static void app_tick_typewriter(summer_app_t *app, uint32_t elapsed)
{
    if (!app->typing || app->page != APP_PAGE_GAME) return;
    const uint32_t speed = app->auto_mode ? 30 : app->store.text_speed;
    if (speed == 0 || app->ff) {
        app->typing = false;
        app->typing_pos = strlen(app->page_text);
        summer_ui_set_text(&app->ui, app->page_text);
        app->auto_accum = 0;
        return;
    }
    app->type_accum += elapsed;
    const size_t total = strlen(app->page_text);
    int guard = 0;
    while (app->type_accum >= speed && app->typing && guard++ < 64) {
        app->type_accum -= speed;
        if (app->typing_pos >= total) {
            app->typing = false;
            app->typing_pos = total;
            break;
        }
        app->typing_pos += summer_utf8_char_len(app->page_text, total, app->typing_pos);
        if (app->typing_pos >= total) {
            app->typing_pos = total;
            app->typing = false;
        }
    }
    const size_t n = app->typing_pos < sizeof(app->display_buf) - 1 ? app->typing_pos
                                                                    : sizeof(app->display_buf) - 1;
    memcpy(app->display_buf, app->page_text, n);
    app->display_buf[n] = 0;
    summer_ui_set_text(&app->ui, app->display_buf);
    if (!app->typing) app->auto_accum = 0;
}

void summer_app_tick(summer_app_t *app, uint32_t elapsed_ms)
{
    if (!app || !app->init_ok) return;
    if (!bsp_lvgl_lock(-1)) return;
    if (app->auto_mode || app->ff) app->idle_ms = 0;
    else app->idle_ms += elapsed_ms;
    app->notice_ms = app->notice_ms > elapsed_ms ? app->notice_ms - elapsed_ms : 0;
    if (app->notice_ms == 0 && app->notice[0]) {
        app->notice[0] = 0;
        summer_ui_hide_notice(&app->ui);
    }
    app->battery_ms += elapsed_ms;
    if (app->battery_ms >= 30000) {
        app->battery_ms = 0;
        app->battery = bsp_battery_soc();
        summer_ui_set_battery(&app->ui, app->battery);
    }
    app_tick_typewriter(app, elapsed_ms);
    if (app->page == APP_PAGE_GAME) {
        if (app->ff) {
            app->ff_accum += elapsed_ms;
            if (app->ff_accum >= 60) {
                app->ff_accum = 0;
                app->idle_ms = 0;
                app_advance_game(app);
            }
        } else if (app->auto_mode && !app->typing) {
            app->auto_accum += elapsed_ms;
            if (app->auto_accum >= app_auto_wait(app)) {
                app->auto_accum = 0;
                app->idle_ms = 0;
                app_advance_game(app);
            }
        }
    }
    app->save_accum += elapsed_ms;
    if (app->save_accum >= 8000) {
        app->save_accum = 0;
        if (app->autosave_dirty) (void)app_autosave(app, false);
    }
    app->progress_accum += elapsed_ms;
    if (app->progress_accum >= 8000) {
        app->progress_accum = 0;
        if (app->store.progress.dirty) (void)summer_store_flush_progress(&app->store);
    }
    bsp_lvgl_unlock();
}
