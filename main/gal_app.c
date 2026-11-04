// main/gal_app.c —— 应用状态机与真实按键语义。
#include "gal_app.h"

#include "bsp_button.h"
#include "bsp_display.h"
#include "lvgl.h"

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define UI_FONT_PX 16
#define UI_LINE_PX 19
#define UI_TEXT_WIDTH 304
#define UI_TEXT_LINES 3
#define TEXT_SPEED_MS 25
#define FF_MS 30
#define IMAGE_RETRY_MS 2000
#define UNLOCK_RETRY_MS 5000
#define AUTOSAVE_RETRY_MS 5000
#define PROGRESS_RETRY_MS 4000

static const char *const k_title_menu[] = {
    "新游戏", "继续游戏", "读取存档", "鉴赏室", "章节选择", "设置", "关于",
};

static const char *const k_about_page0 =
    "《魔法少女的魔女审判》\n"
    "AI Passport 横屏移植版\n"
    "\n"
    "↑ 下一句（长按快进）\n"
    "↓ 历史（长按自动播放）\n"
    "OK 菜单 / 反驳词\n"
    "长OK 返回上一层\n"
    "选项需 OK 显式确认";

static const char *const k_about_page1 =
    "当前源快照的剧情、背景、\n"
    "CG、立绘与表情已全部打包；\n"
    "画面等比 fit，不裁切构图。\n"
    "16px 完备中文字库。\n"
    "\n"
    "30 个手动存档 + 1 个自动档\n"
    "鉴赏/章节全局解锁独立保存\n"
    "\n"
    "剧本/美术版权归原权利人";

static const char *category_name(unsigned cat)
{
    static const char *names[] = { "证据", "人物", "规则", "笔记", "地图" };
    return cat < 5 ? names[cat] : "记录";
}

static bool app_lock(void)
{
    return bsp_lvgl_lock(100);
}

static void app_unlock(void)
{
    bsp_lvgl_unlock();
}

static void copy_cps(char *dst, size_t cap, const char *src, int cps)
{
    if (!dst || cap == 0) return;
    gal_text_prefix(src ? src : "", cps, dst, cap);
}

static void set_notice(gal_app_t *app, const char *fmt, ...)
{
    if (!app || !fmt) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(app->notice, sizeof(app->notice), fmt, ap);
    va_end(ap);
    if (app->ui_ready) gal_ui_notice(&app->ui, app->notice, (int)app->last_now_ms, 2600);
}

static const char *scene_name(gal_app_t *app, uint16_t scene)
{
    const gal_scene_t *s = gal_pack_scene(app->pack, scene);
    return s ? gal_pack_str(app->pack, (uint16_t)s->name) : "未知章节";
}

static void battery_text(gal_app_t *app, char *buf, size_t cap)
{
    if (app->battery_percent < 0) {
        buf[0] = 0;
    } else {
        snprintf(buf, cap, "%d%%", app->battery_percent);
    }
}

static gal_ui_page_t ui_page_for(app_page_t page)
{
    switch (page) {
    case APP_PAGE_TITLE: return GAL_UI_TITLE;
    case APP_PAGE_GAME: return GAL_UI_GAME;
    case APP_PAGE_CG: return GAL_UI_CG;
    case APP_PAGE_BOOK_DETAIL: return GAL_UI_GAME;
    case APP_PAGE_ENDING:
    case APP_PAGE_ABOUT:
    case APP_PAGE_ERROR: return GAL_UI_TEXT;
    default: return GAL_UI_LIST;
    }
}

static void enter_page(gal_app_t *app, app_page_t page)
{
    app->page = page;
    gal_ui_show(&app->ui, ui_page_for(page));
}

static void render_title(gal_app_t *app);
static void render_game_page(gal_app_t *app);
static void render_choice(gal_app_t *app);
static void render_menu(gal_app_t *app);
static void render_history(gal_app_t *app);
static void render_slots(gal_app_t *app, bool save);
static void render_settings(gal_app_t *app);
static void render_gallery(gal_app_t *app);
static void render_cg(gal_app_t *app);
static void render_routes(gal_app_t *app);
static void render_chapters(gal_app_t *app);
static void render_book(gal_app_t *app);
static void render_book_detail(gal_app_t *app);
static void render_ending(gal_app_t *app);
static void render_about(gal_app_t *app);
static void render_error(gal_app_t *app);
static void render_confirm(gal_app_t *app);
static void read_frame(gal_app_t *app);
static void ensure_page_art(gal_app_t *app);
static bool save_auto(gal_app_t *app, bool force);
static bool save_manual(gal_app_t *app, int slot);
static bool flush_unlock(gal_app_t *app);
static bool flush_progress(gal_app_t *app);

// ---------------------------------------------------------------- 存档摘要
static void set_slot_failure(gal_app_t *app, int i, gal_store_status_t status, const char *err)
{
    app->slots[i].state = status == GAL_STORE_CORRUPT ? 2 : 3;
    snprintf(app->slots[i].title, sizeof(app->slots[i].title), "%s",
             status == GAL_STORE_CORRUPT ? "存档无法读取" : "读取失败");
    snprintf(app->slots[i].preview, sizeof(app->slots[i].preview), "%s",
             status == GAL_STORE_CORRUPT ? (err ? err : "记录损坏") : "NVS 读取失败，请重试");
}

static void refresh_slots(gal_app_t *app)
{
    for (int i = 0; i < GAL_SLOT_COUNT; ++i) {
        app->slots[i].state = 0;
        app->slots[i].title[0] = 0;
        app->slots[i].preview[0] = 0;
        size_t len = 0;
        const gal_store_status_t st = gal_store_read_slot(&app->store, i, app->save_blob,
                                                          sizeof(app->save_blob), &len);
        if (st == GAL_STORE_NOT_FOUND) continue;
        if (st != GAL_STORE_OK) {
            set_slot_failure(app, i, st, NULL);
            continue;
        }
        gal_save_ui_t ui;
        const char *err = NULL;
        if (!gal_save_decode(app->save_blob, len, app->pack, &app->scratch.load_vm, &ui, &err)) {
            set_slot_failure(app, i, GAL_STORE_CORRUPT, err);
            continue;
        }
        app->slots[i].state = 1;
        snprintf(app->slots[i].title, sizeof(app->slots[i].title), "%s",
                 scene_name(app, app->scratch.load_vm.root));
        if (!gal_save_preview_vm(app->pack, &app->scratch.load_vm, &ui,
                                 app->slots[i].preview, sizeof(app->slots[i].preview))) {
            app->slots[i].preview[0] = 0;
        }
    }
    app->slots_loaded = true;
}

static void refresh_auto_summary(gal_app_t *app)
{
    app->auto_valid = false;
    app->auto_status = GAL_STORE_NOT_FOUND;
    size_t len = 0;
    app->auto_status = gal_store_read_slot(&app->store, GAL_SLOT_AUTO, app->save_blob,
                                           sizeof(app->save_blob), &len);
    if (app->auto_status != GAL_STORE_OK) return;
    gal_save_ui_t ui;
    const char *err = NULL;
    if (!gal_save_decode(app->save_blob, len, app->pack, &app->scratch.load_vm, &ui, &err)) {
        app->auto_status = GAL_STORE_CORRUPT;
        return;
    }
    app->auto_valid = true;
}

// ---------------------------------------------------------------- 图像
static void unlock_background(gal_app_t *app)
{
    const gal_imgrec_t *im = gal_pack_image(app->pack, app->vm.background);
    if (!im || im->gallery == GAL_NONE) return;
    const uint16_t g = (uint16_t)im->gallery;
    app->vm.unlock_gallery[g >> 3] |= (uint8_t)(1u << (g & 7));
}

static void image_failed(gal_app_t *app)
{
    app->art_ok = false;
    app->image_dirty = true;
    app->image_retry_ms = app->last_now_ms + IMAGE_RETRY_MS;
    set_notice(app, "图片解码失败，稍后自动重试");
}

static void ensure_title_art(gal_app_t *app)
{
    const uint16_t img = (uint16_t)(app->progress >= 6 ? app->pack->meta[11] : app->pack->meta[10]);
    if (app->art_ok && app->art_bg == img) return;
    const char *err = NULL;
    if (!gal_image_show(&app->image, app->pack, img, &err)) {
        image_failed(app);
        return;
    }
    app->art_bg = img;
    app->art_bodies = 0;
    app->art_ok = true;
    app->image_dirty = false;
}

static void ensure_game_art(gal_app_t *app)
{
    uint32_t h = 2166136261u;
    for (uint8_t i = 0; i < app->vm.body_count; ++i) {
        h = (h ^ app->vm.bodies[i].image) * 16777619u;
    }
    h ^= app->vm.body_count;
    if (app->art_ok && app->art_bg == app->vm.background && app->art_bodies == h) return;
    const char *err = NULL;
    if (!gal_image_show(&app->image, app->pack, app->vm.background, &err)) {
        image_failed(app);
        return;
    }
    const int r = gal_image_draw_bodies(&app->image, app->pack, app->vm.bodies,
                                        app->vm.body_count);
    if (r < 0) {
        image_failed(app);
        return;
    }
    app->art_bg = app->vm.background;
    app->art_bodies = h;
    app->art_ok = true;
    app->image_dirty = false;
    unlock_background(app);
    // 成功解码后才把 CG 解锁位并入全局;失败路径不会走到这里。
    gal_unlock_merge(&app->unlock, app->vm.unlock_gallery, NULL);
}

static void ensure_book_art(gal_app_t *app, uint16_t image)
{
    if (app->art_ok && app->art_bg == image && app->art_bodies == 0xFFFFFFFFu) return;
    const char *err = NULL;
    if (!gal_image_show(&app->image, app->pack, image, &err)) {
        image_failed(app);
        return;
    }
    app->art_bg = image;
    app->art_bodies = 0xFFFFFFFFu;
    app->art_ok = true;
    app->image_dirty = false;
}

static void ensure_cg_art(gal_app_t *app, uint16_t image)
{
    if (app->art_ok && app->art_bg == image && app->art_bodies == 0xFFFFFFFEu) return;
    const char *err = NULL;
    if (!gal_image_show(&app->image, app->pack, image, &err)) {
        image_failed(app);
        return;
    }
    app->art_bg = image;
    app->art_bodies = 0xFFFFFFFEu;
    app->art_ok = true;
    app->image_dirty = false;
}

static void ensure_page_art(gal_app_t *app)
{
    switch (app->page) {
    case APP_PAGE_TITLE: ensure_title_art(app); break;
    case APP_PAGE_GAME:
        if (app->vm_started) ensure_game_art(app);
        break;
    case APP_PAGE_BOOK_DETAIL: {
        const gal_book_t *b = gal_vm_book_entry(&app->vm, (uint16_t)app->book_sel);
        ensure_book_art(app, b ? b->image : GAL_NONE);
        break;
    }
    case APP_PAGE_CG: {
        const gal_gallery_item_t *it = gal_pack_gallery_item(app->pack, (uint16_t)app->cg_variant);
        ensure_cg_art(app, it ? it->image : GAL_NONE);
        break;
    }
    default:
        break;
    }
}

// ---------------------------------------------------------------- 文本
static void history_add(gal_app_t *app)
{
    if (app->history_count > 0) {
        const gal_hist_t *last = &app->history[app->history_count - 1];
        if (last->text_off == app->frame.text_off && last->text_len == app->frame.text_len &&
            last->speaker == app->frame.speaker) {
            return;
        }
    }
    if (app->history_count >= GAL_HISTORY_MAX) {
        memmove(app->history, app->history + 1,
                sizeof(app->history[0]) * (GAL_HISTORY_MAX - 1));
        app->history_count = GAL_HISTORY_MAX - 1;
    }
    app->history[app->history_count].text_off = app->frame.text_off;
    app->history[app->history_count].text_len = app->frame.text_len;
    app->history[app->history_count].speaker = app->frame.speaker;
    app->history_count++;
}

static void typing_apply(gal_app_t *app)
{
    if (!app->ui.text) return;
    gal_text_prefix(app->page_text, app->typing_index, app->shown_text,
                    sizeof(app->shown_text));
    lv_label_set_text(app->ui.text, app->shown_text);
}

static void typing_finish(gal_app_t *app)
{
    app->typing_index = app->typing_total;
    app->typing_active = false;
    app->text_accum_ms = 0;
    snprintf(app->shown_text, sizeof(app->shown_text), "%s", app->page_text);
    if (app->ui.text) lv_label_set_text(app->ui.text, app->shown_text);
}

static void show_text_frame(gal_app_t *app)
{
    const bool new_text = app->text_chapter != app->vm.chapter ||
                          app->text_cursor != app->vm.cursor;
    app->text_chapter = app->vm.chapter;
    app->text_cursor = app->vm.cursor;
    if (new_text) {
        history_add(app);
        app->text_page = 0;
    }
    gal_vm_frame_text(&app->vm, &app->frame, app->text_utf8, sizeof(app->text_utf8));
    int pages = 1;
    if (!gal_text_page(app->text_utf8, app->text_page, UI_TEXT_WIDTH, UI_FONT_PX,
                       UI_TEXT_LINES, app->page_text, sizeof(app->page_text), &pages)) {
        app->text_page = 0;
        pages = 1;
        if (!gal_text_page(app->text_utf8, 0, UI_TEXT_WIDTH, UI_FONT_PX,
                           UI_TEXT_LINES, app->page_text, sizeof(app->page_text), &pages)) {
            app->page_text[0] = 0;
        }
    }
    app->pending_choice_valid = false;
    if (app->pending_page_valid) {
        int want = app->pending_page;
        if (want < 0) want = 0;
        if (want >= pages) want = pages - 1;
        app->text_page = (uint8_t)want;
        app->pending_page_valid = false;
        (void)gal_text_page(app->text_utf8, app->text_page, UI_TEXT_WIDTH, UI_FONT_PX,
                            UI_TEXT_LINES, app->page_text, sizeof(app->page_text), &pages);
    }
    app->text_pages = (uint8_t)(pages < 1 ? 1 : pages);
    app->typing_total = gal_text_cp_count(app->page_text);
    app->typing_index = 0;
    app->typing_active = (app->settings.text_speed != 0 && app->typing_total > 0);
    if (app->typing_active) {
        app->shown_text[0] = 0;
    } else {
        snprintf(app->shown_text, sizeof(app->shown_text), "%s", app->page_text);
    }
    app->manual_boundary = false;
    const gal_scene_t *sc = gal_pack_scene(app->pack, app->vm.chapter);
    if (sc) {
        const gal_op_t *op = gal_pack_op(app->pack, sc->op_start + app->vm.cursor);
        if (op && op->op == GAL_OP_PAUSE) app->manual_boundary = true;
    }
    if (app->frame.link_count) app->manual_boundary = true;
    app->checkpoint_mode = 0;
    app->checkpoint_page = app->text_page;
    app->checkpoint_choice = 0;
    enter_page(app, APP_PAGE_GAME);
    ensure_page_art(app);
    render_game_page(app);
}

static void after_frame(gal_app_t *app)
{
    gal_unlock_merge(&app->unlock, app->vm.unlock_gallery, app->vm.unlock_chapter);
    const int32_t p = gal_vm_var_int(&app->vm, "g_gameProgress");
    if (p > (int32_t)app->progress) {
        app->progress = (uint16_t)p;
        app->progress_dirty = true;
    }
    if (app->vm.autosave_due) {
        app->autosave_due = true;
        app->vm.autosave_due = 0;
    }
    if (app->vm_started && app->vm.root != app->last_auto_root) {
        save_auto(app, true);
    }
}

// ---------------------------------------------------------------- 页面渲染
static void render_title(gal_app_t *app)
{
    char bat[24];
    battery_text(app, bat, sizeof(bat));
    ensure_title_art(app);
    enter_page(app, APP_PAGE_TITLE);
    gal_ui_title(&app->ui, "魔法少女的\n魔女审判", "AI PASSPORT 图文版",
                 k_title_menu, (int)(sizeof(k_title_menu) / sizeof(k_title_menu[0])),
                 app->title_sel, bat);
}

static void render_game_page(gal_app_t *app)
{
    char hp[24];
    char pagebuf[16];
    const int hpv = gal_vm_var_int(&app->vm, "currentHp");
    snprintf(hp, sizeof(hp), "HP %d", hpv);
    if (app->text_pages > 1) {
        snprintf(pagebuf, sizeof(pagebuf), "%d/%d", app->text_page + 1, app->text_pages);
    } else {
        pagebuf[0] = 0;
    }
    const char *speaker = app->frame.speaker == GAL_NONE ? "" :
                          gal_pack_str(app->pack, app->frame.speaker);
    const char *hint = ""; // 阅读页不显示按键提示；提示对象保留为隐藏兼容对象
    char bat[24];
    battery_text(app, bat, sizeof(bat));
    if (app->typing_active) {
        gal_text_prefix(app->page_text, app->typing_index, app->shown_text,
                        sizeof(app->shown_text));
    } else {
        snprintf(app->shown_text, sizeof(app->shown_text), "%s", app->page_text);
    }
    gal_ui_game(&app->ui, speaker, app->shown_text, hp, pagebuf, hint, bat);
}

static void render_choice(gal_app_t *app)
{
    const char *title = "作出选择";
    if (app->objection_mode) title = "选择反驳词";
    else if (app->frame.choice_kind == GAL_ROW_MAP) title = "选择调查地点";
    else if (app->frame.choice_kind == GAL_ROW_EVIDENCE) title = "提交证据";
    else if (app->frame.choice_kind == GAL_ROW_CONTINUE) title = "处刑确认";
    const int first = (app->choice_sel / GAL_UI_LIST_ROWS) * GAL_UI_LIST_ROWS;
    int n = 0;
    for (int i = first; i < app->choice_count && n < GAL_UI_LIST_ROWS; ++i, ++n) {
        app->list_rows[n] = app->choice_rows[i];
    }
    char sub[64];
    snprintf(sub, sizeof(sub), "%d / %d", app->choice_sel + 1, app->choice_count);
    enter_page(app, APP_PAGE_CHOICE);
    gal_ui_list(&app->ui, title, sub, app->list_rows, n,
                app->choice_sel - first, "OK 确认   长OK 菜单");
    const char *choice_speaker = app->frame.speaker == GAL_NONE ? "" :
                                 gal_pack_str(app->pack, app->frame.speaker);
    gal_ui_choice_name(&app->ui, choice_speaker);
    app->checkpoint_mode = app->objection_mode ? 2 : 1;
    app->checkpoint_choice = (uint8_t)app->choice_sel;
}

static void render_menu(gal_app_t *app)
{
    static const char *base[] = {
        "返回游戏", "保存进度", "读取进度", "历史文本", "魔女手册",
        "自动播放", "自动速度", "阅读设置", "返回标题",
    };
    char labels[9][64];
    int count = 9;
    for (int i = 0; i < count; ++i) {
        if (i == 5) snprintf(labels[i], sizeof(labels[i]), "自动播放：%s",
                             app->auto_enabled ? "开" : "关");
        else if (i == 6) snprintf(labels[i], sizeof(labels[i]), "自动速度：%s",
                                  app->settings.auto_speed == 0 ? "慢" :
                                  app->settings.auto_speed == 1 ? "标准" : "快");
        else snprintf(labels[i], sizeof(labels[i]), "%s", base[i]);
    }
    const int first = (app->menu_sel / GAL_UI_LIST_ROWS) * GAL_UI_LIST_ROWS;
    int n = 0;
    for (int i = first; i < count && n < GAL_UI_LIST_ROWS; ++i, ++n) {
        app->list_rows[n].text = labels[i];
        app->list_rows[n].dim = 0;
    }
    char sub[64];
    snprintf(sub, sizeof(sub), "%s", scene_name(app, app->vm.root));
    enter_page(app, APP_PAGE_MENU);
    gal_ui_list(&app->ui, "阅读菜单", sub, app->list_rows, n,
                app->menu_sel - first, "OK 执行   长OK 返回");
}

static void render_history(gal_app_t *app)
{
    const int first = (app->history_sel / GAL_UI_LIST_ROWS) * GAL_UI_LIST_ROWS;
    int n = 0;
    for (int i = first; i < app->history_count && n < GAL_UI_LIST_ROWS; ++i, ++n) {
        char text[256];
        char line[160];
        char speaker[64];
        if (gal_pack_text_utf8(app->pack, app->history[i].text_off,
                               app->history[i].text_len, text, sizeof(text)) < 0) {
            text[0] = 0;
        }
        speaker[0] = 0;
        if (app->history[i].speaker != GAL_NONE) {
            snprintf(speaker, sizeof(speaker), "%s：",
                     gal_pack_str(app->pack, app->history[i].speaker));
        }
        char first_line[120];
        size_t k = 0;
        for (size_t j = 0; text[j] && text[j] != '\n' && k + 1 < sizeof(first_line); ++j) {
            first_line[k++] = text[j];
        }
        first_line[k] = 0;
        copy_cps(line, sizeof(line), first_line, 34);
        snprintf(app->list_text[n], sizeof(app->list_text[n]), "%s%s%s",
                 speaker, line, gal_text_cp_count(first_line) > 34 ? "…" : "");
        app->list_rows[n].text = app->list_text[n];
        app->list_rows[n].dim = 0;
    }
    char sub[64];
    snprintf(sub, sizeof(sub), "%d 条", app->history_count);
    enter_page(app, APP_PAGE_HISTORY);
    gal_ui_list(&app->ui, "历史文本", sub, app->list_rows, n,
                app->history_sel - first, "OK 返回   长OK 返回");
}

static void render_slots(gal_app_t *app, bool save)
{
    const int first = (app->slot_sel / GAL_SLOTS_PER_PAGE) * GAL_SLOTS_PER_PAGE;
    int n = 0;
    for (int i = first; i < GAL_SLOT_COUNT && n < GAL_UI_LIST_ROWS; ++i, ++n) {
        const gal_slot_summary_t *s = &app->slots[i];
        if (s->state == 1) {
            snprintf(app->list_text[n], sizeof(app->list_text[n]), "%02d  %s\n%s",
                     i + 1, s->title, s->preview);
        } else if (s->state == 2) {
            snprintf(app->list_text[n], sizeof(app->list_text[n]), "%02d  存档无法读取", i + 1);
        } else if (s->state == 3) {
            snprintf(app->list_text[n], sizeof(app->list_text[n]), "%02d  读取失败", i + 1);
        } else {
            snprintf(app->list_text[n], sizeof(app->list_text[n]), "%02d  空栏位", i + 1);
        }
        app->list_rows[n].text = app->list_text[n];
        app->list_rows[n].dim = s->state != 1;
    }
    char sub[64];
    snprintf(sub, sizeof(sub), "%d 个栏位 · 第 %d / %d 页", GAL_SLOT_COUNT,
             first / GAL_SLOTS_PER_PAGE + 1,
             (GAL_SLOT_COUNT + GAL_SLOTS_PER_PAGE - 1) / GAL_SLOTS_PER_PAGE);
    enter_page(app, save ? APP_PAGE_SAVE : APP_PAGE_LOAD);
    gal_ui_list(&app->ui, save ? "选择保存栏位" : "选择读取栏位", sub,
                app->list_rows, n, app->slot_sel - first,
                save ? "OK 保存   长OK 返回" : "OK 读取   长OK 返回");
}

static void render_settings(gal_app_t *app)
{
    snprintf(app->list_text[0], sizeof(app->list_text[0]), "文字速度：%s",
             app->settings.text_speed ? "逐字" : "即时");
    snprintf(app->list_text[1], sizeof(app->list_text[1]), "自动速度：%s",
             app->settings.auto_speed == 0 ? "慢" :
             app->settings.auto_speed == 1 ? "标准" : "快");
    snprintf(app->list_text[2], sizeof(app->list_text[2]), "返回");
    for (int i = 0; i < 3; ++i) {
        app->list_rows[i].text = app->list_text[i];
        app->list_rows[i].dim = i == 2;
    }
    enter_page(app, APP_PAGE_SETTINGS);
    gal_ui_list(&app->ui, "阅读设置", "仅列出实际可用的选项",
                app->list_rows, 3, app->settings_sel, "OK 切换   长OK 返回");
}

static void render_gallery(gal_app_t *app)
{
    const int groups = (int)app->pack->count[GAL_SEC_GALLERY];
    const int first = (app->gallery_sel / GAL_UI_LIST_ROWS) * GAL_UI_LIST_ROWS;
    int n = 0;
    for (int i = first; i < groups && n < GAL_UI_LIST_ROWS; ++i, ++n) {
        const gal_gallery_group_t *g = gal_pack_gallery_group(app->pack, (uint16_t)i);
        char label[96];
        if (g) {
            gal_pack_text_utf8(app->pack, g->label_off, g->label_len, label, sizeof(label));
        } else {
            snprintf(label, sizeof(label), "CG %d", i + 1);
        }
        // Source index.ux renders every group/member; unlock metadata is
        // retained as a record but never gates the gallery.
        snprintf(app->list_text[n], sizeof(app->list_text[n]), "%s\n%u 张",
                 label, g ? g->count : 0);
        app->list_rows[n].dim = 0;
        app->list_rows[n].text = app->list_text[n];
    }
    char sub[64];
    snprintf(sub, sizeof(sub), "共 %d 组 · 第 %d / %d 页", groups,
             first / GAL_UI_LIST_ROWS + 1,
             (groups + GAL_UI_LIST_ROWS - 1) / GAL_UI_LIST_ROWS);
    enter_page(app, APP_PAGE_GALLERY);
    gal_ui_list(&app->ui, "鉴赏室", sub, app->list_rows, n,
                app->gallery_sel - first, "OK 查看   长OK 返回");
}

static void render_cg(gal_app_t *app)
{
    const gal_gallery_group_t *g = gal_pack_gallery_group(app->pack, (uint16_t)app->cg_sel);
    const gal_gallery_item_t *it = gal_pack_gallery_item(app->pack, (uint16_t)app->cg_variant);
    if (!g || !it) {
        set_notice(app, "鉴赏条目无效");
        return;
    }
    enter_page(app, APP_PAGE_CG);
    ensure_page_art(app);
    char label[96];
    gal_pack_text_utf8(app->pack, g->label_off, g->label_len, label, sizeof(label));
    char cap[160];
    snprintf(cap, sizeof(cap), "%s · 差分 %d/%u", label,
             app->cg_variant - g->first + 1, g->count);
    gal_ui_cg(&app->ui, cap);
}

static void render_routes(gal_app_t *app)
{
    const int count = (int)app->pack->count[GAL_SEC_ROUTES];
    const int first = (app->route_sel / GAL_UI_LIST_ROWS) * GAL_UI_LIST_ROWS;
    int n = 0;
    for (int i = first; i < count && n < GAL_UI_LIST_ROWS; ++i, ++n) {
        const gal_route_t *r = (const gal_route_t *)app->pack->sec[GAL_SEC_ROUTES] + i;
        snprintf(app->list_text[n], sizeof(app->list_text[n]), "%s\n%u 个章节",
                 gal_pack_str(app->pack, (uint16_t)r->name), r->count);
        app->list_rows[n].text = app->list_text[n];
        app->list_rows[n].dim = 0;
    }
    char sub[64];
    snprintf(sub, sizeof(sub), "共 %d 条路线 · 第 %d / %d 页", count,
             first / GAL_UI_LIST_ROWS + 1,
             (count + GAL_UI_LIST_ROWS - 1) / GAL_UI_LIST_ROWS);
    enter_page(app, APP_PAGE_ROUTES);
    gal_ui_list(&app->ui, "选择路线", sub, app->list_rows, n,
                app->route_sel - first, "OK 进入   长OK 返回");
}

static int route_entry_count(gal_app_t *app, int route, uint16_t *out, int cap)
{
    const gal_route_t *r = (const gal_route_t *)app->pack->sec[GAL_SEC_ROUTES] + route;
    int n = 0;
    for (uint16_t i = 0; i < r->count && n < cap; ++i) out[n++] = (uint16_t)(r->first + i);
    return n;
}

static void render_chapters(gal_app_t *app)
{
    uint16_t entries[GAL_PACK_MAX_ROUTE_ENTRIES];
    const int count = route_entry_count(app, app->route_sel, entries, GAL_PACK_MAX_ROUTE_ENTRIES);
    const int first = (app->chapter_sel / GAL_UI_LIST_ROWS) * GAL_UI_LIST_ROWS;
    int n = 0;
    for (int i = first; i < count && n < GAL_UI_LIST_ROWS; ++i, ++n) {
        const uint16_t entry = entries[i];
        const gal_chapter_t *c = (const gal_chapter_t *)app->pack->sec[GAL_SEC_CHAPTERS] + entry;
        // Source chapter list has no unlock gate; visited/unlock metadata is
        // kept for records only.
        snprintf(app->list_text[n], sizeof(app->list_text[n]), "%s",
                 gal_pack_str(app->pack, (uint16_t)c->name));
        app->list_rows[n].text = app->list_text[n];
        app->list_rows[n].dim = 0;
    }
    char sub[96];
    const gal_route_t *r = (const gal_route_t *)app->pack->sec[GAL_SEC_ROUTES] + app->route_sel;
    snprintf(sub, sizeof(sub), "%s · %d 章 · 第 %d / %d 页",
             gal_pack_str(app->pack, (uint16_t)r->name), count,
             first / GAL_UI_LIST_ROWS + 1,
             (count + GAL_UI_LIST_ROWS - 1) / GAL_UI_LIST_ROWS);
    enter_page(app, APP_PAGE_CHAPTERS);
    gal_ui_list(&app->ui, "选择章节", sub, app->list_rows, n,
                app->chapter_sel - first, "OK 开始   长OK 返回");
}

static void render_book(gal_app_t *app)
{
    const int count = (int)app->vm.book_count;
    const int first = (app->book_sel / GAL_UI_LIST_ROWS) * GAL_UI_LIST_ROWS;
    int n = 0;
    for (int i = first; i < count && n < GAL_UI_LIST_ROWS; ++i, ++n) {
        const uint16_t key = app->vm.book_order[i];
        const gal_bookkey_t *bk = gal_pack_bookkey(app->pack, key);
        const gal_book_t *b = gal_vm_book_entry(&app->vm, (uint16_t)i);
        if (!bk || !b) continue;
        char name[96];
        gal_pack_book_name_utf8(app->pack, b, name, sizeof(name));
        snprintf(app->list_text[n], sizeof(app->list_text[n]), "%s\n%s · v%u",
                 name, category_name(bk->category), b->version);
        app->list_rows[n].text = app->list_text[n];
        app->list_rows[n].dim = 0;
    }
    char sub[64];
    snprintf(sub, sizeof(sub), "%d 条记录 · 第 %d / %d 页", count,
             first / GAL_UI_LIST_ROWS + 1,
             (count + GAL_UI_LIST_ROWS - 1) / GAL_UI_LIST_ROWS);
    enter_page(app, APP_PAGE_BOOK);
    gal_ui_list(&app->ui, "魔女手册", sub, app->list_rows, n,
                app->book_sel - first, "OK 查看   长OK 返回");
}

static void render_book_detail(gal_app_t *app)
{
    const gal_book_t *b = gal_vm_book_entry(&app->vm, (uint16_t)app->book_sel);
    if (!b) {
        set_notice(app, "手册记录不存在");
        return;
    }
    char name[96];
    if (gal_pack_book_name_utf8(app->pack, b, name, sizeof(name)) < 0) {
        snprintf(name, sizeof(name), "手册记录");
    }
    const int body_len = gal_pack_book_text_utf8(app->pack, b, app->scratch.book_text,
                                                 sizeof(app->scratch.book_text));
    int pages = 1;
    if (body_len < 0) {
        // Bounded workspace still has to fail visibly, never page an
        // uninitialized/partial buffer.
        snprintf(app->detail_page, sizeof(app->detail_page), "手册正文读取失败");
        app->book_page = 0;
        pages = 1;
    } else if (!gal_text_page(app->scratch.book_text, app->book_page, UI_TEXT_WIDTH, UI_FONT_PX,
                              UI_TEXT_LINES, app->detail_page, sizeof(app->detail_page),
                              &pages)) {
        app->book_page = 0;
        (void)gal_text_page(app->scratch.book_text, 0, UI_TEXT_WIDTH, UI_FONT_PX, UI_TEXT_LINES,
                            app->detail_page, sizeof(app->detail_page), &pages);
    }
    app->book_pages = (uint8_t)(pages < 1 ? 1 : pages);
    enter_page(app, APP_PAGE_BOOK_DETAIL);
    ensure_page_art(app);
    char pagebuf[16];
    snprintf(pagebuf, sizeof(pagebuf), "%d/%d", app->book_page + 1, pages);
    gal_ui_game(&app->ui, name, app->detail_page, "", pagebuf, "OK 下一页 · 长OK 返回", "");
}

static void render_ending(gal_app_t *app)
{
    const char *kind = app->frame.end_kind == GAL_NONE ? "" :
                       gal_pack_str(app->pack, app->frame.end_kind);
    const char *title = "本段剧情结束";
    if (app->vm.trial_fail) title = "审判失败";
    else if (strcmp(kind, "bad") == 0) title = "BAD END";
    else if (strcmp(kind, "clear") == 0) title = "CLEAR";
    else if (strcmp(kind, "stop") == 0) title = "本段剧情结束";
    char body[320];
    snprintf(body, sizeof(body), "%s\n\n故事阅读完毕。\n\nOK 返回标题",
             scene_name(app, app->vm.root));
    enter_page(app, APP_PAGE_ENDING);
    gal_ui_text(&app->ui, title, body, "OK 返回标题");
}

static void render_about(gal_app_t *app)
{
    enter_page(app, APP_PAGE_ABOUT);
    gal_ui_text(&app->ui, "关于",
                app->about_page == 0 ? k_about_page0 : k_about_page1,
                "↑↓ 翻页   长OK 返回");
}

static void render_error(gal_app_t *app)
{
    enter_page(app, APP_PAGE_ERROR);
    gal_ui_text(&app->ui, "暂时无法继续", app->error, "OK 返回标题");
}

static void set_error(gal_app_t *app, const char *msg)
{
    snprintf(app->error, sizeof(app->error), "%s", msg ? msg : "未知错误");
    app->error_image = false;
    render_error(app);
}

static void render_confirm(gal_app_t *app)
{
    snprintf(app->list_text[0], sizeof(app->list_text[0]), "确认覆盖");
    snprintf(app->list_text[1], sizeof(app->list_text[1]), "取消");
    for (int i = 0; i < 2; ++i) {
        app->list_rows[i].text = app->list_text[i];
        app->list_rows[i].dim = i == 1;
    }
    char title[64];
    snprintf(title, sizeof(title), "覆盖存档 %02d？", app->slot_sel + 1);
    enter_page(app, APP_PAGE_CONFIRM);
    gal_ui_list(&app->ui, title, "旧存档将被覆盖。", app->list_rows, 2,
                app->confirm_sel, "OK 确认   长OK 取消");
}

// ---------------------------------------------------------------- 帧处理
static void build_choices(gal_app_t *app)
{
    if (app->vm.row_count > GAL_MAX_ROWS) {
        app->choice_count = 0;
        set_error(app, "选项行超过界面容量");
        return;
    }
    app->choice_count = app->vm.row_count;
    for (int i = 0; i < app->choice_count; ++i) {
        const gal_row_t *r = &app->vm.rows[i];
        char buf[160];
        const char *text = gal_vm_row_text(&app->vm, r, buf, sizeof(buf));
        if (r->kind == GAL_ROW_MAP && r->visited) {
            snprintf(app->choice_texts[i], sizeof(app->choice_texts[i]), "%s · 已调查", text);
        } else if (r->kind == GAL_ROW_CONTINUE && (!text || !text[0])) {
            snprintf(app->choice_texts[i], sizeof(app->choice_texts[i]), "按下处刑按钮");
        } else {
            snprintf(app->choice_texts[i], sizeof(app->choice_texts[i]), "%s", text);
        }
        app->choice_rows[i].text = app->choice_texts[i];
        app->choice_rows[i].dim = r->kind == GAL_ROW_MAP && r->visited;
    }
}

static bool build_objections(gal_app_t *app)
{
    // Runtime fail-closed defense: pack validation rejects this too, but a
    // corrupt/injected frame must never write choice_rows[32] or drop links.
    if (!app) return false;
    app->choice_count = 0;
    if (app->frame.link_count > GAL_MAX_ROWS - 1) {
        set_error(app, "反驳选项超过界面容量");
        return false;
    }
    for (int i = 0; i < app->frame.link_count; ++i) {
        if (!gal_pack_link(app->pack, (uint16_t)(app->frame.link_first + i))) {
            set_error(app, "反驳选项记录缺失");
            return false;
        }
    }
    for (int i = 0; i < app->frame.link_count; ++i) {
        const gal_link_t *l = gal_pack_link(app->pack,
                                            (uint16_t)(app->frame.link_first + i));
        const int n = gal_pack_text_utf8(app->pack, l->text_off, l->text_len,
                                         app->choice_texts[i], sizeof(app->choice_texts[i]));
        if (n < 0) {
            set_error(app, "反驳选项文本读取失败");
            return false;
        }
        app->choice_rows[i].text = app->choice_texts[i];
        app->choice_rows[i].dim = 0;
    }
    const int last = app->frame.link_count;
    snprintf(app->choice_texts[last], sizeof(app->choice_texts[last]), "返回发言");
    app->choice_rows[last].text = app->choice_texts[last];
    app->choice_rows[last].dim = 1;
    app->choice_count = last + 1;
    return true;
}

static void finish_ending(gal_app_t *app)
{
    gal_unlock_merge(&app->unlock, app->vm.unlock_gallery, app->vm.unlock_chapter);
    (void)flush_unlock(app);
    (void)flush_progress(app);
    render_ending(app);
}

static void read_frame(gal_app_t *app)
{
    if (!app->vm_started) return;
    app->frame_valid = false;
    app->objection_mode = false;
    for (int hops = 0; hops < 1000; ++hops) {
        const gal_frame_type_t t = gal_vm_read(&app->vm, &app->frame);
        if (t == GAL_FRAME_YIELD) continue;
        if (t == GAL_FRAME_ERROR) {
            set_error(app, app->frame.error ? app->frame.error : "剧情执行失败");
            return;
        }
        if (t == GAL_FRAME_END) {
            app->frame_valid = true;
            finish_ending(app);
            return;
        }
        if (t == GAL_FRAME_CHOICE) {
            app->frame_valid = true;
            build_choices(app);
            app->choice_sel = 0;
            if (app->pending_choice_valid) {
                app->choice_sel = app->pending_choice;
                if (app->choice_sel < 0) app->choice_sel = 0;
                if (app->choice_sel >= app->choice_count) app->choice_sel = app->choice_count - 1;
                app->pending_choice_valid = false;
            }
            app->pending_objection_valid = false;
            app->checkpoint_mode = 1;
            app->checkpoint_choice = (uint8_t)app->choice_sel;
            render_choice(app);
            after_frame(app);
            return;
        }
        if (t == GAL_FRAME_TEXT) {
            app->frame_valid = true;
            show_text_frame(app);
            if (app->pending_objection_valid) {
                app->pending_objection_valid = false;
                if (app->frame.link_count && build_objections(app)) {
                    app->objection_mode = true;
                    app->choice_sel = app->pending_choice;
                    if (app->choice_sel < 0) app->choice_sel = 0;
                    if (app->choice_sel >= app->choice_count) app->choice_sel = app->choice_count - 1;
                    render_choice(app);
                } else if (app->frame.link_count) {
                    // build_objections already installed a fail-closed error page.
                } else {
                    set_error(app, "存档反驳位置已失效");
                }
            }
            after_frame(app);
            return;
        }
    }
    set_error(app, "剧情执行步数过多");
}

// ---------------------------------------------------------------- 存档动作
static uint8_t current_scene_mode(const gal_app_t *app)
{
    return app->checkpoint_mode;
}

static uint8_t current_save_page(const gal_app_t *app)
{
    return app->checkpoint_page;
}

static int current_choice_row(const gal_app_t *app)
{
    return app->checkpoint_choice;
}

static bool save_auto(gal_app_t *app, bool force)
{
    if (!app->vm_started) return false;
    if (!force && app->last_now_ms < app->autosave_retry_ms) return false;
    memcpy(app->vm.unlock_gallery, app->unlock.gallery, sizeof(app->vm.unlock_gallery));
    memcpy(app->vm.unlock_chapter, app->unlock.chapters, sizeof(app->vm.unlock_chapter));
    gal_save_ui_t ui = {
        .scene_mode = current_scene_mode(app),
        .page = current_save_page(app),
        .choice_row = (uint8_t)(current_choice_row(app) > 255 ? 255 : current_choice_row(app)),
    };
    size_t len = 0;
    if (!gal_save_encode(&app->vm, &ui, app->save_blob, sizeof(app->save_blob), &len)) {
        app->autosave_retry_ms = app->last_now_ms + AUTOSAVE_RETRY_MS;
        set_notice(app, "自动存档编码失败");
        return false;
    }
    if (!gal_store_save_slot(&app->store, GAL_SLOT_AUTO, app->save_blob, len)) {
        app->autosave_retry_ms = app->last_now_ms + AUTOSAVE_RETRY_MS;
        set_notice(app, "自动存档失败，稍后重试");
        return false;
    }
    app->auto_valid = true;
    app->autosave_due = false;
    app->last_auto_root = app->vm.root;
    app->autosave_retry_ms = 0;
    return true;
}

static bool save_manual(gal_app_t *app, int slot)
{
    if (!app->vm_started) {
        set_notice(app, "当前没有可保存的阅读进度");
        return false;
    }
    memcpy(app->vm.unlock_gallery, app->unlock.gallery, sizeof(app->vm.unlock_gallery));
    memcpy(app->vm.unlock_chapter, app->unlock.chapters, sizeof(app->vm.unlock_chapter));
    gal_save_ui_t ui = {
        .scene_mode = app->save_scene_mode,
        .page = app->save_page_idx,
        .choice_row = (uint8_t)(app->save_choice_row > 255 ? 255 : app->save_choice_row),
    };
    size_t len = 0;
    if (!gal_save_encode(&app->vm, &ui, app->save_blob, sizeof(app->save_blob), &len)) {
        set_notice(app, "保存失败：编码错误");
        return false;
    }
    if (!gal_store_save_slot(&app->store, slot, app->save_blob, len)) {
        set_notice(app, "保存失败，请重试");
        return false;
    }
    refresh_slots(app);
    set_notice(app, "已保存到存档 %02d", slot + 1);
    return true;
}

static void apply_loaded_state(gal_app_t *app, const gal_save_ui_t *ui)
{
    app->vm = app->scratch.load_vm;
    app->vm.expr.pack = app->pack;
    app->vm.expr.vars = app->vm.vars;
    app->vm.expr.var_count = app->vm.var_count;
    app->vm_started = true;
    app->auto_enabled = false;
    app->ff_held = false;
    app->history_count = 0;
    app->text_chapter = 0xFFFF;
    app->text_cursor = 0xFFFF;
    app->art_bg = GAL_NONE;
    app->art_bodies = 0xFFFFFFFFu;
    app->art_ok = false;
    app->image_dirty = false;
    app->manual_boundary = false;
    app->pending_page = ui->page;
    app->pending_page_valid = true;
    app->pending_choice = ui->choice_row;
    app->pending_choice_valid = ui->scene_mode == 1;
    app->pending_objection_valid = ui->scene_mode == 2;
    app->checkpoint_mode = ui->scene_mode;
    app->checkpoint_page = ui->page;
    app->checkpoint_choice = ui->choice_row;
    gal_unlock_merge(&app->unlock, app->vm.unlock_gallery, app->vm.unlock_chapter);
    app->last_auto_root = app->vm.root;
    read_frame(app);
}

static bool load_slot(gal_app_t *app, int slot)
{
    size_t len = 0;
    const gal_store_status_t st = gal_store_read_slot(&app->store, slot, app->save_blob,
                                                      sizeof(app->save_blob), &len);
    if (st == GAL_STORE_NOT_FOUND) {
        set_notice(app, "这个栏位还没有存档");
        return false;
    }
    if (st != GAL_STORE_OK) {
        set_notice(app, st == GAL_STORE_CORRUPT ? "存档无法读取" : "读取存档失败，请稍后重试");
        return false;
    }
    gal_save_ui_t ui;
    const char *err = NULL;
    if (!gal_save_decode(app->save_blob, len, app->pack, &app->scratch.load_vm, &ui, &err)) {
        set_notice(app, "存档无法读取：%s", err ? err : "");
        return false;
    }
    apply_loaded_state(app, &ui);
    set_notice(app, "已读取存档 %02d", slot + 1);
    return true;
}

// ---------------------------------------------------------------- 导航
static void to_title(gal_app_t *app)
{
    if (app->vm_started && !app->vm.ended) (void)save_auto(app, true);
    (void)flush_unlock(app);
    (void)flush_progress(app);
    app->vm_started = false;
    app->auto_enabled = false;
    app->ff_held = false;
    app->objection_mode = false;
    app->manual_boundary = false;
    app->title_sel = 0;
    app->pending_page_valid = false;
    app->pending_choice_valid = false;
    app->pending_objection_valid = false;
    app->ui_ready = true;
    refresh_auto_summary(app);
    render_title(app);
}

static void start_story(gal_app_t *app, uint16_t scene, bool story)
{
    const char *err = NULL;
    if (!gal_vm_start(&app->vm, scene, story, false, &err)) {
        set_error(app, err ? err : "无法开始剧情");
        return;
    }
    app->vm_started = true;
    app->auto_enabled = false;
    app->ff_held = false;
    app->history_count = 0;
    app->text_chapter = 0xFFFF;
    app->text_cursor = 0xFFFF;
    app->text_page = 0;
    app->text_pages = 1;
    app->choice_sel = 0;
    app->art_bg = GAL_NONE;
    app->art_bodies = 0xFFFFFFFFu;
    app->art_ok = false;
    app->image_dirty = false;
    app->manual_boundary = false;
    app->pending_page_valid = false;
    app->pending_choice_valid = false;
    app->pending_objection_valid = false;
    app->checkpoint_mode = 0;
    app->checkpoint_page = 0;
    app->checkpoint_choice = 0;
    app->last_auto_root = app->vm.root;
    read_frame(app);
}

static void open_menu(gal_app_t *app)
{
    if (!app->vm_started) return;
    // The checkpoint must survive BOOK/HISTORY/SETTINGS/LOAD sub-pages.
    if (app->page == APP_PAGE_CHOICE) {
        app->checkpoint_mode = app->objection_mode ? 2 : 1;
        app->checkpoint_choice = (uint8_t)app->choice_sel;
    } else if (app->page == APP_PAGE_GAME) {
        app->checkpoint_mode = 0;
        app->checkpoint_page = app->text_page;
    }
    app->menu_source = app->page == APP_PAGE_CHOICE ? APP_PAGE_CHOICE : APP_PAGE_GAME;
    app->menu_sel = 0;
    typing_finish(app);
    enter_page(app, APP_PAGE_MENU);
    render_menu(app);
}

static void resume_menu(gal_app_t *app)
{
    if (app->menu_source == APP_PAGE_CHOICE) {
        render_choice(app);
    } else {
        enter_page(app, APP_PAGE_GAME);
        render_game_page(app);
    }
}

static void open_history(gal_app_t *app, app_page_t source)
{
    app->page_source = source;
    app->history_sel = app->history_count > 0 ? app->history_count - 1 : 0;
    render_history(app);
}

static void open_save(gal_app_t *app)
{
    app->save_source = APP_PAGE_MENU;
    app->slot_sel = 0;
    refresh_slots(app);
    render_slots(app, true);
}

static void open_load(gal_app_t *app, app_page_t source)
{
    app->load_source = source;
    app->slot_sel = 0;
    refresh_slots(app);
    render_slots(app, false);
}

static void open_gallery(gal_app_t *app, app_page_t source)
{
    app->gallery_source = source;
    app->gallery_sel = 0;
    render_gallery(app);
}

static void open_settings(gal_app_t *app, app_page_t source)
{
    app->settings_source = source;
    app->settings_sel = 0;
    render_settings(app);
}

static void open_book(gal_app_t *app)
{
    app->book_source = APP_PAGE_MENU;
    app->book_sel = 0;
    render_book(app);
}

static void open_objections(gal_app_t *app)
{
    if (app->page != APP_PAGE_GAME || app->frame.link_count == 0) return;
    typing_finish(app);
    app->objection_mode = true;
    app->choice_sel = 0;
    if (!build_objections(app)) return;
    render_choice(app);
}

static void advance_text(gal_app_t *app)
{
    if (app->page != APP_PAGE_GAME || !app->vm_started) return;
    if (app->typing_active) {
        typing_finish(app);
        return;
    }
    if (app->text_page + 1 < app->text_pages) {
        app->text_page++;
        app->checkpoint_mode = 0;
        app->checkpoint_page = app->text_page;
        int pages = app->text_pages;
        (void)gal_text_page(app->text_utf8, app->text_page, UI_TEXT_WIDTH, UI_FONT_PX,
                            UI_TEXT_LINES, app->page_text, sizeof(app->page_text), &pages);
        app->typing_total = gal_text_cp_count(app->page_text);
        app->typing_index = 0;
        app->typing_active = (app->settings.text_speed != 0 && app->typing_total > 0);
        if (!app->typing_active) {
            snprintf(app->shown_text, sizeof(app->shown_text), "%s", app->page_text);
        } else {
            app->shown_text[0] = 0;
        }
        render_game_page(app);
        return;
    }
    gal_vm_advance(&app->vm);
    read_frame(app);
}

static void back_to_source(gal_app_t *app)
{
    switch (app->page) {
    case APP_PAGE_SAVE: render_menu(app); break;
    case APP_PAGE_LOAD:
        if (app->load_source == APP_PAGE_TITLE) render_title(app);
        else render_menu(app);
        break;
    case APP_PAGE_SETTINGS:
        if (app->settings_source == APP_PAGE_TITLE) render_title(app);
        else render_menu(app);
        break;
    case APP_PAGE_GALLERY: render_title(app); break;
    case APP_PAGE_CG: render_gallery(app); break;
    case APP_PAGE_ROUTES: render_title(app); break;
    case APP_PAGE_CHAPTERS: render_routes(app); break;
    case APP_PAGE_BOOK: render_menu(app); break;
    case APP_PAGE_BOOK_DETAIL: render_book(app); break;
    case APP_PAGE_HISTORY:
        if (app->page_source == APP_PAGE_GAME) {
            enter_page(app, APP_PAGE_GAME);
            render_game_page(app);
        } else {
            render_menu(app);
        }
        break;
    case APP_PAGE_ABOUT: render_title(app); break;
    case APP_PAGE_ENDING: to_title(app); break;
    case APP_PAGE_ERROR: to_title(app); break;
    case APP_PAGE_CONFIRM: render_slots(app, true); break;
    case APP_PAGE_MENU: resume_menu(app); break;
    default: to_title(app); break;
    }
}

// ---------------------------------------------------------------- 按键
static void adjust_sel(int *sel, int count, int delta)
{
    if (count <= 0) return;
    *sel += delta;
    if (*sel < 0) *sel = 0;
    if (*sel >= count) *sel = count - 1;
}

static void handle_click(gal_app_t *app, uint8_t btn)
{
    const int delta = btn == 0 ? -1 : (btn == 1 ? 1 : 0);
    switch (app->page) {
    case APP_PAGE_TITLE:
        if (btn == 0 || btn == 1) {
            adjust_sel(&app->title_sel, (int)(sizeof(k_title_menu) / sizeof(k_title_menu[0])), delta);
            render_title(app);
        } else if (btn == 2) {
            switch (app->title_sel) {
            case 0: start_story(app, (uint16_t)app->pack->meta[1], true); break;
            case 1: {
                size_t len = 0;
                const gal_store_status_t st = gal_store_read_slot(&app->store, GAL_SLOT_AUTO,
                                                                  app->save_blob,
                                                                  sizeof(app->save_blob), &len);
                if (st == GAL_STORE_NOT_FOUND) {
                    set_notice(app, "还没有自动存档");
                } else if (st != GAL_STORE_OK) {
                    set_notice(app, st == GAL_STORE_CORRUPT ? "自动存档无法读取" :
                               "自动存档读取失败，请重试");
                } else {
                    gal_save_ui_t ui;
                    const char *err = NULL;
                    if (gal_save_decode(app->save_blob, len, app->pack, &app->scratch.load_vm,
                                        &ui, &err)) {
                        apply_loaded_state(app, &ui);
                        set_notice(app, "已读取自动存档");
                    } else {
                        set_notice(app, "自动存档无法读取");
                    }
                }
                break;
            }
            case 2: open_load(app, APP_PAGE_TITLE); break;
            case 3: open_gallery(app, APP_PAGE_TITLE); break;
            case 4:
                app->route_sel = 0;
                render_routes(app);
                break;
            case 5: open_settings(app, APP_PAGE_TITLE); break;
            case 6:
                app->about_page = 0;
                render_about(app);
                break;
            default: break;
            }
        }
        break;
    case APP_PAGE_GAME:
        if (btn == 1) open_history(app, APP_PAGE_GAME);
        else if (btn == 2) {
            if (app->frame.link_count) open_objections(app);
            else open_menu(app);
        }
        break;
    case APP_PAGE_CHOICE:
        if (btn == 0 || btn == 1) {
            adjust_sel(&app->choice_sel, app->choice_count, delta);
            render_choice(app);
        } else if (btn == 2) {
            if (app->objection_mode) {
                if (app->choice_sel == app->choice_count - 1) {
                    app->objection_mode = false;
                    enter_page(app, APP_PAGE_GAME);
                    render_game_page(app);
                } else {
                    const char *err = NULL;
                    if (!gal_vm_object_to(&app->vm, (uint8_t)app->choice_sel, &err)) {
                        set_error(app, err ? err : "反驳失败");
                        break;
                    }
                    app->objection_mode = false;
                    read_frame(app);
                }
            } else {
                const char *err = NULL;
                if (!gal_vm_choose(&app->vm, (uint8_t)app->choice_sel, &err)) {
                    set_error(app, err ? err : "选项无效");
                    break;
                }
                read_frame(app);
            }
        }
        break;
    case APP_PAGE_MENU:
        if (btn == 0 || btn == 1) {
            adjust_sel(&app->menu_sel, 9, delta);
            render_menu(app);
        } else if (btn == 2) {
            switch (app->menu_sel) {
            case 0: resume_menu(app); break;
            case 1:
                app->save_scene_mode = app->checkpoint_mode;
                app->save_page_idx = app->checkpoint_page;
                app->save_choice_row = app->checkpoint_choice;
                open_save(app);
                break;
            case 2: open_load(app, APP_PAGE_MENU); break;
            case 3: open_history(app, APP_PAGE_MENU); break;
            case 4: open_book(app); break;
            case 5:
                app->auto_enabled = !app->auto_enabled;
                app->auto_accum_ms = 0;
                render_menu(app);
                set_notice(app, "自动播放：%s", app->auto_enabled ? "开" : "关");
                break;
            case 6:
                app->settings.auto_speed = (uint16_t)((app->settings.auto_speed + 1) % 3);
                if (!gal_store_save_settings(&app->store, &app->settings)) {
                    set_notice(app, "设置保存失败");
                }
                render_menu(app);
                break;
            case 7: open_settings(app, APP_PAGE_MENU); break;
            case 8: to_title(app); break;
            default: break;
            }
        }
        break;
    case APP_PAGE_HISTORY:
        if (btn == 0 || btn == 1) {
            adjust_sel(&app->history_sel, app->history_count, delta);
            render_history(app);
        } else if (btn == 2) {
            back_to_source(app);
        }
        break;
    case APP_PAGE_SAVE:
    case APP_PAGE_LOAD:
        if (btn == 0 || btn == 1) {
            adjust_sel(&app->slot_sel, GAL_SLOT_COUNT, delta);
            render_slots(app, app->page == APP_PAGE_SAVE);
        } else if (btn == 2) {
            if (app->page == APP_PAGE_SAVE) {
                if (!app->vm_started) { set_notice(app, "当前没有可保存的进度"); break; }
                if (app->slots[app->slot_sel].state != 0) {
                    app->confirm_sel = 0;
                    render_confirm(app);
                } else if (save_manual(app, app->slot_sel)) {
                    render_menu(app);
                }
            } else {
                if (app->slots[app->slot_sel].state != 1) {
                    const uint8_t st = app->slots[app->slot_sel].state;
                    set_notice(app, st == 2 ? "这个存档无法读取" :
                               st == 3 ? "读取存档失败，请稍后重试" :
                               "这个栏位还没有存档");
                    break;
                }
                (void)load_slot(app, app->slot_sel);
            }
        }
        break;
    case APP_PAGE_SETTINGS:
        if (btn == 0 || btn == 1) {
            adjust_sel(&app->settings_sel, 3, delta);
            render_settings(app);
        } else if (btn == 2) {
            if (app->settings_sel == 0) {
                app->settings.text_speed = app->settings.text_speed ? 0 : 1;
                if (!gal_store_save_settings(&app->store, &app->settings)) {
                    set_notice(app, "设置保存失败");
                }
                render_settings(app);
            } else if (app->settings_sel == 1) {
                app->settings.auto_speed = (uint16_t)((app->settings.auto_speed + 1) % 3);
                if (!gal_store_save_settings(&app->store, &app->settings)) {
                    set_notice(app, "设置保存失败");
                }
                render_settings(app);
            } else {
                back_to_source(app);
            }
        }
        break;
    case APP_PAGE_GALLERY:
        if (btn == 0 || btn == 1) {
            adjust_sel(&app->gallery_sel, (int)app->pack->count[GAL_SEC_GALLERY], delta);
            render_gallery(app);
        } else if (btn == 2) {
            const gal_gallery_group_t *g = gal_pack_gallery_group(app->pack,
                                                                 (uint16_t)app->gallery_sel);
            if (!g || g->count == 0) break;
            app->cg_sel = app->gallery_sel;
            app->cg_variant = g->first;
            render_cg(app);
        }
        break;
    case APP_PAGE_CG:
        if (btn == 0 || btn == 1) {
            const int groups = (int)app->pack->count[GAL_SEC_GALLERY];
            const int g = app->cg_sel + delta;
            if (g < 0 || g >= groups) {
                set_notice(app, delta < 0 ? "已经是第一组" : "已经是最后一组");
                break;
            }
            const gal_gallery_group_t *grp = gal_pack_gallery_group(app->pack, (uint16_t)g);
            if (!grp || grp->count == 0) break;
            app->cg_sel = g;
            app->cg_variant = grp->first;
            render_cg(app);
        } else if (btn == 2) {
            const gal_gallery_group_t *g = gal_pack_gallery_group(app->pack, (uint16_t)app->cg_sel);
            if (!g || g->count == 0) break;
            app->cg_variant = g->first + (app->cg_variant - g->first + 1) % g->count;
            render_cg(app);
        }
        break;
    case APP_PAGE_ROUTES:
        if (btn == 0 || btn == 1) {
            adjust_sel(&app->route_sel, (int)app->pack->count[GAL_SEC_ROUTES], delta);
            render_routes(app);
        } else if (btn == 2) {
            app->chapter_sel = 0;
            render_chapters(app);
        }
        break;
    case APP_PAGE_CHAPTERS: {
        uint16_t entries[GAL_PACK_MAX_ROUTE_ENTRIES];
        const int count = route_entry_count(app, app->route_sel, entries, GAL_PACK_MAX_ROUTE_ENTRIES);
        if (btn == 0 || btn == 1) {
            adjust_sel(&app->chapter_sel, count, delta);
            render_chapters(app);
        } else if (btn == 2) {
            if (app->chapter_sel < 0 || app->chapter_sel >= count) break;
            const uint16_t entry = entries[app->chapter_sel];
            const gal_chapter_t *c = (const gal_chapter_t *)app->pack->sec[GAL_SEC_CHAPTERS] + entry;
            start_story(app, (uint16_t)c->scene, false);
        }
        break;
    }
    case APP_PAGE_BOOK:
        if (btn == 0 || btn == 1) {
            adjust_sel(&app->book_sel, (int)app->vm.book_count, delta);
            render_book(app);
        } else if (btn == 2) {
            app->book_page = 0;
            render_book_detail(app);
        }
        break;
    case APP_PAGE_BOOK_DETAIL:
        if (btn == 0 || btn == 1) {
            if (delta < 0 && app->book_page > 0) app->book_page--;
            else if (delta > 0 && app->book_page + 1 < app->book_pages) app->book_page++;
            render_book_detail(app);
        } else if (btn == 2) {
            if (app->book_page + 1 < app->book_pages) {
                app->book_page++;
                render_book_detail(app);
            } else {
                back_to_source(app);   // 末页单击 OK = 返回手册，与页脚提示一致
            }
        }
        break;
    case APP_PAGE_ABOUT:
        if (btn == 0) { if (app->about_page > 0) app->about_page--; render_about(app); }
        else if (btn == 1) { if (app->about_page < 1) app->about_page++; render_about(app); }
        else if (btn == 2) { app->about_page = app->about_page ? 0 : 1; render_about(app); }
        break;
    case APP_PAGE_ENDING:
        if (btn == 2) to_title(app);
        break;
    case APP_PAGE_ERROR:
        if (btn == 2) to_title(app);
        break;
    case APP_PAGE_CONFIRM:
        if (btn == 0 || btn == 1) {
            adjust_sel(&app->confirm_sel, 2, delta);
            render_confirm(app);
        } else if (btn == 2) {
            if (app->confirm_sel == 0) {
                if (save_manual(app, app->slot_sel)) render_menu(app);
            } else {
                render_slots(app, true);
            }
        }
        break;
    default:
        break;
    }
}

void gal_app_key(gal_app_t *app, const gal_key_t *key)
{
    if (!app || !key || !app_lock()) return;
    const uint8_t btn = key->btn;
    const uint8_t ev = key->ev;
    if (btn > 2) { app_unlock(); return; }
    switch (ev) {
    case BSP_BTN_PRESS:
        app->key_action[btn] = 0;
        app->key_long[btn] = 0;
        if (app->page == APP_PAGE_GAME && btn == 0) {
            app->key_action[btn] = 1;
            advance_text(app);
        }
        break;
    case BSP_BTN_LONG:
        app->key_long[btn] = 1;
        if (btn == 2) {
            if (app->page == APP_PAGE_GAME) open_menu(app);
            else if (app->page == APP_PAGE_CHOICE) open_menu(app);
            else if (app->page == APP_PAGE_MENU) resume_menu(app);
            else if (app->page == APP_PAGE_CG) render_gallery(app);
            else if (app->page == APP_PAGE_CONFIRM) render_slots(app, true);
            else if (app->page == APP_PAGE_ERROR) to_title(app);
            else if (app->page == APP_PAGE_TITLE) { app->about_page = 0; render_about(app); }
            else back_to_source(app);
        } else if (btn == 0) {
            if (app->page == APP_PAGE_GAME) {
                app->ff_held = true;
                app->key_action[btn] = 1;
            }
        } else if (btn == 1) {
            if (app->page == APP_PAGE_GAME) {
                app->auto_enabled = !app->auto_enabled;
                app->auto_accum_ms = 0;
                set_notice(app, "自动播放：%s", app->auto_enabled ? "开" : "关");
            }
        }
        break;
    case BSP_BTN_RELEASE:
        if (btn == 0) app->ff_held = false;
        break;
    case BSP_BTN_CLICK:
        if (app->key_action[btn] || app->key_long[btn]) {
            app->key_action[btn] = 0;
            app->key_long[btn] = 0;
        } else {
            handle_click(app, btn);
        }
        break;
    default:
        break;
    }
    app_unlock();
}

// ---------------------------------------------------------------- tick
static bool flush_unlock(gal_app_t *app)
{
    if (!app->unlock.dirty) return true;
    uint8_t blob[GAL_UNLOCK_BLOB];
    const size_t len = gal_unlock_encode(&app->unlock, blob, sizeof(blob));
    if (!len || !gal_store_save_unlock(&app->store, blob, len)) {
        app->unlock_retry_ms = app->last_now_ms + UNLOCK_RETRY_MS;
        return false;
    }
    app->unlock.dirty = 0;
    return true;
}

static bool flush_progress(gal_app_t *app)
{
    if (!app->progress_dirty) return true;
    if (!gal_store_save_progress(&app->store, app->progress)) {
        app->progress_retry_ms = app->last_now_ms + PROGRESS_RETRY_MS;
        return false;
    }
    app->progress_dirty = false;
    return true;
}

void gal_app_tick(gal_app_t *app, uint32_t now_ms)
{
    if (!app || !app_lock()) return;
    uint32_t dt = app->last_tick_ms ? now_ms - app->last_tick_ms : 0;
    if (dt > 500) dt = 500;
    app->last_tick_ms = now_ms;
    app->last_now_ms = now_ms;
    gal_ui_tick(&app->ui, (int)now_ms);

    if (app->typing_active && app->page == APP_PAGE_GAME) {
        app->text_accum_ms += dt;
        const int add = (int)(app->text_accum_ms / TEXT_SPEED_MS);
        if (add > 0) {
            app->text_accum_ms %= TEXT_SPEED_MS;
            app->typing_index += add;
            if (app->typing_index >= app->typing_total) typing_finish(app);
            else typing_apply(app);
        }
    }

    if (app->page == APP_PAGE_GAME && app->vm_started && !app->manual_boundary &&
        app->frame.link_count == 0) {
        if (app->ff_held) {
            app->ff_accum_ms += dt;
            if (app->ff_accum_ms >= FF_MS) {
                app->ff_accum_ms = 0;
                advance_text(app);
            }
        } else if (app->auto_enabled && !app->typing_active) {
            app->auto_accum_ms += dt;
            const int speed = app->settings.auto_speed;
            uint32_t delay = speed == 0 ? 2600u : speed == 1 ? 1500u : 800u;
            if (app->typing_total > 60) {
                uint32_t extra = (uint32_t)(app->typing_total - 60) * 30u;
                if (extra > 5000u) extra = 5000u;
                delay += extra;
            }
            if (app->auto_accum_ms >= delay) {
                app->auto_accum_ms = 0;
                advance_text(app);
            }
        }
    } else {
        app->auto_accum_ms = 0;
        app->ff_accum_ms = 0;
    }

    if (app->image_dirty && now_ms >= app->image_retry_ms) ensure_page_art(app);
    if (app->unlock.dirty && now_ms >= app->unlock_retry_ms) (void)flush_unlock(app);
    if (app->progress_dirty && now_ms >= app->progress_retry_ms) (void)flush_progress(app);
    if (app->autosave_due && now_ms >= app->autosave_retry_ms) {
        (void)save_auto(app, false);
    }
    app_unlock();
}

void gal_app_set_battery(gal_app_t *app, int percent)
{
    if (!app || !app_lock()) return;
    app->battery_percent = percent;
    if (app->page == APP_PAGE_TITLE) render_title(app);
    else if (app->page == APP_PAGE_GAME) render_game_page(app);
    app_unlock();
}

void gal_app_flush(gal_app_t *app)
{
    if (!app) return;
    // 睡前/后台调用不在 LVGL 任务里:临时禁止 notice 触碰界面。
    const bool ready = app->ui_ready;
    app->ui_ready = false;
    (void)save_auto(app, true);
    (void)flush_unlock(app);
    (void)flush_progress(app);
    app->ui_ready = ready;
}

const char *gal_app_page_name(app_page_t page)
{
    switch (page) {
    case APP_PAGE_TITLE: return "title";
    case APP_PAGE_GAME: return "game";
    case APP_PAGE_MENU: return "menu";
    case APP_PAGE_CHOICE: return "choice";
    case APP_PAGE_HISTORY: return "history";
    case APP_PAGE_SAVE: return "save";
    case APP_PAGE_LOAD: return "load";
    case APP_PAGE_SETTINGS: return "settings";
    case APP_PAGE_GALLERY: return "gallery";
    case APP_PAGE_CG: return "cg";
    case APP_PAGE_ROUTES: return "routes";
    case APP_PAGE_CHAPTERS: return "chapters";
    case APP_PAGE_BOOK: return "book";
    case APP_PAGE_BOOK_DETAIL: return "book_detail";
    case APP_PAGE_ENDING: return "ending";
    case APP_PAGE_ABOUT: return "about";
    case APP_PAGE_ERROR: return "error";
    case APP_PAGE_CONFIRM: return "confirm";
    default: return "?";
    }
}

bool gal_app_init(gal_app_t *app, const gal_pack_t *pack,
                  uint16_t *art_px, uint16_t *strip_px, uint8_t *scratch, uint32_t scratch_size)
{
    if (!app || !pack || !art_px || !strip_px || !scratch) return false;
    memset(app, 0, sizeof(*app));
    app->pack = pack;
    app->battery_percent = -1;
    app->art_bg = GAL_NONE;
    app->art_bodies = 0xFFFFFFFFu;
    app->text_chapter = 0xFFFF;
    app->text_cursor = 0xFFFF;
    app->settings.text_speed = 1;
    app->settings.auto_speed = 1;
    app->settings.panel_opa = 2;
    app->settings.version = GAL_SETTINGS_VERSION;
    gal_store_init(&app->store);
    (void)gal_store_load_settings(&app->store, &app->settings);
    (void)gal_store_load_progress(&app->store, &app->progress);
    gal_unlock_init(&app->unlock);
    bool unlock_ok = true;
    uint8_t blob[GAL_UNLOCK_BLOB];
    size_t len = 0;
    if (gal_store_load_unlock(&app->store, blob, sizeof(blob), &len)) {
        unlock_ok = gal_unlock_decode(&app->unlock, blob, len);
    }
    gal_vm_init(&app->vm, pack);
    if (!app_lock()) return false;
    lv_obj_t *root = lv_screen_active();
    if (!gal_image_init(&app->image, root, art_px, strip_px, scratch, scratch_size) ||
        !gal_ui_init(&app->ui, root)) {
        app_unlock();
        return false;
    }
    app->ui_ready = true;
    refresh_auto_summary(app);
    render_title(app);
    if (!app->store.ready) set_notice(app, "NVS 不可用：本次不会保存");
    else if (!unlock_ok) set_notice(app, "解锁记录损坏，已忽略（不会清除）");
    app_unlock();
    return true;
}
