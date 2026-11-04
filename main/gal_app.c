// main/gal_app.c —— 应用状态机实现。
#include "gal_app.h"

#include <stdio.h>
#include <string.h>

#include "bsp_battery.h"
#include "bsp_display.h"
#include "esp_log.h"
#include "gal_text.h"

static const char *TAG = "gal_app";

#define AUTO_MS_SHORT 1200
#define AUTO_MS_MID 1800
#define AUTO_MS_LONG 2800
#define FF_STEP_MS 40
#define TRANSITION_MS 1200
#define AUTOSAVE_MS (90 * 1000)

static const uint32_t k_speed_ms[3] = { 45, 22, 10 };
static const uint16_t k_auto_ms[3] = { AUTO_MS_SHORT, AUTO_MS_MID, AUTO_MS_LONG };
static const uint8_t k_panel_opa[4] = { 55, 70, 85, 95 };

// 前置声明(页面之间互相跳转)。
static void apply_step(gal_app_t *app);
static void open_title(gal_app_t *app);
static void open_menu(gal_app_t *app);
static void open_browse(gal_app_t *app);
static void open_slots(gal_app_t *app, bool saving);
static void open_history(gal_app_t *app);
static void open_chapters(gal_app_t *app);
static void open_gallery(gal_app_t *app, int selected);
static void open_endings(gal_app_t *app);
static void title_refresh(gal_app_t *app);
static void settings_refresh(gal_app_t *app);
static bool load_slot(gal_app_t *app, int slot);

static const char *chapter_name(const gal_app_t *app, uint16_t chapter)
{
    if (chapter >= app->pack.chapter_count) return "";
    return gal_pack_name(&app->pack, app->pack.chapters[chapter].name);
}

static void unlock_touch(gal_app_t *app)
{
    app->unlock_dirty = true;
    app->unlock_try_ms = 0;
    app->unlock_backoff_ms = 0;
}

static void unlock_flush(gal_app_t *app, bool force)
{
    if (!app->unlock_dirty) return;
    const uint32_t now = app->now_ms;
    if (!force && app->unlock_try_ms && (int32_t)(now - app->unlock_try_ms) < 0) return;
    if (!force && (int32_t)(now - app->unlock_try_ms) < (int32_t)app->unlock_backoff_ms) return;
    app->unlock_try_ms = now ? now : 1;
    if (gal_store_save_unlock(&app->unlock)) {
        app->unlock_dirty = false;
        app->unlock_backoff_ms = 0;
    } else {
        // 失败退避:5s -> 10s -> 20s -> 30s 封顶,下个 tick 再试。
        app->unlock_backoff_ms = app->unlock_backoff_ms ? app->unlock_backoff_ms * 2 : 5000;
        if (app->unlock_backoff_ms > 30000) app->unlock_backoff_ms = 30000;
    }
}

static void unlock_by_cg(gal_app_t *app, uint16_t image)
{
    const int idx = gal_gallery_index_for_image(&app->pack, image);
    if (idx >= 0 && !gal_unlock_cg_get(&app->unlock, idx)) {
        gal_unlock_cg_set(&app->unlock, idx);
        unlock_touch(app);
    }
}

// 只有"场景解码 + 全部要求的立绘"都成功,才更新缓存并解锁 CG;
// 失败时保留旧缓存(下次切句会重试)并给一个可见提示。
static void sync_visuals(gal_app_t *app)
{
    const bool cg_mode = app->play.cg != GAL_NONE;
    const bool changed = app->render_invalid || app->play.bg != app->rendered_bg ||
                         app->play.cg != app->rendered_cg ||
                         app->play.spr[0] != app->rendered_spr[0] ||
                         app->play.spr[1] != app->rendered_spr[1] ||
                         app->play.spr[2] != app->rendered_spr[2];
    if (!changed) return;
    const uint16_t scene = cg_mode ? app->play.cg : app->play.bg;
    bool ok = true;
    if (scene != GAL_NONE) {
        ok = gal_image_show(&app->ui.image, &app->pack, scene);
    } else {
        gal_image_clear(&app->ui.image);
    }
    if (ok && !cg_mode) {
        ok = gal_image_draw_sprites_checked(&app->ui.image, &app->pack, app->play.spr) >= 0;
    }
    if (!ok) {
        ESP_LOGE(TAG, "画面解码失败: scene=%u spr=%u/%u/%u", (unsigned)scene,
                 (unsigned)app->play.spr[0], (unsigned)app->play.spr[1],
                 (unsigned)app->play.spr[2]);
        // 失败时画布可能已被写了一半:清成黑屏(不保留半帧),
        // 并把缓存标成"必须重画",下次即使请求同一个 id 也会重新解码。
        gal_image_clear(&app->ui.image);
        app->render_invalid = true;
        gal_ui_toast(&app->ui, "画面解码失败", app->now_ms);
        return;     // rendered_* 保持上一次成功值;CG 不解锁
    }
    app->render_invalid = false;
    if (cg_mode) unlock_by_cg(app, app->play.cg);
    app->rendered_bg = app->play.bg;
    app->rendered_cg = app->play.cg;
    memcpy(app->rendered_spr, app->play.spr, sizeof(app->rendered_spr));
}

// ---------------------------------------------------------------- 正文分页
static void line_show_page(gal_app_t *app)
{
    const char *text = gal_line_text(&app->pack, app->line_id);
    const int units = gal_ui_units_per_line(&app->ui);
    const int lines = gal_ui_lines_per_page(&app->ui);
    uint32_t offsets[GAL_TEXT_MAX_PAGES + 1];
    const int pages = gal_text_pages(text, units, lines, offsets, GAL_TEXT_MAX_PAGES + 1);
    const int page = app->line_page < pages ? app->line_page : pages - 1;
    uint32_t begin = offsets[page];
    uint32_t end = offsets[page + 1];
    if (end < begin) end = begin;
    if (end - begin >= sizeof(app->page_text)) end = begin + sizeof(app->page_text) - 1;
    memcpy(app->page_text, text + begin, end - begin);
    app->page_text[end - begin] = '\0';
    app->line_pages = (uint16_t)pages;
    const char *speaker = gal_pack_name(&app->pack, app->line_speaker);
    // 快进时整页瞬显（0 = 不走打字机）；自动阅读仍逐字。
    gal_ui_set_text(&app->ui, speaker, app->page_text,
                    app->fast_forward ? 0u
                                      : (uint32_t)k_speed_ms[app->settings.text_speed % 3]);
    gal_ui_set_page(&app->ui, page, pages);
}

static void line_setup(gal_app_t *app, uint16_t line_id)
{
    app->line_id = line_id;
    app->line_speaker = app->pack.lines[line_id].speaker;
    app->line_page = 0;
    if (app->history_count < GAL_HISTORY_MAX) {
        app->history[app->history_count++] = line_id;
    } else {
        memmove(app->history, app->history + 1, sizeof(app->history[0]) * (GAL_HISTORY_MAX - 1));
        app->history[GAL_HISTORY_MAX - 1] = line_id;
    }
    line_show_page(app);
}

// ---------------------------------------------------------------- 存档
static void fill_save(gal_app_t *app, gal_save_t *save)
{
    memset(save, 0, sizeof(*save));
    save->pc = app->save_pc;
    save->chapter = app->play.chapter;
    save->bg = app->play.bg;
    save->cg = app->play.cg;
    memcpy(save->spr, app->play.spr, sizeof(save->spr));
    memcpy(save->vars, app->play.vars, sizeof(save->vars));
    save->unlock = app->unlock;
    save->stamp = app->now_ms / 1000u;
    char preview[GAL_SAVE_PREVIEW];
    const char *speaker = gal_pack_name(&app->pack, app->line_speaker);
    const char *text = gal_line_text(&app->pack, app->line_id);
    if (speaker && *speaker) snprintf(preview, sizeof(preview), "%s：%s", speaker, text);
    else snprintf(preview, sizeof(preview), "%s", text);
    gal_utf8_truncate(save->preview, preview, GAL_SAVE_PREVIEW - 1);
}

static void autosave(gal_app_t *app)
{
    if (!app->started) return;
    gal_save_t save;
    fill_save(app, &save);
    if (gal_store_save_slot(GAL_SLOT_AUTO, &save)) app->have_save = true;
    app->autosave_accum_ms = 0;
}

static bool load_slot(gal_app_t *app, int slot)
{
    gal_save_t save;
    if (!gal_store_load_slot(slot, &save)) return false;
    if (save.pc >= app->pack.event_count) {
        ESP_LOGW(TAG, "存档 %d 的 pc 越界: %u", slot, (unsigned)save.pc);
        return false;
    }
    const gal_event_t *ev = &app->pack.events[save.pc];
    if (ev->kind != GAL_EV_LINE && ev->kind != GAL_EV_CHOICE) {
        ESP_LOGW(TAG, "存档 %d 停在不可恢复的事件: kind=%u", slot, (unsigned)ev->kind);
        return false;
    }
    if (save.chapter >= app->pack.chapter_count) return false;
    if (save.bg != GAL_NONE && save.bg >= app->pack.image_count) return false;
    if (save.cg != GAL_NONE && save.cg >= app->pack.image_count) return false;
    for (int i = 0; i < 3; ++i) {
        if (save.spr[i] != GAL_NONE && save.spr[i] >= app->pack.image_count) return false;
    }
    bool preview_ok = false;
    for (size_t i = 0; i < sizeof(save.preview); ++i) {
        if (save.preview[i] == 0) { preview_ok = true; break; }
    }
    if (!preview_ok) return false;
    gal_play_reset(&app->play);
    app->play.pc = save.pc;
    app->play.chapter = save.chapter < app->pack.chapter_count ? save.chapter : 0;
    app->play.bg = save.bg;
    app->play.cg = save.cg;
    memcpy(app->play.spr, save.spr, sizeof(save.spr));
    memcpy(app->play.vars, save.vars, sizeof(save.vars));
    // 全局解锁只合并不回退:读旧档不能把后来解锁的 CG/章节/结局抹掉。
    // 只有真的新增了位才标脏;完全相同的解锁不会触发无意义写盘。
    {
        const gal_unlock_t before = app->unlock;
        gal_unlock_merge(&app->unlock, &save.unlock);
        if (memcmp(&before, &app->unlock, sizeof(before)) != 0) {
            unlock_touch(app);
        }
    }
    app->started = true;
    app->history_count = 0;
    app->line_page = 0;
    app->ending_active = false;
    app->transition_active = false;
    app->choice_pending = false;
    gal_ui_hide_choice(&app->ui);
    gal_ui_hide_transition(&app->ui);
    gal_ui_show_page(&app->ui, GAL_PAGE_GAME);
    app->page = GAL_PAGE_GAME;
    gal_ui_set_chapter(&app->ui, chapter_name(app, app->play.chapter));
    return true;
}

// ---------------------------------------------------------------- 步骤
static void show_transition(gal_app_t *app, uint16_t chapter)
{
    gal_ui_set_transition(&app->ui, chapter_name(app, chapter), "章节");
    app->transition_active = true;
    app->transition_until = app->now_ms + TRANSITION_MS;
}

static void show_ending(gal_app_t *app, uint16_t ending_id)
{
    const gal_ending_t *e = &app->pack.endings[ending_id];
    const char *name = gal_pack_name(&app->pack, e->name);
    const char *text = (e->text != GAL_NONE) ? gal_pack_name(&app->pack, e->text) : "";
    gal_ui_set_ending(&app->ui, (e->kind == 0) ? "END" : "ROUTE", name, text);
    gal_ui_show_page(&app->ui, GAL_PAGE_ENDING);
    app->page = GAL_PAGE_ENDING;
    app->ending_active = true;
    app->auto_mode = false;
    app->fast_forward = false;
    gal_ui_set_mode(&app->ui, false);
    autosave(app);
}

static void apply_step(gal_app_t *app)
{
    for (int guard = 0; guard < 64; ++guard) {
        const gal_step_t step = gal_play_next(&app->play, &app->pack);
        switch (step.kind) {
        case GAL_STEP_LINE:
            app->save_pc = (uint16_t)(app->play.pc - 1);
            sync_visuals(app);
            line_setup(app, step.index);
            return;
        case GAL_STEP_CHOICE: {
            app->choice_group = step.index;
            app->choice_count = gal_choice_visible(&app->pack, &app->play, step.index,
                                                   app->choice_option, 5);
            for (int i = 0; i < app->choice_count; ++i) app->choice_enabled[i] = 1;
            if (app->choice_count == 0) {
                ESP_LOGE(TAG, "选项组 %u 没有可用选项", (unsigned)step.index);
                return;
            }
            app->choice_sel = 0;
            app->save_pc = (uint16_t)(app->play.pc - 1);
            app->choice_pending = true;
            sync_visuals(app);
            gal_ui_set_choice(&app->ui, &app->pack, app->choice_option, app->choice_count, 0);
            return;
        }
        case GAL_STEP_CHAPTER:
            if (!gal_unlock_chapter_get(&app->unlock, (int)step.index)) {
                gal_unlock_chapter_set(&app->unlock, (int)step.index);
                unlock_touch(app);
            }
            gal_ui_set_chapter(&app->ui, chapter_name(app, step.index));
            sync_visuals(app);
            show_transition(app, step.index);
            autosave(app);
            return;
        case GAL_STEP_ENDING:
            if (!gal_unlock_ending_get(&app->unlock, (int)step.index)) {
                gal_unlock_ending_set(&app->unlock, (int)step.index);
                unlock_touch(app);
            }
            sync_visuals(app);
            show_ending(app, step.index);
            return;
        case GAL_STEP_UNLOCK_END:
            if (!gal_unlock_ending_get(&app->unlock, (int)step.index)) {
                gal_unlock_ending_set(&app->unlock, (int)step.index);
                unlock_touch(app);
            }
            continue;
        case GAL_STEP_END:
            gal_ui_toast(&app->ui, "已读完", app->now_ms);
            autosave(app);
            gal_ui_show_page(&app->ui, GAL_PAGE_TITLE);
            app->page = GAL_PAGE_TITLE;
            app->title_sel = 0;
            return;
        case GAL_STEP_ERROR:
        default:
            ESP_LOGE(TAG, "剧情数据异常,停在 pc=%u", (unsigned)app->play.pc);
            gal_ui_toast(&app->ui, "剧情数据异常", app->now_ms);
            return;
        }
    }
    ESP_LOGE(TAG, "单次推进超过上限,停在 pc=%u", (unsigned)app->play.pc);
}

static void advance(gal_app_t *app)
{
    if (app->page != GAL_PAGE_GAME || app->transition_active) return;
    if (app->ending_active) return;
    // 选项未选时绝不能"自动翻页":pc 已经越过 CHOICE 事件,继续推进会走错分支。
    if (app->choice_pending) return;
    if (gal_ui_typing(&app->ui)) {
        gal_ui_show_full_text(&app->ui);
        return;
    }
    if (app->line_page + 1 < app->line_pages) {
        app->line_page++;
        line_show_page(app);
        return;
    }
    apply_step(app);
}

// ---------------------------------------------------------------- 页面
static void settings_refresh(gal_app_t *app)
{
    static const char *const speed[] = { "慢", "标准", "快" };
    static const char *const font[] = { "小 16px", "大 20px" };
    static const char *const autos[] = { "短", "中", "长" };
    static const char *const opa[] = { "55%", "70%", "85%", "95%" };
    const char *labels[] = { "文字速度", "字号", "自动间隔", "对话框透明度" };
    const char *values[] = { speed[app->settings.text_speed % 3], font[app->settings.font_large & 1],
                             autos[app->settings.auto_delay % 3],
                             opa[app->settings.panel_opa & 3] };
    gal_ui_set_list(&app->ui, &app->ui.settings, GAL_PAGE_SETTINGS, "设置", labels, values, 4,
                    app->settings_sel, 30);
}

static void title_refresh(gal_app_t *app)
{
    static const char *const items[] = { "继续游戏", "开始新游戏", "读取存档", "鉴赏室", "设置" };
    app->have_save = gal_store_any_slot();
    for (int i = 0; i < 5; ++i) {
        char buf[64];
        const bool enabled = (i != 0) || app->have_save;
        snprintf(buf, sizeof(buf), "%s%s", (i == app->title_sel) ? "▶ " : "  ", items[i]);
        lv_label_set_text(app->ui.title_menu_text[i], buf);
        lv_obj_set_style_text_color(app->ui.title_menu_text[i],
                                    lv_color_hex(enabled ? ((i == app->title_sel) ? 0x7FD1A8 : 0xEDEFF2)
                                                         : 0x6B7280),
                                    LV_PART_MAIN);
    }
}

static void open_title(gal_app_t *app)
{
    app->title_sel = 0;     // 回到标题时从第一项开始
    gal_ui_show_page(&app->ui, GAL_PAGE_TITLE);
    app->page = GAL_PAGE_TITLE;
    app->transition_active = false;
    gal_ui_hide_transition(&app->ui);
    gal_image_show(&app->ui.image, &app->pack, app->pack.title_image);
    app->rendered_bg = app->pack.title_image;
    app->rendered_cg = GAL_NONE;
    for (int i = 0; i < 3; ++i) app->rendered_spr[i] = GAL_NONE;
    gal_ui_set_mode(&app->ui, false);
    app->auto_mode = false;
    app->fast_forward = false;
    title_refresh(app);
}

// 只重建列表内容,不改来源页(保存后刷新、覆盖确认后刷新都用它)。
static void slots_refresh(gal_app_t *app, bool saving)
{
    static const char *labels[GAL_SLOT_COUNT] = { "存档 1", "存档 2", "存档 3", "自动存档" };
    const char *values[GAL_SLOT_COUNT] = { "", "", "", "" };
    static char storage[GAL_SLOT_COUNT][GAL_SAVE_PREVIEW + 8];
    for (int i = 0; i < GAL_SLOT_COUNT; ++i) {
        gal_save_t save;
        if (gal_store_load_slot(i, &save)) {
            snprintf(storage[i], sizeof(storage[i]), "%s", save.preview);
            values[i] = storage[i];
        } else {
            values[i] = "空";
        }
    }
    app->slots_saving = saving;
    // 保留玩家当前光标（保存成功后不能跳回第 1 槽，否则再按 OK 会弹错槽位的覆盖确认）。
    // 越界/首次进入时归零；"进入页面"的归零由 open_slots() 负责。
    if (app->slots_sel < 0 || app->slots_sel >= GAL_SLOT_COUNT) app->slots_sel = 0;
    gal_ui_set_list(&app->ui, &app->ui.slots, GAL_PAGE_SLOTS,
                    saving ? "保存进度" : "读取进度", labels, values, GAL_SLOT_COUNT,
                    app->slots_sel, 30);
    gal_ui_list_select(&app->ui, &app->ui.slots, app->slots_sel);
}

// 进入存档/读档页:记录来源页,再刷新并显示。
static void open_slots(gal_app_t *app, bool saving)
{
    app->return_page = app->page;
    app->slots_sel = 0;      // 进入存档/读档页从第 1 槽开始；页内刷新保留光标
    slots_refresh(app, saving);
    gal_ui_show_page(&app->ui, GAL_PAGE_SLOTS);
    app->page = GAL_PAGE_SLOTS;
}

static void open_menu(gal_app_t *app)
{
    static const char *const labels[] = { "返回剧情", "保存进度", "读取进度", "历史文本", "设置",
                                          "回到标题" };
    app->menu_sel = 0;
    gal_ui_set_list(&app->ui, &app->ui.menu, GAL_PAGE_MENU, "游戏菜单", labels, NULL, 6, 0, 26);
    gal_ui_show_page(&app->ui, GAL_PAGE_MENU);
    app->page = GAL_PAGE_MENU;
}

static void open_history(gal_app_t *app)
{
    app->return_page = app->page;
    gal_ui_show_page(&app->ui, GAL_PAGE_HISTORY);
    gal_ui_set_history(&app->ui, &app->pack, app->history, app->history_count);
    app->page = GAL_PAGE_HISTORY;
}

static void open_chapters(gal_app_t *app)
{
    app->return_page = app->page;
    gal_ui_show_page(&app->ui, GAL_PAGE_CHAPTERS);
    gal_ui_set_chapters(&app->ui, &app->pack, &app->unlock, app->play.chapter);
    app->page = GAL_PAGE_CHAPTERS;
}

static void open_gallery(gal_app_t *app, int selected)
{
    app->return_page = app->page;
    app->gallery_page = selected / GAL_GALLERY_PER_PAGE;
    app->gallery_sel = selected;
    gal_ui_set_gallery(&app->ui, &app->pack, &app->unlock, app->gallery_page, selected);
    gal_ui_show_page(&app->ui, GAL_PAGE_GALLERY);
    app->page = GAL_PAGE_GALLERY;
}

static void open_cgview(gal_app_t *app, int index)
{
    if (index < 0 || index >= (int)app->pack.gallery_count) return;
    if (!gal_unlock_cg_get(&app->unlock, index)) return;
    const gal_gallery_t *g = &app->pack.gallery[index];
    if (!gal_image_show(&app->ui.image, &app->pack, g->image)) {
        gal_image_clear(&app->ui.image);
        app->render_invalid = true;
        gal_ui_toast(&app->ui, "画面解码失败", app->now_ms);
        return;
    }
    app->rendered_bg = g->image;
    app->rendered_cg = g->image;
    for (int i = 0; i < 3; ++i) app->rendered_spr[i] = GAL_NONE;
    gal_ui_set_cg_caption(&app->ui, gal_pack_name(&app->pack, g->name));
    gal_ui_show_page(&app->ui, GAL_PAGE_CGVIEW);
    app->page = GAL_PAGE_CGVIEW;
    app->gallery_sel = index;
}

static void start_new_game(gal_app_t *app)
{
    gal_play_reset(&app->play);
    app->started = true;
    app->history_count = 0;
    app->line_page = 0;
    app->line_pages = 1;
    app->save_pc = 0;
    app->ending_active = false;
    app->transition_active = false;
    app->choice_pending = false;
    app->auto_mode = false;
    app->fast_forward = false;
    gal_ui_hide_choice(&app->ui);
    app->rendered_bg = GAL_NONE;
    app->rendered_cg = GAL_NONE;
    for (int i = 0; i < 3; ++i) app->rendered_spr[i] = GAL_NONE;
    gal_ui_hide_transition(&app->ui);
    gal_ui_show_page(&app->ui, GAL_PAGE_GAME);
    app->page = GAL_PAGE_GAME;
    gal_ui_set_chapter(&app->ui, "");
    apply_step(app);
}

// ---------------------------------------------------------------- 按键
static void handle_title(gal_app_t *app, const gal_key_t *key)
{
    if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_UP) {
        app->title_sel = (app->title_sel + 4) % 5;
        title_refresh(app);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_DOWN) {
        app->title_sel = (app->title_sel + 1) % 5;
        title_refresh(app);
        return;
    }
    if (key->ev != BSP_BTN_CLICK || key->btn != BSP_BTN_OK) return;
    switch (app->title_sel) {
    case 0:
        if (load_slot(app, GAL_SLOT_AUTO) || load_slot(app, 0)) {
            apply_step(app);
        } else {
            gal_ui_toast(&app->ui, "没有可继续的存档", app->now_ms);
        }
        break;
    case 1:
        start_new_game(app);
        break;
    case 2:
        open_slots(app, false);
        break;
    case 3:
        open_browse(app);
        break;
    case 4:
        app->return_page = GAL_PAGE_TITLE;
        app->settings_sel = 0;
        settings_refresh(app);
        gal_ui_show_page(&app->ui, GAL_PAGE_SETTINGS);
        app->page = GAL_PAGE_SETTINGS;
        break;
    default:
        break;
    }
}

static void handle_game(gal_app_t *app, const gal_key_t *key)
{
    if (key->btn == BSP_BTN_UP) {
        if (key->ev == BSP_BTN_PRESS) {
            app->suppress_next_click = false;
            if (!app->transition_active) {
                advance(app);
                // 同一次点按的尾部 CLICK 不能落进刚生成的选项页/结局页。
                if (app->choice_pending || app->ending_active || app->page != GAL_PAGE_GAME) {
                    app->suppress_next_click = true;
                }
            }
            return;
        }
        if (key->ev == BSP_BTN_LONG) {
            app->fast_forward = true;
            app->ff_accum_ms = 0;
            if (gal_ui_typing(&app->ui)) gal_ui_show_full_text(&app->ui);
            return;
        }
        if (key->ev == BSP_BTN_RELEASE) {
            app->fast_forward = false;
            return;
        }
        return;
    }
    // 5) 长按 DOWN：开/关自动阅读（自动期间 idle=0，不熄屏不深睡）。
    if (key->btn == BSP_BTN_DOWN && key->ev == BSP_BTN_LONG) {
        app->auto_mode = !app->auto_mode;
        app->auto_accum_ms = 0;
        app->fast_forward = false;
        gal_ui_set_mode(&app->ui, app->auto_mode);
        return;
    }
    if (key->btn == BSP_BTN_DOWN && key->ev == BSP_BTN_CLICK) {
        open_history(app);
        return;
    }
    // 4) 长按 OK = 菜单；短按 OK = 下一句（与 UP 同义）。
    if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_LONG) {
        app->fast_forward = false;
        open_menu(app);
        app->suppress_next_click = true;   // 吞掉驱动在 LONG 后补发的 CLICK
        return;
    }
    if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_CLICK) {
        app->fast_forward = false;
        if (!app->transition_active) {
            advance(app);
            if (app->choice_pending || app->ending_active || app->page != GAL_PAGE_GAME) {
                app->suppress_next_click = true;
            }
        }
        return;
    }
}

static void handle_choice(gal_app_t *app, const gal_key_t *key)
{
    if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_UP) {
        app->choice_sel = (app->choice_sel + app->choice_count - 1) % app->choice_count;
        gal_ui_set_choice(&app->ui, &app->pack, app->choice_option, app->choice_count,
                          app->choice_sel);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_DOWN) {
        app->choice_sel = (app->choice_sel + 1) % app->choice_count;
        gal_ui_set_choice(&app->ui, &app->pack, app->choice_option, app->choice_count,
                          app->choice_sel);
        return;
    }
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        // 选择点也允许打开菜单(手动存档/读档/设置);返回时选项仍然可见可选。
        open_menu(app);
        return;
    }
    if (key->ev != BSP_BTN_CLICK || key->btn != BSP_BTN_OK) return;
    if (app->choice_sel < 0 || app->choice_sel >= app->choice_count) return;
    // choice_option[] 存的是全局 options 下标(显示用);模型要的是组内序号,
    // 由 gal_choice_pick 统一换算,避免把全局下标当组内下标。
    if (!gal_choice_pick(&app->play, &app->pack, app->choice_group, app->choice_sel)) {
        gal_ui_toast(&app->ui, "条件不足", app->now_ms);
        return;
    }
    app->choice_pending = false;
    gal_ui_hide_choice(&app->ui);
    apply_step(app);
}

static void settings_apply(gal_app_t *app)
{
    gal_ui_set_large_font(&app->ui, app->settings.font_large != 0);
    gal_ui_apply_panel_alpha(&app->ui, k_panel_opa[app->settings.panel_opa & 3]);
    gal_ui_set_mode(&app->ui, app->auto_mode);
    gal_store_save_settings(&app->settings);
}

static void handle_settings(gal_app_t *app, const gal_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        if (app->return_page == GAL_PAGE_MENU) {
            app->page = GAL_PAGE_MENU;
            open_menu(app);
        } else {
            gal_ui_show_page(&app->ui, GAL_PAGE_TITLE);
            app->page = GAL_PAGE_TITLE;
            title_refresh(app);
        }
        return;
    }
    if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_UP) {
        app->settings_sel = (app->settings_sel + 3) % 4;
        settings_refresh(app);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_DOWN) {
        app->settings_sel = (app->settings_sel + 1) % 4;
        settings_refresh(app);
        return;
    }
    if (key->ev != BSP_BTN_CLICK || key->btn != BSP_BTN_OK) return;
    switch (app->settings_sel) {
    case 0: app->settings.text_speed = (uint8_t)((app->settings.text_speed + 1) % 3); break;
    case 1: app->settings.font_large = (uint8_t)(app->settings.font_large ^ 1); break;
    case 2: app->settings.auto_delay = (uint8_t)((app->settings.auto_delay + 1) % 3); break;
    case 3: app->settings.panel_opa = (uint8_t)((app->settings.panel_opa + 1) % 4); break;
    default: break;
    }
    settings_apply(app);
    settings_refresh(app);
    if (app->settings_sel == 1) {
        // 字号变化会影响当前页的分页,按新字号重排。
        app->line_page = 0;
        line_show_page(app);
    }
}

static void handle_slots(gal_app_t *app, const gal_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        if (app->return_page == GAL_PAGE_MENU) open_menu(app);
        else open_title(app);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_UP) {
        app->slots_sel = (app->slots_sel + GAL_SLOT_COUNT - 1) % GAL_SLOT_COUNT;
        gal_ui_list_select(&app->ui, &app->ui.slots, app->slots_sel);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_DOWN) {
        app->slots_sel = (app->slots_sel + 1) % GAL_SLOT_COUNT;
        gal_ui_list_select(&app->ui, &app->ui.slots, app->slots_sel);
        return;
    }
    if (key->ev != BSP_BTN_CLICK || key->btn != BSP_BTN_OK) return;
    if (app->slots_saving) {
        if (app->slots_sel == GAL_SLOT_AUTO) {
            gal_ui_toast(&app->ui, "自动存档不可手动覆盖", app->now_ms);
            return;
        }
        gal_save_t existing;
        if (gal_store_load_slot(app->slots_sel, &existing)) {
            // 覆盖已有存档:先二次确认,避免误按 OK 丢档。
            char text[64];
            snprintf(text, sizeof(text), "覆盖存档 %d？", app->slots_sel + 1);
            app->confirm_active = true;
            app->confirm_sel = 0;
            app->confirm_slot = app->slots_sel;
            gal_ui_show_confirm(&app->ui, text, 0);
            return;
        }
        gal_save_t save;
        fill_save(app, &save);
        if (gal_store_save_slot(app->slots_sel, &save)) {
            gal_ui_toast(&app->ui, "已保存", app->now_ms);
            app->have_save = true;
            slots_refresh(app, true);
        } else {
            gal_ui_toast(&app->ui, "保存失败", app->now_ms);
        }
        return;
    }
    if (load_slot(app, app->slots_sel)) {
        app->page = GAL_PAGE_GAME;
        apply_step(app);
    } else {
        gal_ui_toast(&app->ui, "该槽位没有存档", app->now_ms);
    }
}

static void handle_menu(gal_app_t *app, const gal_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        gal_ui_show_page(&app->ui, GAL_PAGE_GAME);
        app->page = GAL_PAGE_GAME;
        return;
    }
    if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_UP) {
        app->menu_sel = (app->menu_sel + 5) % 6;
        // 只改下标不重绘 = 实机"菜单上下没反应"。选中的高亮/文字必须跟着刷。
        gal_ui_list_select(&app->ui, &app->ui.menu, app->menu_sel);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_DOWN) {
        app->menu_sel = (app->menu_sel + 1) % 6;
        gal_ui_list_select(&app->ui, &app->ui.menu, app->menu_sel);
        return;
    }
    if (key->ev != BSP_BTN_CLICK || key->btn != BSP_BTN_OK) return;
    switch (app->menu_sel) {
    case 0: gal_ui_show_page(&app->ui, GAL_PAGE_GAME); app->page = GAL_PAGE_GAME; break;
    case 1: open_slots(app, true); break;
    case 2: open_slots(app, false); break;
    case 3: open_history(app); break;
    case 4:
        app->return_page = GAL_PAGE_MENU;
        app->settings_sel = 0;
        settings_refresh(app);
        gal_ui_show_page(&app->ui, GAL_PAGE_SETTINGS);
        app->page = GAL_PAGE_SETTINGS;
        break;
    case 5: autosave(app); open_title(app); break;
    default: break;
    }
}

static void open_browse(gal_app_t *app)
{
    app->return_page = app->page;
    static const char *const labels[] = { "CG 鉴赏", "章节一览", "结局一览" };
    app->browse_sel = 0;
    gal_ui_set_list(&app->ui, &app->ui.browse, GAL_PAGE_BROWSE, "鉴赏室", labels, NULL, 3, 0, 30);
    gal_ui_show_page(&app->ui, GAL_PAGE_BROWSE);
    app->page = GAL_PAGE_BROWSE;
}

static void open_endings(gal_app_t *app)
{
    app->return_page = app->page;
    static const char *labels[8];
    const char *values[8];
    static char storage[8][16];
    const int count = (int)app->pack.ending_count;
    for (int i = 0; i < count && i < 8; ++i) {
        labels[i] = gal_pack_name(&app->pack, app->pack.endings[i].name);
        const bool open = gal_unlock_ending_get(&app->unlock, i);
        snprintf(storage[i], sizeof(storage[i]), "%s", open ? "已解锁" : "未解锁");
        values[i] = storage[i];
    }
    gal_ui_set_list(&app->ui, &app->ui.endings_list, GAL_PAGE_ENDINGS, "结局一览", labels, values,
                    count > 8 ? 8 : count, 0, 28);
    gal_ui_show_page(&app->ui, GAL_PAGE_ENDINGS);
    app->page = GAL_PAGE_ENDINGS;
}

static void handle_browse(gal_app_t *app, const gal_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        gal_ui_show_page(&app->ui, GAL_PAGE_TITLE);
        app->page = GAL_PAGE_TITLE;
        title_refresh(app);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        app->browse_sel = (app->browse_sel + (key->btn == BSP_BTN_UP ? 2 : 1)) % 3;
        gal_ui_list_select(&app->ui, &app->ui.browse, app->browse_sel);
        return;
    }
    if (key->ev != BSP_BTN_CLICK || key->btn != BSP_BTN_OK) return;
    if (app->browse_sel == 0) open_gallery(app, 0);
    else if (app->browse_sel == 1) open_chapters(app);
    else open_endings(app);
}

static void handle_gallery(gal_app_t *app, const gal_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        open_browse(app);
        return;
    }
    const int total = (int)app->pack.gallery_count;
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        int sel = app->gallery_sel + (key->btn == BSP_BTN_UP ? -1 : 1);
        if (sel < 0) sel = total - 1;
        if (sel >= total) sel = 0;
        const int page = sel / GAL_GALLERY_PER_PAGE;
        app->gallery_sel = sel;
        if (page != app->gallery_page) {
            app->gallery_page = page;
            gal_ui_set_gallery(&app->ui, &app->pack, &app->unlock, page, sel);
        } else {
            gal_ui_gallery_select(&app->ui, sel);
        }
        return;
    }
    if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_OK) {
        if (gal_unlock_cg_get(&app->unlock, app->gallery_sel)) open_cgview(app, app->gallery_sel);
        else gal_ui_toast(&app->ui, "尚未解锁", app->now_ms);
    }
}

static void handle_cgview(gal_app_t *app, const gal_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        open_gallery(app, app->gallery_sel);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_OK) {
        open_gallery(app, app->gallery_sel);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        const int step = (key->btn == BSP_BTN_UP) ? -1 : 1;
        int idx = app->gallery_sel;
        for (int i = 0; i < (int)app->pack.gallery_count; ++i) {
            idx += step;
            if (idx < 0) idx = (int)app->pack.gallery_count - 1;
            if (idx >= (int)app->pack.gallery_count) idx = 0;
            if (gal_unlock_cg_get(&app->unlock, idx)) break;
        }
        open_cgview(app, idx);
    }
}

static void handle_simple_back(gal_app_t *app, const gal_key_t *key)
{
    if ((key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_OK) ||
        (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK)) {
        if (app->page == GAL_PAGE_HISTORY) {
            if (app->return_page == GAL_PAGE_MENU) open_menu(app);
            else {
                gal_ui_show_page(&app->ui, GAL_PAGE_GAME);
                app->page = GAL_PAGE_GAME;
            }
        } else {
            open_browse(app);
        }
        return;
    }
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        const int delta = (key->btn == BSP_BTN_UP) ? -1 : 1;
        if (app->page == GAL_PAGE_HISTORY) gal_ui_history_scroll(&app->ui, delta);
        else gal_ui_chapters_scroll(&app->ui, delta);
    }
}

void gal_app_key(gal_app_t *app, const gal_key_t *key)
{
    if (!app || !key) return;
    app->idle_ms = 0;
    if (!bsp_lvgl_lock(-1)) return;
    // 新的按键手势开始:上一次点按的尾部 CLICK 若还没来,就作废。
    if (key->ev == BSP_BTN_PRESS) app->suppress_next_click = false;
    if (key->ev == BSP_BTN_CLICK && app->suppress_next_click) {
        app->suppress_next_click = false;
        bsp_lvgl_unlock();
        return;
    }
    // 除了 UP 自己的按下/长按/抬起,其它任何按键都立刻取消快进。
    const bool up_gesture = (key->btn == BSP_BTN_UP &&
                             (key->ev == BSP_BTN_PRESS || key->ev == BSP_BTN_LONG ||
                              key->ev == BSP_BTN_RELEASE));
    if (app->fast_forward && !up_gesture) {
        app->fast_forward = false;
        app->ff_accum_ms = 0;
    }
    // 快进只在"按住 UP"期间有效:任何页面、任何来源的 UP 抬起都立刻停。
    if (key->ev == BSP_BTN_RELEASE && key->btn == BSP_BTN_UP) {
        app->fast_forward = false;
        app->ff_accum_ms = 0;
        bsp_lvgl_unlock();
        return;
    }
    // 过场画面:任意键立即结束过场。
    if (app->transition_active && key->ev == BSP_BTN_CLICK) {
        app->transition_active = false;
        gal_ui_hide_transition(&app->ui);
        bsp_lvgl_unlock();
        return;
    }
    if (app->confirm_active) {
        if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
            app->confirm_sel ^= 1;
            gal_ui_confirm_select(&app->ui, app->confirm_sel);
        } else if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
            app->confirm_active = false;
            gal_ui_hide_confirm(&app->ui);
        } else if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_OK) {
            app->confirm_active = false;
            gal_ui_hide_confirm(&app->ui);
            if (app->confirm_sel == 1) {
                gal_save_t save;
                fill_save(app, &save);
                if (gal_store_save_slot(app->confirm_slot, &save)) {
                    app->have_save = true;
                    slots_refresh(app, true);
                    gal_ui_toast(&app->ui, "已覆盖", app->now_ms);
                } else {
                    gal_ui_toast(&app->ui, "保存失败", app->now_ms);
                }
            } else {
                slots_refresh(app, true);
                gal_ui_show_page(&app->ui, GAL_PAGE_SLOTS);
                app->page = GAL_PAGE_SLOTS;
            }
        }
        bsp_lvgl_unlock();
        return;
    }
    if (app->page == GAL_PAGE_GAME && app->choice_count > 0 &&
        app->ui.choice_box && !lv_obj_has_flag(app->ui.choice_box, LV_OBJ_FLAG_HIDDEN)) {
        handle_choice(app, key);
        bsp_lvgl_unlock();
        return;
    }
    switch (app->page) {
    case GAL_PAGE_TITLE: handle_title(app, key); break;
    case GAL_PAGE_GAME: handle_game(app, key); break;
    case GAL_PAGE_MENU: handle_menu(app, key); break;
    case GAL_PAGE_SLOTS: handle_slots(app, key); break;
    case GAL_PAGE_SETTINGS: handle_settings(app, key); break;
    case GAL_PAGE_HISTORY:
    case GAL_PAGE_CHAPTERS: handle_simple_back(app, key); break;
    case GAL_PAGE_BROWSE: handle_browse(app, key); break;
    case GAL_PAGE_ENDINGS: handle_simple_back(app, key); break;
    case GAL_PAGE_GALLERY: handle_gallery(app, key); break;
    case GAL_PAGE_CGVIEW: handle_cgview(app, key); break;
    case GAL_PAGE_ENDING:
        if (key->ev == BSP_BTN_CLICK || key->ev == BSP_BTN_LONG) {
            app->ending_active = false;
            open_title(app);
        }
        break;
    default: break;
    }
    bsp_lvgl_unlock();
}

void gal_app_tick(gal_app_t *app, uint32_t elapsed_ms)
{
    if (!app) return;
    app->now_ms += elapsed_ms;
    if (!bsp_lvgl_lock(0)) return;
    if (app->transition_active && (int32_t)(app->now_ms - app->transition_until) >= 0) {
        app->transition_active = false;
        gal_ui_hide_transition(&app->ui);
    }
    if (app->page == GAL_PAGE_GAME && !app->transition_active && !app->ending_active) {
        if (app->fast_forward) {
            app->ff_accum_ms += elapsed_ms;
            while (app->ff_accum_ms >= FF_STEP_MS) {
                app->ff_accum_ms -= FF_STEP_MS;
                advance(app);
                if (app->page != GAL_PAGE_GAME || app->choice_count > 0) break;
            }
        } else if (app->auto_mode) {
            if (!gal_ui_typing(&app->ui)) {
                app->auto_accum_ms += elapsed_ms;
                if (app->auto_accum_ms >= k_auto_ms[app->settings.auto_delay % 3]) {
                    app->auto_accum_ms = 0;
                    advance(app);
                }
            }
        }
    }
    if (app->auto_mode || app->fast_forward) app->idle_ms = 0;
    else app->idle_ms += elapsed_ms;
    app->battery_accum_ms += elapsed_ms;
    if (app->battery_accum_ms >= 5000) {
        app->battery_accum_ms = 0;
        app->battery = bsp_battery_soc();
        gal_ui_set_battery(&app->ui, app->battery);
    }
    app->autosave_accum_ms += elapsed_ms;
    if (app->started && app->autosave_accum_ms >= AUTOSAVE_MS && app->page == GAL_PAGE_GAME) {
        autosave(app);
    }
    unlock_flush(app, false);
    gal_ui_tick(&app->ui, app->now_ms);
    bsp_lvgl_unlock();
}

uint32_t gal_app_idle_ms(const gal_app_t *app)
{
    return app ? app->idle_ms : 0;
}

void gal_app_flush(gal_app_t *app)
{
    if (!app) return;
    if (!bsp_lvgl_lock(-1)) return;
    if (app->started) autosave(app);
    unlock_flush(app, true);
    bsp_lvgl_unlock();
}

void gal_app_show_sleeping(gal_app_t *app)
{
    if (!app) return;
    if (!bsp_lvgl_lock(-1)) return;
    gal_ui_toast(&app->ui, "休眠中，按任意键唤醒", app->now_ms);
    bsp_lvgl_unlock();
}

bool gal_app_init(gal_app_t *app, const uint8_t *pack_data, uint32_t pack_size,
                  const gal_app_buffers_t *buffers)
{
    if (!app || !pack_data || !buffers) return false;
    memset(app, 0, sizeof(*app));
    const char *err = NULL;
    if (!gal_pack_open(&app->pack, pack_data, pack_size, &err)) {
        ESP_LOGE(TAG, "资源包解析失败: %s", err ? err : "?");
        return false;
    }
    ESP_LOGI(TAG, "资源包: %u 事件 / %u 台词 / %u 章节 / %u 鉴赏 / %u 结局",
             (unsigned)app->pack.event_count, (unsigned)app->pack.line_count,
             (unsigned)app->pack.chapter_count, (unsigned)app->pack.gallery_count,
             (unsigned)app->pack.ending_count);
    if (!gal_store_init()) {
        ESP_LOGW(TAG, "NVS 不可用:本次运行的设置/存档/解锁不会持久化");
    }
    gal_settings_defaults(&app->settings);
    gal_store_load_settings(&app->settings);
    gal_unlock_clear(&app->unlock);
    if (!gal_store_load_unlock(&app->unlock)) {
        // 失败保持全 0 的安全初值;不阻塞启动。
        ESP_LOGI(TAG, "没有可用的全局解锁记录,按未解锁启动");
    }
    gal_play_reset(&app->play);
    app->rendered_bg = GAL_NONE;
    app->rendered_cg = GAL_NONE;
    for (int i = 0; i < 3; ++i) app->rendered_spr[i] = GAL_NONE;
    app->save_pc = 0;
    app->line_pages = 1;

    if (!bsp_lvgl_lock(-1)) return false;
    const bool ok = gal_ui_create(&app->ui, buffers->art_px, buffers->strip_px,
                                  buffers->scratch, buffers->scratch_size,
                                  &gal_cjk_16, &gal_cjk_20);
    if (ok) {
        gal_ui_set_large_font(&app->ui, app->settings.font_large != 0);
        gal_ui_apply_panel_alpha(&app->ui, k_panel_opa[app->settings.panel_opa & 3]);
        lv_label_set_text(app->ui.title_label, "想成为Galgame领域大神！！！");
        lv_label_set_text(app->ui.title_sub, "诺提拉观察所");
        app->page = GAL_PAGE_TITLE;
        ESP_LOGI(TAG, "UI 就绪: 根子对象=%u 标题页子对象=%u 字号大=%d",
                 (unsigned)lv_obj_get_child_cnt(lv_screen_active()),
                 (unsigned)lv_obj_get_child_cnt(app->ui.pages[GAL_PAGE_TITLE]),
                 (int)gal_ui_large_font(&app->ui));
        open_title(app);
    }
    bsp_lvgl_unlock();
    if (!ok) ESP_LOGE(TAG, "界面初始化失败");
    return ok;
}
