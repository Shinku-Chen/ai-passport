// 9-nine- 天色天歌天籁音: pack-driven reader application.
#include "nine_app.h"

#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "gal_text.h"
#include "nine_image.h"
#include "nine_op_log.h"

#include "esp_log.h"
#include "lvgl.h"
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#endif

#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(nine_cjk_16);
LV_FONT_DECLARE(nine_cjk_20);

static const char *TAG = "nine";
static const char *TITLE = "9-nine- 天色天歌天籁音";
static const char *SUBTITLE = "新海天 · Episode 2";
static const char *VERSION = "v0.1.0";
static const char *PROGRESS_NOTICE = "试读模式 · 不记录永久进度";

#define NINE_PROGRESS_MS 2000u
#define NINE_PERM_MS 500u
#define NINE_IDLE_FULL_MS 0u

enum {
    MODAL_ACT_NONE = 0,
    MODAL_ACT_SAVE,
};

enum {
    SACT_SPEED = 0,
    SACT_SIZE,
    SACT_AUTO,
    SACT_TRANSPARENCY,
    SACT_TITLE_BG,
    SACT_RESET,
    SACT_BACK,
};

static const char *const TITLE_MENU[GAL_TITLE_MENU_ABOUT + 1] = {
    "开始新游戏", "继续阅读", "读取存档", "设置", "鉴赏室", "关于",
};
static const char *const QUICK_MENU[7] = {
    "返回剧情", "上一句", "保存进度", "读取存档", "历史文本", "设置", "返回标题",
};
enum { GALLERY_ROW_CG = 0, GALLERY_ROW_RECALL, GALLERY_ROW_CHAPTER,
       GALLERY_ROW_MUSIC, GALLERY_ROW_OP, GALLERY_ROW_COUNT };
static const char *const GALLERY_MENU[GALLERY_ROW_COUNT] = {
    "CG 鉴赏", "剧情回想", "章节目录", "音乐鉴赏", "OP 鉴赏",
};
static const char *const CONFIRM_ACTIONS[2] = { "取消", "确认覆盖" };
static uint8_t s_save_buffer[NINE_SAVE_CAPACITY];

static const char *const SETTINGS_READER[7] = {
    "文字速度", "文字大小", "自动阅读", "对话透明度", "标题主视觉", "恢复默认", "返回",
};

static void finish_opening(nine_app_t *app, bool skipped);
static void log_op_stats(nine_app_t *app);
static void goto_page(nine_app_t *app, gal_page_t page);
static void fill_gallery(nine_app_t *app);
static void return_gallery_op(nine_app_t *app);

// ---------------------------------------------------------------- utilities
static uint32_t speed_for(const nine_app_t *app)
{
    static const uint32_t speeds[4] = { 60, 40, 18, 0 };
    return speeds[app->settings.text_speed > 3 ? 1 : app->settings.text_speed];
}

static const char *speed_label(const nine_app_t *app)
{
    static const char *const labels[4] = { "慢", "中", "快", "瞬间" };
    return labels[app->settings.text_speed > 3 ? 1 : app->settings.text_speed];
}

static void copy_utf8(char *dst, size_t cap, const char *src)
{
    if (!dst || !cap) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t length = strlen(src);
    if (length >= cap) {
        length = cap - 1;
        while (length > 0 && ((unsigned char)src[length] & 0xC0u) == 0x80u) length--;
    }
    memcpy(dst, src, length);
    dst[length] = '\0';
}

static void set_notice(nine_app_t *app, const char *text)
{
    if (!app) return;
    copy_utf8(app->canvas_line, sizeof(app->canvas_line), text);
    app->notice_ms = text && text[0] ? 2000u : 0u;
    gal_ui_set_notice(&app->ui, app->notice_ms ? app->canvas_line : NULL);
}

static const char *chapter_title(const nine_app_t *app, uint16_t chapter)
{
    nine_chapter_t c;
    if (chapter != NINE_NODE_NONE && nine_pack_chapter(&app->pack, chapter, &c))
        return nine_pack_string(&app->pack, c.title);
    return "—";
}

static uint16_t node_chapter(const nine_app_t *app, uint16_t node)
{
    if (node == NINE_NODE_NONE) return NINE_NODE_NONE;
    nine_node_t n;
    if (!nine_pack_node(&app->pack, node, &n)) return NINE_NODE_NONE;
    return (uint16_t)n.chapter;
}

static int key_value(const nine_app_t *app, int key)
{
    if (key < 0 || key >= (int)app->pack.key_count) return 0;
    return app->vm.state.flag[key] || app->vm.state.progress[key];
}

static uint32_t progress_hash(const nine_app_t *app)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < app->pack.key_count; ++i) {
        h ^= (uint32_t)app->vm.state.progress[i];
        h *= 16777619u;
    }
    for (uint32_t i = 0; i < (app->pack.key_count + 7) / 8; ++i) {
        h ^= app->vm.state.progress_set[i];
        h *= 16777619u;
    }
    return h;
}

static void update_perm(nine_app_t *app)
{
    const uint32_t hash = progress_hash(app);
    if (hash != app->perm_hash) {
        app->perm_hash = hash;
        app->perm_dirty = true;
    }
}

static void refresh_title_unlocks(nine_app_t *app)
{
    const bool loop = key_value(app, app->key_loop1) || key_value(app, app->key_loop_sr);
    const bool truth = key_value(app, app->key_true) || key_value(app, app->key_cf_sr) ||
                       key_value(app, app->key_clear_true);
    app->title_bg_unlocked[0] = 1;
    app->title_bg_unlocked[1] = (loop || truth) ? 1 : 0;
    app->title_bg_unlocked[2] = (loop && truth) ? 1 : 0;
    if (app->title_bg > 2 || !app->title_bg_unlocked[app->title_bg]) {
        app->title_bg = app->title_bg_unlocked[2] ? 2 : app->title_bg_unlocked[1] ? 1 : 0;
    }
}

static bool show_title_art(nine_app_t *app)
{
    uint32_t image = 0;
    refresh_title_unlocks(app);
    if (!nine_image_title(app->title_bg, &image)) return false;
    return nine_image_show_asset(image);
}

static void sync_mode(nine_app_t *app)
{
    const bool auto_on = app->reader.auto_mode && app->page == GAL_PAGE_GAME;
    gal_ui_set_mode(&app->ui, auto_on);
}

// ---------------------------------------------------------------- persistence
static bool write_perm(nine_app_t *app)
{
    if (!app->perm_dirty || !app->storage_ok) return false;
    uint8_t *data = s_save_buffer;
    const size_t size = nine_vm_progress_encode(&app->vm, data, NINE_SAVE_CAPACITY);
    if (!size || nine_store_write_perm(app->pack.identity, data, size) != NINE_STORE_OK) {
        ESP_LOGW(TAG, "永久进度写入失败");
        return false;
    }
    app->perm_dirty = false;
    refresh_title_unlocks(app);
    return true;
}

static bool write_progress(nine_app_t *app)
{
    if (!app->has_session || app->vm.state.preview || !app->storage_ok) return false;
    if (app->page == GAL_PAGE_ENDING || app->opening) return false;
    uint8_t *data = s_save_buffer;
    const uint16_t page = app->page == GAL_PAGE_CHOICE ? 0 : app->text_page;
    const size_t size = nine_vm_save_encode(&app->vm, page, data, NINE_SAVE_CAPACITY);
    if (!size || nine_store_write_progress(app->pack.identity, data, size, page) != NINE_STORE_OK) {
        ESP_LOGW(TAG, "自动进度写入失败");
        return false;
    }
    app->has_progress = true;
    app->progress_dirty = false;
    return true;
}

void nine_app_flush(nine_app_t *app)
{
    if (!app) return;
    (void)write_perm(app);
    (void)write_progress(app);
}

// ---------------------------------------------------------------- view helpers
static void add_history(nine_app_t *app, const char *speaker, const char *text)
{
    if (!app || !text) return;
    if (app->history_count == NINE_HISTORY_MAX) {
        memmove(app->history, app->history + 1,
                (NINE_HISTORY_MAX - 1) * sizeof(app->history[0]));
        app->history_count--;
    }
    nine_history_entry_t *entry = &app->history[app->history_count++];
    copy_utf8(entry->speaker, sizeof(entry->speaker), speaker);
    copy_utf8(entry->text, sizeof(entry->text), text);
}

static bool show_text_frame(nine_app_t *app, const nine_frame_t *frame, uint16_t node)
{
    const char *text = nine_pack_text(&app->pack, frame);
    if (!text) {
        set_notice(app, "正文读取失败");
        return false;
    }
    if (frame->visual != NINE_NONE && !nine_image_show_visual(frame->visual)) {
        set_notice(app, "图像解码失败");
        return false;
    }
    copy_utf8(app->text, sizeof(app->text), text);
    copy_utf8(app->speaker, sizeof(app->speaker),
              nine_pack_string(&app->pack, frame->speaker));

    // Landscape 320px body: 26 half-width units (13 CJK glyphs) per line at
    // 16px, 20 units (10 CJK glyphs) at 20px; exactly 3 / 2 full lines.
    int units = 26;
    int lines = GAL_TEXT_LINES_SMALL;
    if (app->settings.font_large) {
        units = 20;
        lines = GAL_TEXT_LINES_LARGE;
    }
    int pages = gal_text_pages(app->text, units, lines, app->page_offsets,
                               GAL_TEXT_MAX_PAGES + 1);
    if (pages < 1) pages = 1;
    if (pages > GAL_TEXT_MAX_PAGES) pages = GAL_TEXT_MAX_PAGES;
    if (app->text_page >= pages) app->text_page = (uint16_t)(pages - 1);
    app->page_count = pages;

    const uint32_t begin = app->page_offsets[app->text_page];
    uint32_t end = app->page_offsets[app->text_page + 1];
    if (end < begin) end = begin;
    size_t length = end - begin;
    if (length >= sizeof(app->page_text)) length = sizeof(app->page_text) - 1;
    memcpy(app->page_text, app->text + begin, length);
    app->page_text[length] = '\0';

    gal_ui_set_text(&app->ui, app->speaker, app->page_text,
                    app->reader.fast_forward ? 0 : speed_for(app), NULL, NULL);
    copy_utf8(app->chapter_label, sizeof(app->chapter_label),
              chapter_title(app, node_chapter(app, node)));
    gal_ui_set_chapter(&app->ui, app->chapter_label);
    copy_utf8(app->last_speaker, sizeof(app->last_speaker), app->speaker);
    copy_utf8(app->last_text, sizeof(app->last_text), app->text);

    if (app->text_page == 0) {
        if (!app->history_has_cursor || app->history_node != node ||
            app->history_index != (uint16_t)app->vm.state.index) {
            add_history(app, app->speaker, app->text);
            app->history_has_cursor = true;
            app->history_node = node;
            app->history_index = (uint16_t)app->vm.state.index;
        }
    }
    app->progress_dirty = true;
    app->render_error = false;
    return true;
}

static int ending_from_node(const nine_app_t *app, uint16_t node)
{
    nine_node_t n;
    if (node == NINE_NODE_NONE || !nine_pack_node(&app->pack, node, &n)) return -1;
    for (uint32_t e = 0; e < app->pack.ending_count; ++e) {
        nine_ending_t ending;
        if (!nine_pack_ending(&app->pack, e, &ending)) continue;
        for (uint32_t i = 0; i < n.effect_count; ++i) {
            nine_effect_t effect;
            if (nine_pack_effect(&app->pack, n.first_effect + i, &effect) &&
                effect.key == ending.key && effect.value == 1) {
                return (int)e;
            }
        }
    }
    return -1;
}

static void show_ending(nine_app_t *app)
{
    int index = app->ending_index;
    if (index < 0 || index >= (int)app->pack.ending_count) {
        index = key_value(app, app->key_clear_true) ? 1 : 0;
        if (index >= (int)app->pack.ending_count) index = 0;
    }
    app->ending_index = index;
    nine_ending_t ending;
    char body[512];
    if (nine_pack_ending(&app->pack, (uint32_t)index, &ending)) {
        snprintf(body, sizeof(body), "%s%s\n\n短按确定返回标题",
                 app->vm.state.preview ? "试读 · 不记录永久进度\n\n" : "",
                 nine_pack_string(&app->pack, ending.description));
        gal_ui_set_ending(&app->ui, index == 0 ? "BRANCH END" : "TRUE END",
                          nine_pack_string(&app->pack, ending.name), body);
    } else {
        gal_ui_set_ending(&app->ui, "END", "结局", "短按确定返回标题");
    }
    app->page = GAL_PAGE_ENDING;
    nine_reader_set_reading(&app->reader, false, app->clock_ms);
    gal_ui_set_mode(&app->ui, false);
    gal_ui_show_page(&app->ui, GAL_PAGE_ENDING);
    nine_media_stop();
    app->title_music = false;
    if (app->ending_index == 1) {
        // complete["true"] is applied by the final node before @end; write it
        // now so an immediate power loss cannot lose the true-ending gate.
        update_perm(app);
        (void)write_perm(app);
    } else {
        update_perm(app);
    }
}

static void fill_choice(nine_app_t *app, const nine_view_t *view)
{
    gal_option_t options[NINE_CHOICE_MAX];
    memset(options, 0, sizeof(options));
    const char *labels[NINE_CHOICE_MAX] = { 0 };
    for (uint8_t i = 0; i < view->choice_count; ++i) {
        nine_edge_t edge;
        if (!nine_pack_edge(&app->pack, true, view->choices[i], &edge)) continue;
        labels[i] = nine_pack_string(&app->pack, edge.label);
        options[i].label = labels[i];
        options[i].flag_set = GAL_UNLOCK_NONE;
        options[i].require_flag = GAL_UNLOCK_NONE;
    }
    if (app->choice_sel >= (int)view->choice_count)
        app->choice_sel = view->choice_count ? view->choice_count - 1 : 0;
    gal_ui_set_choice(&app->ui, app->last_text[0] ? app->last_text : "选择",
                      options, view->choice_count, app->choice_sel);
    app->page = GAL_PAGE_CHOICE;
    nine_reader_set_reading(&app->reader, false, app->clock_ms);
    gal_ui_set_mode(&app->ui, false);
    gal_ui_show_page(&app->ui, GAL_PAGE_CHOICE);
}

static void begin_opening(nine_app_t *app, bool gallery)
{
    app->gallery_op = gallery;
    app->gallery_op_error = false;
    app->render_error = false;
    app->opening = true;
    app->op_end_reason = "running";
    app->title_music = false;
    app->page = GAL_PAGE_CG;
    nine_reader_set_reading(&app->reader, false, app->clock_ms);
    gal_ui_set_mode(&app->ui, false);
    gal_ui_show_page(&app->ui, GAL_PAGE_CG);
    gal_ui_set_cg(&app->ui, "OP MOVIE",
                  gallery ? "OP 鉴赏" : "9-nine- 天色天歌天籁音",
                  gallery ? "短按/长按返回鉴赏室" : "短按跳过", NULL);
    nine_image_set_visible(true);
    if (!app->media_ok || !nine_media_play_op(1)) {
        app->opening = false;
        app->gallery_op_error = gallery;
        nine_media_stop();
        nine_image_clear(0);
        app->render_error = true;
        set_notice(app, "片头播放不可用");
        gal_ui_set_cg(&app->ui, "OP MOVIE", "片头播放不可用",
                      gallery ? "短按重试 · 长按返回鉴赏室" : "长按返回鉴赏室", NULL);
        nine_image_set_op_fast(false);
        app->op_end_reason = "error";
        log_op_stats(app);
        return;
    }
    nine_image_set_op_fast(true);
#ifdef ESP_PLATFORM
    bsp_lvgl_flush_stats_reset();
#endif
    ESP_LOGI(TAG, "OP start mode=%s", gallery ? "gallery" : "story");
}

static void start_opening(nine_app_t *app)
{
    begin_opening(app, false);
}

static void start_gallery_op(nine_app_t *app)
{
    begin_opening(app, true);
}

static bool render_view(nine_app_t *app)
{
    const nine_view_t *view = nine_vm_locate(&app->vm);
    if (!view) {
        set_notice(app, "剧情状态错误");
        return false;
    }
    if (view->kind == NINE_VM_ERROR) {
        app->render_error = true;
        set_notice(app, view->error ? view->error : "剧情连接错误");
        return false;
    }
    app->render_error = false;
    if (view->kind == NINE_VM_OP) {
        start_opening(app);
        return true;
    }
    if (view->kind == NINE_VM_CHOICE) {
        fill_choice(app, view);
        return true;
    }
    if (view->kind == NINE_VM_END) {
        show_ending(app);
        return true;
    }
    if (view->kind == NINE_VM_TEXT) {
        nine_frame_t frame;
        if (!nine_pack_frame(&app->pack, view->frame, &frame)) {
            set_notice(app, "正文索引错误");
            return false;
        }
        app->page = GAL_PAGE_GAME;
        nine_reader_set_reading(&app->reader, true, app->clock_ms);
        gal_ui_show_page(&app->ui, GAL_PAGE_GAME);
        const bool ok = show_text_frame(app, &frame, app->vm.state.node);
        app->render_error = !ok;
        return ok;
    }
    set_notice(app, "未知剧情状态");
    return false;
}

static void enter_current_game(nine_app_t *app, bool preview, uint16_t page)
{
    app->has_session = true;
    app->page = GAL_PAGE_GAME;
    gal_ui_release_inactive_rows(&app->ui, GAL_PAGE_GAME);
    app->text_page = page;
    app->ending_index = -1;
    nine_reader_set_reading(&app->reader, true, app->clock_ms);
    nine_reader_set_auto(&app->reader, app->settings.auto_play != 0, app->clock_ms);
    app->title_music = false;
    nine_media_stop();
    if (preview) set_notice(app, PROGRESS_NOTICE);
    if (!render_view(app)) {
        // Keep a visible error page; the reading state is validated, so this is
        // only reached for a damaged pack.
        app->error = true;
        app->error_text = "剧情渲染失败";
    }
}

// ---------------------------------------------------------------- reading
static void fast_forward_reading(nine_app_t *app)
{
    if (!app->has_session || app->page != GAL_PAGE_GAME) {
        nine_reader_stop_ff(&app->reader);
        return;
    }
    // A held fast-forward must stop on a corrupt page. It must never move the
    // VM past an unreadable text/image frame or synthesize a shorter prompt.
    if (app->render_error) {
        nine_reader_stop_ff(&app->reader);
        return;
    }
    const uint16_t node = app->vm.state.node;
    nine_vm_checkpoint(&app->vm, &app->checkpoint);
    const nine_view_t *view = nine_vm_advance(&app->vm);
    if (!view || view->kind == NINE_VM_ERROR) {
        nine_vm_restore(&app->vm, &app->checkpoint);
        app->render_error = true;
        nine_reader_stop_ff(&app->reader);
        set_notice(app, view && view->error ? view->error : "剧情读取失败");
        return;
    }
    update_perm(app);
    if (app->perm_dirty) (void)write_perm(app);
    if (view->kind == NINE_VM_END) {
        app->ending_index = ending_from_node(app, node);
        show_ending(app);
        return;
    }
    // This is the sentence transition, not a page transition: discard every
    // remaining page of the current sentence and any typing/auto wait.
    app->text_page = 0;
    if (!render_view(app)) {
        nine_vm_restore(&app->vm, &app->checkpoint);
        app->render_error = true;
        nine_reader_stop_ff(&app->reader);
        set_notice(app, "剧情渲染失败");
        return;
    }
    // Choice / ending / OP have their own key contract and stop fast-forward.
    if (app->page != GAL_PAGE_GAME || app->render_error)
        nine_reader_stop_ff(&app->reader);
}

static void advance_reading(nine_app_t *app, bool fast)
{
    if (!app->has_session || app->page != GAL_PAGE_GAME) return;
    if (fast) {
        fast_forward_reading(app);
        return;
    }
    // A corrupt text/image page must be retried, never skipped by moving the VM.
    const nine_advance_decision_t decision =
        nine_reader_advance_decide(app->render_error, app->text_page, app->page_count);
    if (decision == NINE_ADVANCE_RETRY) {
        (void)render_view(app);
        return;
    }
    if (decision == NINE_ADVANCE_PAGE) {
        const uint16_t previous = app->text_page;
        app->text_page++;
        nine_frame_t frame;
        if (nine_vm_current_frame(&app->vm, &frame) &&
            show_text_frame(app, &frame, app->vm.state.node)) return;
        app->text_page = previous;
        app->render_error = true;
        set_notice(app, "下一页读取失败");
        return;
    }
    const uint16_t node = app->vm.state.node;
    nine_vm_checkpoint(&app->vm, &app->checkpoint);
    const nine_view_t *view = nine_vm_advance(&app->vm);
    if (!view || view->kind == NINE_VM_ERROR) {
        nine_vm_restore(&app->vm, &app->checkpoint);
        app->render_error = true;
        set_notice(app, view && view->error ? view->error : "剧情读取失败");
        return;
    }
    update_perm(app);
    if (app->perm_dirty) (void)write_perm(app);
    if (view->kind == NINE_VM_END) {
        app->ending_index = ending_from_node(app, node);
        show_ending(app);
        return;
    }
    app->text_page = 0;
    if (!render_view(app)) {
        nine_vm_restore(&app->vm, &app->checkpoint);
        app->render_error = true;
        set_notice(app, "剧情渲染失败");
    }
}

static void fail_opening(nine_app_t *app, const char *reason)
{
    const bool gallery = app->gallery_op;
    app->opening = false;
    app->op_end_reason = "error";
    app->gallery_op_error = gallery;
    // Stop the audio worker but retain nine_media_failed(); finish_opening()
    // must never be allowed to turn this into a normal VM advance.
    nine_media_stop();
    // Audio-only failures must also remove any stale/partial OP picture.
    nine_image_clear(0);
    nine_image_set_op_fast(false);
    log_op_stats(app);
    app->render_error = true;
    set_notice(app, reason ? reason : "片头解码失败");
    gal_ui_set_cg(&app->ui, "OP MOVIE", reason ? reason : "片头解码失败",
                  gallery ? "短按重试 · 长按返回鉴赏室" : "资源错误 · 长按返回鉴赏室",
                  NULL);
    app->page = GAL_PAGE_CG;
    gal_ui_show_page(&app->ui, GAL_PAGE_CG);
}

static void log_op_stats(nine_app_t *app)
{
#ifdef ESP_PLATFORM
    (void)app;
    nine_media_op_stats_t st;
    if (!nine_media_get_op_stats(&st)) return;
    const uint32_t stack = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    const uint32_t heap = (uint32_t)esp_get_free_heap_size();
    const uint32_t largest = (uint32_t)heap_caps_get_largest_free_block(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    char line[384];
    nine_op_stats_format(line, sizeof(line), &st);
    ESP_LOGI(TAG, "OP[%s] end=%s %s heap=%u largest=%u stack=%u",
             app->gallery_op ? "gallery" : "story",
             app->op_end_reason ? app->op_end_reason : "unknown", line,
             (unsigned)heap, (unsigned)largest, (unsigned)stack);
    uint32_t render_count = 0, render_max_us = 0;
    uint32_t flush_count = 0, flush_gap_us = 0, flush_dur_us = 0;
    bsp_lvgl_flush_stats_get(&render_count, &render_max_us, &flush_count,
                             &flush_gap_us, &flush_dur_us);
    ESP_LOGI(TAG, "OPPIPE[%s] renders=%u render_max_us=%u flush=%u "
                  "flush_gap_us=%u flush_dur_us=%u",
             app->gallery_op ? "gallery" : "story", (unsigned)render_count,
             (unsigned)render_max_us, (unsigned)flush_count,
             (unsigned)flush_gap_us, (unsigned)flush_dur_us);
#else
    (void)app;
#endif
}

static void finish_opening(nine_app_t *app, bool skipped)
{
    if (!app->opening) return;
    if (nine_media_failed()) {
        fail_opening(app, "片头解码失败");
        return;
    }
    app->op_end_reason = skipped ? "skip" : "natural";
    log_op_stats(app);
    nine_image_set_op_fast(false);
    if (app->gallery_op) {
        // Gallery OP is independent of the story VM: return to the selected
        // Gallery row and leave every VM/save/progress field untouched.
        app->opening = false;
        app->gallery_op_error = false;
        app->render_error = false;
        nine_media_stop();
        app->gallery_op = false;
        goto_page(app, GAL_PAGE_GALLERY);
        app->gallery_sel = GALLERY_ROW_OP;
        fill_gallery(app);
        return;
    }
    app->opening = false;
    nine_media_stop();
    const uint16_t node = app->vm.state.node;
    nine_vm_checkpoint(&app->vm, &app->checkpoint);
    const nine_view_t *view = nine_vm_advance(&app->vm);
    if (!view || view->kind == NINE_VM_ERROR) {
        nine_vm_restore(&app->vm, &app->checkpoint);
        set_notice(app, "片头后剧情读取失败");
        return;
    }
    update_perm(app);
    if (app->perm_dirty) (void)write_perm(app);
    if (view->kind == NINE_VM_END) {
        app->ending_index = ending_from_node(app, node);
        show_ending(app);
        return;
    }
    app->text_page = 0;
    (void)render_view(app);
}

static void choose_visible(nine_app_t *app, int index)
{
    if (!app->has_session) return;
    const uint16_t node = app->vm.state.node;
    const int ending = ending_from_node(app, node);
    nine_vm_checkpoint(&app->vm, &app->checkpoint);
    const nine_view_t *view = nine_vm_choose(&app->vm, (uint32_t)index);
    if (!view || view->kind == NINE_VM_ERROR) {
        nine_vm_restore(&app->vm, &app->checkpoint);
        set_notice(app, view && view->error ? view->error : "选项执行失败");
        return;
    }
    update_perm(app);
    if (app->perm_dirty) (void)write_perm(app);
    if (ending == 0 && key_value(app, app->key_loop1)) {
        set_notice(app, "已解锁「一周目 · 轮回分支」");
    }
    app->choice_sel = 0;
    app->text_page = 0;
    app->ending_index = -1;
    gal_ui_release_inactive_rows(&app->ui, GAL_PAGE_GAME);
    if (!render_view(app)) {
        nine_vm_restore(&app->vm, &app->checkpoint);
        set_notice(app, "选项后渲染失败");
    }
}

static void previous_line(nine_app_t *app)
{
    if (!app->has_session) return;
    const nine_view_t *view = nine_vm_previous(&app->vm);
    if (!view || view->kind == NINE_VM_ERROR) {
        set_notice(app, "没有更早的上一句");
        return;
    }
    if (app->history_count > 0) app->history_count--;
    app->history_has_cursor = true;
    app->history_node = app->vm.state.node;
    app->history_index = (uint16_t)app->vm.state.index;
    app->text_page = 0;
    app->choice_sel = 0;
    app->ending_index = -1;
    (void)render_view(app);
}

// ---------------------------------------------------------------- save / load
static int first_occupied(const nine_app_t *app)
{
    for (int i = 0; i < NINE_SLOT_COUNT; ++i)
        if (app->slots[i].occupied) return i;
    return 0;
}

static void do_save(nine_app_t *app, int slot)
{
    if (slot < 0 || slot >= NINE_SLOT_COUNT || !app->has_session) return;
    if (app->opening || app->page == GAL_PAGE_ENDING) {
        set_notice(app, "当前页面不可存档");
        return;
    }
    nine_slot_t *value = &app->slots[slot];
    memset(value, 0, sizeof(*value));
    const uint16_t page = app->text_page;
    value->vm_size = nine_vm_save_encode(&app->vm, page, value->vm, sizeof(value->vm));
    if (!value->vm_size) {
        set_notice(app, "存档编码失败");
        return;
    }
    value->occupied = true;
    value->page = page;
    value->chapter = node_chapter(app, app->vm.state.node);
    copy_utf8(value->preview, sizeof(value->preview),
              app->page_text[0] ? app->page_text : app->last_text);
    const nine_store_status_t status =
        app->storage_ok ? nine_store_write_slot(app->pack.identity, slot, value) : NINE_STORE_IO;
    if (status != NINE_STORE_OK) {
        memset(value, 0, sizeof(*value));
        set_notice(app, "保存失败");
        return;
    }
    app->slot_ok[slot] = true;
    set_notice(app, "已保存");
}

static void do_load(nine_app_t *app, int slot)
{
    if (slot < 0 || slot >= NINE_SLOT_COUNT) return;
    const nine_slot_t *value = &app->slots[slot];
    if (!value->occupied) {
        set_notice(app, "空槽位");
        return;
    }
    if (!app->slot_ok[slot]) {
        set_notice(app, "存档校验失败,未读取");
        return;
    }
    uint16_t page = 0;
    nine_vm_checkpoint(&app->vm, &app->checkpoint);
    if (!nine_vm_save_decode(&app->vm, value->vm, value->vm_size, &page)) {
        nine_vm_restore(&app->vm, &app->checkpoint);
        set_notice(app, "存档校验失败,未读取");
        return;
    }
    app->has_session = true;
    app->text_page = page;
    app->ending_index = -1;
    app->history_count = 0;
    app->history_has_cursor = false;
    app->page = GAL_PAGE_GAME;
    gal_ui_release_inactive_rows(&app->ui, GAL_PAGE_GAME);
    nine_reader_set_reading(&app->reader, true, app->clock_ms);
    nine_reader_set_auto(&app->reader, false, app->clock_ms);
    app->title_music = false;
    nine_media_stop();
    if (!render_view(app)) {
        nine_vm_restore(&app->vm, &app->checkpoint);
        set_notice(app, "存档读取后渲染失败");
        return;
    }
    set_notice(app, "已读取");
}

static void fill_slots(nine_app_t *app, gal_list_t list, int selected)
{
    static char labels[NINE_SLOT_COUNT][16];
    static char values[NINE_SLOT_COUNT][64];
    const char *label_ptrs[NINE_SLOT_COUNT];
    const char *value_ptrs[NINE_SLOT_COUNT];
    for (int i = 0; i < NINE_SLOT_COUNT; ++i) {
        snprintf(labels[i], sizeof(labels[i]), "槽位 %02d", i + 1);
        label_ptrs[i] = labels[i];
        if (!app->slots[i].occupied) {
            snprintf(values[i], sizeof(values[i]), "空槽位");
        } else if (!app->slot_ok[i]) {
            snprintf(values[i], sizeof(values[i]), "存档损坏");
        } else {
            snprintf(values[i], sizeof(values[i]), "%s",
                     chapter_title(app, app->slots[i].chapter));
        }
        value_ptrs[i] = values[i];
    }
    gal_ui_set_rows(&app->ui, list, label_ptrs, value_ptrs, NINE_SLOT_COUNT, selected);
    for (int i = 0; i < NINE_SLOT_COUNT; ++i) {
        gal_ui_set_row_enabled(&app->ui, list, i,
                               list == GAL_LIST_SAVE ||
                               (app->slots[i].occupied && app->slot_ok[i]));
    }
    const nine_slot_t *value = &app->slots[selected];
    if (value->occupied && app->slot_ok[selected]) {
        gal_ui_set_save_preview(&app->ui, value->preview);
    } else if (value->occupied) {
        gal_ui_set_save_preview(&app->ui, "存档校验失败\n请选择其他槽位");
    } else {
        gal_ui_set_save_preview(&app->ui, "空槽位\n读取前请先保存一个进度");
    }
    gal_ui_set_footer(&app->ui, GAL_PAGE_SAVE, "共 3 个槽位");
    gal_ui_set_footer(&app->ui, GAL_PAGE_LOAD, "共 3 个槽位");
}

static void open_overwrite(nine_app_t *app)
{
    const nine_slot_t *value = &app->slots[app->save_sel];
    char body[128];
    snprintf(body, sizeof(body), "%s 的进度将被新存档替换。\n此操作无法撤销。",
             value->chapter < 32 ? chapter_title(app, value->chapter) : "原有");
    app->modal_open = true;
    app->modal_action = MODAL_ACT_SAVE;
    app->modal_sel = 0;
    gal_ui_show_modal(&app->ui, GAL_MODAL_CONFIRM, "覆盖该存档?", body,
                      CONFIRM_ACTIONS, 2, 0);
}

static void close_modal(nine_app_t *app)
{
    app->modal_open = false;
    app->modal_action = MODAL_ACT_NONE;
    gal_ui_hide_modal(&app->ui);
    if (app->page == GAL_PAGE_SAVE) fill_slots(app, GAL_LIST_SAVE, app->save_sel);
}

// ---------------------------------------------------------------- list pages
static void fill_title(nine_app_t *app)
{
    gal_ui_set_rows(&app->ui, GAL_LIST_TITLE, TITLE_MENU, NULL,
                    GAL_TITLE_MENU_ABOUT + 1, app->title_sel);
    gal_ui_set_row_enabled(&app->ui, GAL_LIST_TITLE, GAL_TITLE_MENU_CONTINUE,
                           app->has_progress);
    gal_ui_set_row_enabled(&app->ui, GAL_LIST_TITLE, GAL_TITLE_MENU_LOAD,
                           app->slots[0].occupied || app->slots[1].occupied ||
                           app->slots[2].occupied);
}

static void fill_menu(nine_app_t *app)
{
    gal_ui_set_rows(&app->ui, GAL_LIST_MENU, QUICK_MENU, NULL, 7, app->menu_sel);
}

static void fill_settings(nine_app_t *app)
{
    static char transparency[8];
    static char background[32];
    snprintf(transparency, sizeof(transparency), "%u%%",
             (unsigned)app->settings.panel_transparency);
    refresh_title_unlocks(app);
    const int unlocked = app->title_bg_unlocked[0] + app->title_bg_unlocked[1] +
                         app->title_bg_unlocked[2];
    snprintf(background, sizeof(background), "%d / %d", app->title_bg + 1, unlocked);
    const char *values[7] = {
        speed_label(app),
        app->settings.font_large ? "大" : "小",
        app->settings.auto_play ? "开" : "关",
        transparency,
        background,
        "",
        "",
    };
    gal_ui_set_rows(&app->ui, GAL_LIST_SETTINGS, SETTINGS_READER, values, 7,
                    app->settings_sel);
}

static void fill_history(nine_app_t *app)
{
    static char rows[NINE_HISTORY_MAX][NINE_HISTORY_SPEAKER + NINE_HISTORY_TEXT + 8];
    const char *labels[NINE_HISTORY_MAX];
    for (int i = 0; i < app->history_count; ++i) {
        if (app->history[i].speaker[0])
            snprintf(rows[i], sizeof(rows[i]), "%s\n%s", app->history[i].speaker,
                     app->history[i].text);
        else
            snprintf(rows[i], sizeof(rows[i]), "旁白\n%s", app->history[i].text);
        labels[i] = rows[i];
    }
    gal_ui_set_rows(&app->ui, GAL_LIST_HISTORY, labels, NULL, app->history_count,
                    app->history_sel);
    gal_ui_set_page_title(&app->ui, GAL_PAGE_HISTORY,
                          app->history_count ? "历史文本" : "历史文本 · 空");
}

static void fill_chapters(nine_app_t *app)
{
    static char labels[NINE_GALLERY_GROUPS_MAX][64];
    static char values[NINE_GALLERY_GROUPS_MAX][16];
    const char *label_ptrs[NINE_GALLERY_GROUPS_MAX];
    const char *value_ptrs[NINE_GALLERY_GROUPS_MAX];
    const int count = (int)app->pack.chapter_count;
    for (int i = 0; i < count && i < NINE_GALLERY_GROUPS_MAX; ++i) {
        nine_chapter_t c;
        if (!nine_pack_chapter(&app->pack, (uint32_t)i, &c)) continue;
        snprintf(labels[i], sizeof(labels[i]), "%s",
                 nine_pack_string(&app->pack, c.title));
        snprintf(values[i], sizeof(values[i]), "试读");
        label_ptrs[i] = labels[i];
        value_ptrs[i] = values[i];
    }
    gal_ui_set_rows(&app->ui, GAL_LIST_CHAPTER, label_ptrs, value_ptrs,
                    count > NINE_GALLERY_GROUPS_MAX ? NINE_GALLERY_GROUPS_MAX : count,
                    app->chapter_sel);
    nine_chapter_t c;
    if (nine_pack_chapter(&app->pack, (uint32_t)app->chapter_sel, &c)) {
        snprintf(app->canvas_line, sizeof(app->canvas_line), "%s",
                 nine_pack_string(&app->pack, c.title));
        gal_ui_set_chapter_info(&app->ui, nine_pack_string(&app->pack, c.preview));
    }
    gal_ui_set_action_hint(&app->ui, "单 OK 进入章节 · 试读不记录永久进度");
}

static void fill_recalls(nine_app_t *app)
{
    static char labels[NINE_GALLERY_GROUPS_MAX][32];
    static char values[NINE_GALLERY_GROUPS_MAX][48];
    const char *label_ptrs[NINE_GALLERY_GROUPS_MAX];
    const char *value_ptrs[NINE_GALLERY_GROUPS_MAX];
    for (int i = 0; i < app->gallery_groups; ++i) {
        const uint16_t first = app->group_first[i];
        nine_gallery_record_t g;
        if (!nine_pack_gallery(&app->pack, first, &g)) continue;
        snprintf(labels[i], sizeof(labels[i]), "%s",
                 nine_pack_string(&app->pack, g.name_str));
        if (app->recall_node[i] == NINE_NODE_NONE) {
            snprintf(values[i], sizeof(values[i]), "无剧情定位");
        } else {
            snprintf(values[i], sizeof(values[i]), "%s",
                     chapter_title(app, app->recall_chapter[i]));
        }
        label_ptrs[i] = labels[i];
        value_ptrs[i] = values[i];
    }
    if (app->recall_sel >= app->gallery_groups)
        app->recall_sel = app->gallery_groups ? app->gallery_groups - 1 : 0;
    gal_ui_set_rows(&app->ui, GAL_LIST_RECALL, label_ptrs, value_ptrs,
                    app->gallery_groups, app->recall_sel);
    for (int i = 0; i < app->gallery_groups; ++i)
        gal_ui_set_row_enabled(&app->ui, GAL_LIST_RECALL, i,
                               app->recall_node[i] != NINE_NODE_NONE);
    gal_ui_set_footer(&app->ui, GAL_PAGE_RECALL, "回想按试读模式播放");
}

static void fill_tracks(nine_app_t *app)
{
    static char labels[4][32];
    static char values[4][16];
    const char *label_ptrs[4];
    const char *value_ptrs[4];
    const uint32_t count = app->media_ok ? nine_media_track_count() : 0;
    for (uint32_t i = 0; i < count && i < 4; ++i) {
        nine_media_track_t info;
        if (!nine_media_track_info(i, &info)) continue;
        snprintf(labels[i], sizeof(labels[i]), "%s",
                 strcmp(info.name, "opening") == 0 ? "片头曲" : "标题 BGM");
        snprintf(values[i], sizeof(values[i]), "%u:%02u",
                 (unsigned)(info.duration_ms / 60000), (unsigned)(info.duration_ms / 1000 % 60));
        label_ptrs[i] = labels[i];
        value_ptrs[i] = values[i];
    }
    if (app->music_sel >= (int)count) app->music_sel = count ? (int)count - 1 : 0;
    gal_ui_set_rows(&app->ui, GAL_LIST_MUSIC, label_ptrs, value_ptrs, (int)count,
                    app->music_sel);
    gal_ui_set_footer(&app->ui, GAL_PAGE_MUSIC,
                      count ? "短按确定播放 / 停止 · 长按确定返回" : "音频不可用");
}

static void fill_gallery(nine_app_t *app)
{
    static char values[GALLERY_ROW_COUNT][24];
    const char *value_ptrs[GALLERY_ROW_COUNT];
    int located = 0;
    for (int i = 0; i < app->gallery_groups; ++i)
        if (app->recall_node[i] != NINE_NODE_NONE) located++;
    snprintf(values[GALLERY_ROW_CG], sizeof(values[0]), "%d 组", app->gallery_groups);
    snprintf(values[GALLERY_ROW_RECALL], sizeof(values[1]), "%d 段", located);
    snprintf(values[GALLERY_ROW_CHAPTER], sizeof(values[2]), "%u 章",
             (unsigned)app->pack.chapter_count);
    snprintf(values[GALLERY_ROW_MUSIC], sizeof(values[3]), "%u 曲",
             app->media_ok ? (unsigned)nine_media_track_count() : 0u);
    snprintf(values[GALLERY_ROW_OP], sizeof(values[4]), "%u 帧",
             (unsigned)nine_image_op_count());
    for (int i = 0; i < GALLERY_ROW_COUNT; ++i) value_ptrs[i] = values[i];
    gal_ui_set_rows(&app->ui, GAL_LIST_GALLERY, GALLERY_MENU, value_ptrs,
                    GALLERY_ROW_COUNT, app->gallery_sel);
}

static void load_tiles(nine_app_t *app)
{
    const int first_group = app->gallery_page * NINE_TILES;
    int remaining = app->gallery_groups - first_group;
    if (remaining < 0) remaining = 0;
    if (remaining > NINE_TILES) remaining = NINE_TILES;
    gal_cg_t items[NINE_TILES];
    memset(items, 0, sizeof(items));
    for (int i = 0; i < NINE_TILES; ++i) {
        if (i >= remaining) {
            memset(&app->tile_dsc[i], 0, sizeof(app->tile_dsc[i]));
            continue;
        }
        const int group = first_group + i;
        const uint16_t first = app->group_first[group];
        nine_gallery_record_t g;
        if (!nine_pack_gallery(&app->pack, first, &g)) continue;
        const char *name = nine_pack_string(&app->pack, g.name_str);
        memset(app->tile_px[i], 0, sizeof(app->tile_px[i]));
        if (nine_image_show_asset(g.image))
            (void)nine_image_copy_scaled(app->tile_px[i], NINE_TILE_W, NINE_TILE_H);
        lv_image_dsc_t *dsc = &app->tile_dsc[i];
        memset(dsc, 0, sizeof(*dsc));
        dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
        dsc->header.cf = LV_COLOR_FORMAT_RGB565;
        dsc->header.w = NINE_TILE_W;
        dsc->header.h = NINE_TILE_H;
        dsc->header.stride = NINE_TILE_W * 2;
        dsc->data_size = NINE_TILE_BYTES;
        dsc->data = (const uint8_t *)app->tile_px[i];
        snprintf(app->tile_label[i], sizeof(app->tile_label[i]), "%s", name ? name : "CG");
        snprintf(app->tile_value[i], sizeof(app->tile_value[i]), "%u 张",
                 (unsigned)app->group_count[group]);
        items[i].label = app->tile_label[i];
        items[i].caption = app->tile_value[i];
        items[i].unlocked = true;
        items[i].image = dsc;
    }
    if (app->cg_sel >= remaining) app->cg_sel = remaining ? remaining - 1 : 0;
    gal_ui_set_gallery(&app->ui, items, remaining, app->cg_sel);
    const int pages = (app->gallery_groups + NINE_TILES - 1) / NINE_TILES;
    snprintf(app->canvas_line, sizeof(app->canvas_line), "%d / %d 页 · 共 %d 组",
             app->gallery_page + 1, pages ? pages : 1, app->gallery_groups);
    gal_ui_set_footer(&app->ui, GAL_PAGE_CG_LIST, app->canvas_line);
}

static void show_cg_error(nine_app_t *app, const char *name, const char *detail)
{
    // set_notice() is intentionally hidden outside GAME. Keep failures visible
    // on the CG page itself through the existing caption labels.
    app->page = GAL_PAGE_CG;
    gal_ui_show_page(&app->ui, GAL_PAGE_CG);
    gal_ui_set_cg(&app->ui, "CG ERROR", name ? name : "CG 读取失败",
                  detail ? detail : "资源不可用 · 长按返回", NULL);
    set_notice(app, name ? name : "CG 读取失败");
    app->gallery_sel = 0;
}

static void show_cg(nine_app_t *app)
{
    if (app->cg_group < 0 || app->cg_group >= app->gallery_groups) {
        show_cg_error(app, "CG 索引错误", "请返回鉴赏室");
        return;
    }
    gal_ui_release_inactive_rows(&app->ui, GAL_PAGE_CG);
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    ESP_LOGI(TAG, "CG lv_mem free=%u largest=%u used=%u%% frag=%u%%",
             (unsigned)mon.free_size, (unsigned)mon.free_biggest_size,
             (unsigned)mon.used_pct, (unsigned)mon.frag_pct);
    // LVGL's scaled-image path allocates a temporary transform strip. Keep a
    // visible failure page when the pool cannot satisfy it instead of entering
    // the vendor assert loop.
    if (mon.free_biggest_size < 4096) {
        show_cg_error(app, "内存不足,无法显示 CG", "长按返回鉴赏室");
        return;
    }
    const uint16_t first = app->group_first[app->cg_group];
    const uint16_t count = app->group_count[app->cg_group];
    if (!count) {
        show_cg_error(app, "该 CG 组没有图片", "长按返回鉴赏室");
        return;
    }
    if (app->cg_variant < 0) app->cg_variant = 0;
    app->cg_variant %= count;
    nine_gallery_record_t g;
    if (!nine_pack_gallery(&app->pack, first + app->cg_variant, &g)) {
        show_cg_error(app, "CG 索引读取失败", "长按返回鉴赏室");
        return;
    }
    ESP_LOGI(TAG, "CG show group=%d variant=%d/%u image=%u",
             app->cg_group, app->cg_variant, (unsigned)count, (unsigned)g.image);
    const bool image_ok = nine_image_show_asset(g.image);
    if (!image_ok) {
        show_cg_error(app, "CG 图片解码失败", "长按返回鉴赏室");
        return;
    }
    snprintf(app->canvas_line, sizeof(app->canvas_line), "%s · %d / %u",
             nine_pack_string(&app->pack, g.name_str), app->cg_variant + 1,
             (unsigned)count);
    gal_ui_set_cg(&app->ui, app->canvas_line, "CG 鉴赏", "上下切换差分 · 短按确定返回", NULL);
    app->page = GAL_PAGE_CG;
    gal_ui_show_page(&app->ui, GAL_PAGE_CG);
    app->gallery_sel = 0;
}

static void fill_about(nine_app_t *app)
{
    static char text[1600];
    if (app->error || !app->pack.data) {
        snprintf(text, sizeof(text),
                 "资源包错误\n\n%s\n\n请重新烧录带 assets 分区的完整固件。",
                 app->error_text ? app->error_text : "未知错误");
        gal_ui_set_about(&app->ui, TITLE, "AI PASSPORT · 9-NINE READER", text);
        return;
    }
    size_t used = 0;
    const bool loop = key_value(app, app->key_loop1) || key_value(app, app->key_loop_sr);
    const bool truth = key_value(app, app->key_true) || key_value(app, app->key_cf_sr) ||
                       key_value(app, app->key_clear_true);
    const uint8_t *raw_digest = nine_pack_source_digest(&app->pack);
    char digest_text[17] = { 0 };
    if (raw_digest)
        snprintf(digest_text, sizeof(digest_text), "%02x%02x%02x%02x%02x%02x%02x%02x",
                 raw_digest[0], raw_digest[1], raw_digest[2], raw_digest[3],
                 raw_digest[4], raw_digest[5], raw_digest[6], raw_digest[7]);
    used += (size_t)snprintf(text + used, sizeof(text) - used,
        "%s %s\n"
        "离线横屏阅读移植 · 固定源 xuanyide01/9-nine-rw5\n"
        "源提交 %s\n源摘要 %s…\n\n"
        "剧情范围：%u 节点 / %u 句正文 / %u 章 / 2 个结局元数据；\n"
        "选择 4 组 7 项，全部源分支与永久门控保留。\n\n"
        "画面：源 432×243 → 256×144 打包 → 320×180 显示；\n"
        "立绘与表情差分按源锚点合成，无计数占位。\n"
        "音频：12 kHz 单声道 IMA ADPCM（两轨）；\n"
        "片头 623 帧 @5fps，时长 124.7 秒。\n"
        "字体：16/20px 全剧情子集，0 缺字。\n\n"
        "操作：短 OK/上/下 下一句；长 OK 菜单；长下 自动；\n"
        "按住上 快进；二级页长 OK 返回。\n\n"
        "通关记录：一周目·轮回分支 %s；新海天·真结局 %s。\n"
        "原作《9-nine- 天色天歌天籁音》© Palette / 9-nine- Project；\n"
        "本固件仅含离线阅读所需的已加工文本与美术，仅供个人使用。\n"
        "存储记录：%s\n",
        TITLE, VERSION, "6a8907a5ae576f4377abe4b74f15bd9af13efe07",
        digest_text, (unsigned)app->pack.node_count, (unsigned)app->pack.frame_count - 1,
        (unsigned)app->pack.chapter_count, loop ? "已达成" : "未达成",
        truth ? "已达成" : "未达成", app->storage_note);
    if (used >= sizeof(text)) used = sizeof(text) - 1;
    text[used] = 0;
    gal_ui_set_about(&app->ui, TITLE, "AI PASSPORT · 9-NINE READER", text);
}

// ---------------------------------------------------------------- navigation
static void goto_page(nine_app_t *app, gal_page_t page)
{
    if (app->page == GAL_PAGE_MUSIC && page != GAL_PAGE_MUSIC) nine_media_stop();
    if (page != GAL_PAGE_CG && app->opening) {
        app->opening = false;
        nine_media_stop();
    }
    app->page = page;
    app->idle_ms = 0;
    gal_ui_release_inactive_rows(&app->ui, page);
    if (page != GAL_PAGE_GAME) {
        nine_reader_set_reading(&app->reader, false, app->clock_ms);
        gal_ui_set_mode(&app->ui, false);
    }
    gal_ui_show_page(&app->ui, page);
    if (page != GAL_PAGE_TITLE) app->title_music = false;
}

static void show_title(nine_app_t *app)
{
    nine_app_flush(app);
    app->opening = false;
    app->gallery_op = false;
    app->gallery_op_error = false;
    nine_media_stop();
    app->title_music = true;
    goto_page(app, GAL_PAGE_TITLE);
    app->title_sel = 0;
    fill_title(app);
    (void)show_title_art(app);
    if (app->media_ok) (void)nine_media_play(0);
}

static void start_new_game(nine_app_t *app)
{
    app->gallery_op = false;
    app->gallery_op_error = false;
    app->history_count = 0;
    app->history_has_cursor = false;
    app->text_page = 0;
    app->ending_index = -1;
    app->choice_sel = 0;
    app->vm.state.preview = false;
    if (!nine_vm_start(&app->vm, NINE_NODE_NONE, false)) {
        set_notice(app, "剧情起点缺失");
        return;
    }
    app->has_session = true;
    app->has_progress = false;
    app->progress_dirty = true;
    enter_current_game(app, false, 0);
}

static void continue_game(nine_app_t *app)
{
    if (!app->has_progress) {
        set_notice(app, "没有可继续的进度");
        return;
    }
    app->has_session = true;
    app->title_music = false;
    nine_media_stop();
    enter_current_game(app, false, app->text_page);
}

static void open_menu(nine_app_t *app)
{
    app->return_page = GAL_PAGE_GAME;
    app->menu_sel = 0;
    goto_page(app, GAL_PAGE_MENU);
    fill_menu(app);
}

static void open_settings(nine_app_t *app, gal_page_t from)
{
    app->return_page = from;
    app->settings_sel = 0;
    goto_page(app, GAL_PAGE_SETTINGS);
    fill_settings(app);
}

static void open_history(nine_app_t *app)
{
    app->return_page = GAL_PAGE_MENU;
    app->history_sel = app->history_count ? app->history_count - 1 : 0;
    goto_page(app, GAL_PAGE_HISTORY);
    fill_history(app);
}

static void open_save(nine_app_t *app)
{
    app->save_sel = 0;
    app->return_page = GAL_PAGE_MENU;
    goto_page(app, GAL_PAGE_SAVE);
    fill_slots(app, GAL_LIST_SAVE, app->save_sel);
}

static void open_load(nine_app_t *app, gal_page_t from)
{
    app->load_sel = first_occupied(app);
    app->return_page = from;
    goto_page(app, GAL_PAGE_LOAD);
    fill_slots(app, GAL_LIST_LOAD, app->load_sel);
}

static void stop_gallery_autoplay(nine_app_t *app)
{
    // Gallery/CG/recall/chapter browsing is silent. Stop only the automatic
    // title track once; explicit music selection and OP audio are separate
    // intents and must not be muted by sharing GAL_PAGE_CG.
    if (app->media_ok && (app->title_music || nine_media_current_track() == 0))
        nine_media_stop();
    app->title_music = false;
}

static void open_gallery(nine_app_t *app)
{
    stop_gallery_autoplay(app);
    app->gallery_op = false;
    app->gallery_op_error = false;
    app->gallery_sel = GALLERY_ROW_CG;
    goto_page(app, GAL_PAGE_GALLERY);
    fill_gallery(app);
}

static void return_gallery_op(nine_app_t *app)
{
    app->opening = false;
    app->gallery_op = false;
    app->gallery_op_error = false;
    app->render_error = false;
    nine_media_stop();
    goto_page(app, GAL_PAGE_GALLERY);
    app->gallery_sel = GALLERY_ROW_OP;
    fill_gallery(app);
}

static void open_chapters(nine_app_t *app)
{
    app->chapter_sel = 0;
    app->return_page = GAL_PAGE_GALLERY;
    goto_page(app, GAL_PAGE_CHAPTER);
    fill_chapters(app);
}

static void show_chapter_transition(nine_app_t *app)
{
    nine_chapter_t c;
    if (!nine_pack_chapter(&app->pack, (uint32_t)app->chapter_sel, &c)) return;
    app->transition_chapter = app->chapter_sel;
    app->page = GAL_PAGE_TRANSITION;
    nine_reader_set_reading(&app->reader, false, app->clock_ms);
    snprintf(app->canvas_line, sizeof(app->canvas_line), "CHAPTER %02d",
             app->chapter_sel + 1);
    gal_ui_set_transition(&app->ui, app->canvas_line,
                          nine_pack_string(&app->pack, c.title),
                          nine_pack_string(&app->pack, c.preview));
    gal_ui_show_page(&app->ui, GAL_PAGE_TRANSITION);
}

static void start_chapter_preview(nine_app_t *app)
{
    nine_chapter_t c;
    if (!nine_pack_chapter(&app->pack, (uint32_t)app->transition_chapter, &c)) return;
    app->history_count = 0;
    app->history_has_cursor = false;
    app->text_page = 0;
    app->ending_index = -1;
    app->choice_sel = 0;
    if (!nine_vm_seek(&app->vm, c.node, 0, true, true)) {
        set_notice(app, "章节起点缺失");
        return;
    }
    app->has_session = true;
    enter_current_game(app, true, 0);
}

// ---------------------------------------------------------------- key handling
static void handle_reader_action(nine_app_t *app, nine_action_t action)
{
    switch (action) {
    case NINE_ACT_ADVANCE:
        advance_reading(app, false);
        break;
    case NINE_ACT_ADVANCE_FF:
        advance_reading(app, true);
        break;
    case NINE_ACT_MENU:
        open_menu(app);
        break;
    case NINE_ACT_AUTO_TOGGLE:
        app->settings.auto_play = app->reader.auto_mode ? 1 : 0;
        (void)gal_settings_store(&app->settings);
        sync_mode(app);
        break;
    case NINE_ACT_START_FF:
        // Threshold reached: enter the next sentence immediately. This also
        // skips the rest of the current sentence's pages, typing and auto wait.
        advance_reading(app, true);
        break;
    case NINE_ACT_STOP_FF:
        sync_mode(app);
        break;
    default:
        break;
    }
}

static void handle_modal_key(nine_app_t *app, const nine_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        close_modal(app);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        app->modal_sel = key->btn == BSP_BTN_UP ? 0 : 1;
        gal_ui_select_row(&app->ui, GAL_LIST_MODAL, app->modal_sel);
        return;
    }
    if (key->ev != BSP_BTN_CLICK || key->btn != BSP_BTN_OK) return;
    const int action = app->modal_action;
    const int selected = app->modal_sel;
    close_modal(app);
    if (action == MODAL_ACT_SAVE && selected == 1) {
        do_save(app, app->save_sel);
        if (app->page == GAL_PAGE_SAVE) fill_slots(app, GAL_LIST_SAVE, app->save_sel);
    }
}

static void handle_error_key(nine_app_t *app, const nine_key_t *key)
{
    if (app->page == GAL_PAGE_ABOUT) {
        if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
            goto_page(app, GAL_PAGE_START);
        }
        return;
    }
    if (key->ev == BSP_BTN_CLICK) {
        fill_about(app);
        goto_page(app, GAL_PAGE_ABOUT);
    }
}

static void handle_start_key(nine_app_t *app, const nine_key_t *key)
{
    if (key->ev == BSP_BTN_CLICK) {
        goto_page(app, GAL_PAGE_TITLE);
        app->title_sel = 0;
        fill_title(app);
        (void)show_title_art(app);
        if (app->media_ok) {
            app->title_music = true;
            (void)nine_media_play(0);
        }
    }
}

static void handle_title_key(nine_app_t *app, const nine_key_t *key)
{
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        const int count = GAL_TITLE_MENU_ABOUT + 1;
        app->title_sel += key->btn == BSP_BTN_UP ? -1 : 1;
        if (app->title_sel < 0) app->title_sel = 0;
        if (app->title_sel >= count) app->title_sel = count - 1;
        gal_ui_select_row(&app->ui, GAL_LIST_TITLE, app->title_sel);
        return;
    }
    if (key->ev != BSP_BTN_CLICK || key->btn != BSP_BTN_OK) return;
    switch (app->title_sel) {
    case GAL_TITLE_MENU_NEW: start_new_game(app); break;
    case GAL_TITLE_MENU_CONTINUE: continue_game(app); break;
    case GAL_TITLE_MENU_LOAD: open_load(app, GAL_PAGE_TITLE); break;
    case GAL_TITLE_MENU_SETTINGS: open_settings(app, GAL_PAGE_TITLE); break;
    case GAL_TITLE_MENU_GALLERY: open_gallery(app); break;
    default:
        fill_about(app);
        goto_page(app, GAL_PAGE_ABOUT);
        break;
    }
}

static void handle_game_key(nine_app_t *app, const nine_key_t *key)
{
    nine_button_t button;
    switch (key->btn) {
    case BSP_BTN_UP: button = NINE_BTN_UP; break;
    case BSP_BTN_DOWN: button = NINE_BTN_DOWN; break;
    case BSP_BTN_OK: button = NINE_BTN_OK; break;
    default: return;
    }
    nine_event_t event;
    switch (key->ev) {
    case BSP_BTN_CLICK: event = NINE_EV_CLICK; break;
    case BSP_BTN_LONG: event = NINE_EV_LONG; break;
    case BSP_BTN_RELEASE: event = NINE_EV_RELEASE; break;
    default: return;
    }
    const nine_action_t action = nine_reader_key(&app->reader, button, event, app->clock_ms);
    handle_reader_action(app, action);
}

static void handle_choice_key(nine_app_t *app, const nine_key_t *key)
{
    const nine_view_t *view = nine_vm_locate(&app->vm);
    if (!view || view->kind != NINE_VM_CHOICE) return;
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        // A choice is a first-class reading state: the menu gesture opens the
        // menu directly. Returning from the menu re-renders the complete
        // choice with the current choice_sel; never detour through a GAME
        // prompt, which used to leave the UI and VM in different states.
        open_menu(app);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        if (!view->choice_count) return;
        app->choice_sel += key->btn == BSP_BTN_UP ? -1 : 1;
        if (app->choice_sel < 0) app->choice_sel = 0;
        if (app->choice_sel >= view->choice_count) app->choice_sel = view->choice_count - 1;
        gal_ui_select_row(&app->ui, GAL_LIST_CHOICE, app->choice_sel);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_OK) {
        choose_visible(app, app->choice_sel);
    }
}

static void handle_menu_key(nine_app_t *app, const nine_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        goto_page(app, GAL_PAGE_GAME);
        (void)render_view(app);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        app->menu_sel += key->btn == BSP_BTN_UP ? -1 : 1;
        if (app->menu_sel < 0) app->menu_sel = 0;
        if (app->menu_sel >= 7) app->menu_sel = 6;
        gal_ui_select_row(&app->ui, GAL_LIST_MENU, app->menu_sel);
        return;
    }
    if (key->ev != BSP_BTN_CLICK || key->btn != BSP_BTN_OK) return;
    switch (app->menu_sel) {
    case 0: goto_page(app, GAL_PAGE_GAME); (void)render_view(app); break;
    case 1: previous_line(app); break;
    case 2: open_save(app); break;
    case 3: open_load(app, GAL_PAGE_MENU); break;
    case 4: open_history(app); break;
    case 5: open_settings(app, GAL_PAGE_MENU); break;
    default: show_title(app); break;
    }
}

static void handle_save_load_key(nine_app_t *app, const nine_key_t *key)
{
    const bool saving = app->page == GAL_PAGE_SAVE;
    int *selected = saving ? &app->save_sel : &app->load_sel;
    const gal_list_t list = saving ? GAL_LIST_SAVE : GAL_LIST_LOAD;
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        goto_page(app, saving ? GAL_PAGE_MENU : app->return_page);
        if (app->page == GAL_PAGE_MENU) fill_menu(app);
        else if (app->page == GAL_PAGE_TITLE) { fill_title(app); (void)show_title_art(app); }
        return;
    }
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        *selected += key->btn == BSP_BTN_UP ? -1 : 1;
        if (*selected < 0) *selected = 0;
        if (*selected >= NINE_SLOT_COUNT) *selected = NINE_SLOT_COUNT - 1;
        fill_slots(app, list, *selected);
        return;
    }
    if (key->ev != BSP_BTN_CLICK || key->btn != BSP_BTN_OK) return;
    if (saving) {
        if (app->slots[*selected].occupied) open_overwrite(app);
        else do_save(app, *selected);
        if (app->page == GAL_PAGE_SAVE) fill_slots(app, GAL_LIST_SAVE, *selected);
    } else {
        do_load(app, *selected);
    }
}

static void handle_settings_key(nine_app_t *app, const nine_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        goto_page(app, app->return_page);
        if (app->return_page == GAL_PAGE_MENU) fill_menu(app);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        app->settings_sel += key->btn == BSP_BTN_UP ? -1 : 1;
        if (app->settings_sel < 0) app->settings_sel = 0;
        if (app->settings_sel >= 7) app->settings_sel = 6;
        gal_ui_select_row(&app->ui, GAL_LIST_SETTINGS, app->settings_sel);
        return;
    }
    if (key->ev != BSP_BTN_CLICK || key->btn != BSP_BTN_OK) return;
    switch (app->settings_sel) {
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
    case SACT_TITLE_BG: {
        refresh_title_unlocks(app);
        const int unlocked = app->title_bg_unlocked[0] + app->title_bg_unlocked[1] +
                             app->title_bg_unlocked[2];
        if (unlocked <= 1) {
            set_notice(app, "尚未解锁更多主视觉");
            return;
        }
        do {
            app->title_bg = (uint8_t)((app->title_bg + 1) % 3);
        } while (!app->title_bg_unlocked[app->title_bg]);
        if (app->storage_ok) (void)nine_store_write_title_bg(app->title_bg);
        break;
    }
    case SACT_RESET:
        gal_settings_reset(&app->settings);
        gal_ui_set_font_size(&app->ui, false);
        gal_ui_set_panel_transparency(&app->ui, app->settings.panel_transparency);
        break;
    default:
        goto_page(app, app->return_page);
        return;
    }
    (void)gal_settings_store(&app->settings);
    fill_settings(app);
}

static void handle_history_key(nine_app_t *app, const nine_key_t *key)
{
    if ((key->ev == BSP_BTN_LONG || key->ev == BSP_BTN_CLICK) && key->btn == BSP_BTN_OK) {
        goto_page(app, app->return_page);
        if (app->return_page == GAL_PAGE_MENU) fill_menu(app);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        const int count = app->history_count ? app->history_count : 1;
        app->history_sel += key->btn == BSP_BTN_UP ? -1 : 1;
        if (app->history_sel < 0) app->history_sel = 0;
        if (app->history_sel >= count) app->history_sel = count - 1;
        gal_ui_select_row(&app->ui, GAL_LIST_HISTORY, app->history_sel);
    }
}

static void handle_gallery_key(nine_app_t *app, const nine_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        show_title(app);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        app->gallery_sel += key->btn == BSP_BTN_UP ? -1 : 1;
        if (app->gallery_sel < 0) app->gallery_sel = 0;
        if (app->gallery_sel >= GALLERY_ROW_COUNT)
            app->gallery_sel = GALLERY_ROW_COUNT - 1;
        gal_ui_select_row(&app->ui, GAL_LIST_GALLERY, app->gallery_sel);
        return;
    }
    if (key->ev != BSP_BTN_CLICK || key->btn != BSP_BTN_OK) return;
    switch (app->gallery_sel) {
    case 0:
        app->gallery_page = 0;
        app->cg_sel = 0;
        goto_page(app, GAL_PAGE_CG_LIST);
        load_tiles(app);
        break;
    case 1:
        app->recall_sel = 0;
        goto_page(app, GAL_PAGE_RECALL);
        fill_recalls(app);
        break;
    case GALLERY_ROW_CHAPTER:
        open_chapters(app);
        break;
    case GALLERY_ROW_MUSIC:
        app->music_sel = 0;
        goto_page(app, GAL_PAGE_MUSIC);
        fill_tracks(app);
        // Silent gallery browsing: play only on an explicit OK selection.
        break;
    case GALLERY_ROW_OP:
        start_gallery_op(app);
        break;
    default:
        break;
    }
}

static void handle_cg_list_key(nine_app_t *app, const nine_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        goto_page(app, GAL_PAGE_GALLERY);
        fill_gallery(app);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        const int pages = (app->gallery_groups + NINE_TILES - 1) / NINE_TILES;
        if (key->btn == BSP_BTN_UP && app->cg_sel == 0 && app->gallery_page > 0) {
            app->gallery_page--;
            app->cg_sel = NINE_TILES - 1;
            load_tiles(app);
            return;
        }
        if (key->btn == BSP_BTN_DOWN && app->cg_sel == NINE_TILES - 1 &&
            app->gallery_page + 1 < pages) {
            app->gallery_page++;
            app->cg_sel = 0;
            load_tiles(app);
            return;
        }
        app->cg_sel += key->btn == BSP_BTN_UP ? -1 : 1;
        const int remaining = app->gallery_groups - app->gallery_page * NINE_TILES;
        const int max_sel = remaining > NINE_TILES ? NINE_TILES - 1 : remaining - 1;
        if (app->cg_sel < 0) app->cg_sel = 0;
        if (app->cg_sel > max_sel) app->cg_sel = max_sel;
        load_tiles(app);
        return;
    }
    if (key->ev != BSP_BTN_CLICK || key->btn != BSP_BTN_OK) return;
    const int group = app->gallery_page * NINE_TILES + app->cg_sel;
    if (group < 0 || group >= app->gallery_groups) return;
    app->cg_group = group;
    app->cg_variant = 0;
    show_cg(app);
}

static void handle_cg_key(nine_app_t *app, const nine_key_t *key)
{
    if (app->gallery_op) {
        if (app->gallery_op_error) {
            if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_OK) {
                start_gallery_op(app); // explicit retry
            } else if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
                return_gallery_op(app);
            }
        }
        // Normal gallery OP is consumed by the opening branch in nine_app_key.
        return;
    }
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        goto_page(app, GAL_PAGE_GALLERY);
        fill_gallery(app);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        if (app->cg_group < 0 || app->cg_group >= app->gallery_groups) return;
        const int count = app->group_count[app->cg_group];
        if (!count) return;
        app->cg_variant += key->btn == BSP_BTN_UP ? -1 : 1;
        if (app->cg_variant < 0) app->cg_variant = count - 1;
        if (app->cg_variant >= count) app->cg_variant = 0;
        show_cg(app);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_OK) {
        goto_page(app, GAL_PAGE_CG_LIST);
        load_tiles(app);
    }
}

static void handle_recall_key(nine_app_t *app, const nine_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        goto_page(app, GAL_PAGE_GALLERY);
        fill_gallery(app);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        app->recall_sel += key->btn == BSP_BTN_UP ? -1 : 1;
        if (app->recall_sel < 0) app->recall_sel = 0;
        if (app->recall_sel >= app->gallery_groups)
            app->recall_sel = app->gallery_groups ? app->gallery_groups - 1 : 0;
        gal_ui_select_row(&app->ui, GAL_LIST_RECALL, app->recall_sel);
        return;
    }
    if (key->ev != BSP_BTN_CLICK || key->btn != BSP_BTN_OK) return;
    if (app->recall_sel < 0 || app->recall_sel >= app->gallery_groups) return;
    if (app->recall_node[app->recall_sel] == NINE_NODE_NONE) {
        set_notice(app, "该 CG 没有可定位的剧情帧");
        return;
    }
    app->history_count = 0;
    app->history_has_cursor = false;
    app->text_page = 0;
    app->ending_index = -1;
    if (!nine_vm_seek(&app->vm, app->recall_node[app->recall_sel],
                      app->recall_index[app->recall_sel], true, true)) {
        set_notice(app, "回想起点缺失");
        return;
    }
    app->has_session = true;
    enter_current_game(app, true, 0);
    set_notice(app, "回想 · 不记录永久进度");
}

static void handle_music_key(nine_app_t *app, const nine_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        nine_media_stop();
        goto_page(app, GAL_PAGE_GALLERY);
        fill_gallery(app);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        app->music_sel += key->btn == BSP_BTN_UP ? -1 : 1;
        if (app->music_sel < 0) app->music_sel = 0;
        const int count = app->media_ok ? (int)nine_media_track_count() : 0;
        if (app->music_sel >= count) app->music_sel = count ? count - 1 : 0;
        fill_tracks(app);
        return;
    }
    if (key->ev != BSP_BTN_CLICK || key->btn != BSP_BTN_OK) return;
    if (!app->media_ok) return;
    const uint32_t current = nine_media_current_track();
    if (current == (uint32_t)app->music_sel && nine_media_is_playing()) nine_media_stop();
    else (void)nine_media_play((uint32_t)app->music_sel);
    fill_tracks(app);
}

static void handle_chapter_key(nine_app_t *app, const nine_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        goto_page(app, app->return_page);
        fill_gallery(app);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        app->chapter_sel += key->btn == BSP_BTN_UP ? -1 : 1;
        if (app->chapter_sel < 0) app->chapter_sel = 0;
        if (app->chapter_sel >= (int)app->pack.chapter_count)
            app->chapter_sel = (int)app->pack.chapter_count - 1;
        fill_chapters(app);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_OK) {
        show_chapter_transition(app);
    }
}

static void handle_transition_key(nine_app_t *app, const nine_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        goto_page(app, GAL_PAGE_CHAPTER);
        fill_chapters(app);
        return;
    }
    if (key->ev == BSP_BTN_CLICK) start_chapter_preview(app);
}

static void handle_ending_key(nine_app_t *app, const nine_key_t *key)
{
    if ((key->ev == BSP_BTN_CLICK || key->ev == BSP_BTN_LONG) && key->btn == BSP_BTN_OK) {
        show_title(app);
    }
}

static void handle_about_key(nine_app_t *app, const nine_key_t *key)
{
    if ((key->ev == BSP_BTN_CLICK || key->ev == BSP_BTN_LONG) &&
        (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        const int dir = key->btn == BSP_BTN_UP ? -1 : 1;
        gal_ui_scroll(&app->ui, GAL_PAGE_ABOUT, dir * (key->ev == BSP_BTN_LONG ? 80 : 20));
        return;
    }
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        show_title(app);
    }
}

void nine_app_key(nine_app_t *app, const nine_key_t *key)
{
    if (!app || !key) return;
    app->idle_ms = 0;
    if (!bsp_lvgl_lock(200)) {
        ESP_LOGW(TAG, "LVGL 锁超时,忽略按键");
        return;
    }
    nine_key_t normalized = *key;
    if (normalized.ev == BSP_BTN_DOUBLE) normalized.ev = BSP_BTN_CLICK;
    if (normalized.ev == BSP_BTN_PRESS) {
        bsp_lvgl_unlock();
        return;
    }
    if (app->modal_open) {
        handle_modal_key(app, &normalized);
        bsp_lvgl_unlock();
        return;
    }
    if (app->error) {
        handle_error_key(app, &normalized);
        bsp_lvgl_unlock();
        return;
    }
    if (app->opening) {
        if (nine_media_failed()) {
            fail_opening(app, "片头解码失败");
            bsp_lvgl_unlock();
            return;
        }
        if (normalized.ev == BSP_BTN_CLICK ||
            (normalized.ev == BSP_BTN_LONG && normalized.btn == BSP_BTN_OK)) {
            finish_opening(app, true);
        }
        bsp_lvgl_unlock();
        return;
    }
    if (normalized.ev == BSP_BTN_RELEASE) {
        if (app->page == GAL_PAGE_GAME) handle_game_key(app, &normalized);
        bsp_lvgl_unlock();
        return;
    }
    switch (app->page) {
    case GAL_PAGE_START: handle_start_key(app, &normalized); break;
    case GAL_PAGE_TITLE: handle_title_key(app, &normalized); break;
    case GAL_PAGE_GAME: handle_game_key(app, &normalized); break;
    case GAL_PAGE_CHOICE: handle_choice_key(app, &normalized); break;
    case GAL_PAGE_MENU: handle_menu_key(app, &normalized); break;
    case GAL_PAGE_SAVE:
    case GAL_PAGE_LOAD: handle_save_load_key(app, &normalized); break;
    case GAL_PAGE_SETTINGS: handle_settings_key(app, &normalized); break;
    case GAL_PAGE_HISTORY: handle_history_key(app, &normalized); break;
    case GAL_PAGE_GALLERY: handle_gallery_key(app, &normalized); break;
    case GAL_PAGE_CG_LIST: handle_cg_list_key(app, &normalized); break;
    case GAL_PAGE_CG: handle_cg_key(app, &normalized); break;
    case GAL_PAGE_RECALL: handle_recall_key(app, &normalized); break;
    case GAL_PAGE_MUSIC: handle_music_key(app, &normalized); break;
    case GAL_PAGE_CHAPTER: handle_chapter_key(app, &normalized); break;
    case GAL_PAGE_TRANSITION: handle_transition_key(app, &normalized); break;
    case GAL_PAGE_ENDING: handle_ending_key(app, &normalized); break;
    case GAL_PAGE_ABOUT: handle_about_key(app, &normalized); break;
    default: break;
    }
    sync_mode(app);
    bsp_lvgl_unlock();
}

// ---------------------------------------------------------------- tick
void nine_app_tick(nine_app_t *app, uint32_t elapsed_ms)
{
    if (!app) return;
    if (!bsp_lvgl_lock(100)) {
        app->idle_ms += elapsed_ms;
        return;
    }
    app->clock_ms += elapsed_ms;
    app->idle_ms += elapsed_ms;

    if (app->opening) {
        nine_media_tick();
        app->idle_ms = 0;
        if (nine_media_failed()) {
            fail_opening(app, "片头解码失败");
        } else if (!nine_media_is_playing() && app->clock_ms > 1000) {
            finish_opening(app, false);
        }
    } else if (app->page == GAL_PAGE_GAME && !app->render_error) {
        nine_reader_set_typing(&app->reader, gal_ui_typing(&app->ui), app->clock_ms);
        const nine_action_t action = nine_reader_tick(&app->reader, app->clock_ms);
        if (action != NINE_ACT_NONE) handle_reader_action(app, action);
        if (app->reader.auto_mode || app->reader.fast_forward) app->idle_ms = 0;
    }

    if (app->title_music && app->media_ok && app->page == GAL_PAGE_TITLE) {
        if (!nine_media_is_playing()) (void)nine_media_play(0);
    }
    if (app->media_ok && nine_media_is_playing()) app->idle_ms = 0;

    if (app->notice_ms > 0) {
        app->notice_ms = app->notice_ms > elapsed_ms ? app->notice_ms - elapsed_ms : 0;
        if (!app->notice_ms) gal_ui_set_notice(&app->ui, NULL);
    }

    if (app->progress_dirty) {
        app->progress_accum_ms += elapsed_ms;
        if (app->progress_accum_ms >= NINE_PROGRESS_MS) {
            app->progress_accum_ms = 0;
            (void)write_progress(app);
        }
    }
    if (app->perm_dirty) {
        app->perm_accum_ms += elapsed_ms;
        if (app->perm_accum_ms >= NINE_PERM_MS) {
            app->perm_accum_ms = 0;
            (void)write_perm(app);
        }
    }

    app->battery_accum_ms += elapsed_ms;
    if (app->battery_accum_ms >= 10000) {
        app->battery_accum_ms = 0;
        const int soc = bsp_battery_soc();
        if (soc != app->battery_percent) {
            app->battery_percent = soc;
            gal_ui_set_battery(&app->ui, soc);
        }
    }
    sync_mode(app);
    bsp_lvgl_unlock();
}

uint32_t nine_app_idle_ms(const nine_app_t *app)
{
    return app ? app->idle_ms : 0;
}

void nine_app_show_sleeping(nine_app_t *app)
{
    if (!app) return;
    if (bsp_lvgl_lock(100)) {
        gal_ui_set_notice(&app->ui, "已休眠");
        bsp_lvgl_unlock();
    }
}

// ---------------------------------------------------------------- init
static bool validate_pack_images(nine_app_t *app)
{
    const uint32_t image_count = nine_image_asset_count();
    if (!image_count) return false;
    for (uint32_t v = 0; v < app->pack.visual_count; ++v) {
        nine_visual_record_t visual;
        if (!nine_pack_visual(&app->pack, v, &visual)) return false;
        for (uint32_t i = 0; i < visual.layer_count; ++i) {
            nine_layer_record_t layer;
            if (!nine_pack_layer(&app->pack, visual.first_layer + i, &layer))
                return false;
            if (layer.image >= image_count) return false;
        }
    }
    for (uint32_t i = 0; i < app->pack.gallery_count; ++i) {
        nine_gallery_record_t g;
        if (!nine_pack_gallery(&app->pack, i, &g) || g.image >= image_count)
            return false;
    }
    return true;
}

static void build_gallery_index(nine_app_t *app)
{
    app->gallery_groups = 0;
    for (uint32_t i = 0; i < app->pack.gallery_count; ++i) {
        nine_gallery_record_t g;
        if (!nine_pack_gallery(&app->pack, i, &g)) continue;
        if ((int)g.group >= NINE_GALLERY_GROUPS_MAX) continue;
        if ((int)g.group >= app->gallery_groups) {
            app->gallery_groups = (int)g.group + 1;
            app->group_first[g.group] = (uint16_t)i;
            app->group_count[g.group] = 0;
        }
        app->group_count[g.group]++;
    }
    uint16_t cover[NINE_GALLERY_GROUPS_MAX] = { 0 };
    for (int i = 0; i < app->gallery_groups; ++i) {
        nine_gallery_record_t g;
        app->recall_node[i] = NINE_NODE_NONE;
        app->recall_index[i] = 0;
        app->recall_chapter[i] = NINE_NODE_NONE;
        if (nine_pack_gallery(&app->pack, app->group_first[i], &g))
            cover[i] = (uint16_t)g.image;
    }
    // Locate the first story frame that uses each group cover. Only the 22
    // gallery covers are tracked, so no image->frame table is kept in RAM.
    for (uint32_t n = 0; n < app->pack.node_count; ++n) {
        nine_node_t node;
        if (!nine_pack_node(&app->pack, (uint16_t)n, &node)) continue;
        for (uint32_t f = 0; f < node.frame_count; ++f) {
            nine_frame_t frame;
            if (!nine_pack_frame(&app->pack, node.first_frame + f, &frame)) continue;
            if (frame.kind != NINE_FRAME_TEXT || frame.visual == NINE_NONE) continue;
            nine_visual_record_t visual;
            if (!nine_pack_visual(&app->pack, frame.visual, &visual)) continue;
            for (uint32_t l = 0; l < visual.layer_count; ++l) {
                nine_layer_record_t layer;
                if (!nine_pack_layer(&app->pack, visual.first_layer + l, &layer)) continue;
                for (int g = 0; g < app->gallery_groups; ++g) {
                    if (app->recall_node[g] == NINE_NODE_NONE &&
                        cover[g] == layer.image) {
                        app->recall_node[g] = (uint16_t)n;
                        app->recall_index[g] = f;
                        app->recall_chapter[g] = node_chapter(app, (uint16_t)n);
                    }
                }
            }
        }
    }
}

static bool create_ui(nine_app_t *app)
{
    if (!bsp_lvgl_lock(-1)) {
        ESP_LOGE(TAG, "LVGL 未就绪");
        return false;
    }
    bool ok = gal_ui_create(&app->ui, &nine_cjk_16, &nine_cjk_20);
    if (ok) {
        gal_ui_set_brand(&app->ui, TITLE, SUBTITLE);
        gal_ui_set_battery(&app->ui, app->battery_percent);
        gal_ui_set_font_size(&app->ui, app->settings.font_large != 0);
        gal_ui_set_panel_transparency(&app->ui, app->settings.panel_transparency);
        gal_ui_set_title_image(&app->ui, NULL);
        if (!nine_image_init(lv_screen_active())) {
            ESP_LOGE(TAG, "图像画布初始化失败");
            app->error = true;
            app->error_text = "图像画布初始化失败";
        }
        if (!app->error) {
            goto_page(app, GAL_PAGE_START);
            (void)show_title_art(app);
        } else {
            gal_ui_set_brand(&app->ui, TITLE, "资源错误");
            fill_about(app);
            goto_page(app, GAL_PAGE_ABOUT);
            nine_image_clear(0);
        }
    }
    bsp_lvgl_unlock();
    return ok;
}

bool nine_app_init(nine_app_t *app)
{
    if (!app) return false;
    memset(app, 0, sizeof(*app));
    app->battery_percent = -1;
    app->battery_accum_ms = 0;
    app->page = GAL_PAGE_START;
    app->return_page = GAL_PAGE_TITLE;
    app->ending_index = -1;
    app->key_loop1 = -1;
    app->key_loop_sr = -1;
    app->key_cf_sr = -1;
    app->key_clear_true = -1;
    app->key_true = -1;
    app->transition_chapter = 0;
    app->title_bg = 0;

    gal_settings_default(&app->settings);
    if (gal_settings_init()) (void)gal_settings_load(&app->settings);
    app->storage_ok = nine_store_init();

    if (!nine_assets_map(&app->assets)) {
        app->error = true;
        app->error_text = app->assets.error ? app->assets.error : "资源分区缺失";
        ESP_LOGE(TAG, "%s", app->error_text);
        return create_ui(app);
    }
    if (!nine_pack_open(&app->pack, app->assets.script, app->assets.script_size)) {
        app->error = true;
        app->error_text = "剧情包校验失败";
        return create_ui(app);
    }
    if (!nine_pack_validate_text_blocks(&app->pack)) {
        app->error = true;
        app->error_text = "正文块校验失败";
        return create_ui(app);
    }
    if (!nine_vm_init(&app->vm, &app->pack)) {
        app->error = true;
        app->error_text = "阅读器内存不足";
        return create_ui(app);
    }
    if (!nine_image_bind_bundle(app->assets.script, app->assets.script_size,
                                app->assets.images, app->assets.images_size)) {
        app->error = true;
        app->error_text = "图像包校验失败";
        return create_ui(app);
    }
    if (!validate_pack_images(app)) {
        app->error = true;
        app->error_text = "图像引用越界";
        return create_ui(app);
    }
    app->media_ok = nine_media_init(app->assets.script, app->assets.script_size,
                                    app->assets.images, app->assets.images_size);

    app->key_loop1 = nine_pack_find_key(&app->pack, "loop1");
    app->key_loop_sr = nine_pack_find_key(&app->pack, "loop_sr");
    app->key_cf_sr = nine_pack_find_key(&app->pack, "cf_sr");
    app->key_clear_true = nine_pack_find_key(&app->pack, "clear_true");
    app->key_true = nine_pack_find_key(&app->pack, "true");

    if (app->storage_ok) {
        uint8_t *buffer = s_save_buffer;
        size_t size = 0;
        nine_store_status_t status = nine_store_load_perm(
            app->pack.identity, buffer, NINE_SAVE_CAPACITY, &size);
        if (status == NINE_STORE_OK) {
            (void)nine_vm_progress_decode(&app->vm, buffer, size);
        } else if (status != NINE_STORE_MISSING) {
            snprintf(app->storage_note, sizeof(app->storage_note),
                     "永久进度记录损坏,已忽略；");
        }
        app->perm_hash = progress_hash(app);
        status = nine_store_load_progress(app->pack.identity, buffer, NINE_SAVE_CAPACITY,
                                          &size, &app->text_page);
        if (status == NINE_STORE_OK &&
            nine_vm_save_peek(&app->pack, buffer, size, NULL, NULL, &app->text_page, NULL) &&
            nine_vm_save_decode(&app->vm, buffer, size, &app->text_page)) {
            app->has_progress = !app->vm.state.preview;
            if (app->vm.state.preview) {
                // A preview record must never become the Continue point.
                app->has_progress = false;
            }
        } else if (status != NINE_STORE_MISSING) {
            snprintf(app->storage_note + strlen(app->storage_note),
                     sizeof(app->storage_note) - strlen(app->storage_note),
                     "自动进度记录损坏,已忽略；");
        }
        status = nine_store_load_slots(app->pack.identity, app->slots);
        if (status != NINE_STORE_OK && status != NINE_STORE_MISSING) {
            snprintf(app->storage_note + strlen(app->storage_note),
                     sizeof(app->storage_note) - strlen(app->storage_note),
                     "部分存档损坏或不兼容；");
        }
        for (int i = 0; i < NINE_SLOT_COUNT; ++i) {
            app->slot_ok[i] = app->slots[i].occupied &&
                              nine_vm_save_peek(&app->pack, app->slots[i].vm,
                                                app->slots[i].vm_size, NULL, NULL,
                                                NULL, NULL);
        }
        uint8_t bg = 0;
        if (nine_store_load_title_bg(&bg) == NINE_STORE_OK) app->title_bg = bg;
    } else {
        snprintf(app->storage_note, sizeof(app->storage_note),
                 "存储不可用,本次运行不保存；");
    }
    build_gallery_index(app);
    refresh_title_unlocks(app);

    app->battery_percent = bsp_battery_soc();
    if (!create_ui(app)) return false;
    app->ready = !app->error;
    if (!app->media_ok) ESP_LOGW(TAG, "音频不可用,阅读与图像仍可用");
    ESP_LOGI(TAG, "pack %u 字节, %u 节点, %u 变量, 媒体 %s",
             (unsigned)app->assets.container_size, (unsigned)app->pack.node_count,
             (unsigned)app->pack.key_count, app->media_ok ? "可用" : "不可用");
    return true;
}
