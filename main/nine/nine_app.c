// 9-nine- Episode 4 reader. The state machine interprets the already-converted
// watch graph exactly (conditions, first-match next, permanent-key merge,
// chapter preview isolation, 500-hop limits) and only adapts presentation to
// the 320x240 AI Passport panel.
#include "nine_app.h"

#include "nine_assets.h"
#include "nine_image.h"
#include "nine_media.h"

#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "gal_text.h"

#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_log.h"
#ifndef NINE_APP_HOST
#ifndef NINE_MEDIA_HOST
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif
#endif
#ifndef NINE_APP_HOST
#ifndef NINE_MEDIA_HOST
#include "esp_timer.h"
#endif
#endif

#include <stdio.h>
#include <string.h>
#include <time.h>

LV_FONT_DECLARE(nine_cjk_16);
LV_FONT_DECLARE(nine_cjk_20);

static const char *TAG = "nine";

#define TITLE_CN "9-nine- 雪色雪花雪之痕"
#define TITLE_LOGO "9-nine-"
#define TITLE_SUB "EPISODE 4"
#define APP_VERSION "v0.1.0"
#define PAGE_NONE 0xFFu

static const char *const TITLE_MENU[] = {
    "新游戏", "继续阅读", "读取存档", "设置", "鉴赏室", "关于",
};
static const char *const MENU_ITEMS[] = {
    "返回剧情", "保存进度", "读取存档", "历史文本", "设置", "返回标题",
};
static const char *const GALLERY_ITEMS[] = {
    "CG 鉴赏", "章节目录", "结局回顾", "OP 播放", "音乐鉴赏",
};
static const char *const MODAL_ACTIONS[] = { "取消", "覆盖保存" };

enum { TITLE_NEW, TITLE_CONTINUE, TITLE_LOAD, TITLE_SETTINGS, TITLE_GALLERY, TITLE_ABOUT };
enum { MENU_RESUME, MENU_SAVE, MENU_LOAD, MENU_HISTORY, MENU_SETTINGS, MENU_TITLE };
enum { GAL_CG, GAL_CHAPTER, GAL_ENDINGS, GAL_OP, GAL_MUSIC };
enum {
    SACT_SPEED = 0,
    SACT_SIZE,
    SACT_AUTO,
    SACT_TRANSPARENCY,
    SACT_RESET,
    SACT_TITLE,
    SACT_BACK,
};

static const char *const SETTINGS_READER[] = {
    "文字速度", "文字大小", "自动阅读", "对话透明度", "恢复默认", "返回标题", "返回",
};
static const char *const SETTINGS_TITLE[] = {
    "文字速度", "文字大小", "自动阅读", "对话透明度", "恢复默认", "返回",
};

static int32_t progress_value(const nine_app_t *app, int key)
{
    if (!app || key < 0 || (uint32_t)key >= app->pack.key_count) return 0;
    return app->vm.state.progress[key];
}

// Readable means the frame on screen actually decoded. Resource and VM
// failures are not a text-only mode: they block story/progress paths.
static bool app_readable(const nine_app_t *app)
{
    return app && !app->fatal && !app->resource_fatal && !app->resource_error &&
           !app->image_error && !app->vm_error;
}

static uint32_t speed_ms(const nine_app_t *app)
{
    static const uint32_t SPEED_MS[4] = { 60, 40, 18, 0 };
    const uint8_t idx = app->cfg.display.text_speed > 3 ? 1 : app->cfg.display.text_speed;
    return SPEED_MS[idx];
}

static const char *speed_label(const nine_app_t *app)
{
    static const char *const LABEL[4] = { "慢", "中", "快", "瞬间" };
    const uint8_t idx = app->cfg.display.text_speed > 3 ? 1 : app->cfg.display.text_speed;
    return LABEL[idx];
}

static void layout_for(const nine_app_t *app, int *units, int *lines)
{
    // gal_text counts a full-width glyph as two units: 15 chars at 20 px,
    // 19 chars at 16 px, matching the landscape panel width.
    if (app->cfg.display.font_large) {
        *units = 30;
        *lines = GAL_TEXT_LINES_LARGE;
    } else {
        *units = 38;
        *lines = GAL_TEXT_LINES_SMALL;
    }
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    if (!src) src = "";
    const size_t n = strlen(src);
    const size_t take = n < cap - 1 ? n : cap - 1;
    memcpy(dst, src, take);
    dst[take] = '\0';
}

static void show_notice(nine_app_t *app, const char *text)
{
    copy_str(app->notice, sizeof(app->notice), text);
    app->notice_ms = text && text[0] ? 2200u : 0;
    gal_ui_set_notice(&app->ui, app->notice);
}

static void move_selection(int *sel, int count, int delta)
{
    if (count <= 0) {
        *sel = 0;
        return;
    }
    int next = *sel + delta;
    if (next < 0) next = 0;
    if (next >= count) next = count - 1;
    *sel = next;
}

static int window_for(int selected, int window, int page)
{
    if (page <= 0) return 0;
    if (selected < window) return selected;
    if (selected >= window + page) return selected - page + 1;
    return window;
}

static bool bg_unlocked(const nine_app_t *app, int index)
{
    const bool loop_cleared = (app->unlock.endings & 0x1u) != 0;
    const bool true_cleared = (app->unlock.endings & 0x2u) != 0;
    if (index <= 0) return true;
    if (index == 1) return loop_cleared || true_cleared;
    return loop_cleared && true_cleared;
}

static int highest_bg(const nine_app_t *app)
{
    for (int i = 2; i >= 0; --i)
        if (bg_unlocked(app, i)) return i;
    return 0;
}

static void title_music(nine_app_t *app)
{
    if (!app->media_ok || nine_media_is_playing()) return;
    (void)nine_media_play(0);
}

static void silence_music(nine_app_t *app)
{
    if (!app->media_ok) return;
    if (nine_media_current_track() == 0) nine_media_stop();
}

static bool chapter_title(const nine_app_t *app, int node, char *out, size_t cap)
{
    nine_node_t n;
    if (node < 0 || node >= (int)app->pack.node_count ||
        !nine_pack_node(&app->pack, (uint16_t)node, &n) ||
        n.chapter == NINE_NONE) {
        copy_str(out, cap, "");
        return false;
    }
    nine_chapter_t c;
    if (!nine_pack_chapter(&app->pack, n.chapter, &c)) {
        copy_str(out, cap, "");
        return false;
    }
    copy_str(out, cap, nine_pack_string(&app->pack, c.title));
    return true;
}

static bool chapter_preview(nine_app_t *app, int chapter, char *out, size_t cap)
{
    nine_chapter_t c;
    if (!nine_pack_chapter(&app->pack, (uint32_t)chapter, &c)) return false;
    nine_node_t n;
    if (!nine_pack_node(&app->pack, c.node, &n) || n.frame_count == 0) return false;
    nine_frame_t f;
    if (!nine_pack_frame(&app->pack, n.first_frame, &f) || f.kind != NINE_FRAME_TEXT)
        return false;
    const char *text = nine_pack_text(&app->pack, &f);
    copy_str(out, cap, text);
    return text != NULL;
}

// ---------------------------------------------------------------- persistence
static void apply_perm(nine_app_t *app)
{
    if (!app->perm_valid) return;
    (void)nine_vm_progress_decode(&app->vm, app->perm_blob, app->perm_size);
}

static void save_perm(nine_app_t *app)
{
    if (!app || app->preview || !app_readable(app)) return;
    uint8_t blob[NINE_APP_SAVE_CAPACITY];
    const size_t n = nine_vm_progress_encode(&app->vm, blob, sizeof(blob));
    if (!n) return;
    if (app->perm_valid && app->perm_size == n && memcmp(app->perm_blob, blob, n) == 0)
        return;
    memcpy(app->perm_blob, blob, n);
    app->perm_size = n;
    app->perm_valid = true;
    if (!nine_store_put(&app->store, "perm", blob, n)) {
        ESP_LOGW(TAG, "permanent progress write failed");
    }
}

static void save_auto(nine_app_t *app)
{
    if (!app || app->preview || !app_readable(app)) return;
    uint8_t blob[NINE_APP_SAVE_CAPACITY];
    const size_t n = nine_vm_save_encode(&app->vm, app->text_page, blob, sizeof(blob));
    if (!n) return;
    uint16_t node, page;
    uint32_t index;
    bool preview;
    if (!nine_vm_save_peek(&app->pack, blob, n, &node, &index, &page, &preview)) return;
    if (!nine_store_put(&app->store, "auto", blob, n)) {
        app->progress_dirty = true;
        return;
    }
    app->auto_valid = true;
    app->progress_dirty = false;
}

static void grant_ending_bit(nine_app_t *app, int bit)
{
    if (bit < 0 || bit > 1) return;
    const uint8_t before = app->unlock.endings;
    app->unlock.endings |= (uint8_t)(1u << bit);
    if (app->unlock.endings == before) return;
    // First clear switches the title art to the newly unlocked visual, as in
    // the watch reader.
    const bool loop_now = (app->unlock.endings & 1u) != 0;
    const bool true_now = (app->unlock.endings & 2u) != 0;
    if (true_now && loop_now) app->cfg.title_bg = 2;
    else if (loop_now || true_now) app->cfg.title_bg = 1;
    (void)nine_cfg_save(&app->store, &app->cfg);
    (void)nine_unlock_save(&app->store, &app->unlock);
}

static void update_unlocks(nine_app_t *app)
{
    if (!app || app->preview || !app_readable(app)) return;
    const bool loop = progress_value(app, app->key_loop1) != 0 ||
                      progress_value(app, app->key_loop_na) != 0;
    const bool tru = progress_value(app, app->key_true) != 0 ||
                     progress_value(app, app->key_cf_na) != 0 ||
                     progress_value(app, app->key_clear_true) != 0;
    if (loop) grant_ending_bit(app, 0);
    if (tru) grant_ending_bit(app, 1);

    const uint16_t node = app->vm.state.node;
    nine_node_t n;
    if (node != NINE_NODE_NONE && nine_pack_node(&app->pack, node, &n) &&
        n.chapter != NINE_NONE) {
        const uint32_t before = app->unlock.chapters;
        app->unlock.chapters |= (uint32_t)1u << n.chapter;
        if (before != app->unlock.chapters)
            (void)nine_unlock_save(&app->store, &app->unlock);
    }
}

// ---------------------------------------------------------------- rendering
static void render_page(nine_app_t *app);
static void render_slots(nine_app_t *app, bool saving);
static void render_settings(nine_app_t *app);
static void render_history(nine_app_t *app);
static void render_cg_list(nine_app_t *app);
static void render_cg_view(nine_app_t *app);
static void render_op_view(nine_app_t *app);
static void start_gallery_op(nine_app_t *app);
static void render_endings(nine_app_t *app);
static void render_music(nine_app_t *app);
static void render_chapters(nine_app_t *app);
static void render_about(nine_app_t *app);

static void go_page(nine_app_t *app, gal_page_t page)
{
    const gal_page_t old = app->page;
    if (page == GAL_PAGE_TITLE) {
        // Returning to the title abandons an uncommitted failed frame. The
        // next New/Continue/Load attempt is the explicit retry.
        app->resource_error = false;
        app->image_error = false;
    }
    if (old == GAL_PAGE_MUSIC && page != GAL_PAGE_MUSIC && app->media_ok) nine_media_stop();
    app->page = page;
    app->fast_forward = false;
    app->auto_ms = 0;

    if (app->media_ok && page != GAL_PAGE_MUSIC && page != GAL_PAGE_GAME &&
        page != GAL_PAGE_CHOICE && page != GAL_PAGE_MENU && page != GAL_PAGE_SAVE &&
        page != GAL_PAGE_LOAD && page != GAL_PAGE_SETTINGS && page != GAL_PAGE_HISTORY &&
        page != GAL_PAGE_ENDING && page != GAL_PAGE_START) {
        title_music(app);
    } else if (app->media_ok && page != GAL_PAGE_MUSIC) {
        silence_music(app);
    }
    render_page(app);
}

static bool show_frame(nine_app_t *app, const nine_frame_t *frame)
{
    if (!frame || frame->kind != NINE_FRAME_TEXT) return false;
    const char *raw = nine_pack_text(&app->pack, frame);
    if (!raw) {
        app->vm_error = true;
        copy_str(app->error, sizeof(app->error), "正文数据损坏");
        copy_str(app->text, sizeof(app->text), "（正文数据损坏，无法继续）");
    } else {
        copy_str(app->text, sizeof(app->text), raw);
    }
    copy_str(app->speaker, sizeof(app->speaker),
             nine_pack_string(&app->pack, frame->speaker));
    if (app->vm_error) {
        gal_ui_set_text(&app->ui, "", app->text, 0, NULL, NULL);
        return false;
    }

    int units = 19;
    int lines = 4;
    layout_for(app, &units, &lines);
    int pages = gal_text_pages(app->text, units, lines, app->page_offsets,
                               GAL_TEXT_MAX_PAGES + 1);
    if (pages < 1) pages = 1;
    if (pages > GAL_TEXT_MAX_PAGES) pages = GAL_TEXT_MAX_PAGES;
    app->page_count = pages;
    if (app->text_page >= pages) app->text_page = (uint16_t)(pages - 1);
    const uint32_t begin = app->page_offsets[app->text_page];
    uint32_t end = app->page_offsets[app->text_page + 1];
    if (end < begin) end = begin;
    size_t len = end - begin;
    if (len >= sizeof(app->page_text)) len = sizeof(app->page_text) - 1;
    memcpy(app->page_text, app->text + begin, len);
    app->page_text[len] = '\0';

    bool image_done = app->image_ok;
    if (app->image_ok && (!app->visual_valid || app->visual != frame->visual ||
                          app->image_error)) {
        if (nine_image_show_visual(frame->visual)) {
            app->image_error = false;
            app->resource_error = false;
        } else {
            app->image_error = true;
            app->resource_error = true;
            app->auto_mode = false;
            app->fast_forward = false;
            gal_ui_set_mode(&app->ui, false);
            show_notice(app, "图像解码失败，已停在此句；按确定重试");
            image_done = false;
        }
        app->visual = frame->visual;
        app->visual_valid = true;
    }
    if (!app->image_ok) {
        app->resource_fatal = true;
        app->fatal = true;
        image_done = false;
    }
    nine_image_set_visible(app->image_ok);

    gal_ui_set_text(&app->ui, app->speaker, app->page_text,
                    app->fast_forward ? 0 : speed_ms(app), NULL, NULL);
    gal_ui_set_chapter(&app->ui, app->chapter_title);
    if (!image_done) {
        app->shown_node = app->vm.state.node;
        app->shown_index = app->vm.state.index;
        app->shown_page = app->text_page;
        app->progress_dirty = false;
        return false;
    }
    if (app->text_page == 0) {
        if (app->history_count < NINE_APP_HISTORY_MAX) {
            char *row = app->history[app->history_count++];
            if (app->speaker[0])
                snprintf(row, NINE_APP_HISTORY_TEXT, "%.80s\n%.270s", app->speaker, app->text);
            else
                snprintf(row, NINE_APP_HISTORY_TEXT, "旁白\n%.300s", app->text);
        }
    }
    app->shown_node = app->vm.state.node;
    app->shown_index = app->vm.state.index;
    app->shown_page = app->text_page;
    app->progress_dirty = !app->preview;
    return true;
}

static void render_options(nine_app_t *app)
{
    static gal_option_t options[NINE_CHOICE_MAX];
    const nine_view_t *v = &app->vm.view;
    int count = v->choice_count;
    if (count > (int)NINE_CHOICE_MAX) count = NINE_CHOICE_MAX;
    for (int i = 0; i < count; ++i) {
        nine_edge_t e;
        const char *label = "";
        if (nine_pack_edge(&app->pack, true, v->choices[i], &e))
            label = nine_pack_string(&app->pack, e.label);
        options[i].label = label ? label : "";
        options[i].flag_set = GAL_UNLOCK_NONE;
        options[i].require_flag = GAL_UNLOCK_NONE;
    }
    if (app->choice_sel >= count) app->choice_sel = count > 0 ? count - 1 : 0;
    gal_ui_set_choice(&app->ui, "请选择", options, count, app->choice_sel);
}

static void prepare_ending(nine_app_t *app, bool from_gallery, int kind)
{
    nine_ending_t e;
    const char *name = "";
    const char *desc = "";
    if (kind >= 0 && kind < (int)app->pack.ending_count &&
        nine_pack_ending(&app->pack, (uint32_t)kind, &e)) {
        name = nine_pack_string(&app->pack, e.name);
        desc = nine_pack_string(&app->pack, e.description);
    }
    if (!from_gallery && !app->preview) grant_ending_bit(app, kind == 1 ? 1 : 0);
    const char *eyebrow = kind == 1 ? "TRUE END" : "BRANCH END";
    if (app->preview) {
        eyebrow = "CHAPTER PREVIEW";
        desc = "章节试读结束；试读不会写入通关进度。";
    }
    app->ending_kind = (uint16_t)(kind < 0 ? 0 : kind);
    app->ending_return = from_gallery ? GAL_PAGE_RECALL : GAL_PAGE_TITLE;
    gal_ui_set_ending(&app->ui, eyebrow, name ? name : "", desc ? desc : "");
}

static void show_boot_error(nine_app_t *app, const char *text);

static void render_error(nine_app_t *app)
{
    app->auto_mode = false;
    app->fast_forward = false;
    gal_ui_set_mode(&app->ui, false);
    gal_ui_show_page(&app->ui, GAL_PAGE_GAME);
    gal_ui_set_text(&app->ui, "", app->error, 0, NULL, NULL);
    nine_image_set_visible(false);
}

static uint32_t op_now_us(void)
{
#if defined(NINE_APP_HOST) || defined(NINE_MEDIA_HOST)
    return (uint32_t)((uint64_t)clock() * 1000000u / (uint64_t)CLOCKS_PER_SEC);
#else
    int64_t now = esp_timer_get_time();
    return now <= 0 ? 0 : (uint32_t)now;
#endif
}

static uint32_t op_audio_ms(nine_app_t *app)
{
    nine_media_track_t t;
    if (!app->media_ok || !nine_media_track_info(1, &t)) return 0;
    return t.duration_ms;
}

// One OP visual state, shared by the story OP and the Gallery OP. The frame
// clock is the audio position (the same monotonic base as the track); op_ms is
// only a fallback when no audio track is available.
static void begin_op_state(nine_app_t *app, bool gallery)
{
    app->opening = true;
    app->op_gallery = gallery;
    app->fast_forward = false;
    app->op_ms = 0;
    app->op_frame = 0;
    app->op_image = NINE_NONE;
    app->op_frames_drawn = 0;
    app->op_late_skips = 0;
    app->op_max_decode_us = 0;
    app->op_max_gap_ms = 0;
    app->op_last_pos = 0;
    nine_image_perf_reset();
#if !defined(NINE_APP_HOST) && !defined(NINE_MEDIA_HOST)
    bsp_lvgl_flush_stats_reset();
#endif
    uint32_t frame_end = 0;
    const uint32_t count = nine_image_op_count();
    uint32_t image = 0, ts = 0, dur = 0;
    if (count && nine_image_op_frame(count - 1, &image, &ts, &dur))
        frame_end = ts + dur;
    const uint32_t audio_end = op_audio_ms(app);
    app->op_duration = frame_end > audio_end ? frame_end : audio_end;
    if (app->op_duration == 0) app->op_duration = 1;
}

static bool play_op_audio(nine_app_t *app)
{
    // The OP always has an opening track. Running it silently would violate the
    // visible failure contract; media_ok is checked again here rather than
    // relying only on pack initialization.
    if (!app->media_ok) return false;
    return nine_media_play(1);
}

static bool show_first_op_frame(nine_app_t *app)
{
    const uint32_t count = nine_image_op_count();
    uint32_t image = 0, ts = 0, dur = 0;
    if (!app->image_ok || count == 0 || !nine_image_op_frame(0, &image, &ts, &dur))
        return false;
    const uint32_t start = op_now_us();
    const bool ok = nine_image_show_asset_top(image);
    const uint32_t cost = op_now_us() - start;
    if (cost > app->op_max_decode_us) app->op_max_decode_us = cost;
    if (!ok) return false;
    app->op_image = image;
    app->op_frames_drawn = 1;
    return true;
}

static void op_diagnostic(nine_app_t *app, const char *how)
{
#if !defined(NINE_APP_HOST) && !defined(NINE_MEDIA_HOST)
    nine_image_perf_t perf;
    nine_image_perf_get(&perf);
    uint32_t flush_count = 0, flush_gap_us = 0, flush_dur_us = 0;
    bsp_lvgl_flush_stats_get(&flush_count, &flush_gap_us, &flush_dur_us);
    ESP_LOGI(TAG,
             "OP %s gallery=%d elapsed=%u frames=%u late_skips=%u max_decode_us=%u "
             "max_gap_ms=%u crc_max_us=%u jpeg_max_us=%u clear_max_us=%u "
             "invalidate_max_us=%u flush=%u flush_gap_us=%u flush_dur_us=%u "
             "heap=%u stack=%u",
             how ? how : "end", app->op_gallery ? 1 : 0, (unsigned)app->op_ms,
             (unsigned)app->op_frames_drawn, (unsigned)app->op_late_skips,
             (unsigned)app->op_max_decode_us, (unsigned)app->op_max_gap_ms,
             (unsigned)perf.crc_max_us, (unsigned)perf.decode_max_us,
             (unsigned)perf.clear_max_us, (unsigned)perf.invalidate_max_us,
             (unsigned)flush_count, (unsigned)flush_gap_us, (unsigned)flush_dur_us,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
#else
    (void)app;
    (void)how;
#endif
}

static void sync_view(nine_app_t *app, bool new_frame)
{
    const nine_view_t *v = &app->vm.view;
    if (v->kind == NINE_VM_ERROR) {
        app->vm_error = true;
        copy_str(app->error, sizeof(app->error), v->error ? v->error : "剧情数据错误");
        app->page = GAL_PAGE_GAME;
        render_error(app);
        return;
    }
    app->vm_error = false;
    app->error[0] = '\0';
    if (v->kind == NINE_VM_TEXT) {
        nine_frame_t f;
        if (!nine_vm_current_frame(&app->vm, &f)) {
            app->vm_error = true;
            copy_str(app->error, sizeof(app->error), "剧情帧缺失");
            app->page = GAL_PAGE_GAME;
            render_error(app);
            return;
        }
        if (new_frame) app->text_page = 0;
        chapter_title(app, app->vm.state.node, app->chapter_title,
                      sizeof(app->chapter_title));
        app->page = GAL_PAGE_GAME;
        gal_ui_show_page(&app->ui, GAL_PAGE_GAME);
        if (!show_frame(app, &f)) return;
        if (!app->preview) {
            update_unlocks(app);
            save_perm(app);
        }
        return;
    }
    if (v->kind == NINE_VM_OP) {
        app->page = GAL_PAGE_GAME;
        app->fast_forward = false;
        render_page(app);
        begin_op_state(app, false);
        if (!play_op_audio(app) || !show_first_op_frame(app)) {
            app->resource_fatal = true;
            app->fatal = true;
            app->opening = false;
            app->image_ok = false;
            if (app->media_ok) nine_media_stop();
            nine_image_deinit();
            show_boot_error(app, "片头图像或音频无法播放\n请重启设备后重试");
            return;
        }
        return;
    }
    if (v->kind == NINE_VM_CHOICE) {
        app->page = GAL_PAGE_CHOICE;
        app->choice_sel = 0;
        app->fast_forward = false;
        if (!app_readable(app)) {
            render_error(app);
            return;
        }
        if (!app->preview) {
            update_unlocks(app);
            save_perm(app);
            app->progress_dirty = true;
        }
        render_page(app);
        return;
    }
    if (v->kind == NINE_VM_END) {
        app->page = GAL_PAGE_ENDING;
        app->fast_forward = false;
        app->auto_mode = false;
        if (app->media_ok) nine_media_stop();
        if (!app_readable(app)) {
            app->page = GAL_PAGE_GAME;
            render_error(app);
            return;
        }
        if (!app->preview) {
            update_unlocks(app);
            save_perm(app);
        }
        int kind = 0;
        if (progress_value(app, app->key_true) != 0) kind = 1;
        else if (progress_value(app, app->key_loop1) == 0) {
            // A preview may end without completion effects; keep the branch card.
            kind = progress_value(app, app->key_loop_na) != 0 ? 0 : 0;
        }
        prepare_ending(app, false, kind);
        render_page(app);
        return;
    }
}

static bool retry_frame_image(nine_app_t *app)
{
    if (!app->resource_error && !app->image_error) return true;
    if (!app->image_ok || app->vm.view.kind != NINE_VM_TEXT) {
        show_notice(app, "当前资源无法恢复；请重启设备后重试");
        return false;
    }
    nine_frame_t f;
    if (nine_vm_current_frame(&app->vm, &f) &&
        nine_image_show_visual(f.visual)) {
        app->image_error = false;
        app->resource_error = false;
        if (!app->preview) {
            update_unlocks(app);
            save_perm(app);
        }
        app->progress_dirty = !app->preview;
        show_notice(app, "图像已恢复，请再次按确定继续");
        return true;
    }
    show_notice(app, "图像解码失败，已停在此句；按确定重试");
    return false;
}

// force=true is the short-press/fast-forward contract: always move the cursor
// (next complete page, else next sentence) and reveal the destination page.
// It never merely consumes a key while typing or waiting for auto-play.
static void advance_reading(nine_app_t *app, bool force)
{
    if (!app || app->vm_error || app->opening) return;
    // A resource failure retries ONLY the current frame; the same press never
    // advances the cursor or commits story progress.
    if (app->resource_error || app->image_error) {
        (void)retry_frame_image(app);
        return;
    }
    const nine_view_t *v = &app->vm.view;
    if (v->kind != NINE_VM_TEXT) return;
    if ((int)app->text_page + 1 < app->page_count) {
        app->text_page++;
        nine_frame_t f;
        if (nine_vm_current_frame(&app->vm, &f)) {
            app->shown_page = app->text_page;
            show_frame(app, &f);
            if (force) gal_ui_show_full_text(&app->ui);
        }
        return;
    }
    const nine_view_t *next = nine_vm_advance(&app->vm);
    if (!next) {
        app->vm_error = true;
        copy_str(app->error, sizeof(app->error), "剧情推进失败");
        render_error(app);
        return;
    }
    sync_view(app, true);
    if (force && app->page == GAL_PAGE_GAME) gal_ui_show_full_text(&app->ui);
}

// Dedicated whole-sentence fast-forward transaction. It advances exactly one
// VM frame per call, never walks the page remainder, and never uses a catch-up
// loop. Callers hold the LVGL lock.
static bool ff_advance_sentence(nine_app_t *app)
{
    if (!app || app->vm_error || app->opening || app->page != GAL_PAGE_GAME ||
        !app_readable(app) || app->vm.view.kind != NINE_VM_TEXT) {
        if (app) app->fast_forward = false;
        return false;
    }
    const nine_view_t *next = nine_vm_advance(&app->vm);
    if (!next) {
        app->vm_error = true;
        app->fast_forward = false;
        copy_str(app->error, sizeof(app->error), "剧情推进失败");
        render_error(app);
        return false;
    }
    sync_view(app, true);
    if (app->page == GAL_PAGE_GAME && app->vm.view.kind == NINE_VM_TEXT)
        gal_ui_show_full_text(&app->ui);
    // A destination choice/ending, OP, or a failed image/text resource is a
    // hard stop: no next sentence may be consumed while still holding UP.
    if (app->page != GAL_PAGE_GAME ||
        app->vm.view.kind != NINE_VM_TEXT || app->opening || !app_readable(app))
        app->fast_forward = false;
    return true;
}

static void fail_opening(nine_app_t *app, const char *message)
{
    if (!app->opening) return;
    const bool gallery = app->op_gallery;
    app->opening = false;
    app->op_gallery = false;
    app->op_image = NINE_NONE;
    if (app->media_ok) nine_media_stop();
    app->resource_error = true;
    app->image_error = true;
    if (gallery) {
        app->page = GAL_PAGE_GALLERY;
        nine_image_set_visible(false);
        render_page(app);
        show_notice(app, message ? message : "片头资源解码失败；未播放");
    } else {
        copy_str(app->error, sizeof(app->error), message ? message : "片头资源解码失败");
        render_error(app);
    }
}

static void finish_opening(nine_app_t *app, bool skipped)
{
    if (!app->opening) return;
    if (app->media_ok && nine_media_failed()) {
        fail_opening(app, "片头音频播放失败");
        return;
    }
    const bool gallery = app->op_gallery;
    op_diagnostic(app, skipped ? "skip" : "end");
    app->opening = false;
    app->op_gallery = false;
    app->fast_forward = false;
    app->op_image = NINE_NONE;
    if (app->media_ok) nine_media_stop();
    // Gallery playback never touches the live VM, save slots, preview state or
    // unlock progress. It only returns to the same selected Gallery row.
    if (gallery) {
        app->page = GAL_PAGE_GALLERY;
        render_page(app);
        return;
    }
    const nine_view_t *next = nine_vm_advance(&app->vm);
    if (!next) {
        app->vm_error = true;
        copy_str(app->error, sizeof(app->error), "片头结束后无法继续");
        render_error(app);
        return;
    }
    sync_view(app, true);
}

static void tick_opening(nine_app_t *app, uint32_t elapsed)
{
    if (!app->opening) return;
    uint32_t pos;
    if (app->media_ok && nine_media_current_track() == 1) {
        pos = nine_media_position_ms();
        if (pos < app->op_ms) pos = app->op_ms;
        if (nine_media_failed()) {
            fail_opening(app, "片头音频播放失败");
            return;
        }
    } else {
        app->op_ms += elapsed;
        pos = app->op_ms;
    }
    app->op_ms = pos;
    if (app->image_ok) {
        const uint32_t count = nine_image_op_count();
        uint32_t image = 0, ts = 0, dur = 0;
        uint32_t pick = NINE_NONE;
        uint32_t due = 0;
        for (uint32_t i = app->op_frame; i < count; ++i) {
            if (!nine_image_op_frame(i, &image, &ts, &dur)) break;
            if (ts > pos) break;
            pick = image;
            ++due;
            app->op_frame = i + 1;
        }
        if (due > 1) app->op_late_skips += due - 1;
        if (pick != NINE_NONE && pick != app->op_image) {
            const uint32_t start = op_now_us();
            const bool shown = nine_image_show_asset_top(pick);
            const uint32_t cost = op_now_us() - start;
            if (cost > app->op_max_decode_us) app->op_max_decode_us = cost;
            if (!shown) {
                fail_opening(app, "片头画面解码失败");
                return;
            }
            app->op_image = pick;
            ++app->op_frames_drawn;
        }
    }
    if (pos >= app->op_duration) finish_opening(app, false);
}

static void render_page(nine_app_t *app)
{
    if (!app) return;
    gal_ui_show_page(&app->ui, app->page);

    switch (app->page) {
    case GAL_PAGE_START: {
        static const char *const rows[] = { "开始游戏" };
        gal_ui_set_rows(&app->ui, GAL_LIST_START, rows, NULL, 1, 0);
        nine_image_set_visible(false);
        return;
    }
    case GAL_PAGE_TITLE: {
        if (app->cfg.title_bg > 2 || !bg_unlocked(app, app->cfg.title_bg)) {
            app->cfg.title_bg = (uint8_t)highest_bg(app);
            (void)nine_cfg_save(&app->store, &app->cfg);
        }
        if (app->image_ok) {
            uint32_t image = 0;
            if (nine_image_title(app->cfg.title_bg, &image) &&
                image != NINE_NONE && nine_image_show_asset_top(image)) {
                nine_image_set_visible(true);
            } else {
                nine_image_set_visible(false);
                show_notice(app, "标题图像解码失败");
            }
        }
        gal_ui_set_rows(&app->ui, GAL_LIST_TITLE, TITLE_MENU, NULL,
                        (int)(sizeof(TITLE_MENU) / sizeof(TITLE_MENU[0])),
                        app->title_sel);
        gal_ui_set_row_enabled(&app->ui, GAL_LIST_TITLE, TITLE_CONTINUE, app->auto_valid);
        bool any_slot = false;
        for (unsigned i = 0; i < NINE_STORE_SLOTS; ++i) any_slot |= app->slot_used[i];
        gal_ui_set_row_enabled(&app->ui, GAL_LIST_TITLE, TITLE_LOAD, any_slot);
        return;
    }
    case GAL_PAGE_GAME: {
        if (app->vm_error) {
            render_error(app);
            return;
        }
        nine_image_set_visible(app->image_ok);
        if (app->vm.view.kind == NINE_VM_TEXT &&
            (app->shown_node != app->vm.state.node ||
             app->shown_index != app->vm.state.index ||
             app->shown_page != app->text_page)) {
            nine_frame_t f;
            if (nine_vm_current_frame(&app->vm, &f)) show_frame(app, &f);
        }
        gal_ui_set_mode(&app->ui, app->auto_mode);
        return;
    }
    case GAL_PAGE_CHOICE:
        nine_image_set_visible(app->image_ok);
        render_options(app);
        return;
    case GAL_PAGE_MENU:
        gal_ui_set_rows(&app->ui, GAL_LIST_MENU, MENU_ITEMS, NULL,
                        (int)(sizeof(MENU_ITEMS) / sizeof(MENU_ITEMS[0])), app->menu_sel);
        nine_image_set_visible(app->image_ok);
        return;
    case GAL_PAGE_SAVE:
    case GAL_PAGE_LOAD:
        nine_image_set_visible(false);
        render_slots(app, app->page == GAL_PAGE_SAVE);
        return;
    case GAL_PAGE_SETTINGS:
        nine_image_set_visible(false);
        render_settings(app);
        return;
    case GAL_PAGE_HISTORY:
        nine_image_set_visible(false);
        render_history(app);
        return;
    case GAL_PAGE_GALLERY: {
        nine_image_set_visible(false);
        char values[5][24];
        snprintf(values[0], sizeof(values[0]), "%u 张", (unsigned)app->pack.gallery_count);
        snprintf(values[1], sizeof(values[1]), "%u 章", (unsigned)app->pack.chapter_count);
        snprintf(values[2], sizeof(values[2]), "%u 个", (unsigned)app->pack.ending_count);
        snprintf(values[3], sizeof(values[3]), "%s", "110.18 秒");
        snprintf(values[4], sizeof(values[4]), "%u 曲", app->media_ok ? 2u : 0u);
        const char *vp[5] = { values[0], values[1], values[2], values[3], values[4] };
        gal_ui_set_rows(&app->ui, GAL_LIST_GALLERY, GALLERY_ITEMS, vp, 5,
                        app->gallery_sel);
        return;
    }
    case GAL_PAGE_CG_LIST:
        nine_image_set_visible(false);
        render_cg_list(app);
        return;
    case GAL_PAGE_CG:
        if (app->image_ok) nine_image_set_visible(true);
        if (app->op_gallery) render_op_view(app);
        else render_cg_view(app);
        return;
    case GAL_PAGE_RECALL:
        nine_image_set_visible(false);
        render_endings(app);
        return;
    case GAL_PAGE_MUSIC:
        nine_image_set_visible(false);
        render_music(app);
        return;
    case GAL_PAGE_CHAPTER:
        nine_image_set_visible(false);
        render_chapters(app);
        return;
    case GAL_PAGE_ENDING:
        nine_image_set_visible(false);
        return;
    case GAL_PAGE_ABOUT:
        nine_image_set_visible(false);
        render_about(app);
        return;
    default:
        return;
    }
}

// ---------------------------------------------------------------- page fills
static void fill_slots(nine_app_t *app, bool saving)
{
    const gal_list_t list = saving ? GAL_LIST_SAVE : GAL_LIST_LOAD;
    const int sel = saving ? app->save_sel : app->load_sel;
    static char labels[NINE_STORE_SLOTS][20];
    static char values[NINE_STORE_SLOTS][80];
    const char *lp[NINE_STORE_SLOTS];
    const char *vp[NINE_STORE_SLOTS];
    for (unsigned i = 0; i < NINE_STORE_SLOTS; ++i) {
        snprintf(labels[i], sizeof(labels[i]), "存档 %02u", i + 1);
        lp[i] = labels[i];
        if (app->slot_used[i]) {
            char chapter[64];
            if (!chapter_title(app, app->slot_node[i], chapter, sizeof(chapter)))
                copy_str(chapter, sizeof(chapter), "未知章节");
            snprintf(values[i], sizeof(values[i]), "%.60s · 第 %u 页", chapter,
                     (unsigned)app->slot_page[i] + 1u);
        } else {
            copy_str(values[i], sizeof(values[i]), "空槽位");
        }
        vp[i] = values[i];
    }
    gal_ui_set_rows(&app->ui, list, lp, vp, NINE_STORE_SLOTS, sel);
    for (unsigned i = 0; i < NINE_STORE_SLOTS; ++i)
        gal_ui_set_row_enabled(&app->ui, list, i, saving || app->slot_used[i]);
    if (app->slot_used[sel]) {
        char preview[128];
        char chapter[64];
        if (!chapter_title(app, app->slot_node[sel], chapter, sizeof(chapter)))
            copy_str(chapter, sizeof(chapter), "未知章节");
        snprintf(preview, sizeof(preview), "%s\n第 %d 页", chapter, app->slot_page[sel] + 1);
        gal_ui_set_save_preview(&app->ui, preview);
    } else {
        gal_ui_set_save_preview(&app->ui, "空槽位\n选择一个槽位后再操作");
    }
    gal_ui_set_footer(&app->ui, GAL_PAGE_SAVE, "共 3 个槽位 · 长按确定返回");
    gal_ui_set_footer(&app->ui, GAL_PAGE_LOAD,
                      app->slot_used[0] || app->slot_used[1] || app->slot_used[2]
                          ? "共 3 个槽位 · 长按确定返回"
                          : "暂无存档");
}

static void render_slots(nine_app_t *app, bool saving)
{
    fill_slots(app, saving);
}

static void render_settings(nine_app_t *app)
{
    const bool reader = app->return_page == GAL_PAGE_GAME || app->return_page == GAL_PAGE_MENU;
    static char transparency[8];
    snprintf(transparency, sizeof(transparency), "%u%%",
             (unsigned)app->cfg.display.panel_transparency);
    const char *values_reader[7] = {
        speed_label(app), app->cfg.display.font_large ? "大" : "小",
        app->cfg.display.auto_play ? "开" : "关", transparency, "", "", "",
    };
    const char *values_title[6] = {
        speed_label(app), app->cfg.display.font_large ? "大" : "小",
        app->cfg.display.auto_play ? "开" : "关", transparency, "", "",
    };
    if (reader)
        gal_ui_set_rows(&app->ui, GAL_LIST_SETTINGS, SETTINGS_READER, values_reader, 7,
                        app->settings_sel);
    else
        gal_ui_set_rows(&app->ui, GAL_LIST_SETTINGS, SETTINGS_TITLE, values_title, 6,
                        app->settings_sel);
}

static void render_history(nine_app_t *app)
{
    static const char *lp[NINE_APP_HISTORY_WINDOW];
    const int count = app->history_count;
    app->history_window = window_for(app->history_sel, app->history_window,
                                     NINE_APP_HISTORY_WINDOW);
    int shown = count - app->history_window;
    if (shown > NINE_APP_HISTORY_WINDOW) shown = NINE_APP_HISTORY_WINDOW;
    if (shown < 0) shown = 0;
    for (int i = 0; i < shown; ++i) lp[i] = app->history[app->history_window + i];
    gal_ui_set_rows(&app->ui, GAL_LIST_HISTORY, lp, NULL, shown,
                    app->history_sel - app->history_window);
    static char footer[48];
    if (count) snprintf(footer, sizeof(footer), "%d-%d / %d · 长按确定返回",
                        app->history_window + 1, app->history_window + shown, count);
    gal_ui_set_footer(&app->ui, GAL_PAGE_HISTORY,
                      count ? footer : "还没有历史文本");
}

static void render_cg_list(nine_app_t *app)
{
    static char labels[NINE_APP_CG_WINDOW][32];
    static char values[NINE_APP_CG_WINDOW][12];
    const char *lp[NINE_APP_CG_WINDOW];
    const char *vp[NINE_APP_CG_WINDOW];
    const int total = (int)app->pack.gallery_count;
    app->cg_sel = total ? (app->cg_sel < 0 ? 0 : app->cg_sel % total) : 0;
    app->cg_window = window_for(app->cg_sel, app->cg_window, NINE_APP_CG_WINDOW);
    int shown = total - app->cg_window;
    if (shown > NINE_APP_CG_WINDOW) shown = NINE_APP_CG_WINDOW;
    if (shown < 0) shown = 0;
    for (int i = 0; i < shown; ++i) {
        nine_gallery_record_t g;
        if (!nine_pack_gallery(&app->pack, (uint32_t)(app->cg_window + i), &g)) {
            copy_str(labels[i], sizeof(labels[i]), "?");
            values[i][0] = '\0';
            continue;
        }
        const char *name = nine_pack_string(&app->pack, g.name_str);
        snprintf(labels[i], sizeof(labels[i]), "%03d  %s", app->cg_window + i + 1,
                 name ? name : "");
        snprintf(values[i], sizeof(values[i]), "组 %u", (unsigned)g.group + 1u);
    }
    for (int i = 0; i < shown; ++i) {
        lp[i] = labels[i];
        vp[i] = values[i];
    }
    gal_ui_set_rows(&app->ui, GAL_LIST_CG, lp, vp, shown, app->cg_sel - app->cg_window);
    static char title[48];
    snprintf(title, sizeof(title), "CG 鉴赏 · %d-%d / %d", app->cg_window + 1,
             app->cg_window + shown, total);
    gal_ui_set_page_title(&app->ui, GAL_PAGE_CG_LIST, title);
    gal_ui_set_footer(&app->ui, GAL_PAGE_CG_LIST, "上下选择 · 确定全屏 · 长按返回");
}

static void render_cg_view(nine_app_t *app)
{
    nine_gallery_record_t g;
    const char *name = "";
    if (app->cg_sel >= 0 && app->cg_sel < (int)app->pack.gallery_count &&
        nine_pack_gallery(&app->pack, (uint32_t)app->cg_sel, &g)) {
        name = nine_pack_string(&app->pack, g.name_str);
        if (app->image_ok) {
            if (!nine_image_show_asset(g.image)) show_notice(app, "CG 解码失败");
        }
    }
    char index[32];
    snprintf(index, sizeof(index), "CG %03d / %u", app->cg_sel + 1,
             (unsigned)app->pack.gallery_count);
    gal_ui_set_cg(&app->ui, index, name ? name : "", "完整构图 · 上下切换 · 确定返回",
                  NULL);
}

static void render_op_view(nine_app_t *app)
{
    if (app->image_ok) nine_image_set_visible(true);
    gal_ui_set_cg(&app->ui, "OP", "片头", "确定跳过 · 播完返回", NULL);
}

static void render_endings(nine_app_t *app)
{
    static char labels[2][64];
    static char values[2][16];
    const char *lp[2];
    const char *vp[2];
    const int count = (int)(app->pack.ending_count > 2 ? 2 : app->pack.ending_count);
    for (int i = 0; i < count; ++i) {
        nine_ending_t e;
        const char *name = "";
        if (nine_pack_ending(&app->pack, (uint32_t)i, &e))
            name = nine_pack_string(&app->pack, e.name);
        snprintf(labels[i], sizeof(labels[i]), "%s", name ? name : "");
        copy_str(values[i], sizeof(values[i]),
                 (app->unlock.endings & (1u << i)) ? "已达成" : "未达成");
        lp[i] = labels[i];
        vp[i] = values[i];
    }
    if (app->ending_sel >= count) app->ending_sel = count > 0 ? count - 1 : 0;
    gal_ui_set_page_title(&app->ui, GAL_PAGE_RECALL, "结局回顾");
    gal_ui_set_rows(&app->ui, GAL_LIST_RECALL, lp, vp, count, app->ending_sel);
    gal_ui_set_footer(&app->ui, GAL_PAGE_RECALL, "上下选择 · 确定回看 · 长按返回");
}

static void render_music(nine_app_t *app)
{
    static char labels[2][24];
    static char values[2][16];
    const char *lp[2];
    const char *vp[2];
    const int count = app->media_ok ? (int)nine_media_track_count() : 0;
    const uint32_t playing = app->media_ok && nine_media_is_playing()
                                   ? nine_media_current_track() : NINE_NONE;
    for (int i = 0; i < count; ++i) {
        nine_media_track_t t;
        const char *label = i == 0 ? "标题曲" : (i == 1 ? "片头曲" : "曲目");
        snprintf(labels[i], sizeof(labels[i]), "%s", label);
        if (nine_media_track_info((uint32_t)i, &t)) {
            if (playing == (uint32_t)i) {
                copy_str(values[i], sizeof(values[i]), "播放中");
            } else {
                snprintf(values[i], sizeof(values[i]), "%u:%02u",
                         (unsigned)(t.duration_ms / 60000u),
                         (unsigned)((t.duration_ms / 1000u) % 60u));
            }
        }
        lp[i] = labels[i];
        vp[i] = values[i];
    }
    if (app->music_sel >= count) app->music_sel = count > 0 ? count - 1 : 0;
    gal_ui_set_rows(&app->ui, GAL_LIST_MUSIC, lp, vp, count, app->music_sel);
    gal_ui_set_footer(&app->ui, GAL_PAGE_MUSIC, "确定播放/停止 · 长按返回");
}

static void render_chapters(nine_app_t *app)
{
    static char labels[NINE_APP_CHAPTER_WINDOW][48];
    static char values[NINE_APP_CHAPTER_WINDOW][12];
    const char *lp[NINE_APP_CHAPTER_WINDOW];
    const char *vp[NINE_APP_CHAPTER_WINDOW];
    const int total = (int)app->pack.chapter_count;
    app->chapter_window = window_for(app->chapter_sel, app->chapter_window,
                                     NINE_APP_CHAPTER_WINDOW);
    int shown = total - app->chapter_window;
    if (shown > NINE_APP_CHAPTER_WINDOW) shown = NINE_APP_CHAPTER_WINDOW;
    if (shown < 0) shown = 0;
    for (int i = 0; i < shown; ++i) {
        nine_chapter_t c;
        const char *title = "";
        if (nine_pack_chapter(&app->pack, (uint32_t)(app->chapter_window + i), &c))
            title = nine_pack_string(&app->pack, c.title);
        snprintf(labels[i], sizeof(labels[i]), "%s", title ? title : "");
        copy_str(values[i], sizeof(values[i]),
                 (app->unlock.chapters & (1u << (app->chapter_window + i))) ? "已读" : "");
        lp[i] = labels[i];
        vp[i] = values[i];
    }
    gal_ui_set_rows(&app->ui, GAL_LIST_CHAPTER, lp, vp, shown,
                    app->chapter_sel - app->chapter_window);
    char info[256];
    char preview[160];
    if (!chapter_preview(app, app->chapter_sel, preview, sizeof(preview)))
        copy_str(preview, sizeof(preview), "章节开头");
    snprintf(info, sizeof(info), "%s\n%s", preview, "试读不会写入通关进度");
    gal_ui_set_chapter_info(&app->ui, info);
    static char footer[48];
    snprintf(footer, sizeof(footer), "%d / %u 章 · 长按返回", app->chapter_sel + 1,
             (unsigned)app->pack.chapter_count);
    gal_ui_set_action_hint(&app->ui, footer);
}

static void render_about(nine_app_t *app)
{
    char text[768];
    const char *title = nine_pack_title(&app->pack);
    snprintf(text, sizeof(text),
             "作品：%s\n"
             "版本：%s\n"
             "原作：Palette《9-nine-》\n"
             "脚本范围：第 01–23 章，含 D02 三分枝与 D20 四分枝\n"
             "内容：简体中文主线，离线阅读\n"
             "素材：标题/片头/立绘/表情/CG 均来自上游移植仓库\n"
             "说明：本移植只重做界面与运行层，原作权利归原作者。",
             title ? title : TITLE_CN, APP_VERSION);
    gal_ui_set_about(&app->ui, TITLE_CN, TITLE_LOGO " " TITLE_SUB, text);
}

// ---------------------------------------------------------------- reader actions
static void start_new_game(nine_app_t *app)
{
    if (app->fatal || app->resource_fatal) return;
    if (app->media_ok) nine_media_stop();
    app->history_count = 0;
    app->history_sel = 0;
    app->preview = false;
    app->choice_sel = 0;
    app->text_page = 0;
    app->visual_valid = false;
    app->vm_error = false;
    app->error[0] = '\0';
    app->auto_mode = app->cfg.display.auto_play != 0;
    app->auto_ms = 0;
    app->fast_forward = false;
    apply_perm(app);
    (void)nine_vm_start(&app->vm, NINE_NODE_NONE, false);
    sync_view(app, true);
    save_auto(app);
}

static void continue_game(nine_app_t *app)
{
    if (app->fatal || app->resource_fatal) return;
    uint8_t blob[NINE_APP_SAVE_CAPACITY];
    size_t len = 0;
    if (!nine_store_get(&app->store, "auto", blob, sizeof(blob), &len)) {
        show_notice(app, "没有可继续的进度");
        app->auto_valid = false;
        return;
    }
    uint16_t node;
    uint32_t index;
    uint16_t page;
    bool preview;
    if (!nine_vm_save_peek(&app->pack, blob, len, &node, &index, &page, &preview)) {
        show_notice(app, "自动进度损坏");
        app->auto_valid = false;
        return;
    }
    if (!preview) apply_perm(app);
    if (!nine_vm_save_decode(&app->vm, blob, len, &page)) {
        show_notice(app, "自动进度损坏");
        return;
    }
    app->preview = preview;
    app->text_page = page;
    app->visual_valid = false;
    app->vm_error = false;
    app->history_count = 0;
    app->auto_mode = app->cfg.display.auto_play != 0;
    (void)nine_vm_locate(&app->vm);
    sync_view(app, false);
    if (app->media_ok) nine_media_stop();
}

static void start_chapter(nine_app_t *app, int chapter)
{
    if (app->fatal || app->resource_fatal || app->resource_error || app->image_error)
        return;
    nine_chapter_t c;
    if (!nine_pack_chapter(&app->pack, (uint32_t)chapter, &c)) return;
    if (app->media_ok) nine_media_stop();
    app->history_count = 0;
    app->preview = true;
    app->choice_sel = 0;
    app->text_page = 0;
    app->visual_valid = false;
    app->vm_error = false;
    app->auto_mode = false;
    app->fast_forward = false;
    apply_perm(app);
    (void)nine_vm_start(&app->vm, c.node, true);
    sync_view(app, true);
}

static void do_save(nine_app_t *app, int slot)
{
    if (slot < 0 || slot >= (int)NINE_STORE_SLOTS) return;
    if (!app_readable(app)) {
        show_notice(app, "当前画面未正常显示，不能存档");
        return;
    }
    uint8_t blob[NINE_APP_SAVE_CAPACITY];
    const size_t n = nine_vm_save_encode(&app->vm, app->text_page, blob, sizeof(blob));
    if (!n) {
        show_notice(app, "保存失败：状态不可编码");
        return;
    }
    uint16_t node, page;
    uint32_t index;
    bool preview;
    if (!nine_vm_save_peek(&app->pack, blob, n, &node, &index, &page, &preview)) {
        show_notice(app, "保存失败：状态校验不通过");
        return;
    }
    static const char *const keys[NINE_STORE_SLOTS] = { "slot0", "slot1", "slot2" };
    if (!nine_store_put(&app->store, keys[slot], blob, n)) {
        show_notice(app, "保存失败：存储不可写");
        return;
    }
    app->slot_used[slot] = true;
    app->slot_node[slot] = node == NINE_NODE_NONE ? -1 : (int)node;
    app->slot_page[slot] = page;
    show_notice(app, "已保存");
    fill_slots(app, true);
}

static bool do_load(nine_app_t *app, int slot)
{
    if (app->fatal || app->resource_fatal) return false;
    if (slot < 0 || slot >= (int)NINE_STORE_SLOTS || !app->slot_used[slot]) {
        show_notice(app, "空槽位");
        return false;
    }
    static const char *const keys[NINE_STORE_SLOTS] = { "slot0", "slot1", "slot2" };
    uint8_t blob[NINE_APP_SAVE_CAPACITY];
    size_t len = 0;
    if (!nine_store_get(&app->store, keys[slot], blob, sizeof(blob), &len)) {
        show_notice(app, "读取失败：存储不可读");
        return false;
    }
    uint16_t node, page;
    uint32_t index;
    bool preview;
    if (!nine_vm_save_peek(&app->pack, blob, len, &node, &index, &page, &preview)) {
        show_notice(app, "存档损坏，未改变当前进度");
        return false;
    }
    if (!preview) apply_perm(app);
    if (!nine_vm_save_decode(&app->vm, blob, len, &page)) {
        show_notice(app, "存档损坏，未改变当前进度");
        return false;
    }
    app->preview = preview;
    app->text_page = page;
    app->visual_valid = false;
    app->vm_error = false;
    app->history_count = 0;
    app->fast_forward = false;
    app->auto_mode = false;
    (void)nine_vm_locate(&app->vm);
    sync_view(app, false);
    if (!app->preview) save_auto(app);
    if (app->media_ok) nine_media_stop();
    show_notice(app, "已读取");
    return true;
}

// ---------------------------------------------------------------- key handlers
static bool is_click(const nine_key_t *key)
{
    return key->ev == BSP_BTN_CLICK || key->ev == BSP_BTN_DOUBLE;
}

static void handle_start(nine_app_t *app, const nine_key_t *key)
{
    if (app->fatal) return;
    if (is_click(key) && key->btn == BSP_BTN_OK) {
        app->title_sel = 0;
        go_page(app, GAL_PAGE_TITLE);
    }
}

static void handle_title(nine_app_t *app, const nine_key_t *key)
{
    if (is_click(key) && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        move_selection(&app->title_sel, 6, key->btn == BSP_BTN_UP ? -1 : 1);
        gal_ui_select_row(&app->ui, GAL_LIST_TITLE, app->title_sel);
        return;
    }
    if (is_click(key) && key->btn == BSP_BTN_OK) {
        switch (app->title_sel) {
        case TITLE_NEW:
            start_new_game(app);
            break;
        case TITLE_CONTINUE:
            if (!app->auto_valid) {
                show_notice(app, "没有可继续的进度");
                break;
            }
            continue_game(app);
            break;
        case TITLE_LOAD: {
            bool any = false;
            for (unsigned i = 0; i < NINE_STORE_SLOTS; ++i) any |= app->slot_used[i];
            if (!any) {
                show_notice(app, "暂无存档");
                break;
            }
            app->load_sel = 0;
            for (unsigned i = 0; i < NINE_STORE_SLOTS; ++i)
                if (app->slot_used[i]) { app->load_sel = (int)i; break; }
            app->return_page = GAL_PAGE_TITLE;
            go_page(app, GAL_PAGE_LOAD);
            break;
        }
        case TITLE_SETTINGS:
            app->return_page = GAL_PAGE_TITLE;
            app->settings_sel = 0;
            go_page(app, GAL_PAGE_SETTINGS);
            break;
        case TITLE_GALLERY:
            app->gallery_sel = 0;
            go_page(app, GAL_PAGE_GALLERY);
            break;
        default:
            go_page(app, GAL_PAGE_ABOUT);
            break;
        }
        return;
    }
    // Long DOWN cycles the unlocked title visuals, mirroring the watch reader's
    // three title backgrounds without adding a fourth key.
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_DOWN) {
        int unlocked[3], count = 0;
        for (int i = 0; i < 3; ++i) if (bg_unlocked(app, i)) unlocked[count++] = i;
        if (count > 1) {
            int pos = 0;
            for (int i = 0; i < count; ++i) if (unlocked[i] == app->cfg.title_bg) pos = i;
            app->cfg.title_bg = (uint8_t)unlocked[(pos + 1) % count];
            (void)nine_cfg_save(&app->store, &app->cfg);
            render_page(app);
        }
    }
}

static void handle_game(nine_app_t *app, const nine_key_t *key)
{
    if (app->opening) {
        if (app->media_ok && nine_media_failed()) {
            // A failed audio sink wins over a skip/end key. Never let a key
            // turn a fault into a VM advance or a save/progress action.
            fail_opening(app, "片头音频播放失败");
            return;
        }
        if (key->ev == BSP_BTN_RELEASE) {
            if (key->btn == BSP_BTN_UP) app->fast_forward = false;
            return;
        }
        if (key->ev == BSP_BTN_CLICK || key->ev == BSP_BTN_LONG) finish_opening(app, true);
        return;
    }
    if (is_click(key) && (key->btn == BSP_BTN_OK || key->btn == BSP_BTN_UP ||
                          key->btn == BSP_BTN_DOWN)) {
        if (app->auto_mode) {
            app->auto_mode = false;
            app->cfg.display.auto_play = 0;
            (void)nine_cfg_save(&app->store, &app->cfg);
            gal_ui_set_mode(&app->ui, false);
        }
        advance_reading(app, true);
        return;
    }
    if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_LONG) {
        app->fast_forward = false;
        app->auto_mode = false;
        app->menu_sel = MENU_RESUME;
        app->menu_origin = GAL_PAGE_GAME;
        app->return_page = GAL_PAGE_GAME;
        go_page(app, GAL_PAGE_MENU);
        return;
    }
    if (key->btn == BSP_BTN_UP && key->ev == BSP_BTN_LONG) {
        app->auto_mode = false;
        app->cfg.display.auto_play = 0;
        (void)nine_cfg_save(&app->store, &app->cfg);
        gal_ui_set_mode(&app->ui, false);
        app->fast_forward = true;
        // The long-press threshold itself advances the next whole sentence;
        // do not wait for a tick or for the page remainder.
        (void)ff_advance_sentence(app);
        if (app->page == GAL_PAGE_GAME && app->vm.view.kind == NINE_VM_TEXT)
            gal_ui_show_full_text(&app->ui);
        return;
    }
    if (key->btn == BSP_BTN_UP && key->ev == BSP_BTN_RELEASE) {
        app->fast_forward = false;
        return;
    }
    if (key->btn == BSP_BTN_DOWN && key->ev == BSP_BTN_LONG) {
        app->auto_mode = !app->auto_mode;
        app->cfg.display.auto_play = app->auto_mode ? 1 : 0;
        (void)nine_cfg_save(&app->store, &app->cfg);
        app->auto_ms = 0;
        app->fast_forward = false;
        if (app->auto_mode) gal_ui_show_full_text(&app->ui);
        gal_ui_set_mode(&app->ui, app->auto_mode);
        return;
    }
}

static void handle_choice(nine_app_t *app, const nine_key_t *key)
{
    const int count = app->vm.view.choice_count;
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        app->menu_sel = MENU_RESUME;
        app->menu_origin = GAL_PAGE_CHOICE;
        app->return_page = GAL_PAGE_CHOICE;
        go_page(app, GAL_PAGE_MENU);
        return;
    }
    if (is_click(key) && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        move_selection(&app->choice_sel, count, key->btn == BSP_BTN_UP ? -1 : 1);
        gal_ui_select_row(&app->ui, GAL_LIST_CHOICE, app->choice_sel);
        return;
    }
    if (is_click(key) && key->btn == BSP_BTN_OK) {
        if (count <= 0) return;
        const nine_view_t *v = nine_vm_choose(&app->vm, (uint32_t)app->choice_sel);
        if (!v) {
            app->vm_error = true;
            copy_str(app->error, sizeof(app->error), "选项推进失败");
            render_error(app);
            return;
        }
        app->fast_forward = false;
        sync_view(app, true);
    }
}

static void handle_menu(nine_app_t *app, const nine_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        go_page(app, app->menu_origin == GAL_PAGE_CHOICE ? GAL_PAGE_CHOICE : GAL_PAGE_GAME);
        return;
    }
    if (is_click(key) && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        move_selection(&app->menu_sel, 6, key->btn == BSP_BTN_UP ? -1 : 1);
        gal_ui_select_row(&app->ui, GAL_LIST_MENU, app->menu_sel);
        return;
    }
    if (!is_click(key) || key->btn != BSP_BTN_OK) return;
    switch (app->menu_sel) {
    case MENU_RESUME:
        go_page(app, app->menu_origin == GAL_PAGE_CHOICE ? GAL_PAGE_CHOICE : GAL_PAGE_GAME);
        break;
    case MENU_SAVE:
        save_auto(app);
        app->save_sel = 0;
        app->return_page = GAL_PAGE_MENU;
        go_page(app, GAL_PAGE_SAVE);
        break;
    case MENU_LOAD:
        app->load_sel = 0;
        app->return_page = GAL_PAGE_MENU;
        go_page(app, GAL_PAGE_LOAD);
        break;
    case MENU_HISTORY:
        app->history_sel = app->history_count > 0 ? app->history_count - 1 : 0;
        app->return_page = GAL_PAGE_MENU;
        go_page(app, GAL_PAGE_HISTORY);
        break;
    case MENU_SETTINGS:
        app->return_page = GAL_PAGE_MENU;
        app->settings_sel = 0;
        go_page(app, GAL_PAGE_SETTINGS);
        break;
    default:
        // A conscious return to the title abandons the uncommitted failed
        // frame; the next New/Continue attempt is the explicit retry.
        app->preview = false;
        app->resource_error = false;
        app->image_error = false;
        app->title_sel = 0;
        go_page(app, GAL_PAGE_TITLE);
        break;
    }
}

static void handle_save_load(nine_app_t *app, const nine_key_t *key)
{
    const bool saving = app->page == GAL_PAGE_SAVE;
    int *sel = saving ? &app->save_sel : &app->load_sel;
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        go_page(app, app->return_page);
        return;
    }
    if (is_click(key) && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        move_selection(sel, NINE_STORE_SLOTS, key->btn == BSP_BTN_UP ? -1 : 1);
        fill_slots(app, saving);
        return;
    }
    if (!is_click(key) || key->btn != BSP_BTN_OK) return;
    if (saving) {
        if (app->slot_used[*sel]) {
            app->modal = true;
            app->modal_sel = 0;
            gal_ui_show_modal(&app->ui, GAL_MODAL_CONFIRM, "覆盖该存档?",
                              "原有进度将被新存档替换。\n此操作无法撤销。",
                              MODAL_ACTIONS, 2, 0);
            return;
        }
        do_save(app, *sel);
        return;
    }
    if (do_load(app, *sel)) {
        app->return_page = GAL_PAGE_GAME;
    }
}

static void handle_modal(nine_app_t *app, const nine_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        gal_ui_hide_modal(&app->ui);
        app->modal = false;
        return;
    }
    if (is_click(key) && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        move_selection(&app->modal_sel, 2, key->btn == BSP_BTN_UP ? -1 : 1);
        gal_ui_select_row(&app->ui, GAL_LIST_MODAL, app->modal_sel);
        return;
    }
    if (!is_click(key) || key->btn != BSP_BTN_OK) return;
    const int sel = app->modal_sel;
    gal_ui_hide_modal(&app->ui);
    app->modal = false;
    if (sel == 1) do_save(app, app->save_sel);
}

static bool settings_from_reader(const nine_app_t *app)
{
    return app->return_page == GAL_PAGE_GAME || app->return_page == GAL_PAGE_MENU;
}

static void handle_settings(nine_app_t *app, const nine_key_t *key)
{
    const int count = settings_from_reader(app) ? 7 : 6;
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        go_page(app, app->return_page);
        return;
    }
    if (is_click(key) && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        move_selection(&app->settings_sel, count, key->btn == BSP_BTN_UP ? -1 : 1);
        gal_ui_select_row(&app->ui, GAL_LIST_SETTINGS, app->settings_sel);
        return;
    }
    if (!is_click(key) || key->btn != BSP_BTN_OK) return;
    int action = app->settings_sel;
    if (!settings_from_reader(app) && action >= 5) action = SACT_BACK;
    switch (action) {
    case SACT_SPEED:
        app->cfg.display.text_speed = (uint8_t)((app->cfg.display.text_speed + 1) % 4);
        break;
    case SACT_SIZE:
        app->cfg.display.font_large = app->cfg.display.font_large ? 0 : 1;
        gal_ui_set_font_size(&app->ui, app->cfg.display.font_large != 0);
        app->shown_index = UINT32_MAX;
        break;
    case SACT_AUTO:
        app->cfg.display.auto_play = app->cfg.display.auto_play ? 0 : 1;
        app->auto_mode = app->cfg.display.auto_play != 0;
        break;
    case SACT_TRANSPARENCY:
        app->cfg.display.panel_transparency =
            gal_transparency_next(app->cfg.display.panel_transparency);
        gal_ui_set_panel_transparency(&app->ui, app->cfg.display.panel_transparency);
        break;
    case SACT_RESET: {
        nine_cfg_t reset;
        nine_cfg_default(&reset);
        reset.title_bg = app->cfg.title_bg;
        app->cfg = reset;
        app->auto_mode = false;
        gal_ui_set_font_size(&app->ui, false);
        gal_ui_set_panel_transparency(&app->ui, app->cfg.display.panel_transparency);
        app->shown_index = UINT32_MAX;
        break;
    }
    case SACT_TITLE:
        app->preview = false;
        go_page(app, GAL_PAGE_TITLE);
        return;
    default:
        go_page(app, app->return_page);
        return;
    }
    (void)nine_cfg_save(&app->store, &app->cfg);
    render_settings(app);
}

static void handle_history(nine_app_t *app, const nine_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        go_page(app, app->return_page);
        return;
    }
    if (is_click(key) && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        const int count = app->history_count > 0 ? app->history_count : 1;
        move_selection(&app->history_sel, count, key->btn == BSP_BTN_UP ? -1 : 1);
        render_history(app);
    }
}

static void start_gallery_op(nine_app_t *app)
{
    if (!app || !app->image_ok || nine_image_op_count() == 0) {
        show_notice(app, "片头资源不可用");
        return;
    }
    app->page = GAL_PAGE_CG;
    begin_op_state(app, true);
    if (!play_op_audio(app) || !show_first_op_frame(app)) {
        app->opening = false;
        app->op_gallery = false;
        if (app->media_ok) nine_media_stop();
        nine_image_set_visible(false);
        app->page = GAL_PAGE_GALLERY;
        render_page(app);
        show_notice(app, "片头图像或音频无法播放");
        return;
    }
    render_page(app);
}

static void handle_gallery(nine_app_t *app, const nine_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        go_page(app, GAL_PAGE_TITLE);
        return;
    }
    if (is_click(key) && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        move_selection(&app->gallery_sel, 5, key->btn == BSP_BTN_UP ? -1 : 1);
        gal_ui_select_row(&app->ui, GAL_LIST_GALLERY, app->gallery_sel);
        return;
    }
    if (!is_click(key) || key->btn != BSP_BTN_OK) return;
    switch (app->gallery_sel) {
    case GAL_CG:
        app->cg_sel = 0;
        app->cg_window = 0;
        go_page(app, GAL_PAGE_CG_LIST);
        break;
    case GAL_CHAPTER:
        app->chapter_sel = 0;
        app->chapter_window = 0;
        app->return_page = GAL_PAGE_GALLERY;
        go_page(app, GAL_PAGE_CHAPTER);
        break;
    case GAL_ENDINGS:
        app->ending_sel = 0;
        go_page(app, GAL_PAGE_RECALL);
        break;
    case GAL_OP:
        start_gallery_op(app);
        break;
    default:
        app->music_sel = 0;
        go_page(app, GAL_PAGE_MUSIC);
        break;
    }
}

static void handle_cg_list(nine_app_t *app, const nine_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        go_page(app, GAL_PAGE_GALLERY);
        return;
    }
    if (is_click(key) && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        move_selection(&app->cg_sel, (int)app->pack.gallery_count,
                       key->btn == BSP_BTN_UP ? -1 : 1);
        render_cg_list(app);
        return;
    }
    if (is_click(key) && key->btn == BSP_BTN_OK) {
        if (app->cg_sel < 0 || app->cg_sel >= (int)app->pack.gallery_count) return;
        go_page(app, GAL_PAGE_CG);
    }
}

static void handle_cg(nine_app_t *app, const nine_key_t *key)
{
    if (app->op_gallery) {
        if ((is_click(key) && key->btn == BSP_BTN_OK) ||
            (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK))
            finish_opening(app, true);
        return;
    }
    if (is_click(key) && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        move_selection(&app->cg_sel, (int)app->pack.gallery_count,
                       key->btn == BSP_BTN_UP ? -1 : 1);
        render_cg_view(app);
        return;
    }
    if ((is_click(key) || key->ev == BSP_BTN_LONG) && key->btn == BSP_BTN_OK) {
        go_page(app, GAL_PAGE_CG_LIST);
    }
}

static void handle_chapters(nine_app_t *app, const nine_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        go_page(app, app->return_page);
        return;
    }
    if (is_click(key) && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        move_selection(&app->chapter_sel, (int)app->pack.chapter_count,
                       key->btn == BSP_BTN_UP ? -1 : 1);
        render_chapters(app);
        return;
    }
    if (is_click(key) && key->btn == BSP_BTN_OK) start_chapter(app, app->chapter_sel);
}

static void handle_endings(nine_app_t *app, const nine_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        go_page(app, GAL_PAGE_GALLERY);
        return;
    }
    if (is_click(key) && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        move_selection(&app->ending_sel, (int)app->pack.ending_count,
                       key->btn == BSP_BTN_UP ? -1 : 1);
        render_endings(app);
        return;
    }
    if (is_click(key) && key->btn == BSP_BTN_OK) {
        app->preview = false;
        prepare_ending(app, true, app->ending_sel);
        go_page(app, GAL_PAGE_ENDING);
    }
}

static void handle_music(nine_app_t *app, const nine_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        if (app->media_ok) nine_media_stop();
        go_page(app, GAL_PAGE_GALLERY);
        return;
    }
    if (is_click(key) && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        move_selection(&app->music_sel, (int)nine_media_track_count(),
                       key->btn == BSP_BTN_UP ? -1 : 1);
        render_music(app);
        return;
    }
    if (!is_click(key) || key->btn != BSP_BTN_OK || !app->media_ok) return;
    const uint32_t current = nine_media_current_track();
    if (current == (uint32_t)app->music_sel) nine_media_stop();
    else (void)nine_media_play((uint32_t)app->music_sel);
    render_music(app);
}

static void handle_ending(nine_app_t *app, const nine_key_t *key)
{
    if ((is_click(key) || key->ev == BSP_BTN_LONG) && key->btn == BSP_BTN_OK) {
        app->preview = false;
        go_page(app, app->ending_return ? app->ending_return : GAL_PAGE_TITLE);
    }
}

static void handle_about(nine_app_t *app, const nine_key_t *key)
{
    if ((is_click(key) || key->ev == BSP_BTN_LONG) &&
        (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        const int dir = key->btn == BSP_BTN_UP ? -1 : 1;
        gal_ui_scroll(&app->ui, GAL_PAGE_ABOUT, dir * (key->ev == BSP_BTN_LONG ? 80 : 20));
        return;
    }
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) go_page(app, GAL_PAGE_TITLE);
}

void nine_app_key(nine_app_t *app, const nine_key_t *key)
{
    if (!app || !key) return;
    app->idle_ms = 0;

    // A long press consumes its release click. A new press clears the guard, so
    // a genuinely new short click is never swallowed.
    if (key->ev == BSP_BTN_PRESS) {
        app->ignore_click_mask &= (uint8_t)~(1u << key->btn);
        return;
    }
    if (key->ev == BSP_BTN_LONG) {
        app->ignore_click_mask |= (uint8_t)(1u << key->btn);
        app->ignore_click_ms = 0;
    }
    if (is_click(key) && (app->ignore_click_mask & (1u << key->btn))) {
        app->ignore_click_mask &= (uint8_t)~(1u << key->btn);
        return;
    }
    if (app->fatal) return;
    if (!bsp_lvgl_lock(200)) {
        ESP_LOGW(TAG, "LVGL lock timeout; key ignored");
        return;
    }
    if (app->modal) {
        handle_modal(app, key);
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
    case GAL_PAGE_CG: handle_cg(app, key); break;
    case GAL_PAGE_CHAPTER: handle_chapters(app, key); break;
    case GAL_PAGE_RECALL: handle_endings(app, key); break;
    case GAL_PAGE_MUSIC: handle_music(app, key); break;
    case GAL_PAGE_ENDING: handle_ending(app, key); break;
    case GAL_PAGE_ABOUT: handle_about(app, key); break;
    default: break;
    }
    bsp_lvgl_unlock();
}

// ---------------------------------------------------------------- tick
void nine_app_tick(nine_app_t *app, uint32_t elapsed_ms)
{
    if (!app) return;
    app->idle_ms += elapsed_ms;
    if (app->fatal) return;

    if (app->opening) {
        if (bsp_lvgl_lock(100)) {
            tick_opening(app, elapsed_ms);
            bsp_lvgl_unlock();
        }
        app->idle_ms = 0;
    }

    if (app->fast_forward && app->page == GAL_PAGE_GAME && !app->opening) {
        // At most one whole sentence per scheduler/input-control tick. There is
        // deliberately no 90/120 ms accumulation and no catch-up loop.
        if (bsp_lvgl_lock(100)) {
            (void)ff_advance_sentence(app);
            if (app->page == GAL_PAGE_GAME && app->vm.view.kind == NINE_VM_TEXT)
                gal_ui_show_full_text(&app->ui);
            bsp_lvgl_unlock();
        }
        app->idle_ms = 0;
    } else if (app->auto_mode && app->page == GAL_PAGE_GAME && !app->opening) {
        bool typing = false;
        if (bsp_lvgl_lock(100)) {
            typing = gal_ui_typing(&app->ui);
            bsp_lvgl_unlock();
        }
        if (typing) {
            app->auto_ms = 0;
        } else {
            app->auto_ms += elapsed_ms;
            if (app->auto_ms >= NINE_APP_AUTO_MS) {
                app->auto_ms = 0;
                if (bsp_lvgl_lock(100)) {
                    advance_reading(app, false);
                    bsp_lvgl_unlock();
                }
            }
        }
        app->idle_ms = 0;
    } else {
        app->auto_ms = 0;
        }

    if (app->page == GAL_PAGE_MUSIC && app->media_ok && nine_media_is_playing())
        app->idle_ms = 0;

    if (app->notice_ms > 0) {
        app->notice_ms = app->notice_ms > elapsed_ms ? app->notice_ms - elapsed_ms : 0;
        if (app->notice_ms == 0 && bsp_lvgl_lock(50)) {
            gal_ui_set_notice(&app->ui, NULL);
            bsp_lvgl_unlock();
        }
    }

    if (app->progress_dirty && !app->preview) {
        app->progress_ms += elapsed_ms;
        if (app->progress_ms >= NINE_APP_PROGRESS_MS) {
            app->progress_ms = 0;
            if (bsp_lvgl_lock(100)) {
                save_auto(app);
                save_perm(app);
                bsp_lvgl_unlock();
            }
        }
    }

    app->battery_ms += elapsed_ms;
    if (app->battery_ms < 10000) return;
    app->battery_ms = 0;
    const int soc = bsp_battery_soc();
    if (soc == app->battery_percent) return;
    app->battery_percent = soc;
    if (!bsp_lvgl_lock(50)) return;
    gal_ui_set_battery(&app->ui, soc);
    bsp_lvgl_unlock();
}

uint32_t nine_app_idle_ms(const nine_app_t *app)
{
    return app ? app->idle_ms : 0;
}

void nine_app_flush(nine_app_t *app)
{
    if (!app || app->fatal) return;
    if (!bsp_lvgl_lock(200)) return;
    if (!app->preview) {
        save_auto(app);
        save_perm(app);
    }
    (void)nine_cfg_save(&app->store, &app->cfg);
    bsp_lvgl_unlock();
}

void nine_app_suspend(nine_app_t *app)
{
    if (!app) return;
    if (app->media_ok) nine_media_stop();
    nine_app_flush(app);
}

void nine_app_show_sleeping(nine_app_t *app)
{
    if (!app || app->fatal) return;
    if (bsp_lvgl_lock(100)) {
        gal_ui_set_notice(&app->ui, "已休眠");
        bsp_lvgl_unlock();
    }
}

// ---------------------------------------------------------------- init
static void show_boot_error(nine_app_t *app, const char *text)
{
    app->fatal = true;
    copy_str(app->error, sizeof(app->error), text);
    lv_obj_t *scr = lv_screen_active();
    if (!scr) return;
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &nine_cjk_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_width(label, 300);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(label, 10, 10);
    lv_refr_now(NULL);
}

static void load_slot_meta(nine_app_t *app)
{
    static const char *const keys[NINE_STORE_SLOTS] = { "slot0", "slot1", "slot2" };
    uint8_t blob[NINE_APP_SAVE_CAPACITY];
    for (unsigned i = 0; i < NINE_STORE_SLOTS; ++i) {
        size_t len = 0;
        app->slot_used[i] = false;
        if (!nine_store_get(&app->store, keys[i], blob, sizeof(blob), &len)) continue;
        uint16_t node, page;
        uint32_t index;
        bool preview;
        if (!nine_vm_save_peek(&app->pack, blob, len, &node, &index, &page, &preview))
            continue;
        app->slot_used[i] = true;
        app->slot_node[i] = node == NINE_NODE_NONE ? -1 : (int)node;
        app->slot_page[i] = page;
    }
}

bool nine_app_init(nine_app_t *app)
{
    if (!app) return false;
    memset(app, 0, sizeof(*app));
    app->battery_percent = -1;
    app->return_page = GAL_PAGE_TITLE;
    app->menu_origin = GAL_PAGE_TITLE;
    app->ending_return = GAL_PAGE_TITLE;
    nine_cfg_default(&app->cfg);
    nine_unlock_clear(&app->unlock);

    size_t script_size = 0, image_size = 0;
    const uint8_t *script = nine_assets_script(&script_size);
    const uint8_t *images = nine_assets_images(&image_size);

    if (!bsp_lvgl_lock(-1)) {
        ESP_LOGE(TAG, "LVGL is not ready");
        return false;
    }
    // Orientation is a hard precondition: a failed rotation must never be
    // reported as a ready landscape reader.
    if (bsp_lvgl_set_landscape(true) != ESP_OK) {
        show_boot_error(app, "横屏显示初始化失败");
        bsp_lvgl_unlock();
        return true;
    }
    if (!nine_pack_open(&app->pack, script, script_size)) {
        ESP_LOGE(TAG, "script pack rejected (size=%u)", (unsigned)script_size);
        show_boot_error(app, "资源包损坏或版本不匹配");
        bsp_lvgl_unlock();
        return true;
    }

    nine_vm_init(&app->vm, &app->pack);
    app->key_loop1 = (uint16_t)nine_pack_find_key(&app->pack, "loop1");
    app->key_true = (uint16_t)nine_pack_find_key(&app->pack, "true");
    app->key_clear_true = (uint16_t)nine_pack_find_key(&app->pack, "clear_true");
    app->key_loop_na = (uint16_t)nine_pack_find_key(&app->pack, "loop_na");
    app->key_cf_na = (uint16_t)nine_pack_find_key(&app->pack, "cf_na");

    app->nvs_ok = nine_store_open(&app->store);
    if (app->nvs_ok) {
        (void)nine_cfg_load(&app->store, &app->cfg);
        (void)nine_unlock_load(&app->store, &app->unlock);
        app->perm_size = 0;
        app->perm_valid = false;
        if (nine_store_get(&app->store, "perm", app->perm_blob, sizeof(app->perm_blob),
                           &app->perm_size) &&
            nine_vm_progress_decode(&app->vm, app->perm_blob, app->perm_size)) {
            app->perm_valid = true;
        }
        apply_perm(app);
        load_slot_meta(app);
        // The auto-save blob is 700 B today and the full capacity is 1120 B.
        // Keep it off the 4 KiB main stack: boot's first LVGL refresh needs that
        // stack for the draw/malloc chain. Failure here only disables Continue;
        // it is not a fatal pack/UI failure.
        uint8_t *auto_blob = heap_caps_malloc(NINE_APP_SAVE_CAPACITY,
                                              MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
        if (auto_blob) {
            size_t auto_len = 0;
            if (nine_store_get(&app->store, "auto", auto_blob, NINE_APP_SAVE_CAPACITY,
                               &auto_len)) {
                uint16_t node, page;
                uint32_t index;
                bool preview;
                app->auto_valid = nine_vm_save_peek(&app->pack, auto_blob, auto_len, &node,
                                                    &index, &page, &preview);
            }
            heap_caps_free(auto_blob);
        } else {
            ESP_LOGW(TAG, "auto-save buffer unavailable; Continue disabled");
        }
    }
    if (app->cfg.title_bg > 2 || !bg_unlocked(app, app->cfg.title_bg))
        app->cfg.title_bg = (uint8_t)highest_bg(app);
    app->auto_mode = app->cfg.display.auto_play != 0;

    app->image_ok = nine_image_init(lv_screen_active()) &&
                    nine_image_bind_bundle(script, script_size, images, image_size);
    if (!app->image_ok) {
        // No silent text-only fallback: an unavailable image layer is an
        // unreadable-title failure, not a degraded reader.
        nine_image_deinit();
        app->fatal = true;
        app->resource_fatal = true;
        show_boot_error(app, "图像资源初始化失败\n请重启设备后重试");
        bsp_lvgl_unlock();
        ESP_LOGE(TAG, "image layer init/bind failed; unreadable");
        return true;
    }
    if (!gal_ui_create(&app->ui, &nine_cjk_16, &nine_cjk_20)) {
        nine_image_deinit();
        show_boot_error(app, "界面初始化失败");
        bsp_lvgl_unlock();
        ESP_LOGE(TAG, "gal_ui_create failed");
        return true;
    }
    // The framework gradient must sit behind the reader's band layer.
    lv_obj_move_background(app->ui.bg);
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    ESP_LOGI(TAG, "LVGL pool: free=%u used=%u%%", (unsigned)mon.free_size,
             (unsigned)mon.used_pct);
    gal_ui_set_brand(&app->ui, TITLE_CN, TITLE_LOGO " " TITLE_SUB);
    gal_ui_set_font_size(&app->ui, app->cfg.display.font_large != 0);
    gal_ui_set_panel_transparency(&app->ui, app->cfg.display.panel_transparency);
    gal_ui_set_battery(&app->ui, app->battery_percent);
    app->media_ok = nine_media_init(script, script_size, images, image_size);
    if (!app->media_ok) ESP_LOGW(TAG, "audio unavailable");
    if (nine_image_asset_count() == 0) {
        nine_image_deinit();
        app->image_ok = false;
        app->fatal = true;
        app->resource_fatal = true;
        show_boot_error(app, "图像资源为空\n请重启设备后重试");
        bsp_lvgl_unlock();
        return true;
    }
    app->visual_valid = false;
    go_page(app, GAL_PAGE_START);
    if (!app->media_ok) show_notice(app, "音频不可用，阅读功能正常");
    // Commit the first visible frame before main turns the backlight on.
    lv_refr_now(NULL);
    bsp_lvgl_unlock();

    ESP_LOGI(TAG, "pack nodes=%u frames=%u vars=%u gallery=%u heap=%u largest=%u",
             (unsigned)app->pack.node_count, (unsigned)app->pack.frame_count,
             (unsigned)app->pack.key_count, (unsigned)app->pack.gallery_count,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
    return true;
}
