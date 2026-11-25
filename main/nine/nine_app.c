// 9-nine- 九次九日九重色 reader. See nine_app.h for the key contract.
#include "nine_app.h"
#include <stdio.h>

#include <stdio.h>
#include <string.h>

#include "bsp_battery.h"
#include "bsp_display.h"
#include "gal_text.h"
#include "nine_image.h"

LV_FONT_DECLARE(nine_cjk_16);
LV_FONT_DECLARE(nine_cjk_20);

#define NINE_TEXT_MAX_BYTES 2048
#define NINE_AUTO_BASE_MS 1500u
#define NINE_AUTO_WRITE_MS 2000u
#define NINE_NOTICE_MS 1900u
#define NINE_BATTERY_MS 10000u
#define NINE_CG_TILES 8

static const char *const TITLE_ROWS[] = {
    "开始新游戏", "继续游戏", "读取存档", "设置", "鉴赏室", "关于",
};
static const char *const MENU_ROWS[] = {
    "保存进度", "读取存档", "历史文本", "跳到下一章", "设置", "返回标题",
};
static const char *const PREVIEW_MENU_ROWS[] = {
    "历史文本", "设置", "返回章节",
};
static const char *const SETTINGS_ROWS[] = {
    "文本速度", "字号", "自动阅读", "对话框透明度", "标题背景", "音乐",
};
static const char *const GALLERY_ROWS[] = {
    "CG 鉴赏", "章节回顾", "OP 动画", "音乐鉴赏",
};
static const char *const ABOUT_BODY =
    "9-nine- 九次九日九重色\n"
    "Episode 1 · 完全离线移植\n"
    "\n"
    "本作包含 17 个章节、74 个剧情节点、8049 句正文、\n"
    "5 组共 9 个选项、2 个结局（一周目轮回终点与\n"
    "九條都真结局）。全部正文与分支来自固定提交的\n"
    "公开 RW5 移植工程，未截断剧情或路线。\n"
    "\n"
    "操作：短按 OK/上/下推进；长按 OK 打开菜单；\n"
    "按住上快进；长按下切换自动阅读；二级页面长按\n"
    "OK 返回。\n"
    "\n"
    "资源：320x180 舞台（源 432x243 等比 16:9）、\n"
    "JPEG 背景、8 位调色板立绘与表情差分、\n"
    "8 kHz 单声道 IMA ADPCM 音乐。OP 无视频解码器，\n"
    "以 3 fps 图片序列 + 原曲播放，详见 README。\n"
    "\n"
    "来源：github.com/xuanyide01/9-nine-rw5\n"
    "提交：6a8907a5ae576f4377abe4b74f15bd9af13efe07\n"
    "原作版权归 Palette 所有；本移植仅使用该公开\n"
    "工程中的已加工文本与转码素材。";

// ---------------------------------------------------------------- helpers
static uint8_t speed_ms(const nine_app_t *app)
{
    static const uint8_t table[4] = {60, 35, 20, 0};
    uint8_t idx = app->cfg.text_speed > 3 ? 1 : app->cfg.text_speed;
    return table[idx];
}
static int units_per_line(const nine_app_t *app)
{
    return app->cfg.font_large ? 28 : 36;
}
static int lines_per_page(const nine_app_t *app)
{
    return app->cfg.font_large ? 2 : 3;
}
static void notice(nine_app_t *app, const char *text)
{
    snprintf(app->notice, sizeof(app->notice), "%.*s",
             (int)(sizeof(app->notice) - 1), text ? text : "");
    app->notice_ms = NINE_NOTICE_MS;
    gal_ui_set_notice(&app->ui, app->notice);
}
static void set_page(nine_app_t *app, gal_page_t page)
{
    if (page != GAL_PAGE_GAME) app->ff_hold = false;
    // Leaving the reading layer clears transient modes. CHOICE keeps auto-play
    // so choosing an option resumes the same auto session; every other page
    // (menu, save, gallery, ...) turns it off instead of hiding a running clock.
    if (page != GAL_PAGE_GAME && page != GAL_PAGE_CHOICE) {
        app->auto_mode = false;
        app->auto_accum = 0;
        gal_ui_set_mode(&app->ui, false);
    }
    app->page = page;
    gal_ui_show_page(&app->ui, page);
    const bool art = page == GAL_PAGE_GAME || page == GAL_PAGE_CHOICE ||
                     page == GAL_PAGE_MENU || page == GAL_PAGE_CG ||
                     page == GAL_PAGE_OPENING;
    nine_image_set_visible(art);
}
static bool bit_get(const uint8_t *bits, int i)
{
    return (bits[i / 8] & (uint8_t)(1u << (i % 8))) != 0;
}
static bool bit_set(uint8_t *bits, int i)
{
    uint8_t mask = (uint8_t)(1u << (i % 8));
    if (bits[i / 8] & mask) return false;
    bits[i / 8] |= mask;
    return true;
}
static void unlock_store(nine_app_t *app)
{
    (void)nine_store_unlock_save(&app->store, app->unlock);
}
static void cfg_store(nine_app_t *app)
{
    (void)nine_store_cfg_save(&app->store, &app->cfg);
    (void)nine_store_sound_save(&app->store, app->cfg.sound);
}
static void progress_store(nine_app_t *app)
{
    uint8_t buf[NINE_PROGRESS_RECORD_MAX];
    size_t n = nine_vm_progress_encode(&app->vm, buf, sizeof(buf));
    if (n) (void)nine_store_progress_save(&app->store, buf, n);
    nine_vm_clear_progress_dirty(&app->vm);
}

// Item 1 is the loop ending, item 2 the true ending (pack order).
static bool ending_reached(const nine_app_t *app, bool is_true)
{
    return nine_vm_ending_reached(&app->vm, is_true ? "true" : "loop1");
}
static int title_bg_unlocked(const nine_app_t *app, int idx)
{
    const bool loop = ending_reached(app, false) || nine_vm_ending_reached(&app->vm, "loop_my");
    const bool truth = ending_reached(app, true);
    if (idx <= 0) return 1;
    if (idx == 1) return loop || truth;
    return loop && truth;
}
static int title_bg_selected(const nine_app_t *app)
{
    int idx = app->cfg.title_bg > 2 ? 0 : app->cfg.title_bg;
    while (idx > 0 && !title_bg_unlocked(app, idx)) idx--;
    return idx;
}
static void set_title_art(nine_app_t *app)
{
    uint32_t image = NINE_NONE;
    int idx = title_bg_selected(app);
    if (nine_image_title((unsigned)idx, &image) && image != NINE_NONE) {
        uint32_t w = 0, h = 0;
        const uint16_t *px = NULL;
        if (nine_image_raw_asset(image, &w, &h, &px)) {
            lv_image_dsc_t *d = &app->title_dsc[idx];
            memset(d, 0, sizeof(*d));
            d->header.magic = LV_IMAGE_HEADER_MAGIC;
            d->header.cf = LV_COLOR_FORMAT_RGB565;
            d->header.w = (uint16_t)w;
            d->header.h = (uint16_t)h;
            d->header.stride = (uint16_t)(w * 2);
            d->data_size = w * h * 2;
            d->data = (const uint8_t *)px;
            gal_ui_set_title_image(&app->ui, d);
            return;
        }
    }
    gal_ui_set_title_image(&app->ui, NULL);
}

// ---------------------------------------------------------------- music
static bool title_area(const nine_app_t *app)
{
    switch (app->page) {
    case GAL_PAGE_TITLE:
    case GAL_PAGE_GALLERY:
    case GAL_PAGE_CG_LIST:
    case GAL_PAGE_CG:
    case GAL_PAGE_CHAPTER:
    case GAL_PAGE_MUSIC:
    case GAL_PAGE_ABOUT:
        return true;
    case GAL_PAGE_SETTINGS:
    case GAL_PAGE_LOAD:
        return app->return_page == GAL_PAGE_TITLE;
    default:
        return false;
    }
}
static void music_sync(nine_app_t *app)
{
    if (!app->media_ok || !app->cfg.sound) {
        nine_media_stop();
        return;
    }
    if (app->op_active) return;   // OP owns the opening track
    if (title_area(app)) {
        if (nine_media_current_track() != 0 || !nine_media_is_playing())
            (void)nine_media_play(0);
    } else {
        nine_media_stop();
    }
}

// ---------------------------------------------------------------- view
static void render_page(nine_app_t *app);
static void handle_view(nine_app_t *app, uint16_t want_page);
static void finish_ending(nine_app_t *app);
static void auto_write_mark(nine_app_t *app);
static void fatal_screen(nine_app_t *app, const char *message);
static void open_gallery(nine_app_t *app);
static void fill_chapter_rows(nine_app_t *app)
{
    int total = (int)app->pack.chapter_count;
    int top = app->chapter_sel - NINE_CHAPTER_ROWS / 2;
    if (top < 0) top = 0;
    if (total > NINE_CHAPTER_ROWS && top > total - NINE_CHAPTER_ROWS)
        top = total - NINE_CHAPTER_ROWS;
    int n = total - top;
    if (n > NINE_CHAPTER_ROWS) n = NINE_CHAPTER_ROWS;
    char labels[NINE_CHAPTER_ROWS][64];
    char values[NINE_CHAPTER_ROWS][8];
    const char *lab[NINE_CHAPTER_ROWS];
    const char *val[NINE_CHAPTER_ROWS];
    for (int i = 0; i < n; ++i) {
        nine_chapter_t c;
        if (!nine_pack_chapter(&app->pack, (uint32_t)(top + i), &c)) { n = i; break; }
        const char *t = nine_pack_string(&app->pack, c.title);
        snprintf(labels[i], sizeof(labels[i]), "%s", t ? t : "");
        values[i][0] = 0;
        lab[i] = labels[i];
        val[i] = values[i];
    }
    int local = app->chapter_sel - top;
    if (local < 0) local = 0;
    if (local >= n) local = n ? n - 1 : 0;
    gal_ui_set_rows(&app->ui, GAL_LIST_CHAPTER, lab, val, n, local);
    char hint[64];
    snprintf(hint, sizeof(hint), "第 %d/%d 章 · 上/下窗口 · 长按 OK 返回",
             app->chapter_sel + 1, total);
    gal_ui_set_chapter_info(&app->ui, "短按 OK 试读本章；试读不会写入通关记录。");
    gal_ui_set_action_hint(&app->ui, hint);
}
static void open_chapters(nine_app_t *app)
{
    app->return_page = GAL_PAGE_GALLERY;
    app->chapter_sel = 0;
    fill_chapter_rows(app);
    set_page(app, GAL_PAGE_CHAPTER);
    music_sync(app);
}
static void push_history(nine_app_t *app, const char *speaker, const char *text)
{
    if (!text || !text[0]) return;
    int i = app->history_head;
    snprintf(app->history[i].speaker, sizeof(app->history[i].speaker), "%.*s",
             (int)(sizeof(app->history[i].speaker) - 1), speaker ? speaker : "");
    snprintf(app->history[i].text, sizeof(app->history[i].text), "%.*s",
             (int)(sizeof(app->history[i].text) - 1), text);
    app->history_head = (app->history_head + 1) % NINE_HISTORY_ROWS;
    if (app->history_count < NINE_HISTORY_ROWS) app->history_count++;
}
static void history_row(const nine_app_t *app, int back, nine_history_row_t *out)
{
    int idx = (app->history_head - 1 - back + NINE_HISTORY_ROWS * 2) % NINE_HISTORY_ROWS;
    *out = app->history[idx];
}
static void mark_art(nine_app_t *app, uint32_t visual)
{
    if (app->vm.state.preview) return;   // a preview never pollutes unlock state
    bool changed = false;
    nine_visual_record_t v;
    if (!nine_pack_visual(&app->pack, visual, &v)) return;
    for (uint32_t i = 0; i < v.layer_count; ++i) {
        nine_layer_record_t l;
        if (!nine_pack_layer(&app->pack, v.first_layer + i, &l)) return;
        for (uint32_t g = 0; g < nine_pack_gallery_count(&app->pack); ++g) {
            nine_gallery_record_t rec;
            if (!nine_pack_gallery(&app->pack, g, &rec)) break;
            if (rec.image == l.image && l.opacity && bit_set(app->unlock, (int)rec.group))
                changed = true;
        }
    }
    if (changed) unlock_store(app);
}

static void load_text_frame(nine_app_t *app, const nine_frame_t *f)
{
    const char *text = nine_pack_text(&app->pack, f);
    const char *speaker = nine_pack_string(&app->pack, f->speaker);
    if (!text) {
        fatal_screen(app, "剧情正文校验失败");
        return;
    }
    snprintf(app->text, sizeof(app->text), "%.*s", (int)(sizeof(app->text) - 1), text);
    snprintf(app->speaker, sizeof(app->speaker), "%.*s",
             (int)(sizeof(app->speaker) - 1), speaker ? speaker : "");
    app->page_count = gal_text_pages(app->text, units_per_line(app), lines_per_page(app),
                                     app->page_offsets, GAL_TEXT_MAX_PAGES + 1);
    if (app->page_count > GAL_TEXT_MAX_PAGES) app->page_count = GAL_TEXT_MAX_PAGES;
    if (app->page_count < 1) app->page_count = 1;
    if (f->visual != app->visual) {
        if (!nine_image_show_visual(f->visual)) {
            fatal_screen(app, "画面解码失败");
            return;
        }
        app->visual = f->visual;
        mark_art(app, f->visual);
    }
    if (!app->vm.state.preview) {
        nine_node_t node;
        if (nine_pack_node(&app->pack, app->vm.state.node, &node) &&
            node.chapter != NINE_NONE && bit_set(app->unlock, 32 + (int)node.chapter))
            unlock_store(app);
    }
    if (app->hist_frame != app->vm.view.frame) {
        push_history(app, app->speaker, app->text);
        app->hist_frame = app->vm.view.frame;
    }
    memset(app->page_text, 0, sizeof(app->page_text));
    if (app->text_page >= app->page_count) app->text_page = (uint16_t)(app->page_count - 1);
    render_page(app);
}

static void render_page(nine_app_t *app)
{
    if (app->page_count < 1) app->page_count = 1;
    if (app->text_page >= app->page_count) app->text_page = (uint16_t)(app->page_count - 1);
    uint32_t a = app->page_offsets[app->text_page];
    uint32_t b = app->page_offsets[app->text_page + 1];
    if (b < a) b = a;
    size_t n = b - a;
    if (n >= sizeof(app->page_text)) n = sizeof(app->page_text) - 1;
    memcpy(app->page_text, app->text + a, n);
    app->page_text[n] = '\0';
    gal_ui_set_text(&app->ui, app->speaker[0] ? app->speaker : NULL, app->page_text,
                    speed_ms(app), NULL, NULL);
    if (app->ff_hold) gal_ui_show_full_text(&app->ui);
    gal_ui_set_mode(&app->ui, app->auto_mode && app->page == GAL_PAGE_GAME);
    auto_write_mark(app);
}

static void render_choice(nine_app_t *app)
{
    gal_option_t opts[NINE_CHOICE_MAX];
    memset(opts, 0, sizeof(opts));
    const nine_view_t *v = &app->vm.view;
    int n = v->choice_count;
    for (int i = 0; i < n; ++i) {
        nine_edge_t e;
        if (!nine_pack_edge(&app->pack, true, v->choices[i], &e)) {
            n = i;
            break;
        }
        opts[i].label = nine_pack_string(&app->pack, e.label);
        opts[i].flag_set = GAL_UNLOCK_NONE;
        opts[i].require_flag = GAL_UNLOCK_NONE;
    }
    if (app->choice_sel >= n) app->choice_sel = n ? n - 1 : 0;
    gal_ui_set_choice(&app->ui, app->text[0] ? app->page_text : "", opts, n, app->choice_sel);
    set_page(app, GAL_PAGE_CHOICE);
}

static void finish_ending(nine_app_t *app)
{
    app->ff_hold = false;
    app->auto_mode = false;
    int idx = nine_vm_ending_reached(&app->vm, "true") ? 1 : 0;
    if (bit_set(app->unlock, 64 + idx)) unlock_store(app);
    app->cfg.title_bg = (uint8_t)(idx ? 2 : (app->cfg.title_bg < 1 ? 1 : app->cfg.title_bg));
    cfg_store(app);
    progress_store(app);
    app->auto_dirty = false;
    (void)nine_store_auto_clear(&app->store);
    nine_ending_t e;
    if (nine_pack_ending(&app->pack, (uint32_t)idx, &e)) {
        const char *name = nine_pack_string(&app->pack, e.name);
        const char *desc = nine_pack_string(&app->pack, e.description);
        gal_ui_set_ending(&app->ui, idx ? "TRUE END" : "END", name ? name : "",
                          desc ? desc : "");
    } else {
        gal_ui_set_ending(&app->ui, "END", "结局", "");
    }
    nine_image_set_visible(false);
    set_page(app, GAL_PAGE_ENDING);
    music_sync(app);
}

static void start_op(nine_app_t *app, bool extra, gal_page_t ret)
{
    if (!app->media_ok) {
        notice(app, "OP 动画不可用");
        if (extra) {
            open_gallery(app);
        } else {
            const nine_view_t *v = nine_vm_advance(&app->vm);
            app->text_page = 0;
            handle_view(app, 0);
            (void)v;
        }
        return;
    }
    app->op_active = true;
    app->op_extra = extra;
    app->op_return = ret;
    app->op_frame = 0;
    app->ff_hold = false;
    nine_media_stop();
    (void)nine_media_play_op(1);
    nine_image_set_visible(true);
    gal_ui_set_cg(&app->ui, "OP MOVIE", "", "短按 OK 跳过", NULL);
    set_page(app, GAL_PAGE_OPENING);
}
static void finish_op(nine_app_t *app)
{
    if (!app->op_active) return;
    app->op_active = false;
    nine_media_stop();
    if (app->op_extra) {
        open_gallery(app);
        return;
    }
    const nine_view_t *v = nine_vm_advance(&app->vm);
    app->text_page = 0;
    app->auto_accum = 0;
    handle_view(app, 0);
    (void)v;
}

static void handle_view(nine_app_t *app, uint16_t want_page)
{
    if (nine_vm_progress_dirty(&app->vm)) progress_store(app);
    switch (app->vm.view.kind) {
    case NINE_VM_TEXT: {
        nine_frame_t f;
        if (!nine_vm_current_frame(&app->vm, &f)) {
            fatal_screen(app, "剧情帧读取失败");
            return;
        }
        app->text_page = want_page;
        set_page(app, GAL_PAGE_GAME);
        load_text_frame(app, &f);
        app->auto_dirty = true;
        if (app->ff_hold) gal_ui_show_full_text(&app->ui);
        break;
    }
    case NINE_VM_CHOICE:
        render_choice(app);
        app->auto_dirty = true;
        break;
    case NINE_VM_END:
        if (app->vm.state.preview) {
            fill_chapter_rows(app);
            set_page(app, GAL_PAGE_CHAPTER);
            music_sync(app);
            notice(app, "试读结束");
        } else {
            finish_ending(app);
        }
        break;
    case NINE_VM_OP:
        start_op(app, false, GAL_PAGE_GAME);
        break;
    default:
        fatal_screen(app, app->vm.view.error ? app->vm.view.error : "剧情连接错误");
        break;
    }
}

static void refresh_slots(nine_app_t *app)
{
    nine_store_slot_scan(&app->store, &app->pack, app->slots);
}
static bool any_slot(const nine_app_t *app)
{
    for (int i = 0; i < NINE_SLOTS; ++i) if (app->slots[i].occupied) return true;
    return false;
}
static void write_auto(nine_app_t *app)
{
    if (!app->ready || app->vm.state.preview || app->page == GAL_PAGE_ENDING || app->fatal)
        return;
    size_t n = nine_vm_save_encode(&app->vm, app->text_page, app->auto_blob,
                                   sizeof(app->auto_blob));
    if (!n) return;
    app->auto_size = n;
    (void)nine_store_auto_save(&app->store, app->auto_blob, n);
    app->auto_dirty = false;
    app->auto_write_ms = 0;
}
static void auto_write_mark(nine_app_t *app)
{
    if (app->vm.state.preview) return;
    app->auto_dirty = true;
    if (app->auto_write_ms > NINE_AUTO_WRITE_MS) write_auto(app);
}
static void auto_progress_load(nine_app_t *app)
{
    size_t n = 0;
    if (!nine_store_auto_load(&app->store, app->auto_blob, sizeof(app->auto_blob), &n)) {
        app->auto_size = 0;
        return;
    }
    uint16_t node, page;
    uint32_t index;
    bool preview;
    if (!nine_vm_save_peek(&app->pack, app->auto_blob, n, &node, &index, &page, &preview) ||
        preview) {
        app->auto_size = 0;
        return;
    }
    app->auto_size = n;
}

static void show_title(nine_app_t *app)
{
    // Only leave an active reading/menu session. START->TITLE, ending, gallery,
    // about and the title-owned settings/save pages must not overwrite the
    // already loaded auto record with the initial n0 VM state.
    if (app->page == GAL_PAGE_GAME || app->page == GAL_PAGE_CHOICE ||
        app->page == GAL_PAGE_MENU) {
        write_auto(app);
    }
    app->auto_mode = false;
    app->ff_hold = false;
    set_title_art(app);
    gal_ui_set_brand(&app->ui, "", "");
    gal_ui_set_rows(&app->ui, GAL_LIST_TITLE, TITLE_ROWS, NULL, 6, app->title_sel);
    gal_ui_set_row_enabled(&app->ui, GAL_LIST_TITLE, 0, true);
    gal_ui_set_row_enabled(&app->ui, GAL_LIST_TITLE, 1, app->auto_size > 0);
    gal_ui_set_row_enabled(&app->ui, GAL_LIST_TITLE, 2, any_slot(app));
    for (int i = 3; i < 6; ++i) gal_ui_set_row_enabled(&app->ui, GAL_LIST_TITLE, i, true);
    set_page(app, GAL_PAGE_TITLE);
    music_sync(app);
}
static void to_title(nine_app_t *app)
{
    app->auto_mode = false;
    app->ff_hold = false;
    app->op_active = false;
    nine_media_stop();
    app->title_sel = 0;
    show_title(app);
}
static void start_new(nine_app_t *app)
{
    nine_vm_start(&app->vm, NINE_NODE_NONE, false);
    app->text_page = 0;
    app->visual = NINE_NONE;
    app->history_count = app->history_head = 0;
    app->hist_frame = NINE_NONE;
    app->auto_mode = app->cfg.auto_play != 0;
    app->auto_accum = 0;
    app->ff_hold = false;
    set_page(app, GAL_PAGE_GAME);
    music_sync(app);
    handle_view(app, 0);
    write_auto(app);
}

static void fill_settings(nine_app_t *app)
{
    static const char *const speeds[] = {"慢", "标准", "快", "瞬间"};
    static const char *const trans[] = {"10%", "15%", "20%", "25%", "30%",
                                        "35%", "40%", "45%", "50%", "55%"};
    char values[6][40] = {{0}};
    char tbuf[20];
    snprintf(values[0], sizeof(values[0]), "%s",
             speeds[app->cfg.text_speed > 3 ? 1 : app->cfg.text_speed]);
    snprintf(values[1], sizeof(values[1]), "%s", app->cfg.font_large ? "20px" : "16px");
    snprintf(values[2], sizeof(values[2]), "%s", app->cfg.auto_play ? "开" : "关");
    int ti = (app->cfg.transparency - 10) / 5;
    if (ti < 0) ti = 0;
    if (ti > 9) ti = 9;
    snprintf(values[3], sizeof(values[3]), "%s", trans[ti]);
    int bg = title_bg_selected(app);
    snprintf(tbuf, sizeof(tbuf), "主视觉 %u", (unsigned)(bg + 1));
    snprintf(values[4], sizeof(values[4]), "%s", tbuf);
    snprintf(values[5], sizeof(values[5]), "%s", app->cfg.sound ? "开" : "关");
    const char *labels[6] = {SETTINGS_ROWS[0], SETTINGS_ROWS[1], SETTINGS_ROWS[2],
                             SETTINGS_ROWS[3], SETTINGS_ROWS[4], SETTINGS_ROWS[5]};
    const char *vals[6] = {values[0], values[1], values[2], values[3],
                           values[4], values[5]};
    gal_ui_set_rows(&app->ui, GAL_LIST_SETTINGS, labels, vals, 6, app->settings_sel);
    gal_ui_set_row_enabled(&app->ui, GAL_LIST_SETTINGS, 4, title_bg_unlocked(app, 1));
}

static void open_settings(nine_app_t *app, gal_page_t ret)
{
    app->return_page = ret;
    app->settings_sel = 0;
    fill_settings(app);
    set_page(app, GAL_PAGE_SETTINGS);
    music_sync(app);
}

static void fill_slots(nine_app_t *app, bool save_mode)
{
    char labels[NINE_PAGE_ROWS][28];
    char values[NINE_PAGE_ROWS][72];
    for (int i = 0; i < NINE_PAGE_ROWS; ++i) {
        int idx = app->slot_page * NINE_PAGE_ROWS + i;
        snprintf(labels[i], sizeof(labels[i]), "存档 %02d", idx + 1);
        if (idx >= NINE_SLOTS) {
            snprintf(values[i], sizeof(values[i]), "-");
        } else if (!app->slots[idx].occupied) {
            snprintf(values[i], sizeof(values[i]), "空");
        } else if (app->slots[idx].chapter == NINE_NODE_NONE) {
            snprintf(values[i], sizeof(values[i]), "损坏");
        } else {
            nine_chapter_t c;
            if (nine_pack_chapter(&app->pack, app->slots[idx].chapter, &c)) {
                const char *t = nine_pack_string(&app->pack, c.title);
                snprintf(values[i], sizeof(values[i]), "%s · 第%u句",
                         t ? t : "章节", (unsigned)app->slots[idx].index + 1);
            } else {
                snprintf(values[i], sizeof(values[i]), "损坏");
            }
        }
    }
    const char *lab[NINE_PAGE_ROWS] = {labels[0], labels[1], labels[2], labels[3], labels[4]};
    const char *val[NINE_PAGE_ROWS] = {values[0], values[1], values[2], values[3], values[4]};
    gal_list_t list = save_mode ? GAL_LIST_SAVE : GAL_LIST_LOAD;
    gal_ui_set_rows(&app->ui, list, lab, val, NINE_PAGE_ROWS,
                    save_mode ? app->save_sel : app->load_sel);
    gal_ui_set_save_preview(&app->ui, "上/下到边缘翻页 · 短按 OK 选择");
    char footer[48];
    snprintf(footer, sizeof(footer), "第 %d/%d 页 · 长按 OK 返回", app->slot_page + 1,
             (NINE_SLOTS + NINE_PAGE_ROWS - 1) / NINE_PAGE_ROWS);
    gal_ui_set_footer(&app->ui, save_mode ? GAL_PAGE_SAVE : GAL_PAGE_LOAD, footer);
}
static void open_slots(nine_app_t *app, bool save_mode)
{
    refresh_slots(app);
    app->return_page = app->page == GAL_PAGE_MENU ? app->menu_return_page : GAL_PAGE_TITLE;
    app->slot_page = 0;
    if (save_mode) app->save_sel = 0; else app->load_sel = 0;
    fill_slots(app, save_mode);
    set_page(app, save_mode ? GAL_PAGE_SAVE : GAL_PAGE_LOAD);
    music_sync(app);
}

static void open_menu(nine_app_t *app)
{
    if (app->page == GAL_PAGE_GAME || app->page == GAL_PAGE_CHOICE)
        app->menu_return_page = app->page;
    app->return_page = app->menu_return_page;
    app->menu_sel = 0;
    const bool preview = app->vm.state.preview;
    int count = preview ? 3 : 6;
    const char *const *rows = preview ? PREVIEW_MENU_ROWS : MENU_ROWS;
    gal_ui_set_rows(&app->ui, GAL_LIST_MENU, rows, NULL, count, app->menu_sel);
    set_page(app, GAL_PAGE_MENU);
    music_sync(app);
}

static void open_gallery(nine_app_t *app)
{
    app->return_page = GAL_PAGE_TITLE;
    app->gallery_sel = 0;
    gal_ui_set_rows(&app->ui, GAL_LIST_GALLERY, GALLERY_ROWS, NULL, 4, 0);
    set_page(app, GAL_PAGE_GALLERY);
    music_sync(app);
}

static uint32_t gallery_groups(const nine_app_t *app)
{
    uint32_t max = 0;
    for (uint32_t i = 0; i < nine_pack_gallery_count(&app->pack); ++i) {
        nine_gallery_record_t g;
        if (!nine_pack_gallery(&app->pack, i, &g)) break;
        if (g.group + 1 > max) max = g.group + 1;
    }
    return max;
}
static int cg_page_of(int group) { return group / NINE_CG_TILES; }
static void fill_cg_page(nine_app_t *app)
{
    uint32_t groups = gallery_groups(app);
    int page = app->cg_page;
    int first = page * NINE_CG_TILES;
    int n = 0;
    for (int i = 0; i < NINE_CG_TILES && first + i < (int)groups; ++i) {
        int group = first + i;
        uint32_t thumb = NINE_NONE;
        for (uint32_t g = 0; g < nine_pack_gallery_count(&app->pack); ++g) {
            nine_gallery_record_t rec;
            if (!nine_pack_gallery(&app->pack, g, &rec)) break;
            if ((int)rec.group == group) { thumb = rec.thumb; break; }
        }
        uint32_t w = 0, h = 0;
        const uint16_t *px = NULL;
        bool ok = thumb != NINE_NONE && nine_image_raw_asset(thumb, &w, &h, &px);
        lv_image_dsc_t *d = &app->thumb_dsc[i];
        if (ok) {
            memset(d, 0, sizeof(*d));
            d->header.magic = LV_IMAGE_HEADER_MAGIC;
            d->header.cf = LV_COLOR_FORMAT_RGB565;
            d->header.w = (uint16_t)w;
            d->header.h = (uint16_t)h;
            d->header.stride = (uint16_t)(w * 2);
            d->data_size = w * h * 2;
            d->data = (const uint8_t *)px;
        }
        snprintf(app->cg_labels[i], sizeof(app->cg_labels[i]), "CG %02d", group + 1);
        app->cg_items[i].label = app->cg_labels[i];
        app->cg_items[i].caption = "";
        app->cg_items[i].unlock_event = GAL_EVENT_NONE;
        app->cg_items[i].image = ok ? d : NULL;
        app->cg_items[i].unlocked = bit_get(app->unlock, group);
        n++;
    }
    for (int i = n; i < NINE_CG_TILES; ++i) memset(&app->cg_items[i], 0, sizeof(app->cg_items[i]));
    int local = app->cg_sel - first;
    if (local < 0) local = 0;
    gal_ui_set_gallery(&app->ui, app->cg_items, n, local);
    char footer[48];
    uint32_t unlocked = 0;
    for (uint32_t g = 0; g < groups; ++g) if (bit_get(app->unlock, (int)g)) unlocked++;
    snprintf(footer, sizeof(footer), "第 %d/%d 页 · 已解锁 %u/%u", page + 1,
             (int)((groups + NINE_CG_TILES - 1) / NINE_CG_TILES), (unsigned)unlocked,
             (unsigned)groups);
    gal_ui_set_footer(&app->ui, GAL_PAGE_CG_LIST, footer);
}

static uint32_t variant_image(const nine_app_t *app, int group, int variant,
                              char *name, size_t cap)
{
    int seen = 0;
    for (uint32_t g = 0; g < nine_pack_gallery_count(&app->pack); ++g) {
        nine_gallery_record_t rec;
        if (!nine_pack_gallery(&app->pack, g, &rec)) break;
        if ((int)rec.group != group) continue;
        if (seen == variant) {
            const char *s = nine_pack_string(&app->pack, rec.name_str);
            snprintf(name, cap, "%s", s ? s : "");
            return rec.image;
        }
        seen++;
    }
    return NINE_NONE;
}
static int variant_count(const nine_app_t *app, int group)
{
    int n = 0;
    for (uint32_t g = 0; g < nine_pack_gallery_count(&app->pack); ++g) {
        nine_gallery_record_t rec;
        if (!nine_pack_gallery(&app->pack, g, &rec)) break;
        if ((int)rec.group == group) n++;
    }
    return n;
}
static void show_cg(nine_app_t *app)
{
    char name[48];
    uint32_t image = variant_image(app, app->cg_sel, app->cg_variant, name, sizeof(name));
    char idx[48];
    snprintf(idx, sizeof(idx), "CG %02d  %d/%d", app->cg_sel + 1, app->cg_variant + 1,
             variant_count(app, app->cg_sel) > 0 ? variant_count(app, app->cg_sel) : 1);
    if (image != NINE_NONE && !nine_image_show_asset(image)) notice(app, "CG 解码失败");
    gal_ui_set_cg(&app->ui, idx, "", name, NULL);
    set_page(app, GAL_PAGE_CG);
    nine_image_set_visible(true);
}

static void start_chapter_preview(nine_app_t *app, int chapter)
{
    nine_chapter_t c;
    if (!nine_pack_chapter(&app->pack, (uint32_t)chapter, &c)) return;
    nine_vm_start(&app->vm, c.node, true);
    app->text_page = 0;
    app->visual = NINE_NONE;
    app->auto_mode = false;
    app->ff_hold = false;
    app->history_count = app->history_head = 0;
    app->hist_frame = NINE_NONE;
    set_page(app, GAL_PAGE_GAME);
    music_sync(app);
    handle_view(app, 0);
}

static void open_music(nine_app_t *app)
{
    app->return_page = GAL_PAGE_GALLERY;
    app->music_sel = 0;
    uint32_t n = nine_media_track_count();
    if (n > 8) n = 8;
    char labels[8][24];
    char values[8][24];
    const char *lab[8];
    const char *val[8];
    for (uint32_t i = 0; i < n; ++i) {
        nine_media_track_t t;
        if (!nine_media_track_info(i, &t)) { n = i; break; }
        snprintf(labels[i], sizeof(labels[i]), "%s",
                 t.name && t.name[0] ? (strcmp(t.name, "title") ? "OP 主题曲" : "标题曲") : "曲目");
        snprintf(values[i], sizeof(values[i]), "%u:%02u", (unsigned)(t.duration_ms / 60000),
                 (unsigned)((t.duration_ms / 1000) % 60));
        lab[i] = labels[i];
        val[i] = values[i];
    }
    gal_ui_set_rows(&app->ui, GAL_LIST_MUSIC, lab, val, (int)n, app->music_sel);
    gal_ui_set_footer(&app->ui, GAL_PAGE_MUSIC, "短按 OK 播放/停止 · 长按返回");
    set_page(app, GAL_PAGE_MUSIC);
    music_sync(app);
}

static void open_about(nine_app_t *app)
{
    app->return_page = GAL_PAGE_TITLE;
    gal_ui_set_about(&app->ui, "9-nine-", "Episode 1", ABOUT_BODY);
    set_page(app, GAL_PAGE_ABOUT);
    music_sync(app);
}

static void fill_history_rows(nine_app_t *app)
{
    int total = app->history_count;
    if (app->history_off > total - NINE_HISTORY_WINDOW)
        app->history_off = total - NINE_HISTORY_WINDOW;
    if (app->history_off < 0) app->history_off = 0;
    int n = total - app->history_off;
    if (n > NINE_HISTORY_WINDOW) n = NINE_HISTORY_WINDOW;
    static char labels[NINE_HISTORY_WINDOW][200];
    static char values[NINE_HISTORY_WINDOW][8];
    const char *lab[NINE_HISTORY_WINDOW];
    const char *val[NINE_HISTORY_WINDOW];
    for (int i = 0; i < n; ++i) {
        nine_history_row_t r;
        history_row(app, app->history_off + i, &r);
        if (r.speaker[0]) snprintf(labels[i], sizeof(labels[i]), "%s：%s", r.speaker, r.text);
        else snprintf(labels[i], sizeof(labels[i]), "%s", r.text);
        values[i][0] = 0;
        lab[i] = labels[i];
        val[i] = values[i];
    }
    gal_ui_set_rows(&app->ui, GAL_LIST_HISTORY, lab, val, n, 0);
    char footer[64];
    snprintf(footer, sizeof(footer), "第 %d-%d/%d 句 · 上/下窗口",
             total ? app->history_off + 1 : 0, app->history_off + n, total);
    gal_ui_set_footer(&app->ui, GAL_PAGE_HISTORY, footer);
}
static void open_history(nine_app_t *app)
{
    app->return_page = app->page == GAL_PAGE_MENU ? app->menu_return_page : app->page;
    app->history_off = 0;
    fill_history_rows(app);
    set_page(app, GAL_PAGE_HISTORY);
    music_sync(app);
}
// ---------------------------------------------------------------- saves
static void save_slot(nine_app_t *app, int slot)
{
    uint8_t buf[NINE_SAVE_RECORD_MAX];
    size_t n = nine_vm_save_encode(&app->vm, app->text_page, buf, sizeof(buf));
    if (!n || !nine_store_slot_save(&app->store, slot, buf, n)) {
        notice(app, "保存失败");
        return;
    }
    refresh_slots(app);
    fill_slots(app, true);
    notice(app, "已保存");
}
static bool load_slot(nine_app_t *app, int slot)
{
    uint8_t buf[NINE_SAVE_RECORD_MAX];
    size_t n = 0;
    if (!nine_store_slot_load(&app->store, slot, buf, sizeof(buf), &n)) {
        notice(app, "存档读取失败");
        return false;
    }
    nine_state_t backup = app->vm.state;
    uint16_t page = 0;
    if (!nine_vm_save_decode(&app->vm, buf, n, &page)) {
        app->vm.state = backup;
        notice(app, "存档损坏，未应用");
        return false;
    }
    const nine_view_t *v = nine_vm_locate(&app->vm);
    if (!v || v->kind == NINE_VM_ERROR) {
        app->vm.state = backup;
        notice(app, "存档位置无效，未应用");
        return false;
    }
    app->text_page = page;
    app->visual = NINE_NONE;
    app->ff_hold = false;
    app->auto_mode = false;
    nine_vm_clear_progress_dirty(&app->vm);
    set_page(app, GAL_PAGE_GAME);
    music_sync(app);
    handle_view(app, page);
    write_auto(app);
    return true;
}

// ---------------------------------------------------------------- input
static void advance_reading(nine_app_t *app)
{
    if (app->page != GAL_PAGE_GAME) return;
    // A short press is a real advance: reveal the current pagination unit and
    // move to the next unit/line in the same press. Never consume a press just
    // to finish the typewriter.
    if (gal_ui_typing(&app->ui)) gal_ui_show_full_text(&app->ui);
    if (app->text_page + 1 < app->page_count) {
        app->text_page++;
        render_page(app);
        app->auto_accum = 0;
        app->idle_ms = 0;
        return;
    }
    const nine_view_t *v = nine_vm_advance(&app->vm);
    app->text_page = 0;
    app->auto_accum = 0;
    app->idle_ms = 0;
    if (!v) return;
    handle_view(app, 0);
}
static void ff_step(nine_app_t *app)
{
    if (app->page != GAL_PAGE_GAME) { app->ff_hold = false; return; }
    // Fast-forward is sentence-granular: skip the rest of the current sentence,
    // including all its pagination units, instead of replaying page-by-page.
    const nine_view_t *v = nine_vm_advance(&app->vm);
    app->text_page = 0;
    app->auto_accum = 0;
    app->idle_ms = 0;
    if (!v) { app->ff_hold = false; return; }
    handle_view(app, 0);
    if (app->page != GAL_PAGE_GAME) app->ff_hold = false;
}
static void toggle_auto(nine_app_t *app)
{
    app->ff_hold = false;
    app->auto_mode = !app->auto_mode;
    app->auto_accum = 0;
    if (app->auto_mode) gal_ui_show_full_text(&app->ui);
    gal_ui_set_mode(&app->ui, app->auto_mode);
    app->cfg.auto_play = app->auto_mode ? 1 : 0;
    cfg_store(app);
    notice(app, app->auto_mode ? "自动阅读：开" : "自动阅读：关");
}
static void start_ff(nine_app_t *app)
{
    if (app->page != GAL_PAGE_GAME) return;
    if (app->auto_mode) {
        app->auto_mode = false;
        app->cfg.auto_play = 0;
        cfg_store(app);
        gal_ui_set_mode(&app->ui, false);
    }
    app->ff_hold = true;
    ff_step(app);
}
static void press_advance(nine_app_t *app)
{
    if (app->auto_mode) {
        app->auto_mode = false;
        app->cfg.auto_play = 0;
        cfg_store(app);
        gal_ui_set_mode(&app->ui, false);
    }
    advance_reading(app);
}

static void menu_action(nine_app_t *app, int action)
{
    if (app->vm.state.preview) {
        switch (action) {
        case 0: open_history(app); return;
        case 1: open_settings(app, GAL_PAGE_MENU); return;
        default: set_page(app, GAL_PAGE_CHAPTER); music_sync(app); return;
        }
    }
    switch (action) {
    case 0: open_slots(app, true); break;
    case 1: open_slots(app, false); break;
    case 2: open_history(app); break;
    case 3: {
        const nine_view_t *v = nine_vm_skip_chapter(&app->vm);
        app->text_page = 0;
        if (!v) break;
        set_page(app, GAL_PAGE_GAME);
        music_sync(app);
        handle_view(app, 0);
        break;
    }
    case 4: open_settings(app, GAL_PAGE_MENU); break;
    default: to_title(app); break;
    }
}

static void title_action(nine_app_t *app, int sel)
{
    switch (sel) {
    case 0: start_new(app); break;
    case 1:
        if (app->auto_size) {
            uint16_t page = 0;
            nine_state_t backup = app->vm.state;
            if (nine_vm_save_decode(&app->vm, app->auto_blob, app->auto_size, &page)) {
                const nine_view_t *v = nine_vm_locate(&app->vm);
                if (v && v->kind != NINE_VM_ERROR) {
                    app->text_page = page;
                    app->visual = NINE_NONE;
                    app->auto_mode = false;
                    app->ff_hold = false;
                    set_page(app, GAL_PAGE_GAME);
                    music_sync(app);
                    handle_view(app, page);
                    break;
                }
            }
            app->vm.state = backup;
            notice(app, "自动进度损坏");
        } else {
            notice(app, "没有可继续的进度");
        }
        break;
    case 2: open_slots(app, false); break;
    case 3: open_settings(app, GAL_PAGE_TITLE); break;
    case 4: open_gallery(app); break;
    default: open_about(app); break;
    }
}

static void settings_toggle(nine_app_t *app)
{
    switch (app->settings_sel) {
    case 0:
        app->cfg.text_speed = (uint8_t)((app->cfg.text_speed + 1) % 4);
        cfg_store(app);
        if (app->page == GAL_PAGE_SETTINGS) fill_settings(app);
        break;
    case 1:
        app->cfg.font_large = app->cfg.font_large ? 0 : 1;
        cfg_store(app);
        gal_ui_set_font_size(&app->ui, app->cfg.font_large != 0);
        if (app->page == GAL_PAGE_GAME) {
            app->page_count = gal_text_pages(app->text, units_per_line(app),
                                             lines_per_page(app), app->page_offsets,
                                             GAL_TEXT_MAX_PAGES + 1);
            if (app->page_count > GAL_TEXT_MAX_PAGES) app->page_count = GAL_TEXT_MAX_PAGES;
            app->text_page = 0;
            render_page(app);
        }
        fill_settings(app);
        break;
    case 2:
        app->cfg.auto_play = app->cfg.auto_play ? 0 : 1;
        cfg_store(app);
        fill_settings(app);
        break;
    case 3:
        app->cfg.transparency = (uint8_t)(app->cfg.transparency >= 55 ? 10
                                          : app->cfg.transparency + 5);
        cfg_store(app);
        gal_ui_set_panel_transparency(&app->ui, app->cfg.transparency);
        fill_settings(app);
        break;
    case 4: {
        int idx = title_bg_selected(app);
        for (int step = 1; step <= 3; ++step) {
            int next = (idx + step) % 3;
            if (title_bg_unlocked(app, next)) {
                app->cfg.title_bg = (uint8_t)next;
                cfg_store(app);
                break;
            }
        }
        fill_settings(app);
        break;
    }
    case 5:
        app->cfg.sound = app->cfg.sound ? 0 : 1;
        cfg_store(app);
        music_sync(app);
        fill_settings(app);
        break;
    default:
        break;
    }
}

static void handle_click(nine_app_t *app, uint8_t btn)
{
    switch (app->page) {
    case GAL_PAGE_START:
        if (btn == NINE_BTN_OK) {
            app->title_sel = 0;
            show_title(app);
        }
        break;
    case GAL_PAGE_TITLE:
        if (btn == NINE_BTN_UP) app->title_sel = (app->title_sel + 5) % 6;
        else if (btn == NINE_BTN_DOWN) app->title_sel = (app->title_sel + 1) % 6;
        else title_action(app, app->title_sel);
        if (app->page == GAL_PAGE_TITLE)
            gal_ui_select_row(&app->ui, GAL_LIST_TITLE, app->title_sel);
        break;
    case GAL_PAGE_GAME:
        press_advance(app);
        break;
    case GAL_PAGE_CHOICE:
        if (btn == NINE_BTN_OK) {
            const nine_view_t *v = nine_vm_choose(&app->vm, (uint32_t)app->choice_sel);
            app->text_page = 0;
            if (v) {
                set_page(app, GAL_PAGE_GAME);
                handle_view(app, 0);
                write_auto(app);
            }
        } else if (btn == NINE_BTN_UP) {
            if (app->choice_sel > 0) app->choice_sel--;
            gal_ui_select_row(&app->ui, GAL_LIST_CHOICE, app->choice_sel);
        } else {
            if (app->choice_sel + 1 < app->vm.view.choice_count) app->choice_sel++;
            gal_ui_select_row(&app->ui, GAL_LIST_CHOICE, app->choice_sel);
        }
        break;
    case GAL_PAGE_MENU: {
        const int menu_count = app->vm.state.preview ? 3 : 6;
        if (btn == NINE_BTN_UP) app->menu_sel = (app->menu_sel + menu_count - 1) % menu_count;
        else if (btn == NINE_BTN_DOWN) app->menu_sel = (app->menu_sel + 1) % menu_count;
        else menu_action(app, app->menu_sel);
        if (app->page == GAL_PAGE_MENU)
            gal_ui_select_row(&app->ui, GAL_LIST_MENU, app->menu_sel);
        break;
    }
    case GAL_PAGE_SAVE:
    case GAL_PAGE_LOAD: {
        const bool save_mode = app->page == GAL_PAGE_SAVE;
        int *sel = save_mode ? &app->save_sel : &app->load_sel;
        const int pages = (NINE_SLOTS + NINE_PAGE_ROWS - 1) / NINE_PAGE_ROWS;
        if (btn == NINE_BTN_UP) {
            if (*sel > 0) (*sel)--;
            else if (app->slot_page > 0) { app->slot_page--; *sel = NINE_PAGE_ROWS - 1; }
            fill_slots(app, save_mode);
        } else if (btn == NINE_BTN_DOWN) {
            if (*sel + 1 < NINE_PAGE_ROWS) (*sel)++;
            else if (app->slot_page + 1 < pages) { app->slot_page++; *sel = 0; }
            fill_slots(app, save_mode);
        } else {
            int slot = app->slot_page * NINE_PAGE_ROWS + *sel;
            if (save_mode) {
                if (app->slots[slot].occupied && !app->overwrite_confirm) {
                    app->overwrite_confirm = true;
                    static const char *const actions[] = {"取消", "覆盖"};
                    gal_ui_show_modal(&app->ui, GAL_MODAL_CONFIRM, "覆盖存档？",
                                      "该栏位已有进度。", actions, 2, 0);
                    app->modal_open = true;
                    app->modal_sel = 0;
                } else {
                    save_slot(app, slot);
                }
            } else if (app->slots[slot].occupied) {
                if (load_slot(app, slot)) app->return_page = GAL_PAGE_GAME;
            } else {
                notice(app, "空栏位");
            }
        }
        if (app->page == GAL_PAGE_SAVE || app->page == GAL_PAGE_LOAD)
            gal_ui_select_row(&app->ui, save_mode ? GAL_LIST_SAVE : GAL_LIST_LOAD, *sel);
        break;
    }
    case GAL_PAGE_SETTINGS:
        if (btn == NINE_BTN_UP) app->settings_sel = (app->settings_sel + 5) % 6;
        else if (btn == NINE_BTN_DOWN) app->settings_sel = (app->settings_sel + 1) % 6;
        else settings_toggle(app);
        if (app->page == GAL_PAGE_SETTINGS)
            gal_ui_select_row(&app->ui, GAL_LIST_SETTINGS, app->settings_sel);
        break;
    case GAL_PAGE_HISTORY:
        if (btn == NINE_BTN_UP && app->history_off > 0) {
            app->history_off--;
            fill_history_rows(app);
        } else if (btn == NINE_BTN_DOWN &&
                   app->history_off + NINE_HISTORY_WINDOW < app->history_count) {
            app->history_off++;
            fill_history_rows(app);
        } else if (btn == NINE_BTN_OK) {
            set_page(app, app->return_page);
        }
        break;
    case GAL_PAGE_GALLERY:
        if (btn == NINE_BTN_UP) app->gallery_sel = (app->gallery_sel + 3) % 4;
        else if (btn == NINE_BTN_DOWN) app->gallery_sel = (app->gallery_sel + 1) % 4;
        else {
            switch (app->gallery_sel) {
            case 0:
                app->cg_sel = 0;
                app->cg_page = 0;
                fill_cg_page(app);
                set_page(app, GAL_PAGE_CG_LIST);
                break;
            case 1: open_chapters(app); break;
            case 2: start_op(app, true, GAL_PAGE_GALLERY); break;
            default: open_music(app); break;
            }
        }
        if (app->page == GAL_PAGE_GALLERY)
            gal_ui_select_row(&app->ui, GAL_LIST_GALLERY, app->gallery_sel);
        break;
    case GAL_PAGE_CG_LIST: {
        uint32_t groups = gallery_groups(app);
        if (btn == NINE_BTN_UP && app->cg_sel > 0) app->cg_sel--;
        else if (btn == NINE_BTN_DOWN && (uint32_t)(app->cg_sel + 1) < groups) app->cg_sel++;
        else if (btn == NINE_BTN_OK) {
            if (bit_get(app->unlock, app->cg_sel)) {
                app->cg_variant = 0;
                show_cg(app);
            } else {
                notice(app, "尚未解锁");
            }
            break;
        }
        app->cg_page = cg_page_of(app->cg_sel);
        fill_cg_page(app);
        break;
    }
    case GAL_PAGE_CG:
        if (btn == NINE_BTN_UP || btn == NINE_BTN_DOWN) {
            int n = variant_count(app, app->cg_sel);
            if (n > 1) {
                app->cg_variant = (app->cg_variant + (btn == NINE_BTN_UP ? n - 1 : 1)) % n;
                show_cg(app);
            }
        } else {
            set_page(app, GAL_PAGE_CG_LIST);
            fill_cg_page(app);
        }
        break;
    case GAL_PAGE_CHAPTER:
        if (btn == NINE_BTN_UP && app->chapter_sel > 0) {
            app->chapter_sel--;
            fill_chapter_rows(app);
        } else if (btn == NINE_BTN_DOWN &&
                   (uint32_t)(app->chapter_sel + 1) < app->pack.chapter_count) {
            app->chapter_sel++;
            fill_chapter_rows(app);
        } else if (btn == NINE_BTN_OK) {
            start_chapter_preview(app, app->chapter_sel);
        }
        break;
    case GAL_PAGE_MUSIC: {
        uint32_t n = nine_media_track_count();
        if (btn == NINE_BTN_UP && app->music_sel > 0) app->music_sel--;
        else if (btn == NINE_BTN_DOWN && (uint32_t)(app->music_sel + 1) < n) app->music_sel++;
        else if (btn == NINE_BTN_OK) {
            if (!app->media_ok) { notice(app, "音频不可用"); break; }
            if (nine_media_current_track() == (uint32_t)app->music_sel &&
                nine_media_is_playing()) nine_media_stop();
            else (void)nine_media_play((uint32_t)app->music_sel);
        }
        if (app->page == GAL_PAGE_MUSIC)
            gal_ui_select_row(&app->ui, GAL_LIST_MUSIC, app->music_sel);
        break;
    }
    case GAL_PAGE_ABOUT:
        if (btn == NINE_BTN_UP) gal_ui_scroll(&app->ui, GAL_PAGE_ABOUT, -26);
        else if (btn == NINE_BTN_DOWN) gal_ui_scroll(&app->ui, GAL_PAGE_ABOUT, 26);
        else show_title(app);
        break;
    case GAL_PAGE_ENDING:
        to_title(app);
        break;
    case GAL_PAGE_OPENING:
        finish_op(app);
        break;
    default:
        break;
    }
}

static void resume_reading(nine_app_t *app)
{
    // Restore the exact reading layer, not just the GAME page. `text_page`
    // and `choice_sel` were never moved by menu/save/settings/history.
    if (app->vm.view.kind == NINE_VM_CHOICE) {
        render_choice(app);
        return;
    }
    if (app->vm.view.kind == NINE_VM_TEXT) {
        set_page(app, GAL_PAGE_GAME);
        render_page(app);
        return;
    }
    set_page(app, GAL_PAGE_GAME);
}

static void handle_long(nine_app_t *app, uint8_t btn)
{
    switch (app->page) {
    case GAL_PAGE_GAME:
        if (btn == NINE_BTN_OK) open_menu(app);
        else if (btn == NINE_BTN_UP) start_ff(app);
        else toggle_auto(app);
        break;
    case GAL_PAGE_CHOICE:
        // One long OK enters the menu directly; never drop the choice layer
        // to a stale GAME page first.
        if (btn == NINE_BTN_OK) open_menu(app);
        break;
    case GAL_PAGE_MENU:
        if (btn == NINE_BTN_OK) { resume_reading(app); music_sync(app); }
        break;
    case GAL_PAGE_SAVE:
    case GAL_PAGE_LOAD:
        if (btn == NINE_BTN_OK) {
            if (app->modal_open) {
                gal_ui_hide_modal(&app->ui);
                app->modal_open = false;
                app->overwrite_confirm = false;
            } else if (app->return_page == GAL_PAGE_TITLE) {
                show_title(app);
            } else {
                resume_reading(app);
                music_sync(app);
            }
        }
        break;
    case GAL_PAGE_SETTINGS:
        if (btn == NINE_BTN_OK) {
            if (app->return_page == GAL_PAGE_TITLE) show_title(app);
            else { resume_reading(app); music_sync(app); }
        }
        break;
    case GAL_PAGE_HISTORY:
        if (btn == NINE_BTN_OK) { resume_reading(app); music_sync(app); }
        break;
    case GAL_PAGE_ABOUT:
        if (btn == NINE_BTN_OK) show_title(app);
        break;
    case GAL_PAGE_GALLERY:
        if (btn == NINE_BTN_OK) to_title(app);
        break;
    case GAL_PAGE_CG_LIST:
    case GAL_PAGE_CG:
    case GAL_PAGE_CHAPTER:
    case GAL_PAGE_MUSIC:
        if (btn == NINE_BTN_OK) open_gallery(app);
        break;
    case GAL_PAGE_ENDING:
    case GAL_PAGE_OPENING:
        if (btn == NINE_BTN_OK) handle_click(app, NINE_BTN_OK);
        break;
    default:
        break;
    }
}

static void handle_modal(nine_app_t *app, uint8_t btn)
{
    if (btn == NINE_BTN_UP || btn == NINE_BTN_DOWN) {
        app->modal_sel = app->modal_sel ? 0 : 1;
        gal_ui_select_row(&app->ui, GAL_LIST_MODAL, app->modal_sel);
        return;
    }
    if (btn != NINE_BTN_OK) return;
    int slot = app->slot_page * NINE_PAGE_ROWS + app->save_sel;
    bool do_save = app->modal_sel == 1;
    gal_ui_hide_modal(&app->ui);
    app->modal_open = false;
    app->overwrite_confirm = false;
    if (do_save) save_slot(app, slot);
}

static void key_body(nine_app_t *app, const nine_key_t *key)
{
    app->idle_ms = 0;
    // iot_button merges two fast taps into DOUBLE and does not send a CLICK.
    // Reading is tap-heavy, so a double tap must advance exactly once.
    nine_key_t as_click;
    if (key->ev == NINE_EV_DOUBLE) {
        as_click = *key;
        as_click.ev = NINE_EV_CLICK;
        key = &as_click;
    }
    if (key->ev == NINE_EV_RELEASE) {
        if (app->ff_hold && key->btn == NINE_BTN_UP) app->ff_hold = false;
        return;
    }
    if (key->ev == NINE_EV_PRESS) {
        app->suppress_click = false;
        return;
    }
    if (key->ev == NINE_EV_CLICK) {
        if (app->suppress_click) { app->suppress_click = false; return; }
        if (app->modal_open) { handle_modal(app, key->btn); return; }
        handle_click(app, key->btn);
        return;
    }
    // LONG: only the long-press action runs; the next click is suppressed.
    app->suppress_click = true;
    if (app->modal_open) {
        gal_ui_hide_modal(&app->ui);
        app->modal_open = false;
        app->overwrite_confirm = false;
        return;
    }
    handle_long(app, key->btn);
}

static void tick_body(nine_app_t *app, uint32_t elapsed_ms)
{
    if (elapsed_ms > 500) elapsed_ms = 500;   // never fast-forward on a stall
    app->clock_ms += elapsed_ms;
    app->idle_ms += elapsed_ms;
    app->battery_ms += elapsed_ms;
    app->auto_write_ms += elapsed_ms;
    if (app->notice_ms) {
        app->notice_ms = app->notice_ms > elapsed_ms ? app->notice_ms - elapsed_ms : 0;
        if (!app->notice_ms) gal_ui_set_notice(&app->ui, NULL);
    }
    if (app->battery_ms >= NINE_BATTERY_MS) {
        app->battery_ms = 0;
        gal_ui_set_battery(&app->ui, bsp_battery_soc());
    }
    if (app->op_active) {
        app->idle_ms = 0;   // do not dim while the OP movie is playing
        nine_media_tick();
        if (!nine_media_is_playing()) finish_op(app);
        return;
    }
    if (app->page == GAL_PAGE_GAME) {
        if (app->ff_hold) {
            // One sentence per control tick, with no additional inter-sentence
            // delay. The input task calls this every 50 ms; a hold therefore
            // advances continuously without a page/typewriter wait.
            ff_step(app);
            app->idle_ms = 0;
        } else if (app->auto_mode && !gal_ui_typing(&app->ui)) {
            uint32_t len = (uint32_t)strlen(app->page_text);
            uint32_t wait = NINE_AUTO_BASE_MS;
            if (len > 60) {
                uint32_t extra = (len - 60) * 55;
                if (extra > 5000) extra = 5000;
                wait += extra;
            }
            app->auto_accum += elapsed_ms;
            if (app->auto_accum >= wait) {
                app->auto_accum = 0;
                advance_reading(app);
                app->idle_ms = 0;
            }
        }
    }
    if (app->auto_dirty && app->auto_write_ms >= NINE_AUTO_WRITE_MS) write_auto(app);
    if (app->media_ok && app->cfg.sound && !app->op_active && title_area(app) &&
        !nine_media_is_playing()) {
        if (nine_media_failed()) app->media_ok = false;
        else (void)nine_media_play(0);
    }
}

void nine_app_key(nine_app_t *app, const nine_key_t *key)
{
    if (!app || !key || !app->ready || app->fatal) return;
    if (!bsp_lvgl_lock(200)) return;
    key_body(app, key);
    bsp_lvgl_unlock();
}

void nine_app_tick(nine_app_t *app, uint32_t elapsed_ms)
{
    if (!app || !app->ready || app->fatal) return;
    if (!bsp_lvgl_lock(100)) return;
    tick_body(app, elapsed_ms);
    bsp_lvgl_unlock();
}

uint32_t nine_app_idle_ms(const nine_app_t *app)
{
    return app ? app->idle_ms : 0;
}

void nine_app_flush(nine_app_t *app)
{
    if (!app) return;
    if (nine_vm_progress_dirty(&app->vm)) progress_store(app);
    if (app->auto_dirty) write_auto(app);
}

void nine_app_show_sleeping(nine_app_t *app)
{
    if (!app || !app->ready) return;
    static const char *const rows[] = {"设备休眠 · 按任意键唤醒"};
    gal_ui_set_rows(&app->ui, GAL_LIST_START, rows, NULL, 1, 0);
    gal_ui_set_brand(&app->ui, "设备休眠", "按任意键唤醒");
    nine_image_set_visible(false);
    gal_ui_show_page(&app->ui, GAL_PAGE_START);
}

// ---------------------------------------------------------------- init
static void fatal_screen(nine_app_t *app, const char *message)
{
    app->fatal = true;
    app->page = GAL_PAGE_TITLE;
    snprintf(app->fatal_text, sizeof(app->fatal_text), "%.*s",
             (int)(sizeof(app->fatal_text) - 1), message ? message : "资源包错误");
    // The title page is the one screen that renders the brand labels. Use it for
    // errors so the CJK message is actually visible (the start page hides them).
    static const char *const rows[] = {"无法继续"};
    gal_ui_set_rows(&app->ui, GAL_LIST_TITLE, rows, NULL, 1, 0);
    gal_ui_set_brand(&app->ui, "资源包错误", app->fatal_text);
    gal_ui_set_title_image(&app->ui, NULL);
    nine_image_set_visible(false);
    gal_ui_show_page(&app->ui, GAL_PAGE_TITLE);
}

bool nine_app_init(nine_app_t *app, const uint8_t *script, size_t script_size,
                   const uint8_t *images, size_t image_size)
{
    if (!app || !script || !images) return false;
    memset(app, 0, sizeof(*app));
    app->menu_return_page = GAL_PAGE_GAME;
    nine_cfg_defaults(&app->cfg);
    nine_store_init(&app->store, true);
    (void)nine_store_cfg_load(&app->store, &app->cfg);
    app->cfg.sound = nine_store_sound_load(&app->store);

    // LVGL is created by the port task before app_main reaches us; every lv_*
    // call in init and in the key/tick paths runs under the recursive BSP lock.
    if (!bsp_lvgl_lock(1000)) return false;
    if (!gal_ui_create(&app->ui, &nine_cjk_16, &nine_cjk_20)) {
        bsp_lvgl_unlock();
        return false;
    }
    gal_ui_set_font_size(&app->ui, app->cfg.font_large != 0);
    gal_ui_set_panel_transparency(&app->ui, app->cfg.transparency);
    gal_ui_set_battery(&app->ui, bsp_battery_soc());

    if (!nine_pack_open(&app->pack, script, script_size)) {
        fatal_screen(app, "剧情包校验失败");
        app->ready = true;   // UI must stay usable enough to show the error
        bsp_lvgl_unlock();
        return true;
    }
    if (!nine_image_init(lv_screen_active()) ||
        !nine_image_bind_bundle(script, script_size, images, image_size)) {
        fatal_screen(app, "图像包校验失败");
        app->ready = true;
        bsp_lvgl_unlock();
        return true;
    }
    nine_image_set_base_layer(1);   // above ui->bg, below all UI pages
    app->media_ok = nine_media_init(script, script_size, images, image_size);
    if (!app->media_ok) notice(app, "音频不可用");

    if (!nine_vm_init(&app->vm, &app->pack)) {
        fatal_screen(app, "剧情状态初始化失败");
        app->ready = true;
        bsp_lvgl_unlock();
        return true;
    }
    uint8_t progress[NINE_PROGRESS_RECORD_MAX];
    size_t pn = 0;
    if (nine_store_progress_load(&app->store, progress, sizeof(progress), &pn))
        (void)nine_vm_progress_decode(&app->vm, progress, pn);
    memset(app->unlock, 0, sizeof(app->unlock));
    (void)nine_store_unlock_load(&app->store, app->unlock);
    refresh_slots(app);
    auto_progress_load(app);
    nine_vm_start(&app->vm, NINE_NODE_NONE, false);
    app->visual = NINE_NONE;

    gal_ui_set_brand(&app->ui, "", "");
    static const char *const start_rows[] = {"开始阅读"};
    gal_ui_set_rows(&app->ui, GAL_LIST_START, start_rows, NULL, 1, 0);
    set_page(app, GAL_PAGE_START);
    app->ready = true;
    bsp_lvgl_unlock();
    return true;
}

bool nine_app_landscape_ok(bool lock_ok, bool set_ok, int width, int height)
{
    return lock_ok && set_ok && width == 320 && height == 240;
}

bool nine_app_first_frame_ready(bool lock_ok, bool submitted)
{
    return lock_ok && submitted;
}

const char *nine_app_current_text(const nine_app_t *app)
{
    return app ? app->page_text : "";
}
int nine_app_current_page(const nine_app_t *app) { return app ? (int)app->page : -1; }
bool nine_app_auto_on(const nine_app_t *app) { return app && app->auto_mode; }
bool nine_app_fast_forwarding(const nine_app_t *app) { return app && app->ff_hold; }
int nine_app_history_count(const nine_app_t *app) { return app ? app->history_count : 0; }
