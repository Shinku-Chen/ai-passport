// main/anemoi/anemoi_app.c —— 应用状态机实现（纯逻辑，可在宿主机测试）。
#include "anemoi_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ANEM_LIST_ROWS 8
#define ANEM_CHARS_PER_LINE 19
#define ANEM_LINES_PER_PAGE 3   // 与渲染层 3 行一致（面板 176..240）
#define ANEM_NOTICE_MS 2600u
#define ANEM_FF_STEP_MS 110u
#define ANEM_PROGRESS_RETRY_MS 2000u

static const uint16_t k_type_delay_ms[] = { 0, 18, 34, 60 };
static const uint16_t k_auto_wait_ms[] = { 0, 4200, 2600, 1400 };

static uint16_t title_image_for_level(const anem_app_t *app, uint8_t level);
static void apply_title_image(anem_app_t *app);

static void set_notice(anem_app_t *app, uint8_t kind, const char *text, uint32_t now)
{
    snprintf(app->notice, sizeof(app->notice), "%s", text ? text : "");
    app->notice_kind = kind;
    app->notice_until_ms = now + ANEM_NOTICE_MS;
}

static bool scene_started(uint16_t chapter)
{
    return chapter != 0;
}

// ---------------------------------------------------------------- 画面

static void refresh_picture(anem_app_t *app)
{
    anem_picture_t picture;
    anem_script_picture(&app->script, &picture);
    app->art_image = picture.back_image;
    app->art_cg = picture.cg_layer;
    memcpy(app->art_sprites, picture.sprites, sizeof(app->art_sprites));
    app->art_sprite_count = picture.sprite_count;
    app->art_dirty = true;
}

static uint16_t line_pages(const char *text)
{
    return anem_text_pages(text, ANEM_CHARS_PER_LINE, ANEM_LINES_PER_PAGE);
}

static void layout_line(anem_app_t *app)
{
    const uint16_t start = (uint16_t)(app->page * ANEM_CHARS_PER_LINE * ANEM_LINES_PER_PAGE);
    app->page_line_count = 0;
    memset(app->page_lines, 0, sizeof(app->page_lines));
    for (uint8_t row = 0; row < ANEM_LINES_PER_PAGE; row++) {
        anem_utf8_slice(app->line_text, (uint16_t)(start + row * ANEM_CHARS_PER_LINE),
                        ANEM_CHARS_PER_LINE, app->page_lines[row], ANEM_LINE_TEXT);
        if (app->page_lines[row][0] != 0) {
            app->page_line_count++;
        }
    }
}

static void begin_line(anem_app_t *app)
{
    app->line_text = anem_pack_text(&app->pack, app->script.st.text);
    if (app->line_text == NULL) {
        app->line_text = "";
    }
    app->line_chars = anem_utf8_length(app->line_text);
    app->pages = line_pages(app->line_text);
    if (app->pages == 0) {
        app->pages = 1;
    }
    // 快进模式必须瞬显：否则快进与自动阅读看不出区别。
    app->typed_chars = (app->text_speed == ANEM_SPEED_INSTANT || app->ff_held) ? app->line_chars : 0;
    app->next_type_ms = app->now_ms + k_type_delay_ms[app->text_speed];
    layout_line(app);
    if (app->signature_bg != app->script.st.regular_bg || app->signature_cg != app->script.st.cg ||
        app->signature_bodies != app->script.st.body_count) {
        app->signature_bg = app->script.st.regular_bg;
        app->signature_cg = app->script.st.cg;
        app->signature_bodies = app->script.st.body_count;
        refresh_picture(app);
    }
    // CG 见闻写入全局解锁
    if (app->script.st.cg != 0 && app->progress_ready) {
        const char *name = anem_pack_text(&app->pack, app->script.st.cg);
        if (name != NULL) {
            for (uint16_t i = 0; i < anem_pack_gallery_count(&app->pack); i++) {
                anem_gal_t gal;
                if (!anem_pack_gallery(&app->pack, i, &gal)) {
                    continue;
                }
                const char *label = anem_pack_text(&app->pack, gal.label);
                const char *group = anem_pack_text(&app->pack, gal.group);
                (void)label;
                (void)group;
                if (gal.image == app->script.st.cg) {
                    anem_progress_mark_cg(&app->progress, (uint8_t)i);
                    app->progress_dirty = true;
                    break;
                }
            }
        }
    }
}

static void start_chapter(anem_app_t *app, uint16_t scene, uint16_t entry)
{
    if (!anem_script_start_chapter(&app->script, scene, entry)) {
        set_notice(app, 2, "这个章节不在本卷内", app->now_ms);
        return;
    }
    app->screen = ANEM_SCR_READ;
    app->page = 0;
    app->art_dirty = true;
    refresh_picture(app);
    begin_line(app);
}

static void enter_reader(anem_app_t *app)
{
    app->screen = ANEM_SCR_READ;
    app->page = 0;
    refresh_picture(app);
    begin_line(app);
    app->art_dirty = true;
}

static void start_new_game(anem_app_t *app)
{
    anem_script_new(&app->script, (uint16_t)app->flow_scene, true);
    if (app->progress_ready) {
        (void)anem_progress_apply(&app->progress, &app->script);
    }
    enter_reader(app);
    (void)anem_app_flush_auto(app);
}

// ---------------------------------------------------------------- 存档元数据

static void load_slot_meta(anem_app_t *app, uint8_t meta_index, uint8_t slot_index, bool is_auto)
{
    anem_slot_meta_t *meta = &app->slot_meta[meta_index];
    if (!anem_store_workspace_acquire()) {
        return;   // 工作区忙：元数据保持原样（此时尚未 memset）
    }
    memset(meta, 0, sizeof(*meta));
    anem_slot_t *slot = anem_store_workspace_slot();
    if (slot == NULL) {
        anem_store_workspace_release();
        return;
    }
    const bool ok = is_auto ? anem_store_load_auto(slot) : anem_store_load_slot(slot_index, slot);
    if (!ok) {
        anem_store_workspace_release();
        return;
    }
    meta->valid = true;
    meta->chapter = slot->chapter;
    meta->text = slot->text;
    meta->page = slot->page;
    meta->mode = slot->mode;
    meta->saved_at = slot->saved_at;
    anem_store_workspace_release();
}

void anem_app_refresh_store(anem_app_t *app, uint32_t now_ms)
{
    app->now_ms = now_ms;
    load_slot_meta(app, 0, 0, true);
    app->has_auto = app->slot_meta[0].valid;
    app->has_any_save = false;
    for (uint8_t i = 0; i < ANEM_SLOT_COUNT; i++) {
        load_slot_meta(app, (uint8_t)(i + 1), i, false);
        if (app->slot_meta[i + 1].valid) {
            app->has_any_save = true;
        }
    }
    // 396 变量级结构不能放栈上：用 store 的共享 scratch（同一非重入工作区）。
    if (!anem_store_workspace_acquire()) {
        return;
    }
    anem_progress_t *progress = anem_store_workspace_progress();
    if (progress != NULL && anem_store_load_progress(progress)) {
        app->progress = *progress;
        app->progress_ready = true;
    } else if (!app->progress_ready) {
        anem_progress_clear(&app->progress);
        app->progress_ready = true;
    }
    anem_store_workspace_release();
    if (app->screen == ANEM_SCR_TITLE) {
        app->title_level = app->progress_ready ? app->progress.level : 0;
        apply_title_image(app);
        app->art_dirty = true;
    }
    anem_app_view(app, &(anem_view_t){ 0 });
}

static anem_slot_meta_t *meta_at(anem_app_t *app, uint16_t index)
{
    if (index >= ANEM_SLOT_META_MAX) {
        return NULL;
    }
    return &app->slot_meta[index];
}

// ---------------------------------------------------------------- 读写档

static bool do_save(anem_app_t *app, uint16_t meta_index)
{
    if (!anem_store_workspace_acquire()) {
        set_notice(app, 2, "保存失败：存储忙", app->now_ms);
        return false;
    }
    anem_slot_t *slot = anem_store_workspace_slot();
    if (slot == NULL) {
        anem_store_workspace_release();
        set_notice(app, 2, "保存失败：存储不可用", app->now_ms);
        return false;
    }
    anem_slot_from_state(slot, &app->script, app->screen == ANEM_SCR_CHOICE ? 1 : 0, app->page,
                         app->now_ms / 1000u);
    if (meta_index == 0) {
        if (!anem_store_save_auto(slot)) {
            anem_store_workspace_release();
            set_notice(app, 2, "保存失败：存储不可用", app->now_ms);
            return false;
        }
    } else if (!anem_store_save_slot((uint8_t)(meta_index - 1), slot)) {
        set_notice(app, 2, "保存失败：存储不可用", app->now_ms);
        anem_store_workspace_release();
        return false;
    }
    anem_slot_meta_t *meta = meta_at(app, meta_index);
    if (meta != NULL) {
        meta->valid = true;
        meta->chapter = slot->chapter;
        meta->text = slot->text;
        meta->page = slot->page;
        meta->mode = slot->mode;
        meta->saved_at = slot->saved_at;
    }
    anem_store_workspace_release();
    if (meta_index == 0) {
        app->has_auto = true;
    } else {
        app->has_any_save = true;
    }
    set_notice(app, 1, meta_index == 0 ? "已保存到自动档" : "已保存", app->now_ms);
    return true;
}

static bool do_load(anem_app_t *app, uint16_t meta_index)
{
    if (!anem_store_workspace_acquire()) {
        set_notice(app, 2, "这个栏位没有可读取的存档", app->now_ms);
        return false;
    }
    anem_slot_t *slot = anem_store_workspace_slot();
    if (slot == NULL) {
        anem_store_workspace_release();
        set_notice(app, 2, "这个栏位没有可读取的存档", app->now_ms);
        return false;
    }
    const bool ok = meta_index == 0 ? anem_store_load_auto(slot)
                                    : anem_store_load_slot((uint8_t)(meta_index - 1), slot);
    if (!ok) {
        anem_store_workspace_release();
        set_notice(app, 2, "这个栏位没有可读取的存档", app->now_ms);
        return false;
    }
    if (!anem_slot_apply(slot, &app->script)) {
        anem_store_workspace_release();
        set_notice(app, 2, "存档损坏或不兼容", app->now_ms);
        return false;
    }
    app->page = slot->page;
    app->screen = slot->mode == 1 ? ANEM_SCR_CHOICE : ANEM_SCR_READ;
    app->art_dirty = true;
    refresh_picture(app);
    begin_line(app);
    app->art_dirty = true;
    if (app->screen == ANEM_SCR_CHOICE) {
        anem_frame_t frame = anem_script_read(&app->script);
        if (frame.kind != ANEM_FRAME_CHOICE && frame.kind != ANEM_FRAME_BOARD) {
            app->screen = ANEM_SCR_READ;
        } else {
            app->choice_sel = 0;
        }
    }
    anem_store_workspace_release();
    return true;
}

bool anem_app_flush_auto(anem_app_t *app)
{
    if (!scene_started(app->script.st.chapter) || app->screen == ANEM_SCR_TITLE ||
        app->screen == ANEM_SCR_END || app->screen == ANEM_SCR_ERROR) {
        return false;
    }
    if (!anem_store_workspace_acquire()) {
        return false;
    }
    anem_slot_t *slot = anem_store_workspace_slot();
    if (slot == NULL) {
        anem_store_workspace_release();
        return false;
    }
    anem_slot_from_state(slot, &app->script, app->screen == ANEM_SCR_CHOICE ? 1 : 0, app->page,
                         app->now_ms / 1000u);
    if (!anem_store_save_auto(slot)) {
        anem_store_workspace_release();
        return false;
    }
    app->has_auto = true;
    anem_slot_meta_t *meta = &app->slot_meta[0];
    meta->valid = true;
    meta->chapter = slot->chapter;
    meta->text = slot->text;
    meta->page = slot->page;
    meta->mode = slot->mode;
    meta->saved_at = slot->saved_at;
    anem_store_workspace_release();
    return true;
}

bool anem_app_flush_progress(anem_app_t *app)
{
    if (!app->progress_dirty || !app->progress_ready) {
        return false;
    }
    if (!anem_store_workspace_acquire()) {
        return false;
    }
    anem_progress_t *current = anem_store_workspace_progress();
    if (current == NULL) {
        anem_store_workspace_release();
        return false;
    }
    anem_progress_from_script(current, &app->script);
    // Unlock level is global history. Keep the higher of the level computed
    // from the current script and the previously loaded global level, so a
    // stale level-0 value cannot erase an END-earned level and NewGame/
    // Continue with older slot variables cannot lower existing unlocks.
    if (app->progress.level > current->level) {
        current->level = app->progress.level;
    }
    memcpy(current->cg, app->progress.cg, sizeof(current->cg));
    memcpy(current->ending, app->progress.ending, sizeof(current->ending));
    if (!anem_store_save_progress(current)) {
        anem_store_workspace_release();
        return false;
    }
    app->progress = *current;
    anem_store_workspace_release();
    app->progress_dirty = false;
    return true;
}

// ---------------------------------------------------------------- 推进

static void advance_reader(anem_app_t *app)
{
    if (app->typed_chars < app->line_chars) {
        app->typed_chars = app->line_chars;
        layout_line(app);
        app->auto_deadline = app->now_ms + 300;
        return;
    }
    if (app->page + 1 < app->pages) {
        app->page++;
        layout_line(app);
        return;
    }
    // 历史靠 anem_script_next() 记录（它会把当前 st 与文字 id 压进环形缓冲）。
    // 直接 cursor += 1 会跳过记录，导致“历史”页永远是空的。
    anem_script_next(&app->script);
    app->page = 0;
    const uint16_t before = app->script.st.chapter;
    anem_frame_t frame = anem_script_read(&app->script);
    if (scene_started(before) && before != app->script.st.chapter) {
        (void)anem_app_flush_auto(app);
    }
    switch (frame.kind) {
    case ANEM_FRAME_LINE:
        begin_line(app);
        app->auto_deadline = app->now_ms + k_auto_wait_ms[app->auto_speed ? app->auto_speed : 1];
        break;
    case ANEM_FRAME_CHOICE:
        app->screen = ANEM_SCR_CHOICE;
        app->choice_sel = 0;
        break;
    case ANEM_FRAME_BOARD:
        app->screen = ANEM_SCR_CHOICE;
        app->choice_sel = 0;
        break;
    case ANEM_FRAME_END:
        app->screen = ANEM_SCR_END;
        app->progress_dirty = true;
        if (app->progress_ready) {
            anem_progress_mark_ending(&app->progress, 0);
        }
        (void)anem_app_flush_progress(app);
        break;
    case ANEM_FRAME_BOUNDARY:
        app->screen = ANEM_SCR_BOUNDARY;
        break;
    default:
        app->screen = ANEM_SCR_ERROR;
        break;
    }
    if (app->screen == ANEM_SCR_CHOICE) {
        app->auto_enabled = false;  // 碰到选择必须停住
        app->ff_held = false;
    }
}

static void choose_current(anem_app_t *app)
{
    if (app->script.choice_count == 0) {
        return;
    }
    const uint8_t index = app->choice_sel;
    const bool board = anem_script_choice_item(&app->script, index) != NULL &&
                       anem_script_choice_item(&app->script, index)->board;
    if (!anem_script_choose(&app->script, index)) {
        app->screen = ANEM_SCR_BOUNDARY;
        return;
    }
    if (board) {
        app->screen = ANEM_SCR_READ;
        app->art_dirty = true;
    } else {
        app->screen = ANEM_SCR_READ;
    }
    app->page = 0;
    anem_frame_t frame = anem_script_read(&app->script);
    switch (frame.kind) {
    case ANEM_FRAME_LINE:
        begin_line(app);
        break;
    case ANEM_FRAME_CHOICE:
    case ANEM_FRAME_BOARD:
        app->screen = ANEM_SCR_CHOICE;
        app->choice_sel = 0;
        break;
    case ANEM_FRAME_END:
        app->screen = ANEM_SCR_END;
        break;
    case ANEM_FRAME_BOUNDARY:
        app->screen = ANEM_SCR_BOUNDARY;
        break;
    default:
        app->screen = ANEM_SCR_ERROR;
        break;
    }
    app->art_dirty = true;
    refresh_picture(app);
    (void)anem_app_flush_auto(app);
}

// ---------------------------------------------------------------- 列表

static uint16_t list_count(anem_app_t *app)
{
    switch (app->screen) {
    case ANEM_SCR_MENU: return 6;
    case ANEM_SCR_SAVE:
    case ANEM_SCR_LOAD: return ANEM_SLOT_META_MAX;
    case ANEM_SCR_CHAPTERS: return (uint16_t)anem_pack_chapter_count(&app->pack);
    case ANEM_SCR_GALLERY: return (uint16_t)anem_pack_gallery_count(&app->pack);
    case ANEM_SCR_GALLERY_MENU: return 2;
    case ANEM_SCR_HISTORY: return anem_script_history_text_count(&app->script);
    case ANEM_SCR_SETTINGS: return 5;
    case ANEM_SCR_TITLE: return 6;
    case ANEM_SCR_END: return 2;
    case ANEM_SCR_BOUNDARY:
    case ANEM_SCR_ERROR: return 1;
    case ANEM_SCR_CG: return 1;
    case ANEM_SCR_ABOUT: return 1;
    default: return 0;
    }
}

static void move_selection(anem_app_t *app, int delta)
{
    const uint16_t count = list_count(app);
    if (count == 0) {
        return;
    }
    int next = (int)app->sel[app->screen] + delta;
    if (next < 0) {
        next = 0;
    }
    if (next >= (int)count) {
        next = (int)count - 1;
    }
    app->sel[app->screen] = (uint16_t)next;
    const uint16_t sel = app->sel[app->screen];
    if (sel < app->scroll[app->screen]) {
        app->scroll[app->screen] = sel;
    } else if (sel >= app->scroll[app->screen] + ANEM_LIST_ROWS) {
        app->scroll[app->screen] = (uint16_t)(sel - ANEM_LIST_ROWS + 1);
    }
}

static void open_list(anem_app_t *app, uint8_t screen)
{
    if (app->back_depth < 8) {
        app->back_stack_screen[app->back_depth] = screen;
        app->back_stack_back[app->back_depth] = app->back_screen;
        app->back_depth++;
    }
    app->back_screen = app->screen;
    app->screen = screen;
    app->sel[screen] = 0;
    app->scroll[screen] = 0;
}

static uint16_t title_image_for_level(const anem_app_t *app, uint8_t level)
{
    // Source title-buttons.js: levels 1/2 use title-eternicle, levels 3/4
    // use title-anemoi, and fresh/normal (0, 5+) use title-bg.
    if (level == 1 || level == 2) {
        return app->title_image_eternicle;
    }
    if (level == 3 || level == 4) {
        return app->title_image_anemoi;
    }
    return app->title_image_normal;
}

static void apply_title_image(anem_app_t *app)
{
    const uint16_t selected = title_image_for_level(app, app->title_level);
    if (selected != ANEM_NONE16) {
        app->title_image = selected;
        app->art_image = selected;
    }
}

// 进入标题页：必须清掉上一局残留的立绘/场景图，否则标题上会盖着游戏里的立绘。
static void enter_title(anem_app_t *app)
{
    app->screen = ANEM_SCR_TITLE;
    app->auto_enabled = false;
    app->ff_held = false;
    app->art_sprite_count = 0;
    app->art_cg = false;
    app->title_level = app->progress_ready ? app->progress.level : 0;
    apply_title_image(app);
    app->art_dirty = true;
}

static void back_to(anem_app_t *app, uint8_t screen)
{
    // 回到上一层时恢复那一层原来的 back_screen，避免链式返回丢失。
    for (int i = (int)app->back_depth - 1; i >= 0; i--) {
        if (app->back_stack_screen[i] == screen) {
            app->back_screen = app->back_stack_back[i];
            app->back_depth = (uint8_t)i;
            break;
        }
    }
    if (screen == ANEM_SCR_TITLE) {
        enter_title(app);
        return;
    }
    app->screen = screen;
    if (screen == ANEM_SCR_READ) {
        app->page = app->page < app->pages ? app->page : 0;
        layout_line(app);
    }
}

// ---------------------------------------------------------------- 键处理

static void key_title(anem_app_t *app, uint8_t key, uint8_t event)
{
    if (event == ANEM_EV_CLICK && (key == ANEM_KEY_UP || key == ANEM_KEY_DOWN)) {
        move_selection(app, key == ANEM_KEY_UP ? -1 : 1);
        return;
    }
    if (event != ANEM_EV_CLICK || key != ANEM_KEY_OK) {
        return;
    }
    switch (app->sel[ANEM_SCR_TITLE]) {
    case 0:  // 开始游戏
        start_new_game(app);
        break;
    case 1:  // 继续游戏
        if (app->has_auto) {
            if (!do_load(app, 0)) {
                set_notice(app, 2, "自动档不可读取", app->now_ms);
            }
        } else {
            set_notice(app, 1, "还没有可继续的进度", app->now_ms);
        }
        break;
    case 2:  // 读取存档
        open_list(app, ANEM_SCR_LOAD);
        break;
    case 3:  // 鉴赏
        open_list(app, ANEM_SCR_GALLERY_MENU);
        break;
    case 4:  // 设置
        open_list(app, ANEM_SCR_SETTINGS);
        break;
    default:  // 关于
        open_list(app, ANEM_SCR_ABOUT);
        break;
    }
}

static void key_reader(anem_app_t *app, uint8_t key, uint8_t event)
{
    if (key == ANEM_KEY_OK && event == ANEM_EV_LONG) {
        app->menu_back = app->screen;
        open_list(app, ANEM_SCR_MENU);
        return;
    }
    if (key == ANEM_KEY_UP && event == ANEM_EV_LONG) {
        app->ff_held = true;
        app->ff_next_ms = app->now_ms;
        return;
    }
    if (key == ANEM_KEY_UP && event == ANEM_EV_RELEASE) {
        app->ff_held = false;
        return;
    }
    if (key == ANEM_KEY_DOWN && event == ANEM_EV_LONG) {
        app->auto_enabled = !app->auto_enabled;
        app->auto_deadline = app->now_ms + k_auto_wait_ms[app->auto_speed ? app->auto_speed : 1];
        set_notice(app, 1, app->auto_enabled ? "自动播放：开" : "自动播放：关", app->now_ms);
        return;
    }
    if (event != ANEM_EV_CLICK) {
        return;
    }
    if (key == ANEM_KEY_DOWN) {
        app->back_screen = app->screen;
        app->screen = ANEM_SCR_HISTORY;
        app->sel[ANEM_SCR_HISTORY] = 0;
        app->scroll[ANEM_SCR_HISTORY] = 0;
        return;
    }
    if (key == ANEM_KEY_UP || key == ANEM_KEY_OK) {
        app->auto_enabled = false;
        advance_reader(app);
    }
}

static void key_choice(anem_app_t *app, uint8_t key, uint8_t event)
{
    if (key == ANEM_KEY_OK && event == ANEM_EV_LONG) {
        app->menu_back = ANEM_SCR_CHOICE;
        open_list(app, ANEM_SCR_MENU);
        return;
    }
    if (event != ANEM_EV_CLICK) {
        return;
    }
    if (key == ANEM_KEY_UP || key == ANEM_KEY_DOWN) {
        if (app->script.choice_count == 0) {
            return;
        }
        int next = (int)app->choice_sel + (key == ANEM_KEY_UP ? -1 : 1);
        if (next < 0) {
            next = 0;
        }
        if (next >= app->script.choice_count) {
            next = app->script.choice_count - 1;
        }
        app->choice_sel = (uint8_t)next;
        return;
    }
    choose_current(app);
}

static void key_menu(anem_app_t *app, uint8_t key, uint8_t event)
{
    if (event == ANEM_EV_LONG && key == ANEM_KEY_OK) {
        back_to(app, app->menu_back);
        return;
    }
    if (event == ANEM_EV_CLICK && (key == ANEM_KEY_UP || key == ANEM_KEY_DOWN)) {
        move_selection(app, key == ANEM_KEY_UP ? -1 : 1);
        return;
    }
    if (event != ANEM_EV_CLICK || key != ANEM_KEY_OK) {
        return;
    }
    switch (app->sel[ANEM_SCR_MENU]) {
    case 0:
        back_to(app, app->menu_back);
        break;
    case 1:
        open_list(app, ANEM_SCR_SAVE);
        break;
    case 2:
        open_list(app, ANEM_SCR_LOAD);
        break;
    case 3:
        app->back_screen = app->menu_back;
        app->screen = ANEM_SCR_HISTORY;
        app->sel[ANEM_SCR_HISTORY] = 0;
        app->scroll[ANEM_SCR_HISTORY] = 0;
        break;
    case 4:
        open_list(app, ANEM_SCR_SETTINGS);
        break;
    default:
        (void)anem_app_flush_auto(app);
        (void)anem_app_flush_progress(app);
        app->progress.level = anem_script_unlock_level(&app->script);
        enter_title(app);
        break;
    }
}

static void key_slots(anem_app_t *app, uint8_t key, uint8_t event)
{
    const bool saving = app->screen == ANEM_SCR_SAVE;
    if (event == ANEM_EV_LONG && key == ANEM_KEY_OK) {
        app->overwrite_pending = 0;
        back_to(app, app->back_screen);
        return;
    }
    if (event == ANEM_EV_CLICK && (key == ANEM_KEY_UP || key == ANEM_KEY_DOWN)) {
        app->overwrite_pending = 0;
        move_selection(app, key == ANEM_KEY_UP ? -1 : 1);
        return;
    }
    if (event != ANEM_EV_CLICK || key != ANEM_KEY_OK) {
        return;
    }
    const uint16_t index = app->sel[app->screen];
    if (saving) {
        if (index != 0 && app->slot_meta[index].valid && app->overwrite_pending != index + 1) {
            app->overwrite_pending = (uint8_t)(index + 1);
            set_notice(app, 1, "再按一次确定覆盖这个存档", app->now_ms);
            return;
        }
        app->overwrite_pending = 0;
        if (do_save(app, index)) {
            back_to(app, app->menu_back);
        }
        return;
    }
    if (!app->slot_meta[index].valid) {
        set_notice(app, 2, "这个栏位没有可读取的存档", app->now_ms);
        return;
    }
    if (do_load(app, index)) {
        app->screen = ANEM_SCR_READ;
    }
}

static void key_chapters(anem_app_t *app, uint8_t key, uint8_t event)
{
    if (event == ANEM_EV_LONG && key == ANEM_KEY_OK) {
        back_to(app, app->back_screen);
        return;
    }
    if (event == ANEM_EV_CLICK && (key == ANEM_KEY_UP || key == ANEM_KEY_DOWN)) {
        move_selection(app, key == ANEM_KEY_UP ? -1 : 1);
        return;
    }
    if (event != ANEM_EV_CLICK || key != ANEM_KEY_OK) {
        return;
    }
    anem_chap_t chap;
    if (!anem_pack_chapter(&app->pack, app->sel[app->screen], &chap)) {
        return;
    }
    uint32_t first = 0, count = 0;
    if (!anem_pack_scene_range(&app->pack, chap.scene, &first, &count)) {
        set_notice(app, 2, "本卷未收录这一章", app->now_ms);
        return;
    }
    (void)anem_app_flush_auto(app);
    start_chapter(app, chap.scene, 0);
}

static void key_gallery_menu(anem_app_t *app, uint8_t key, uint8_t event)
{
    if (event == ANEM_EV_LONG && key == ANEM_KEY_OK) {
        back_to(app, app->back_screen);
        return;
    }
    if (event == ANEM_EV_CLICK && (key == ANEM_KEY_UP || key == ANEM_KEY_DOWN)) {
        move_selection(app, key == ANEM_KEY_UP ? -1 : 1);
        return;
    }
    if (event != ANEM_EV_CLICK || key != ANEM_KEY_OK) {
        return;
    }
    if (app->sel[ANEM_SCR_GALLERY_MENU] == 0) {
        open_list(app, ANEM_SCR_GALLERY);
    } else {
        open_list(app, ANEM_SCR_CHAPTERS);
    }
}

static void key_gallery(anem_app_t *app, uint8_t key, uint8_t event)
{
    if (event == ANEM_EV_LONG && key == ANEM_KEY_OK) {
        back_to(app, app->back_screen);
        return;
    }
    if (event == ANEM_EV_CLICK && (key == ANEM_KEY_UP || key == ANEM_KEY_DOWN)) {
        move_selection(app, key == ANEM_KEY_UP ? -1 : 1);
        return;
    }
    if (event != ANEM_EV_CLICK || key != ANEM_KEY_OK) {
        return;
    }
    anem_gal_t gal;
    if (!anem_pack_gallery(&app->pack, app->sel[ANEM_SCR_GALLERY], &gal)) {
        return;
    }
    app->cg_index = (uint8_t)app->sel[ANEM_SCR_GALLERY];
    app->art_image = gal.image;
    app->art_cg = true;
    app->art_sprite_count = 0;
    app->art_dirty = true;
    app->back_screen = ANEM_SCR_GALLERY;
    app->screen = ANEM_SCR_CG;
    if (app->progress_ready) {
        anem_progress_mark_cg(&app->progress, app->cg_index);
        app->progress_dirty = true;
        (void)anem_app_flush_progress(app);
    }
}

static void key_history(anem_app_t *app, uint8_t key, uint8_t event)
{
    if (event == ANEM_EV_LONG && key == ANEM_KEY_OK) {
        back_to(app, app->back_screen);
        return;
    }
    if (event == ANEM_EV_CLICK && (key == ANEM_KEY_UP || key == ANEM_KEY_DOWN)) {
        move_selection(app, key == ANEM_KEY_UP ? -1 : 1);
    }
}

static void key_settings(anem_app_t *app, uint8_t key, uint8_t event)
{
    if (event == ANEM_EV_LONG && key == ANEM_KEY_OK) {
        back_to(app, app->back_screen);
        return;
    }
    if (event == ANEM_EV_CLICK && (key == ANEM_KEY_UP || key == ANEM_KEY_DOWN)) {
        move_selection(app, key == ANEM_KEY_UP ? -1 : 1);
        return;
    }
    if (event != ANEM_EV_CLICK || key != ANEM_KEY_OK) {
        return;
    }
    switch (app->sel[ANEM_SCR_SETTINGS]) {
    case 0:
        app->text_speed = (uint8_t)((app->text_speed + 1) % 4);
        break;
    case 1:
        app->auto_speed = (uint8_t)(app->auto_speed % 3 + 1);
        break;
    case 2:
        app->panel_alpha = app->panel_alpha > 80 ? 80 : 100;
        break;
    case 3: {
        // 字号：当前只提供 16px（完整字库）；不显示无法提供的选项。
        set_notice(app, 1, "字号：16px（唯一可用档）", app->now_ms);
        break;
    }
    default:
        open_list(app, ANEM_SCR_ABOUT);
        break;
    }
}

static void key_cg(anem_app_t *app, uint8_t key, uint8_t event)
{
    if (event == ANEM_EV_LONG && key == ANEM_KEY_OK) {
        back_to(app, app->back_screen);
        return;
    }
    if (event != ANEM_EV_CLICK) {
        return;
    }
    if (key == ANEM_KEY_OK) {
        back_to(app, app->back_screen);
        return;
    }
    const int delta = key == ANEM_KEY_UP ? -1 : 1;
    int next = (int)app->cg_index + delta;
    const int count = (int)anem_pack_gallery_count(&app->pack);
    if (next < 0) {
        next = 0;
    }
    if (next >= count) {
        next = count - 1;
    }
    anem_gal_t gal;
    if (anem_pack_gallery(&app->pack, (uint16_t)next, &gal)) {
        app->cg_index = (uint8_t)next;
        app->art_image = gal.image;
        app->art_cg = true;
        app->art_sprite_count = 0;
        app->art_dirty = true;
    }
}

static void app_key_impl(anem_app_t *app, uint8_t key, uint8_t event, uint32_t now_ms);

void anem_app_key(anem_app_t *app, uint8_t key, uint8_t event, uint32_t now_ms)
{
    // 统一“二级页 + 长按 OK = 返回”：在分发前处理，任何页面忘了写 LONG 分支也能退。
    if (app != NULL && event == ANEM_EV_LONG && key == ANEM_KEY_OK) {
        switch (app->screen) {
        case ANEM_SCR_MENU:
            back_to(app, app->menu_back);
            return;
        case ANEM_SCR_SAVE:
        case ANEM_SCR_LOAD:
        case ANEM_SCR_SETTINGS:
        case ANEM_SCR_HISTORY:
        case ANEM_SCR_GALLERY:
        case ANEM_SCR_GALLERY_MENU:
        case ANEM_SCR_CHAPTERS:
        case ANEM_SCR_CG:
        case ANEM_SCR_ABOUT:
            back_to(app, app->back_screen);
            return;
        default:
            break;
        }
    }
    app_key_impl(app, key, event, now_ms);
}

static void app_key_impl(anem_app_t *app, uint8_t key, uint8_t event, uint32_t now_ms)
{
    app->now_ms = now_ms;
    if (app->screen == ANEM_SCR_TITLE) {
        key_title(app, key, event);
        return;
    }
    if (app->screen == ANEM_SCR_READ) {
        key_reader(app, key, event);
        return;
    }
    if (app->screen == ANEM_SCR_CHOICE) {
        key_choice(app, key, event);
        return;
    }
    if (app->screen == ANEM_SCR_MENU) {
        key_menu(app, key, event);
        return;
    }
    if (app->screen == ANEM_SCR_SAVE || app->screen == ANEM_SCR_LOAD) {
        key_slots(app, key, event);
        return;
    }
    if (app->screen == ANEM_SCR_CHAPTERS) {
        key_chapters(app, key, event);
        return;
    }
    if (app->screen == ANEM_SCR_GALLERY_MENU) {
        key_gallery_menu(app, key, event);
        return;
    }
    if (app->screen == ANEM_SCR_GALLERY) {
        key_gallery(app, key, event);
        return;
    }
    if (app->screen == ANEM_SCR_CG) {
        key_cg(app, key, event);
        return;
    }
    if (app->screen == ANEM_SCR_HISTORY) {
        key_history(app, key, event);
        return;
    }
    if (app->screen == ANEM_SCR_SETTINGS) {
        key_settings(app, key, event);
        return;
    }
    if (event == ANEM_EV_CLICK && key == ANEM_KEY_OK) {
        if (app->screen == ANEM_SCR_ABOUT) {
            back_to(app, app->back_screen);
        } else {
            enter_title(app);
        }
    }
}
// ---------------------------------------------------------------- 视图

static const char *speed_label(uint8_t speed)
{
    switch (speed) {
    case ANEM_SPEED_INSTANT: return "即时";
    case ANEM_SPEED_FAST: return "快";
    case ANEM_SPEED_NORMAL: return "标准";
    default: return "慢";
    }
}

static const char *auto_label(uint8_t speed)
{
    switch (speed) {
    case ANEM_AUTO_SLOW: return "慢";
    case ANEM_AUTO_FAST: return "快";
    default: return "标准";
    }
}

static void build_rows(anem_app_t *app, anem_view_t *view)
{
    const uint16_t sel = app->sel[app->screen];
    const uint16_t scroll = app->scroll[app->screen];
    const int y = 30;
    switch (app->screen) {
    case ANEM_SCR_TITLE: {
        static const char *const items[6] = { "开始游戏", "继续游戏", "读取存档", "鉴赏", "设置",
                                              "关于" };
        for (int i = 0; i < 6; i++) {
            const bool enabled = i != 1 || app->has_auto;
            anem_view_add_row(view, y + i * 24, ANEM_VIEW_TEXT_ROW, items[i],
                              sel == (uint16_t)i, enabled);
        }
        snprintf(view->header, sizeof(view->header), "anemoi");
        break;
    }
    case ANEM_SCR_MENU: {
        static const char *const items[6] = { "返回游戏", "存档", "读档", "历史", "设置",
                                              "返回标题" };
        for (int i = 0; i < 6; i++) {
            anem_view_add_row(view, y + i * 24, ANEM_VIEW_TEXT_ROW, items[i], sel == (uint16_t)i,
                              true);
        }
        snprintf(view->header, sizeof(view->header), "菜单");
        break;
    }
    case ANEM_SCR_SAVE:
    case ANEM_SCR_LOAD: {
        snprintf(view->header, sizeof(view->header), "%s",
                 app->screen == ANEM_SCR_SAVE ? "保存进度" : "读取进度");
        for (uint16_t i = scroll; i < ANEM_SLOT_META_MAX && view->row_count < ANEM_LIST_ROWS; i++) {
            const anem_slot_meta_t *meta = &app->slot_meta[i];
            char text[ANEM_ROW_TEXT];
            char preview[ANEM_ROW_TEXT];
            if (i == 0) {
                snprintf(preview, sizeof(preview), "自动档");
            } else {
                snprintf(preview, sizeof(preview), "存档 %02u", (unsigned)i);
            }
            if (meta->valid) {
                char brief[28] = "";
                if (anem_store_workspace_acquire()) {
                    anem_slot_t *slot = anem_store_workspace_slot();
                    const bool ok = slot != NULL &&
                                    (i == 0 ? anem_store_load_auto(slot)
                                            : anem_store_load_slot((uint8_t)(i - 1), slot));
                    if (ok) {
                        (void)anem_slot_preview(slot, &app->pack, brief, sizeof(brief));
                    }
                    anem_store_workspace_release();
                }
                snprintf(text, sizeof(text), "%.*s  %.*s", 20, preview, 60, brief);
            } else {
                snprintf(text, sizeof(text), "%.*s  （空）", 20, preview);
            }
            anem_view_add_row(view, y + view->row_count * 20, ANEM_VIEW_TEXT_ROW, text,
                              sel == i, true);
        }
        break;
    }
    case ANEM_SCR_CHAPTERS: {
        snprintf(view->header, sizeof(view->header), "章节 / 路线");
        const uint16_t count = (uint16_t)anem_pack_chapter_count(&app->pack);
        for (uint16_t i = scroll; i < count && view->row_count < ANEM_LIST_ROWS; i++) {
            anem_chap_t chap;
            if (!anem_pack_chapter(&app->pack, i, &chap)) {
                continue;
            }
            const char *name = anem_pack_text(&app->pack, chap.name);
            const char *day = anem_pack_text(&app->pack, chap.day);
            uint32_t first = 0, ops = 0;
            const bool in_volume = anem_pack_scene_range(&app->pack, chap.scene, &first, &ops);
            char text[ANEM_ROW_TEXT];
            if (in_volume) {
                snprintf(text, sizeof(text), "%s%s%s", name ? name : "",
                         day != NULL && day[0] ? "  " : "", day ? day : "");
            } else {
                snprintf(text, sizeof(text), "%s（本卷未收录）", name ? name : "");
            }
            anem_view_add_row(view, y + view->row_count * 20, ANEM_VIEW_TEXT_ROW, text, sel == i,
                              in_volume);
        }
        break;
    }
    case ANEM_SCR_GALLERY: {
        snprintf(view->header, sizeof(view->header), "CG 鉴赏");
        const uint16_t count = (uint16_t)anem_pack_gallery_count(&app->pack);
        for (uint16_t i = scroll; i < count && view->row_count < ANEM_LIST_ROWS; i++) {
            anem_gal_t gal;
            if (!anem_pack_gallery(&app->pack, i, &gal)) {
                continue;
            }
            const char *label = anem_pack_text(&app->pack, gal.label);
            char text[ANEM_ROW_TEXT];
            snprintf(text, sizeof(text), "%s%s", label ? label : "",
                     anem_progress_has_cg(&app->progress, (uint8_t)i) ? "  ✓" : "");
            anem_view_add_row(view, y + view->row_count * 20, ANEM_VIEW_TEXT_ROW, text, sel == i,
                              true);
        }
        break;
    }
    case ANEM_SCR_GALLERY_MENU:
        snprintf(view->header, sizeof(view->header), "鉴赏");
        anem_view_add_row(view, y, ANEM_VIEW_TEXT_ROW, "CG 鉴赏", sel == 0, true);
        anem_view_add_row(view, y + 24, ANEM_VIEW_TEXT_ROW, "章节 / 路线", sel == 1, true);
        break;
    case ANEM_SCR_HISTORY: {
        snprintf(view->header, sizeof(view->header), "历史");
        const uint16_t count = anem_script_history_text_count(&app->script);
        for (uint16_t i = scroll; i < count && view->row_count < ANEM_LIST_ROWS; i++) {
            const char *text =
                anem_pack_text(&app->pack, anem_script_history_text(&app->script, (uint8_t)i));
            anem_view_add_row(view, y + view->row_count * 20, ANEM_VIEW_TEXT_ROW,
                              text ? text : "", false, true);
        }
        break;
    }
    case ANEM_SCR_SETTINGS: {
        snprintf(view->header, sizeof(view->header), "设置");
        char text[ANEM_ROW_TEXT];
        snprintf(text, sizeof(text), "文字速度：%s", speed_label(app->text_speed));
        anem_view_add_row(view, y, ANEM_VIEW_TEXT_ROW, text, sel == 0, true);
        snprintf(text, sizeof(text), "自动播放速度：%s", auto_label(app->auto_speed));
        anem_view_add_row(view, y + 24, ANEM_VIEW_TEXT_ROW, text, sel == 1, true);
        snprintf(text, sizeof(text), "对话框不透明度：%u%%", (unsigned)app->panel_alpha);
        anem_view_add_row(view, y + 48, ANEM_VIEW_TEXT_ROW, text, sel == 2, true);
        anem_view_add_row(view, y + 72, ANEM_VIEW_TEXT_ROW, "字号：16px（完整字库）", sel == 3,
                          true);
        anem_view_add_row(view, y + 96, ANEM_VIEW_TEXT_ROW, "关于本卷", sel == 4, true);
        break;
    }
    case ANEM_SCR_ABOUT:
        snprintf(view->header, sizeof(view->header), "关于");
        anem_view_add_row(view, 28, ANEM_VIEW_TEXT_ROW, "anemoi · AI Passport 横屏阅读器", false,
                          true);
        anem_view_add_row(view, 52, ANEM_VIEW_TEXT_ROW, "本卷：共通篇（Day 1-25 主线）", false,
                          true);
        anem_view_add_row(view, 76, ANEM_VIEW_TEXT_ROW, "立绘：身体 + 原生表情分层合成", false,
                          true);
        anem_view_add_row(view, 100, ANEM_VIEW_TEXT_ROW, "背景/CG：等比缩放到 320x180", false,
                          true);
        anem_view_add_row(view, 124, ANEM_VIEW_TEXT_ROW, "路线卷与委托任务见分卷说明", false,
                          true);
        anem_view_add_row(view, 160, ANEM_VIEW_TEXT_ROW, "按确定返回", false, true);
        break;
    case ANEM_SCR_END:
        snprintf(view->header, sizeof(view->header), "本篇阅读完毕");
        anem_view_add_row(view, 44, ANEM_VIEW_TEXT_ROW, "返回标题", sel == 0, true);
        anem_view_add_row(view, 68, ANEM_VIEW_TEXT_ROW, "查看章节 / 鉴赏", sel == 1, true);
        break;
    case ANEM_SCR_BOUNDARY: {
        snprintf(view->header, sizeof(view->header), "本卷到此为止");
        char text[ANEM_ROW_TEXT];
        snprintf(text, sizeof(text), "下一段内容属于：%s",
                 app->script.boundary_chapter[0] ? app->script.boundary_chapter
                                                 : "路线卷 / 委托任务");
        anem_view_add_row(view, 40, ANEM_VIEW_TEXT_ROW, text, false, true);
        anem_view_add_row(view, 64, ANEM_VIEW_TEXT_ROW, "本卷收录共通篇 Day 1-25 主线", false,
                          true);
        anem_view_add_row(view, 100, ANEM_VIEW_TEXT_ROW, "按确定返回标题", sel == 0, true);
        break;
    }
    case ANEM_SCR_ERROR:
        snprintf(view->header, sizeof(view->header), "暂时无法继续");
        anem_view_add_row(view, 40, ANEM_VIEW_TEXT_ROW, anem_script_notice(&app->script), false,
                          true);
        anem_view_add_row(view, 76, ANEM_VIEW_TEXT_ROW, "按确定返回标题", sel == 0, true);
        break;
    default:
        break;
    }
}

void anem_app_view(anem_app_t *app, anem_view_t *view)
{
    anem_view_clear(view);
    view->screen = app->screen;
    view->sel = app->sel[app->screen];
    if (app->notice[0] != 0 && (int32_t)(app->notice_until_ms - app->now_ms) > 0) {
        snprintf(view->notice, sizeof(view->notice), "%s", app->notice);
        view->notice_kind = app->notice_kind;
    }
    if (app->screen == ANEM_SCR_READ || app->screen == ANEM_SCR_CHOICE) {
        view->show_art = true;
        view->show_panel = true;
        view->art_image = app->art_image;
        view->sprite_count = app->art_sprite_count;
        memcpy(view->sprite, app->art_sprites, sizeof(view->sprite));
        const char *speaker = app->script.st.speaker == ANEM_NONE16
                                  ? ""
                                  : anem_pack_text(&app->pack, app->script.st.speaker);
        snprintf(view->speaker, sizeof(view->speaker), "%s", speaker ? speaker : "");
        snprintf(view->header, sizeof(view->header), "%s  %s",
                 anem_script_chapter_name(&app->script), anem_script_day_label(&app->script));
        if (app->screen == ANEM_SCR_READ) {
            const uint16_t page_start =
                (uint16_t)(app->page * ANEM_CHARS_PER_LINE * ANEM_LINES_PER_PAGE);
            uint16_t shown = 0;
            if (app->typed_chars > page_start) {
                shown = (uint16_t)(app->typed_chars - page_start);
            }
            for (uint8_t row = 0; row < ANEM_LINES_PER_PAGE; row++) {
                const uint16_t row_start = (uint16_t)(row * ANEM_CHARS_PER_LINE);
                if (app->typed_chars < app->line_chars && shown <= row_start) {
                    break;
                }
                uint16_t count = ANEM_CHARS_PER_LINE;
                if (app->typed_chars < app->line_chars) {
                    const uint16_t remain = shown > row_start ? (uint16_t)(shown - row_start) : 0;
                    if (remain < count) {
                        count = remain;
                    }
                }
                anem_utf8_slice(app->line_text, (uint16_t)(page_start + row_start), count,
                                view->line[row], ANEM_LINE_TEXT);
                view->line_count++;
            }
            view->page = app->page;
            view->pages = app->pages;
            view->typing = app->typed_chars < app->line_chars;
        }
    }
    if (app->screen == ANEM_SCR_TITLE) {
        view->fullscreen_art = true;
        view->show_art = true;
        view->art_image = app->title_image;
        // 断链修复：draw 用的是 app->art_image（不是 view），标题图必须在进入标题时同步过去。
        // 只在真正换图时置 dirty，否则每次 view() 都重画 JPEG 会把 LVGL 锁占满，按键任务卡死。
        if (app->title_image != ANEM_NONE16 && app->art_image != app->title_image) {
            app->art_image = app->title_image;
            app->art_dirty = true;
        }
        snprintf(view->header, sizeof(view->header), "anemoi");
    }
    if (app->screen == ANEM_SCR_CG) {
        view->fullscreen_art = true;
        view->show_art = true;
        view->art_image = app->art_image;
        snprintf(view->header, sizeof(view->header), "CG 鉴赏");
    }
    if (app->screen == ANEM_SCR_CHOICE) {
        const uint8_t count = app->script.choice_count;
        for (uint8_t i = 0; i < count && i < ANEM_ROW_MAX; i++) {
            const anem_choice_item_t *item = anem_script_choice_item(&app->script, i);
            if (item == NULL) {
                continue;
            }
            const char *label = anem_script_text(&app->script, item->label);
            char text[ANEM_ROW_TEXT];
            if (item->board && item->detail != 0) {
                const char *detail = anem_script_text(&app->script, item->detail);
                snprintf(text, sizeof(text), "%s  %s", label ? label : "",
                         detail ? detail : "");
            } else {
                snprintf(text, sizeof(text), "%s", label ? label : "");
            }
            anem_view_add_row(view, 26 + i * 24, ANEM_VIEW_TEXT_ROW, text,
                              app->choice_sel == i, item->enabled != 0);
        }
        view->sel = app->choice_sel;
    } else if (app->screen != ANEM_SCR_READ) {
        build_rows(app, view);
    }
}

// ---------------------------------------------------------------- 绘制

bool anem_app_draw(anem_app_t *app)
{
    app->art_dirty = false;
    if (app->screen == ANEM_SCR_TITLE) {
        anem_canvas_fill(&app->canvas, 0xFFDE);   // 标题图两侧使用源纸色 #fcfaf0
    }
    if (!app->draw_art || app->art_image == ANEM_NONE16) {
        return true;
    }
    anem_image_t image;
    if (!anem_pack_image(&app->pack, app->art_image, &image)) {
        app->art_failed = true;
        return false;
    }
    bool ok;
    if (image.kind == ANEM_IMG_SOLID) {
        ok = anem_image_draw_solid(&app->canvas, &app->pack, app->art_image);
    } else {
        ok = anem_image_draw_scene(&app->image, &app->canvas, &app->pack, app->art_image);
    }
    if (!ok) {
        app->art_failed = true;
        return false;
    }
    if (!app->art_cg) {
        for (uint8_t i = 0; i < app->art_sprite_count; i++) {
            if (!app->art_sprites[i].valid) {
                continue;
            }
            // 面板从 y=162 开始；立绘按脚本坐标画到 180，底部会被对话框盖住，
            // 整体上移 (162-180)=-18，让“脚”落在面板上沿（不缩放，只平移）。
            const int sprite_dy = 156 - ANEM_CANVAS_H;   // 面板上沿 156：立绘脚落在框顶，不被盖
            if (!anem_image_draw_sprite(&app->image, &app->canvas, &app->pack,
                                        app->art_sprites[i].image, app->art_sprites[i].x,
                                        app->art_sprites[i].y + sprite_dy)) {
                app->art_failed = true;
                return false;
            }
        }
    }
    app->art_failed = false;
    return true;
}

bool anem_app_art_dirty(const anem_app_t *app)
{
    return app->art_dirty;
}

// ---------------------------------------------------------------- 时间推进

static void tick_typing(anem_app_t *app)
{
    if (app->screen != ANEM_SCR_READ || app->typed_chars >= app->line_chars) {
        return;
    }
    if (app->text_speed == ANEM_SPEED_INSTANT) {
        app->typed_chars = app->line_chars;
        return;
    }
    while ((int32_t)(app->now_ms - app->next_type_ms) >= 0 &&
           app->typed_chars < app->line_chars) {
        app->typed_chars++;
        app->next_type_ms += k_type_delay_ms[app->text_speed];
    }
    if (app->typed_chars >= app->line_chars) {
        app->auto_deadline = app->now_ms + k_auto_wait_ms[app->auto_speed ? app->auto_speed : 1];
    }
}

void anem_app_tick(anem_app_t *app, uint32_t now_ms)
{
    app->now_ms = now_ms;
    if (app->notice[0] != 0 && (int32_t)(now_ms - app->notice_until_ms) >= 0) {
        app->notice[0] = 0;
        app->notice_kind = 0;
    }
    if (app->progress_dirty && (int32_t)(now_ms - app->progress_retry_ms) >= 0) {
        app->progress_retry_ms = now_ms + ANEM_PROGRESS_RETRY_MS;
        (void)anem_app_flush_progress(app);
    }
    if (app->screen != ANEM_SCR_READ) {
        return;
    }
    if (app->ff_held) {
        app->typed_chars = app->line_chars;   // 快进：文字瞬显，不再逐字
    }
    tick_typing(app);
    if (app->ff_held) {
        if ((int32_t)(now_ms - app->ff_next_ms) >= 0) {
            app->ff_next_ms = now_ms + ANEM_FF_STEP_MS;
            advance_reader(app);
        }
        return;
    }
    if (app->auto_enabled) {
        if (app->typed_chars < app->line_chars) {
            return;
        }
        if ((int32_t)(now_ms - app->auto_deadline) >= 0) {
            advance_reader(app);
            app->auto_deadline = now_ms + k_auto_wait_ms[app->auto_speed ? app->auto_speed : 1];
        }
    }
}

// ---------------------------------------------------------------- 初始化

static uint16_t meta_u16(const anem_pack_t *pack, const char *key, uint16_t fallback)
{
    const char *value = NULL;
    uint32_t length = 0;
    if (!anem_pack_meta(pack, key, &value, &length) || value == NULL) {
        return fallback;
    }
    char buffer[16];
    const uint32_t copy = length < sizeof(buffer) - 1 ? length : sizeof(buffer) - 1;
    memcpy(buffer, value, copy);
    buffer[copy] = 0;
    return (uint16_t)strtoul(buffer, NULL, 10);
}

bool anem_app_init(anem_app_t *app, const void *pack_data, size_t pack_size)
{
    if (app == NULL || pack_data == NULL) {
        return false;
    }
    memset(app, 0, sizeof(*app));
    if (!anem_pack_open(&app->pack, pack_data, pack_size)) {
        return false;
    }
    if (!anem_script_init(&app->script, &app->pack)) {
        return false;
    }
    if (!anem_canvas_init(&app->canvas)) {
        return false;
    }
    anem_image_init(&app->image);
    app->draw_art = true;
    app->screen = ANEM_SCR_TITLE;
    app->menu_back = ANEM_SCR_TITLE;
    app->back_screen = ANEM_SCR_TITLE;
    app->text_speed = ANEM_SPEED_NORMAL;
    app->auto_speed = ANEM_AUTO_NORMAL;
    app->panel_alpha = 100;
    app->flow_scene = meta_u16(&app->pack, "flow_scene", 2);
    app->title_image_normal = meta_u16(
        &app->pack, "title_image_normal",
        meta_u16(&app->pack, "title_image", ANEM_NONE16));
    app->title_image_anemoi = meta_u16(
        &app->pack, "title_image_anemoi", app->title_image_normal);
    app->title_image_eternicle = meta_u16(
        &app->pack, "title_image_eternicle", app->title_image_normal);
    app->title_image = app->title_image_normal;
    app->art_image = app->title_image;
    app->title_level = 0;
    anem_script_new(&app->script, app->flow_scene, true);
    return true;
}
