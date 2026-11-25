// main/cafe/cafe_app.c —— 横屏阅读器状态机。
#include "cafe_app.h"
#include "cafe_audio.h"

#include "cafe_compat.h"
#include <cafe_pack.h>
#include "gal_text.h"

#include "esp_log.h"
#if defined(ESP_PLATFORM)
#include "esp_timer.h"
#else
#include <time.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif
#endif

#include <stdio.h>
#include <string.h>

static const char *TAG = "cafe_app";

#define CAFE_LONG_MS 500u
#define CAFE_RECALL_ROWS 6
#define CAFE_AUTO_MS 900u
#define CAFE_AUTO_TRANS_MS 1200u
#define CAFE_NOTICE_MS 2000u
#define CAFE_PROGRESS_SAVE_MS 2000u

enum {
    MODAL_NONE = 0,
    MODAL_OVERWRITE,
    MODAL_RESOURCE,
};

enum {
    FAULT_NONE = 0,
    FAULT_READING,
    FAULT_TITLE,
    FAULT_CG,
    FAULT_OP,
    FAULT_AUDIO,
};

static void show_frame(cafe_app_t *app, bool force_art);
static void show_title(cafe_app_t *app, const char *reason);
static void audio_for_page(cafe_app_t *app, cafe_page_t page);
static void op_finish(cafe_app_t *app, bool skipped);
static void op_tick(cafe_app_t *app);
static void refresh_gallery(cafe_app_t *app);

static uint32_t ff_now_us(void)
{
#if defined(ESP_PLATFORM)
    return (uint32_t)esp_timer_get_time();
#elif defined(_WIN32)
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (uint32_t)((uint64_t)now.QuadPart * 1000000u / (uint64_t)freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u);
#endif
}

static void ff_finish(cafe_app_t *app, const char *reason)
{
    if (!app) return;
    if (app->fast_forward && app->ff_steps) {
        ESP_LOGI(TAG,
                 "ff summary steps=%u wall_ms=%u max_step_us=%u scenes=%u verifies=%u reason=%s",
                 (unsigned)app->ff_steps, (unsigned)(app->now_ms - app->ff_start_ms),
                 (unsigned)app->ff_max_us,
                 (unsigned)(cafe_image_render_scene_count() - app->ff_scene_start),
                 (unsigned)(cafe_image_verify_count() - app->ff_verify_start),
                 reason ? reason : "stop");
    }
    app->fast_forward = false;
    app->ff_steps = 0;
    app->ff_total_us = 0;
    app->ff_max_us = 0;
}

static void invalidate_art_cache(cafe_app_t *app)
{
    if (!app) return;
    app->last_art_hash = 0;
    app->last_art_valid = false;
}
static void open_cg_view(cafe_app_t *app);
static void fault_retry(cafe_app_t *app);
static void fault_return_title(cafe_app_t *app);

static const char *TITLE_TEXT = "星光咖啡馆与死神之蝶";
static const char *SUBTITLE_TEXT = "Cafe Stella and the Reaper's Butterflies";

static const char *ROUTE_LABEL(uint8_t id)
{
    const char *label = "剧情";
    uint8_t rid = 0, adult = 0;
    if (cafe_route_get(id, &label, &rid, &adult)) return label;
    return "剧情";
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    snprintf(dst, cap, "%s", src ? src : "");
}

static bool art_page(cafe_page_t page)
{
    return page == CAFE_PAGE_TITLE || page == CAFE_PAGE_GAME ||
           page == CAFE_PAGE_CHOICE || page == CAFE_PAGE_CG;
}

static void set_page(cafe_app_t *app, cafe_page_t page)
{
    if (app->page != page) {
        ESP_LOGI(TAG, "page %u->%u vm=n%u.%u", (unsigned)app->page, (unsigned)page,
                 (unsigned)app->vm.node, (unsigned)app->vm.index);
    }
    app->page = page;
    cafe_ui_show_page(app->ui, page);
    cafe_ui_art_visible(app->ui, art_page(page));
    audio_for_page(app, page);
}

static void app_notice(cafe_app_t *app, const char *text)
{
    cafe_ui_set_notice(app->ui, text);
    app->notice_ms = app->now_ms + CAFE_NOTICE_MS;
    if (app->page == CAFE_PAGE_SAVE || app->page == CAFE_PAGE_LOAD ||
        app->page == CAFE_PAGE_SETTINGS || app->page == CAFE_PAGE_CHAPTER) {
        cafe_ui_set_footer(app->ui, app->page, text);
    }
}

static void touch(cafe_app_t *app)
{
    app->last_activity_ms = app->now_ms;
}

static void audio_scene(cafe_app_t *app, uint8_t scene)
{
    if (!app || app->audio_scene == scene) return;
    ESP_LOGI(TAG, "audio scene %u->%u page=%u", (unsigned)app->audio_scene,
             (unsigned)scene, (unsigned)app->page);
    app->audio_scene = scene;
    if (scene == 1) {
        (void)cafe_audio_play_title(true);
    } else if (scene == 2) {
        (void)cafe_audio_play_op();
    } else if (scene != 3) {
        cafe_audio_stop();
    }
}

static void audio_for_page(cafe_app_t *app, cafe_page_t page)
{
    if (page == CAFE_PAGE_MUSIC) return;
    uint8_t scene = 0;
    switch (page) {
    case CAFE_PAGE_TITLE:
    case CAFE_PAGE_MENU:
    case CAFE_PAGE_SAVE:
    case CAFE_PAGE_LOAD:
    case CAFE_PAGE_SETTINGS:
    case CAFE_PAGE_HISTORY:
    case CAFE_PAGE_GALLERY:
    case CAFE_PAGE_CG_LIST:
    case CAFE_PAGE_CG:
    case CAFE_PAGE_RECALL:
    case CAFE_PAGE_CHAPTER:
    case CAFE_PAGE_ABOUT:
        scene = 1;
        break;
    default:
        scene = 0;
        break;
    }
    audio_scene(app, scene);
}

static void checkpoint_progress(cafe_app_t *app)
{
    app->progress_checkpoint = app->unlock.progress;
}

static void app_fault(cafe_app_t *app, uint8_t kind, const char *message)
{
    app->fault_page = app->page;
    app->fault_audio_scene = app->audio_scene;
    app->fault_op_pending = app->op_pending;
    app->fault_op_gallery = app->op_gallery;
    app->fault_op_frame = app->op_frame;
    app->fault_op_elapsed_ms = app->op_elapsed_ms;
    app->fault_op_auto_resume = app->op_auto_resume;
    app->resource_fault = true;
    invalidate_art_cache(app);
    app->fault_kind = kind;
    app->auto_mode = false;
    ff_finish(app, "fault");
    app->progress_dirty = false;   // 失败位置绝不落成成功阅读进度
    if (!app->op_gallery) {
        app->unlock.progress = app->progress_checkpoint;  // 撤销剧情失败操作新增的永久位
    }
    app->unlock_dirty = false;
    app->op_pending = false;
    app->audio_scene = 0;
    cafe_audio_stop();
    copy_str(app->fault_message, sizeof(app->fault_message), message);
    ESP_LOGE(TAG, "resource fault kind=%u page=%u vm=n%u.%u: %s", (unsigned)kind,
             (unsigned)app->fault_page, (unsigned)app->vm.node, (unsigned)app->vm.index,
             message ? message : "");
    cafe_ui_set_mode(app->ui, false);
    app->modal_open = true;
    app->modal_action = MODAL_RESOURCE;
    app->modal_sel = 0;
    static const char *const actions[] = { "重试", "返回标题" };
    cafe_ui_show_modal(app->ui, "资源读取失败", app->fault_message, actions, 2, 0);
    ESP_LOGE(TAG, "resource fault kind=%u: %s", (unsigned)kind, app->fault_message);
}

// ---------------------------------------------------------------- 永久进度 / CG
static bool progress_any_main(uint32_t progress)
{
    const uint32_t mask = (1u << CAFE_PROG_KANNA) | (1u << CAFE_PROG_NATSUME) |
                          (1u << CAFE_PROG_NOZOMI) | (1u << CAFE_PROG_MEI) |
                          (1u << CAFE_PROG_SUZUNE) | (1u << CAFE_PROG_NORMAL);
    return (progress & mask) != 0;
}

static bool cg_seen(const cafe_app_t *app, int group)
{
    if (group < 0 || group >= 121) return false;
    return (app->unlock.cg_seen[group >> 3] & (uint8_t)(1u << (group & 7))) != 0;
}

static void cg_mark(cafe_app_t *app, int group)
{
    if (group < 0 || group >= 121) return;
    const uint8_t bit = (uint8_t)(1u << (group & 7));
    if (app->unlock.cg_seen[group >> 3] & bit) return;
    app->unlock.cg_seen[group >> 3] |= bit;
    app->unlock_dirty = true;
}

static void sync_permanent(cafe_app_t *app)
{
    const uint32_t before = app->unlock.progress;
    const uint32_t merged = app->unlock.progress | app->vm.progress;
    if (merged == before) return;
    const uint32_t added = merged & ~before;
    app->unlock.progress = merged;
    for (uint8_t bit = CAFE_PROG_KANNA; bit <= CAFE_PROG_SUZUNE; ++bit) {
        if (added & (1u << bit)) app->unlock.last_cleared = bit;
    }
    app->unlock_dirty = true;
    app->art_dirty = true;
}

static bool title_bg_unlocked(const cafe_app_t *app, int mode)
{
    const uint32_t p = app->unlock.progress;
    switch (mode) {
    case 0: return true;   // auto
    case 1: return true;   // default
    case 2: return progress_any_main(p);
    case 3: return (p & (1u << CAFE_PROG_KANNA)) != 0;
    case 4: return (p & (1u << CAFE_PROG_NATSUME)) != 0;
    case 5: return (p & (1u << CAFE_PROG_NOZOMI)) != 0;
    case 6: return (p & (1u << CAFE_PROG_MEI)) != 0;
    case 7:
        return (p & (1u << CAFE_PROG_SUZUNE)) != 0 ||
               ((p & ((1u << CAFE_PROG_KANNA) | (1u << CAFE_PROG_NATSUME) |
                       (1u << CAFE_PROG_NOZOMI) | (1u << CAFE_PROG_MEI))) ==
                ((1u << CAFE_PROG_KANNA) | (1u << CAFE_PROG_NATSUME) |
                 (1u << CAFE_PROG_NOZOMI) | (1u << CAFE_PROG_MEI)));
    default: return false;
    }
}

static int resolve_title_bg(const cafe_app_t *app)
{
    int mode = app->settings.title_bg;
    if (mode == 0) {
        const uint8_t last = app->unlock.last_cleared;
        if (last == CAFE_PROG_NORMAL) mode = 2;
        else if (last >= CAFE_PROG_KANNA && last <= CAFE_PROG_MEI) mode = 3 + (last - CAFE_PROG_KANNA);
        else if (last == CAFE_PROG_SUZUNE) mode = 7;
        else if (progress_any_main(app->unlock.progress)) mode = 2;
        else mode = 1;
    }
    if (!title_bg_unlocked(app, mode)) mode = 1;
    return mode;
}

static uint16_t title_bg_image(int mode)
{
    int ui_index = 0;
    if (mode == 1) ui_index = 0;
    else if (mode == 2) ui_index = 1;
    else if (mode >= 3 && mode <= 7) ui_index = mode - 1;
    const char *name = NULL;
    uint16_t image = CAFE_IMAGE_NONE;
    if (!cafe_ui_image_get((uint16_t)ui_index, &name, &image)) return CAFE_IMAGE_NONE;
    return image;
}

static void render_title_art(cafe_app_t *app)
{
    invalidate_art_cache(app);
    const int mode = resolve_title_bg(app);
    app->title_bg_resolved = mode;
    const uint16_t image = title_bg_image(mode);
    if (image == CAFE_IMAGE_NONE || !cafe_image_render_image(app->bands, image)) {
        app->title_art_failed = true;
        app_fault(app, FAULT_TITLE, "标题图读取/CRC 失败");
    } else {
        cafe_ui_art_invalidate(app->ui);
    }
}

// ---------------------------------------------------------------- 存档
static void build_save(const cafe_app_t *app, cafe_save_t *out)
{
    memset(out, 0, sizeof(*out));
    out->occupied = true;
    out->node = app->vm.node;
    out->index = app->vm.index;
    out->progress = app->vm.progress;
    memcpy(out->flags, app->vm.flags, sizeof(out->flags));
    out->progression = app->vm.progression;
    out->last_complete = app->vm.last_complete;
    copy_str(out->chapter, sizeof(out->chapter), app->current_chapter);
}

static void save_progress_now(cafe_app_t *app)
{
    if (app->resource_fault) return;
    if (app->vm.progression == CAFE_PROGRESSION_CHAPTER) return;
    if (!app->progress_dirty) return;
    cafe_save_t s;
    build_save(app, &s);
    if (cafe_store_save_auto(&s)) {
        app->progress_dirty = false;
        app->save_accum_ms = 0;
    } else {
        app_notice(app, "进度保存失败");
    }
}

static void save_unlock_now(cafe_app_t *app)
{
    if (!app->unlock_dirty) return;
    if (cafe_store_save_unlock(&app->unlock)) app->unlock_dirty = false;
    else app_notice(app, "解锁记录保存失败");
}

static void save_settings_now(cafe_app_t *app)
{
    if (!app->settings_dirty) return;
    if (cafe_store_save_settings(&app->settings)) app->settings_dirty = false;
}

static void mark_progress_dirty(cafe_app_t *app)
{
    // 失败状态与章节/后日谈试读都不写自动进度。
    if (app->resource_fault) return;
    if (app->vm.progression == CAFE_PROGRESSION_CHAPTER) return;
    app->progress_dirty = true;
    app->save_accum_ms = 0;
}

static void restore_into_app(cafe_app_t *app, const cafe_save_t *s)
{
    cafe_vm_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    snap.node = s->node;
    snap.index = s->index;
    snap.progress = s->progress | app->unlock.progress;
    snap.progression = s->progression;
    snap.last_complete = s->last_complete;
    memcpy(snap.flags, s->flags, sizeof(snap.flags));
    if (!cafe_vm_restore(&app->vm, &snap)) {
        app_notice(app, "存档与当前版本不匹配");
        return;
    }
    app->history_count = 0;
    app->history_head = 0;
    app->fast_forward = false;
    app->auto_mode = false;
    set_page(app, CAFE_PAGE_GAME);
    app->current_frame = 0;
    app->frame_page = 0;
    app->frame_pages = 0;
    invalidate_art_cache(app);
    app->preview_pending = false;
    app->op_pending = false;
}

// ---------------------------------------------------------------- 历史
static void history_add(cafe_app_t *app)
{
    if (!app->current_text[0] && !app->current_speaker[0]) return;
    const int slot = app->history_head;
    if (app->current_speaker[0]) {
        snprintf(app->history[slot], sizeof(app->history[slot]), "%.60s\n%.180s",
                 app->current_speaker, app->current_text);
    } else {
        snprintf(app->history[slot], sizeof(app->history[slot]), "旁白\n%.180s",
                 app->current_text);
    }
    app->history_head = (app->history_head + 1) % CAFE_HISTORY_ROWS;
    if (app->history_count < CAFE_HISTORY_ROWS) app->history_count++;
}

// ---------------------------------------------------------------- 画面
static uint32_t art_hash(const cafe_frame_t *f)
{
    uint32_t h = 2166136261u;
    h = (h ^ f->bg_asset) * 16777619u;
    for (unsigned i = 0; i < f->sprite_count; ++i) {
        cafe_sprite_t sp;
        if (!cafe_frame_sprite(f, i, &sp)) break;
        // Hash semantic fields explicitly; never hash struct padding or
        // uninitialized bytes.  A zero hash is not a valid-cache sentinel.
        h = (h ^ sp.asset) * 16777619u;
        h = (h ^ ((uint16_t)sp.left)) * 16777619u;
        h = (h ^ ((uint16_t)sp.top)) * 16777619u;
        h = (h ^ sp.opacity) * 16777619u;
        h = (h ^ sp.reserved) * 16777619u;
    }
    return h;
}

static bool render_frame_art(cafe_app_t *app, const cafe_frame_t *frame, bool force)
{
    const uint32_t h = art_hash(frame);
    if (!force && app->last_art_valid && h == app->last_art_hash) return true;
    cafe_sprite_t sprites[5];
    const unsigned count = frame->sprite_count;
    if (count > 5) return false;
    for (unsigned i = 0; i < count; ++i) {
        if (!cafe_frame_sprite(frame, i, &sprites[i])) return false;
    }
    if (!cafe_image_render_scene(app->bands, frame->bg_asset, sprites, count)) {
        invalidate_art_cache(app);
        return false;
    }
    cafe_ui_art_invalidate(app->ui);
    app->last_art_hash = h;
    app->last_art_valid = true;
    return true;
}

static void set_frame_pages(cafe_app_t *app, const char *text)
{
    const int units = app->settings.font_large ? CAFE_TEXT_LARGE_UNITS : CAFE_TEXT_SMALL_UNITS;
    const int lines = app->settings.font_large ? CAFE_TEXT_LARGE_LINES : CAFE_TEXT_SMALL_LINES;
    int pages = 1;
    (void)gal_text_page_count(text, units, lines, &pages);
    if (pages < 1) pages = 1;
    app->frame_pages = (uint16_t)pages;
    if (app->frame_page >= app->frame_pages) app->frame_page = (uint16_t)(app->frame_pages - 1);
}

static void show_frame_page(cafe_app_t *app, uint32_t ms)
{
    const int units = app->settings.font_large ? CAFE_TEXT_LARGE_UNITS : CAFE_TEXT_SMALL_UNITS;
    const int lines = app->settings.font_large ? CAFE_TEXT_LARGE_LINES : CAFE_TEXT_SMALL_LINES;
    int pages = 1;
    (void)gal_text_page_count(app->current_text, units, lines, &pages);
    if (pages < 1) pages = 1;
    if (app->frame_page >= pages) app->frame_page = (uint16_t)(pages - 1);
    uint32_t begin = 0, end = (uint32_t)strlen(app->current_text);
    (void)gal_text_page_bounds(app->current_text, units, lines, app->frame_page, &begin, &end);
    if (begin > end) begin = end = 0;
    size_t len = end > begin ? end - begin : 0;
    if (len >= sizeof(app->current_text)) len = sizeof(app->current_text) - 1;
    char page_text[sizeof(app->current_text)];
    memcpy(page_text, app->current_text + begin, len);
    page_text[len] = '\0';
    cafe_ui_set_text(app->ui, app->current_speaker, page_text, ms);
    cafe_ui_set_chapter(app->ui, app->current_chapter);
    if (app->frame_page == 0) history_add(app);
}

static void show_frame(cafe_app_t *app, bool force_art)
{
    if (app->resource_fault) return;
    cafe_frame_t frame;
    if (!cafe_frame_get(app->vm.node, app->current_frame, &frame)) {
        app_fault(app, FAULT_READING, "剧情正文读取/CRC 失败");
        return;
    }
    copy_str(app->current_speaker, sizeof(app->current_speaker), frame.speaker);
    copy_str(app->current_text, sizeof(app->current_text), frame.text);
    cafe_node_t n;
    if (cafe_node_get(app->vm.node, &n)) {
        copy_str(app->current_chapter, sizeof(app->current_chapter),
                 cafe_pack_string(n.title_off));
    }
    if (!render_frame_art(app, &frame, force_art)) {
        app_fault(app, FAULT_READING, "图像读取/CRC/解码失败");
        return;
    }
    // CG 只在文字和图像都成功显示后才算看过。
    if (app->vm.progression != CAFE_PROGRESSION_CHAPTER) {
        cg_mark(app, cafe_image_gallery_group(frame.bg_asset));
        for (uint16_t i = 0;; ++i) {
            uint16_t group = 0xFFFF;
            if (!cafe_image_gallery_alias(frame.bg_asset, i, &group)) break;
            cg_mark(app, group);
        }
    }
    set_frame_pages(app, app->current_text);
    const uint32_t ms = app->fast_forward ? 0 : (app->settings.text_speed == 3 ? 0 :
                       (app->settings.text_speed == 0 ? 45 :
                        (app->settings.text_speed == 1 ? 30 : 14)));
    show_frame_page(app, ms);
    cafe_ui_set_mode(app->ui, app->auto_mode);
    checkpoint_progress(app);
    mark_progress_dirty(app);
}

static void show_choice_view(cafe_app_t *app)
{
    const char *labels[CAFE_CHOICE_MAX];
    for (int i = 0; i < app->choice_count; ++i) labels[i] = app->choice_labels[i];
    app->auto_mode = false;
    app->fast_forward = false;
    set_page(app, CAFE_PAGE_CHOICE);
    cafe_ui_set_choice(app->ui, labels, NULL, app->choice_count, app->choice_sel);
    cafe_ui_set_mode(app->ui, false);
}

static void enter_choice(cafe_app_t *app, uint16_t first, uint8_t count)
{
    if (count > CAFE_CHOICE_MAX) count = CAFE_CHOICE_MAX;
    if (app->resource_fault) return;
    app->choice_count = count;
    app->choice_sel = 0;
    for (uint8_t i = 0; i < count; ++i) {
        cafe_choice_t c;
        if (!cafe_choice_get((uint16_t)(first + i), &c)) {
            app_fault(app, FAULT_READING, "选项数据读取/CRC 失败");
            return;
        }
        copy_str(app->choice_labels[i], sizeof(app->choice_labels[i]),
                 cafe_pack_string(c.text_off));
    }
    show_choice_view(app);
    mark_progress_dirty(app);
}

static void enter_ending(cafe_app_t *app)
{
    app->auto_mode = false;
    app->fast_forward = false;
    if (app->vm.progression == CAFE_PROGRESSION_CHAPTER) {
        cafe_ui_set_ending(app->ui, "PREVIEW END", "试读结束",
                           "章节试读不会写入通关或解锁记录。");
        set_page(app, CAFE_PAGE_ENDING);
        return;
    }
    uint8_t bit = app->vm.last_complete;
    if (bit >= CAFE_PROGRESS_BITS) {
        // 读档直接落在 @end 时没有“本次新增”位，取已解锁的最高优先级位。
        for (int i = CAFE_PROGRESS_BITS - 1; i >= 0; --i) {
            if (app->vm.progress & (1u << i)) {
                bit = (uint8_t)i;
                break;
            }
        }
    }
    app->ending_bit = bit;
    cafe_ending_t ending;
    bool found = false;
    for (uint16_t i = 0; i < cafe_meta_u32(6); ++i) {
        if (!cafe_ending_get(i, &ending)) continue;
        if (ending.bit == bit) {
            found = true;
            break;
        }
    }
    if (!found) {
        app_fault(app, FAULT_READING, "结局数据读取/CRC 失败");
        return;
    } else {
        cafe_ui_set_ending(app->ui, cafe_pack_string(ending.title_off),
                           cafe_pack_string(ending.title_off),
                           cafe_pack_string(ending.body_off));
    }
    sync_permanent(app);
    save_unlock_now(app);
    set_page(app, CAFE_PAGE_ENDING);
    mark_progress_dirty(app);
    checkpoint_progress(app);
}

static void enter_location(cafe_app_t *app)
{
    uint16_t node = 0, index = 0, first = 0;
    uint8_t count = 0;
    const int loc = cafe_vm_locate(&app->vm, &node, &index, &first, &count);
    ESP_LOGI(TAG, "enter_location loc=%d node=n%u idx=%u count=%u page=%u", loc,
             (unsigned)node, (unsigned)index, (unsigned)count, (unsigned)app->page);
    sync_permanent(app);
    if (loc == CAFE_LOC_TEXT) {
        app->current_frame = index;
        app->frame_page = 0;
        set_page(app, CAFE_PAGE_GAME);
        show_frame(app, false);
    } else if (loc == CAFE_LOC_OP) {
        cafe_op_t op;
        if (!cafe_op_get(&op) || op.frames == 0) {
            app_fault(app, FAULT_OP, "OP 媒体索引/CRC 失败");
            return;
        }
        app->op_pending = true;
        app->op_gallery = false;
        app->op_auto_resume = app->auto_mode;
        app->op_frame = 0;
        app->op_elapsed_ms = 0;
        app->op_start_ms = app->now_ms;
        app->fast_forward = false;
        app->auto_mode = false;
        invalidate_art_cache(app);
        set_page(app, CAFE_PAGE_TRANSITION);
        cafe_ui_art_visible(app->ui, true);
        cafe_ui_set_transition_op(app->ui, "短按跳过");
        app->audio_scene = 2;
        (void)cafe_audio_play_op_at(0);
        const uint16_t first = cafe_op_image_id(0);
        if (first == CAFE_IMAGE_NONE || !cafe_image_render_op(app->bands, first)) {
            app_fault(app, FAULT_OP, "OP 图像读取/CRC/解码失败");
            return;
        }
        cafe_ui_art_invalidate(app->ui);
        checkpoint_progress(app);
    } else if (loc == CAFE_LOC_CHOICE) {
        enter_choice(app, first, count);
        if (!app->resource_fault) checkpoint_progress(app);
    } else if (loc == CAFE_LOC_END) {
        enter_ending(app);
    } else {
        app_fault(app, FAULT_READING, "剧情连接损坏或越界");
        ESP_LOGE(TAG, "locate error node=%u index=%u", app->vm.node, app->vm.index);
    }
}

static void op_finish(cafe_app_t *app, bool skipped)
{
    if (!app->op_pending) return;
    const bool auto_resume = app->op_auto_resume;
    app->op_pending = false;
    app->op_auto_resume = false;
    app->op_elapsed_ms = 0;
    app->fast_forward = false;
    app->audio_scene = 0;
    cafe_audio_stop();
    if (app->op_gallery) {
        // Gallery OP is an independent appreciation view: no VM/flags/
        // progression/preview/history/manual slot/NVS changes.
        app->op_gallery = false;
        app->op_auto_resume = false;
        app->gallery_sel = 4;
        set_page(app, CAFE_PAGE_GALLERY);
        refresh_gallery(app);
        return;
    }
    (void)skipped;
    cafe_vm_advance(&app->vm);
    sync_permanent(app);
    enter_location(app);
    if (auto_resume && app->page == CAFE_PAGE_GAME) {
        app->auto_mode = true;
        app->auto_accum_ms = 0;
        cafe_ui_set_mode(app->ui, true);
    }
    if (!app->resource_fault) checkpoint_progress(app);
}

static void op_tick(cafe_app_t *app)
{
    if (!app->op_pending || app->page != CAFE_PAGE_TRANSITION) return;
    cafe_op_t op;
    if (!cafe_op_get(&op) || op.duration_ms == 0 || op.frames == 0) {
        app_fault(app, FAULT_OP, "OP 时间轴读取/CRC 失败");
        return;
    }
    const uint32_t elapsed = app->now_ms - app->op_start_ms;
    app->op_elapsed_ms = elapsed;
    uint32_t frame = elapsed * (uint32_t)op.fps / 1000u;
    if (frame >= op.frames) frame = op.frames - 1;
    if (frame != app->op_frame) {
        const uint16_t id = cafe_op_image_id((uint16_t)frame);
        app->op_frame = (uint16_t)frame;   // retry must resume the same OP frame
        if (id == CAFE_IMAGE_NONE || !cafe_image_render_op(app->bands, id)) {
            app_fault(app, FAULT_OP, "OP 图像读取/CRC/解码失败");
            return;
        }
        cafe_ui_art_invalidate(app->ui);
    }
    if (elapsed >= op.duration_ms) op_finish(app, false);
}

static int advance_reading(cafe_app_t *app, bool fast);

static void fast_forward_step(cafe_app_t *app)
{
    if (!app || app->resource_fault || app->page != CAFE_PAGE_GAME) {
        if (app) app->fast_forward = false;
        return;
    }
    // Long UP skips typing and every remaining page of this sentence, then
    // advances exactly one VM sentence.  One call per input control tick.
    if (cafe_ui_typing(app->ui)) cafe_ui_show_full_text(app->ui);
    const uint32_t t0 = ff_now_us();
    advance_reading(app, true);
    const uint32_t cost = ff_now_us() - t0;
    app->ff_steps++;
    app->ff_total_us += cost;
    if (cost > app->ff_max_us) app->ff_max_us = cost;
    touch(app);
    if (app->page != CAFE_PAGE_GAME || app->resource_fault) ff_finish(app, "stop");
}

static int advance_reading(cafe_app_t *app, bool fast)
{
    if (app->resource_fault) return app->page;
    if (app->page != CAFE_PAGE_GAME) return app->page;
    // Normal short presses advance one complete page.  Fast-forward skips the
    // remaining pages of this sentence and advances the VM exactly once.
    if (!fast && app->frame_page + 1 < app->frame_pages) {
        app->frame_page++;
        show_frame_page(app, 0);
        mark_progress_dirty(app);
        return app->page;
    }
    cafe_vm_snapshot_t before;
    cafe_vm_checkpoint(&app->vm, &before);
    const int loc = cafe_vm_advance(&app->vm);
    if (loc == CAFE_LOC_ERROR) {
        // A failed decompression/CRC must not leave the story cursor on the
        // next frame.  The save/NVS format is unchanged; this is run-state only.
        (void)cafe_vm_restore(&app->vm, &before);
        app_fault(app, FAULT_READING, "剧情正文读取/CRC 失败，进度未前移");
        return app->page;
    }
    sync_permanent(app);
    if (loc == CAFE_LOC_TEXT) {
        app->current_frame = app->vm.index;
        app->frame_page = 0;
        show_frame(app, false);
        if (app->resource_fault) (void)cafe_vm_restore(&app->vm, &before);
    } else {
        enter_location(app);
        if (app->resource_fault) (void)cafe_vm_restore(&app->vm, &before);
    }
    return app->page;
}

static void choose_option(cafe_app_t *app, int index)
{
    if (index < 0 || index >= app->choice_count) return;
    cafe_vm_snapshot_t before;
    cafe_vm_checkpoint(&app->vm, &before);
    const int loc = cafe_vm_choose(&app->vm, (uint8_t)index);
    if (loc == CAFE_LOC_ERROR) {
        (void)cafe_vm_restore(&app->vm, &before);
        app_fault(app, FAULT_READING, "剧情正文读取/CRC 失败，选项未提交");
        return;
    }
    sync_permanent(app);
    if (loc == CAFE_LOC_TEXT) {
        app->current_frame = app->vm.index;
        app->frame_page = 0;
        set_page(app, CAFE_PAGE_GAME);
        show_frame(app, false);
    } else {
        enter_location(app);
    }
    if (app->resource_fault) {
        (void)cafe_vm_restore(&app->vm, &before);
        return;
    }
    mark_progress_dirty(app);
    checkpoint_progress(app);
}

// ---------------------------------------------------------------- 开局/读档
static void start_new_game(cafe_app_t *app)
{
    ESP_LOGI(TAG, "NewGame begin progress=%08x page=%u", (unsigned)app->unlock.progress,
             (unsigned)app->page);
    cafe_vm_new_game(&app->vm, app->unlock.progress);
    (void)cafe_store_clear_auto();
    app->progress_dirty = false;
    app->history_count = 0;
    app->history_head = 0;
    app->auto_mode = false;
    app->fast_forward = false;
    invalidate_art_cache(app);
    app->preview_pending = false;
    app->op_pending = false;
    app->frame_page = 0;
    enter_location(app);
    ESP_LOGI(TAG, "NewGame end page=%u vm=n%u.%u", (unsigned)app->page,
             (unsigned)app->vm.node, (unsigned)app->vm.index);
    mark_progress_dirty(app);
}

static void continue_game(cafe_app_t *app)
{
    cafe_save_t s;
    if (!cafe_store_load_auto(&s)) {
        app_notice(app, "没有可继续的进度");
        return;
    }
    restore_into_app(app, &s);
    if (app->vm.node == CAFE_NODE_ID_NONE) {
        enter_ending(app);
    } else {
        enter_location(app);
    }
}

static void load_slot(cafe_app_t *app, int slot)
{
    if (slot < 0 || slot >= CAFE_SAVE_SLOTS) return;
    cafe_save_t s;
    if (!cafe_store_load_slot(slot, &s)) {
        app_notice(app, "空槽位");
        return;
    }
    restore_into_app(app, &s);
    if (app->vm.node == CAFE_NODE_ID_NONE) enter_ending(app);
    else enter_location(app);
    app_notice(app, "已读取");
}

static void save_slot(cafe_app_t *app, int slot)
{
    if (slot < 0 || slot >= CAFE_SAVE_SLOTS) return;
    cafe_save_t s;
    build_save(app, &s);
    if (cafe_store_save_slot(slot, &s)) {
        app->slots[slot] = s;
        app_notice(app, "已保存");
    } else {
        app_notice(app, "保存失败");
    }
}

// ---------------------------------------------------------------- 标题/菜单
static void refresh_title(cafe_app_t *app)
{
    app->title_bg_resolved = resolve_title_bg(app);
    render_title_art(app);
}

static const char *speed_label(uint8_t speed)
{
    static const char *const labels[4] = { "慢", "中", "快", "瞬间" };
    return labels[speed > 3 ? 1 : speed];
}

static const char *title_bg_label(int mode)
{
    static const char *const names[8] = { "自动", "主视觉", "主视觉 2", "栞那", "夏目",
                                          "希", "爱衣", "凉音" };
    return mode >= 0 && mode < 8 ? names[mode] : "自动";
}

static void show_title(cafe_app_t *app, const char *reason)
{
    ESP_LOGI(TAG, "show_title reason=%s vm=n%u.%u page=%u", reason ? reason : "unknown",
             (unsigned)app->vm.node, (unsigned)app->vm.index, (unsigned)app->page);
    app->fast_forward = false;
    app->auto_mode = false;
    app->page = CAFE_PAGE_TITLE;
    cafe_ui_show_page(app->ui, CAFE_PAGE_TITLE);
    cafe_ui_art_visible(app->ui, !app->title_art_failed);
    cafe_ui_set_brand(app->ui, TITLE_TEXT, SUBTITLE_TEXT);
    if (!app->title_art_failed) refresh_title(app);
    static const char *labels[6] = { "开始游戏", "继续阅读", "读取进度", "设置", "鉴赏室", "关于" };
    static char values[6][48];
    values[0][0] = '\0';
    cafe_save_t auto_save;
    copy_str(values[1], sizeof(values[1]), cafe_store_load_auto(&auto_save) ? "有进度" : "无进度");
    copy_str(values[2], sizeof(values[2]), cafe_store_any_slot() ? "有存档" : "无存档");
    const char *vals[6] = { values[0], values[1], values[2], values[3], values[4], values[5] };
    cafe_ui_set_rows(app->ui, CAFE_LIST_TITLE, labels, vals, 6, app->title_sel);
    cafe_ui_set_row_enabled(app->ui, CAFE_LIST_TITLE, 1, cafe_store_load_auto(&auto_save));
    cafe_ui_set_row_enabled(app->ui, CAFE_LIST_TITLE, 2, cafe_store_any_slot());
    cafe_ui_set_footer(app->ui, CAFE_PAGE_TITLE, "");
    invalidate_art_cache(app);
}

static void open_menu(cafe_app_t *app)
{
    app->fast_forward = false;
    app->auto_mode = false;
    if (app->page == CAFE_PAGE_GAME || app->page == CAFE_PAGE_CHOICE)
        app->menu_source = app->page;
    app->return_page = CAFE_PAGE_GAME;
    app->menu_sel = 0;
    static const char *labels[6] = { "返回故事", "保存进度", "读取进度", "历史文本", "设置",
                                     "返回标题" };
    cafe_ui_set_rows(app->ui, CAFE_LIST_MENU, labels, NULL, 6, 0);
    set_page(app, CAFE_PAGE_MENU);
}

static void close_menu(cafe_app_t *app)
{
    if (app->menu_source == CAFE_PAGE_CHOICE && app->choice_count > 0) {
        show_choice_view(app);
        return;
    }
    set_page(app, CAFE_PAGE_GAME);
    show_frame_page(app, 0);
    cafe_ui_set_mode(app->ui, app->auto_mode);
}

// ---------------------------------------------------------------- 存/读档页
static void refresh_slots(cafe_app_t *app, bool save_page)
{
    static char labels[3][16];
    static char values[3][64];
    const char *label_ptrs[3];
    const char *value_ptrs[3];
    for (int i = 0; i < CAFE_SAVE_SLOTS; ++i) {
        snprintf(labels[i], sizeof(labels[i]), "槽位 %d", i + 1);
        cafe_save_t s;
        if (cafe_store_load_slot(i, &s)) {
            snprintf(values[i], sizeof(values[i]), "%s", s.chapter[0] ? s.chapter : "已保存");
        } else {
            snprintf(values[i], sizeof(values[i]), "空");
        }
        label_ptrs[i] = labels[i];
        value_ptrs[i] = values[i];
    }
    const cafe_list_t list = save_page ? CAFE_LIST_SAVE : CAFE_LIST_LOAD;
    const int sel = save_page ? app->save_sel : app->load_sel;
    cafe_ui_set_rows(app->ui, list, label_ptrs, value_ptrs, CAFE_SAVE_SLOTS, sel);
    for (int i = 0; i < CAFE_SAVE_SLOTS; ++i) {
        cafe_save_t s;
        const bool ok = save_page || cafe_store_load_slot(i, &s);
        cafe_ui_set_row_enabled(app->ui, list, i, ok);
    }
    cafe_ui_set_save_preview(app->ui, save_page ? "保存当前阅读位置与本周目选择" : "读取槽位");
    cafe_ui_set_footer(app->ui, save_page ? CAFE_PAGE_SAVE : CAFE_PAGE_LOAD,
                       "短按 OK 确认 · 长按 OK 返回");
}

static void open_slots(cafe_app_t *app, bool save_page)
{
    app->return_page = CAFE_PAGE_GAME;
    if (save_page) {
        app->save_sel = 0;
        set_page(app, CAFE_PAGE_SAVE);
    } else {
        app->load_sel = 0;
        set_page(app, CAFE_PAGE_LOAD);
    }
    refresh_slots(app, save_page);
}

// ---------------------------------------------------------------- 设置
static void apply_settings(cafe_app_t *app)
{
    cafe_ui_set_font_large(app->ui, app->settings.font_large);
    cafe_ui_set_panel_transparency(app->ui, app->settings.panel_opacity);
    cafe_audio_set_volume(app->settings.sound_on ? app->settings.volume : 0);
    app->settings_dirty = true;
}

static void refresh_settings(cafe_app_t *app)
{
    static const char *labels[8] = { "文字速度", "文字大小", "自动阅读", "对话透明度",
                                     "标题背景", "声音", "音量", "返回" };
    static char vals[8][32];
    snprintf(vals[0], sizeof(vals[0]), "%s", speed_label(app->settings.text_speed));
    snprintf(vals[1], sizeof(vals[1]), "%s", app->settings.font_large ? "20px" : "16px");
    snprintf(vals[2], sizeof(vals[2]), "%s", app->auto_mode ? "开" : "关");
    snprintf(vals[3], sizeof(vals[3]), "%u%%", (unsigned)app->settings.panel_opacity);
    snprintf(vals[4], sizeof(vals[4]), "%s", title_bg_label(app->settings.title_bg));
    snprintf(vals[5], sizeof(vals[5]), "%s", app->settings.sound_on ? "开" : "关");
    snprintf(vals[6], sizeof(vals[6]), "%u%%", (unsigned)app->settings.volume);
    vals[7][0] = '\0';
    const char *v[8] = { vals[0], vals[1], vals[2], vals[3], vals[4], vals[5], vals[6], vals[7] };
    cafe_ui_set_rows(app->ui, CAFE_LIST_SETTINGS, labels, v, 8, app->settings_sel);
    cafe_ui_set_page_title(app->ui, CAFE_PAGE_SETTINGS, "设置");
}

static void open_settings(cafe_app_t *app)
{
    app->return_page = app->page;
    app->settings_sel = 0;
    set_page(app, CAFE_PAGE_SETTINGS);
    refresh_settings(app);
}

static void cycle_title_bg(cafe_app_t *app)
{
    for (int n = 0; n < 8; ++n) {
        app->settings.title_bg = (uint8_t)((app->settings.title_bg + 1) % 8);
        if (title_bg_unlocked(app, app->settings.title_bg)) break;
    }
    app->settings_dirty = true;
}

static void settings_action(cafe_app_t *app)
{
    switch (app->settings_sel) {
    case 0:
        app->settings.text_speed = (uint8_t)((app->settings.text_speed + 1) % 4);
        app->settings_dirty = true;
        break;
    case 1:
        app->settings.font_large = !app->settings.font_large;
        apply_settings(app);
        app->frame_page = 0;
        if (app->page == CAFE_PAGE_GAME) show_frame_page(app, 0);
        break;
    case 2:
        app->auto_mode = !app->auto_mode;
        app->settings.auto_play = app->auto_mode;
        app->auto_accum_ms = 0;
        cafe_ui_set_mode(app->ui, app->auto_mode);
        app->settings_dirty = true;
        break;
    case 3:
        app->settings.panel_opacity = (uint8_t)(app->settings.panel_opacity + 5);
        if (app->settings.panel_opacity > 55) app->settings.panel_opacity = 10;
        apply_settings(app);
        break;
    case 4:
        cycle_title_bg(app);
        break;
    case 5:
        app->settings.sound_on = !app->settings.sound_on;
        apply_settings(app);
        break;
    case 6:
        app->settings.volume = (uint8_t)((app->settings.volume + 25) % 125);
        apply_settings(app);
        break;
    case 7:
        set_page(app, app->return_page);
        if (app->return_page == CAFE_PAGE_TITLE) show_title(app, "settings_return_title");
        else if (app->return_page == CAFE_PAGE_MENU) open_menu(app);
        else if (app->return_page == CAFE_PAGE_GAME) close_menu(app);
        break;
    default:
        break;
    }
    if (app->page == CAFE_PAGE_SETTINGS) refresh_settings(app);
}

// ---------------------------------------------------------------- 历史/鉴赏
static void open_history(cafe_app_t *app)
{
    static char labels[CAFE_HISTORY_ROWS][256];
    const char *ptrs[CAFE_HISTORY_ROWS];
    const int count = app->history_count;
    const int start = (app->history_head - count + CAFE_HISTORY_ROWS) % CAFE_HISTORY_ROWS;
    for (int i = 0; i < count; ++i) {
        const int idx = (start + i) % CAFE_HISTORY_ROWS;
        snprintf(labels[i], sizeof(labels[i]), "%s", app->history[idx]);
        ptrs[i] = labels[i];
    }
    if (app->history_sel >= count) app->history_sel = count > 0 ? count - 1 : 0;
    set_page(app, CAFE_PAGE_HISTORY);
    cafe_ui_set_rows(app->ui, CAFE_LIST_HISTORY, ptrs, NULL, count, app->history_sel);
    cafe_ui_set_page_title(app->ui, CAFE_PAGE_HISTORY, "历史文本");
}

static void refresh_gallery(cafe_app_t *app)
{
    static char values[5][32];
    static const char *labels[5] = { "CG 鉴赏", "场景回想", "剧情鉴赏", "音乐鉴赏", "OP 播放" };
    snprintf(values[0], sizeof(values[0]), "%d 组", 121);
    int seen = 0;
    for (int i = 0; i < 121; ++i) if (cg_seen(app, i)) seen++;
    snprintf(values[1], sizeof(values[1]), "%d 组", seen);
    snprintf(values[2], sizeof(values[2]), "%u 章", (unsigned)cafe_meta_u32(4));
    snprintf(values[3], sizeof(values[3]), "2 曲 · 4 kHz");
    snprintf(values[4], sizeof(values[4]), "287 张 · 8 kHz");
    const char *v[5] = { values[0], values[1], values[2], values[3], values[4] };
    cafe_ui_set_rows(app->ui, CAFE_LIST_GALLERY, labels, v, 5, app->gallery_sel);
    cafe_ui_set_page_title(app->ui, CAFE_PAGE_GALLERY, "鉴赏室");
}

static void open_gallery(cafe_app_t *app)
{
    app->gallery_sel = 0;
    app->return_page = CAFE_PAGE_TITLE;
    set_page(app, CAFE_PAGE_GALLERY);
    refresh_gallery(app);
}

static void open_gallery_op(cafe_app_t *app)
{
    cafe_op_t op;
    if (!cafe_op_get(&op) || op.frames == 0 || op.duration_ms == 0) {
        app_notice(app, "OP 媒体索引/CRC 失败");
        return;
    }
    app->op_gallery = true;
    app->op_pending = true;
    app->op_auto_resume = false;
    app->op_frame = 0;
    app->op_elapsed_ms = 0;
    app->op_start_ms = app->now_ms;
    app->fast_forward = false;
    app->auto_mode = false;
    invalidate_art_cache(app);
    set_page(app, CAFE_PAGE_TRANSITION);
    cafe_ui_art_visible(app->ui, true);
    cafe_ui_set_transition_op(app->ui, "短按跳过 · 长按 OK 返回鉴赏室");
    app->audio_scene = 2;
    (void)cafe_audio_play_op_at(0);
    const uint16_t first = cafe_op_image_id(0);
    if (first == CAFE_IMAGE_NONE || !cafe_image_render_op(app->bands, first)) {
        app_fault(app, FAULT_OP, "OP 图像读取/CRC/解码失败");
        return;
    }
    cafe_ui_art_invalidate(app->ui);
}

static uint16_t gallery_first_asset(int group)
{
    cafe_gallery_t g;
    if (!cafe_gallery_get((uint16_t)group, &g) || g.variant_count == 0) return 0xFFFF;
    uint16_t asset = 0xFFFF;
    if (!cafe_gallery_variant_get(g.first_variant, &asset)) return 0xFFFF;
    return asset;
}

static void refresh_cg_list(cafe_app_t *app)
{
    const int page = app->cg_sel / 6;
    const int first = page * 6;
    static char labels[6][24];
    const char *label_ptrs[6];
    uint16_t images[6];
    bool unlocked[6];
    const int count = (121 - first) < 6 ? (121 - first) : 6;
    bool missing = false;
    for (int i = 0; i < 6; ++i) {
        if (i >= count) {
            label_ptrs[i] = "";
            images[i] = CAFE_IMAGE_NONE;
            unlocked[i] = false;
            continue;
        }
        const int g = first + i;
        snprintf(labels[i], sizeof(labels[i]), "%s", cg_seen(app, g) ? "CG" : "？？？");
        label_ptrs[i] = labels[i];
        unlocked[i] = cg_seen(app, g);
        images[i] = CAFE_IMAGE_NONE;
        if (unlocked[i]) {
            cafe_asset_t a;
            cafe_image_t img;
            if (cafe_asset_get(gallery_first_asset(g), &a) && cafe_image_get(a.image_id, &img)) {
                images[i] = img.id;
            } else {
                missing = true;
            }
        }
    }
    cafe_ui_set_gallery_tiles(app->ui, label_ptrs, images, unlocked, count,
                              app->cg_sel - first);
    if (missing) {
        app_fault(app, FAULT_CG, "CG 资源缺失或记录损坏");
        return;
    }
    if (app->ui->thumb_failed) {
        app_fault(app, FAULT_CG, "CG 缩略图读取/CRC/解码失败");
        return;
    }
    static char footer[64];
    snprintf(footer, sizeof(footer), "%d / 121 组 · %d / 21 页", app->cg_sel + 1,
             (app->cg_sel / 6) + 1);
    cafe_ui_set_footer(app->ui, CAFE_PAGE_CG_LIST, footer);
}

static void open_cg_list(cafe_app_t *app)
{
    app->cg_sel = 0;
    set_page(app, CAFE_PAGE_CG_LIST);
    refresh_cg_list(app);
}

static void open_cg_view(cafe_app_t *app)
{
    cafe_gallery_t g;
    if (!cafe_gallery_get((uint16_t)app->cg_sel, &g)) {
        app_fault(app, FAULT_CG, "CG 索引读取/CRC 失败");
        return;
    }
    if (!cg_seen(app, app->cg_sel)) {
        app_notice(app, "尚未看过这张 CG");
        return;
    }
    if (app->cg_variant >= g.variant_count) app->cg_variant = 0;
    uint16_t asset = 0xFFFF;
    if (!cafe_gallery_variant_get((uint16_t)(g.first_variant + app->cg_variant), &asset)) {
        app_fault(app, FAULT_CG, "CG 差分索引读取/CRC 失败");
        return;
    }
    cafe_asset_t a;
    cafe_image_t img;
    if (!cafe_asset_get(asset, &a) || !cafe_image_get(a.image_id, &img)) {
        app_fault(app, FAULT_CG, "CG 资源缺失或记录损坏");
        return;
    }
    invalidate_art_cache(app);
    set_page(app, CAFE_PAGE_CG);
    if (!cafe_image_render_image(app->bands, img.id)) {
        app_fault(app, FAULT_CG, "CG 图像读取/CRC 失败");
        return;
    }
    static char index_text[32], name[64], caption[64];
    snprintf(index_text, sizeof(index_text), "CG %03d · %u/%u", app->cg_sel + 1,
             (unsigned)(app->cg_variant + 1), (unsigned)g.variant_count);
    snprintf(name, sizeof(name), "%s", cafe_pack_string(g.label_off));
    snprintf(caption, sizeof(caption), "上下切换差分 · 短按返回");
    cafe_ui_set_cg(app->ui, index_text, name, caption);
}

static int recall_count(const cafe_app_t *app)
{
    int n = 0;
    for (int i = 0; i < 121; ++i) if (cg_seen(app, i)) n++;
    return n;
}

// Six rows fit the 320x240 page.  recall_sel is a global index into the full
// unlocked-GC order; only the visible window is rebuilt, so group -> first_node
// stays exact beyond the first page.
static void refresh_recall(cafe_app_t *app)
{
    const int total = recall_count(app);
    if (total <= 0) {
        app->recall_sel = 0;
        static const char *empty[1] = { "尚未解锁任何 CG" };
        cafe_ui_set_rows(app->ui, CAFE_LIST_RECALL, empty, NULL, 1, 0);
        cafe_ui_set_footer(app->ui, CAFE_PAGE_RECALL,
                           "继续阅读剧情后，这里会列出已看过的 CG");
        return;
    }
    if (app->recall_sel < 0) app->recall_sel = 0;
    if (app->recall_sel >= total) app->recall_sel = total - 1;
    const int page = app->recall_sel / CAFE_RECALL_ROWS;
    const int first = page * CAFE_RECALL_ROWS;
    const int count = total - first > CAFE_RECALL_ROWS ? CAFE_RECALL_ROWS : total - first;
    static char labels[CAFE_RECALL_ROWS][48];
    const char *ptrs[CAFE_RECALL_ROWS];
    int seen = 0, row = 0;
    for (int i = 0; i < 121 && row < count; ++i) {
        if (!cg_seen(app, i)) continue;
        if (seen++ < first) continue;
        cafe_gallery_t g;
        const char *label = NULL;
        if (cafe_gallery_get((uint16_t)i, &g)) label = cafe_pack_string(g.label_off);
        if (!label || !label[0]) label = "未命名 CG";
        snprintf(labels[row], sizeof(labels[row]), "%s", label);
        ptrs[row] = labels[row];
        row++;
    }
    cafe_ui_set_rows(app->ui, CAFE_LIST_RECALL, ptrs, NULL, row,
                     app->recall_sel - first);
    static char footer[64];
    snprintf(footer, sizeof(footer), "%d / %d 组 · %d / %d 页",
             app->recall_sel + 1, total, page + 1,
             (total + CAFE_RECALL_ROWS - 1) / CAFE_RECALL_ROWS);
    cafe_ui_set_footer(app->ui, CAFE_PAGE_RECALL, footer);
}

static void open_recall(cafe_app_t *app)
{
    app->recall_sel = 0;
    set_page(app, CAFE_PAGE_RECALL);
    refresh_recall(app);
    cafe_ui_set_page_title(app->ui, CAFE_PAGE_RECALL, "场景回想");
}

static int recall_group_at(cafe_app_t *app, int row)
{
    int n = 0;
    for (int i = 0; i < 121; ++i) {
        if (!cg_seen(app, i)) continue;
        if (n == row) return i;
        n++;
    }
    return -1;
}

static void open_music(cafe_app_t *app)
{
    static const char *labels[2] = { "标题曲 Smiling-Swinging!!", "OP 曲" };
    static const char *values[2] = { "4 kHz 完整", "4 kHz 完整" };
    set_page(app, CAFE_PAGE_MUSIC);
    app->audio_scene = 3;
    cafe_ui_set_rows(app->ui, CAFE_LIST_MUSIC, labels, values, 2, app->music_sel);
    cafe_ui_set_footer(app->ui, CAFE_PAGE_MUSIC,
                       "短按播放/重播 · 长按 OK 返回鉴赏室");
    cafe_ui_set_page_title(app->ui, CAFE_PAGE_MUSIC, "音乐鉴赏");
    (void)cafe_audio_play_preview((uint8_t)app->music_sel, false);
}

static void refresh_chapter_page(cafe_app_t *app)
{
    static const char *route_labels[7];
    for (int i = 0; i < 7; ++i) route_labels[i] = ROUTE_LABEL((uint8_t)i);
    if (app->chapter_routes) {
        cafe_ui_set_rows(app->ui, CAFE_LIST_ROUTE, route_labels, NULL, 7, app->route_sel);
        cafe_ui_set_rows(app->ui, CAFE_LIST_CHAPTER, NULL, NULL, 0, 0);
        cafe_ui_set_page_title(app->ui, CAFE_PAGE_CHAPTER, "剧情鉴赏 · 路线");
        cafe_ui_set_chapter_info(app->ui, "选择路线，后日谈需通关解锁");
        cafe_ui_set_footer(app->ui, CAFE_PAGE_CHAPTER, "短按 OK 进入 · 长按 OK 返回鉴赏室");
        return;
    }
    cafe_ui_set_rows(app->ui, CAFE_LIST_ROUTE, NULL, NULL, 0, 0);
    // 路线列表仍保留在 route 列表对象中，章节列表填当前页。
    static char labels[7][48];
    const char *label_ptrs[7];
    const char *value_ptrs[7];
    int count = 0;
    if (app->route_sel == 6) {
        for (int i = 0; i < 5; ++i) {
            cafe_after_t a;
            if (!cafe_after_get((uint16_t)i, &a)) continue;
            const bool ok = (app->unlock.progress & (1u << a.requires)) != 0;
            snprintf(labels[count], sizeof(labels[count]), "%s%s",
                     ok ? "" : "未解锁 · ", cafe_pack_string(a.title_off));
            label_ptrs[count] = labels[count];
            value_ptrs[count] = ok ? "后日谈" : "锁";
            count++;
        }
    } else {
        int seen = 0;
        for (uint16_t i = 0; i < cafe_meta_u32(4); ++i) {
            cafe_chapter_t c;
            if (!cafe_chapter_get(i, &c) || c.route != app->route_sel) continue;
            if (seen++ < app->chapter_page * 7) continue;
            if (count >= 7) break;
            snprintf(labels[count], sizeof(labels[count]), "%s", cafe_pack_string(c.title_off));
            label_ptrs[count] = labels[count];
            value_ptrs[count] = cafe_pack_string(c.chapter_off);
            count++;
        }
    }
    if (app->chapter_sel >= count) app->chapter_sel = count > 0 ? count - 1 : 0;
    cafe_ui_set_rows(app->ui, CAFE_LIST_CHAPTER, label_ptrs, value_ptrs, count,
                     app->chapter_sel);
    cafe_ui_set_page_title(app->ui, CAFE_PAGE_CHAPTER,
                           app->route_sel == 6 ? "剧情鉴赏 · 后日谈" : "剧情鉴赏 · 章节");
    cafe_ui_set_chapter_info(app->ui, app->route_sel == 6
                                          ? "后日谈试读不写入通关记录"
                                          : "试读不写入通关记录");
    cafe_ui_set_footer(app->ui, CAFE_PAGE_CHAPTER, "短按 OK 开始 · 长按 OK 返回路线");
}

static void open_chapter(cafe_app_t *app)
{
    app->chapter_routes = true;
    app->route_sel = 0;
    app->chapter_page = 0;
    app->chapter_sel = 0;
    set_page(app, CAFE_PAGE_CHAPTER);
    refresh_chapter_page(app);
}

static void start_preview(cafe_app_t *app, uint16_t node)
{
    cafe_vm_init(&app->vm, app->unlock.progress);
    app->vm.node = node;
    app->vm.progression = CAFE_PROGRESSION_CHAPTER;
    app->history_count = 0;
    app->history_head = 0;
    app->current_frame = 0;
    app->frame_page = 0;
    invalidate_art_cache(app);
    app->auto_mode = false;
    app->fast_forward = false;
    app->preview_pending = true;
    cafe_node_t n;
    if (cafe_node_get(node, &n)) {
        copy_str(app->current_chapter, sizeof(app->current_chapter),
                 cafe_pack_string(n.title_off));
        cafe_ui_set_transition(app->ui, "CHAPTER", cafe_pack_string(n.title_off),
                               "短按 OK 开始试读");
    } else {
        cafe_ui_set_transition(app->ui, "CHAPTER", "章节", "短按 OK 开始试读");
    }
    set_page(app, CAFE_PAGE_TRANSITION);
}

static void open_about(cafe_app_t *app)
{
    app->return_page = CAFE_PAGE_TITLE;
    set_page(app, CAFE_PAGE_ABOUT);
    cafe_ui_set_about(app->ui,
                      "本作：星光咖啡馆与死神之蝶\n"
                      "固定源：xuanyide01/cafestella-rw5\n"
                      "提交：90ddb9e020970907b61ad4181f25c65267e6e98b\n\n"
                      "923 个剧情节点 / 53,727 句正文\n"
                      "69 组选项 / 141 个选项 / 7 结局 / 5 后日谈\n"
                      "121 组鉴赏 CG / 1,631 张差分\n\n"
                      "图像：224x126 舞台源，运行时条带放大到 320x180，\n"
                      "立绘与表情按源坐标合成。音频与 OP 视频未内置\n"
                      "（8MB Flash 预算，详见 docs）。\n\n"
                      "操作：短按 OK/UP/DOWN 下一句，长按 OK 菜单，\n"
                      "长按 UP 快进，长按 DOWN 自动阅读。\n\n"
                      "原作版权归 YUZUSOFT 所有，本固件仅供个人离线阅读。");
}

// ---------------------------------------------------------------- 按键
static void move_sel(int *sel, int count, int delta)
{
    if (count <= 0) return;
    *sel += delta;
    if (*sel < 0) *sel = 0;
    if (*sel >= count) *sel = count - 1;
}

static int chapter_route_total(cafe_app_t *app)
{
    if (app->route_sel == 6) return 5;
    int n = 0;
    for (uint16_t i = 0; i < cafe_meta_u32(4); ++i) {
        cafe_chapter_t c;
        if (cafe_chapter_get(i, &c) && c.route == app->route_sel) n++;
    }
    return n;
}

static int chapter_page_count(cafe_app_t *app)
{
    const int total = chapter_route_total(app);
    if (total <= 0) return 0;
    if (app->route_sel == 6) return total;
    if (app->chapter_page < 0) return 0;
    const int first = app->chapter_page * 7;
    if (first >= total) return 0;
    return total - first > 7 ? 7 : total - first;
}

static void move_chapter_selection(cafe_app_t *app, int delta)
{
    if (app->chapter_routes) {
        move_sel(&app->route_sel, 7, delta);
        app->chapter_page = 0;
        app->chapter_sel = 0;
        refresh_chapter_page(app);
        return;
    }
    const int count = chapter_page_count(app);
    if (count <= 0) {
        app->chapter_page = 0;
        app->chapter_sel = 0;
        refresh_chapter_page(app);
        return;
    }
    if (app->chapter_sel >= count) app->chapter_sel = count - 1;
    if (delta < 0) {
        if (app->chapter_sel > 0) app->chapter_sel--;
        else if (app->route_sel != 6 && app->chapter_page > 0) {
            app->chapter_page--;
            const int prev = chapter_page_count(app);
            app->chapter_sel = prev > 0 ? (uint8_t)(prev - 1) : 0;
        }
    } else {
        if (app->chapter_sel + 1 < count) app->chapter_sel++;
        else if (app->route_sel != 6) {
            const int total = chapter_route_total(app);
            const int next_first = (app->chapter_page + 1) * 7;
            if (next_first < total) {
                app->chapter_page++;
                app->chapter_sel = 0;
            }
        }
    }
    refresh_chapter_page(app);
}

static void move_music_selection(cafe_app_t *app, int delta)
{
    move_sel(&app->music_sel, 2, delta);
    cafe_ui_select_row(app->ui, CAFE_LIST_MUSIC, app->music_sel);
}

static void move_recall_selection(cafe_app_t *app, int delta)
{
    const int count = recall_count(app);
    if (count <= 0) return;
    move_sel(&app->recall_sel, count, delta);
    refresh_recall(app);
}

static void handle_short(cafe_app_t *app, uint8_t key)
{
    touch(app);
    const cafe_page_t page = app->page;
    if (page == CAFE_PAGE_START) {
        show_title(app, "start_short");
        return;
    }
    if (page == CAFE_PAGE_TITLE) {
        if (key == CAFE_KEY_UP) move_sel(&app->title_sel, 6, -1);
        else if (key == CAFE_KEY_DOWN) move_sel(&app->title_sel, 6, 1);
        else {
            switch (app->title_sel) {
            case 0: start_new_game(app); break;
            case 1: continue_game(app); break;
            case 2:
                if (!cafe_store_any_slot()) app_notice(app, "暂无存档");
                else { open_slots(app, false); app->return_page = CAFE_PAGE_TITLE; }
                break;
            case 3: open_settings(app); break;
            case 4: open_gallery(app); break;
            case 5: open_about(app); break;
            default: break;
            }
        }
        if (app->page == CAFE_PAGE_TITLE) {
            cafe_ui_select_row(app->ui, CAFE_LIST_TITLE, app->title_sel);
        }
        return;
    }
    if (page == CAFE_PAGE_GAME) {
        const bool was_auto = app->auto_mode;
        if (was_auto) {
            app->auto_mode = false;
            cafe_ui_set_mode(app->ui, false);
        }
        app->fast_forward = false;
        // 自动阅读中手动短按：不吞掉这次操作——跳过打字/句间等待，直接推进。
        advance_reading(app, was_auto);
        return;
    }
    if (page == CAFE_PAGE_CHOICE) {
        if (key == CAFE_KEY_UP) move_sel(&app->choice_sel, app->choice_count, -1);
        else if (key == CAFE_KEY_DOWN) move_sel(&app->choice_sel, app->choice_count, 1);
        else choose_option(app, app->choice_sel);
        const char *labels[CAFE_CHOICE_MAX];
        for (int i = 0; i < app->choice_count; ++i) labels[i] = app->choice_labels[i];
        if (app->page == CAFE_PAGE_CHOICE) {
            cafe_ui_set_choice(app->ui, labels, NULL, app->choice_count, app->choice_sel);
        }
        return;
    }
    if (page == CAFE_PAGE_MENU) {
        if (key == CAFE_KEY_UP) move_sel(&app->menu_sel, 6, -1);
        else if (key == CAFE_KEY_DOWN) move_sel(&app->menu_sel, 6, 1);
        else {
            switch (app->menu_sel) {
            case 0: close_menu(app); break;
            case 1: open_slots(app, true); break;
            case 2: open_slots(app, false); break;
            case 3: { app->history_sel = app->history_count > 0 ? app->history_count - 1 : 0;
                      open_history(app); } break;
            case 4: open_settings(app); break;
            case 5: { save_progress_now(app); save_unlock_now(app); show_title(app, "menu_return_title"); } break;
            default: break;
            }
        }
        if (app->page == CAFE_PAGE_MENU) {
            static const char *menu[6] = { "返回故事", "保存进度", "读取进度", "历史文本",
                                           "设置", "返回标题" };
            cafe_ui_set_rows(app->ui, CAFE_LIST_MENU, menu, NULL, 6, app->menu_sel);
        }
        return;
    }
    if (page == CAFE_PAGE_SAVE || page == CAFE_PAGE_LOAD) {
        int *sel = page == CAFE_PAGE_SAVE ? &app->save_sel : &app->load_sel;
        if (key == CAFE_KEY_UP) move_sel(sel, CAFE_SAVE_SLOTS, -1);
        else if (key == CAFE_KEY_DOWN) move_sel(sel, CAFE_SAVE_SLOTS, 1);
        else if (page == CAFE_PAGE_SAVE) {
            cafe_save_t old;
            if (cafe_store_load_slot(*sel, &old)) {
                app->modal_open = true;
                app->modal_action = MODAL_OVERWRITE;
                app->modal_sel = 0;
                cafe_ui_show_modal(app->ui, "覆盖该存档？",
                                   "原有进度将被替换，此操作无法撤销。",
                                   (const char *const[]){ "取消", "覆盖" }, 2, 0);
            } else {
                save_slot(app, *sel);
            }
        } else {
            load_slot(app, *sel);
        }
        if (app->page == page) refresh_slots(app, page == CAFE_PAGE_SAVE);
        return;
    }
    if (page == CAFE_PAGE_SETTINGS) {
        if (key == CAFE_KEY_UP) move_sel(&app->settings_sel, 8, -1);
        else if (key == CAFE_KEY_DOWN) move_sel(&app->settings_sel, 8, 1);
        else settings_action(app);
        return;
    }
    if (page == CAFE_PAGE_HISTORY) {
        if (key == CAFE_KEY_UP) move_sel(&app->history_sel, app->history_count, -1);
        else if (key == CAFE_KEY_DOWN) move_sel(&app->history_sel, app->history_count, 1);
        if (app->page == CAFE_PAGE_HISTORY) {
            cafe_ui_select_row(app->ui, CAFE_LIST_HISTORY, app->history_sel);
        }
        return;
    }
    if (page == CAFE_PAGE_GALLERY) {
        if (key == CAFE_KEY_UP) move_sel(&app->gallery_sel, 5, -1);
        else if (key == CAFE_KEY_DOWN) move_sel(&app->gallery_sel, 5, 1);
        else {
            if (app->gallery_sel == 0) open_cg_list(app);
            else if (app->gallery_sel == 1) open_recall(app);
            else if (app->gallery_sel == 2) open_chapter(app);
            else if (app->gallery_sel == 3) open_music(app);
            else open_gallery_op(app);
            return;
        }
        if (app->page == CAFE_PAGE_GALLERY) refresh_gallery(app);
        return;
    }
    if (page == CAFE_PAGE_CG_LIST) {
        if (key == CAFE_KEY_UP) {
            if (app->cg_sel > 0) app->cg_sel--;
        } else if (key == CAFE_KEY_DOWN) {
            if (app->cg_sel + 1 < 121) app->cg_sel++;
        } else {
            app->cg_variant = 0;
            open_cg_view(app);
            return;
        }
        if (app->page == CAFE_PAGE_CG_LIST) refresh_cg_list(app);
        return;
    }
    if (page == CAFE_PAGE_CG) {
        cafe_gallery_t g;
        if (cafe_gallery_get((uint16_t)app->cg_sel, &g)) {
            if (key == CAFE_KEY_UP) {
                app->cg_variant = (app->cg_variant + g.variant_count - 1) % g.variant_count;
                open_cg_view(app);
            } else if (key == CAFE_KEY_DOWN) {
                app->cg_variant = (app->cg_variant + 1) % g.variant_count;
                open_cg_view(app);
            } else {
                set_page(app, CAFE_PAGE_CG_LIST);
                refresh_cg_list(app);
            }
        }
        return;
    }
    if (page == CAFE_PAGE_RECALL) {
        if (key == CAFE_KEY_UP) move_recall_selection(app, -1);
        else if (key == CAFE_KEY_DOWN) move_recall_selection(app, 1);
        else {
            const int group = recall_group_at(app, app->recall_sel);
            if (group < 0) {
                app_notice(app, "尚未解锁任何 CG");
                return;
            }
            cafe_gallery_t g;
            if (cafe_gallery_get((uint16_t)group, &g) && g.first_node != 0xFFFF) {
                start_preview(app, g.first_node);
            } else {
                app_notice(app, "该 CG 没有可定位的场景");
            }
        }
        return;
    }
    if (page == CAFE_PAGE_MUSIC) {
        if (key == CAFE_KEY_UP) move_music_selection(app, -1);
        else if (key == CAFE_KEY_DOWN) move_music_selection(app, 1);
        else {
            app->audio_scene = 3;
            if (!cafe_audio_play_preview((uint8_t)app->music_sel, false))
                app_notice(app, "音频播放失败");
            else
                app_notice(app, "播放中 · 4 kHz");
        }
        return;
    }
    if (page == CAFE_PAGE_CHAPTER) {
        if (app->chapter_routes) {
            if (key == CAFE_KEY_UP) move_chapter_selection(app, -1);
            else if (key == CAFE_KEY_DOWN) move_chapter_selection(app, 1);
            else {
                app->chapter_routes = false;
                app->chapter_page = 0;
                app->chapter_sel = 0;
                refresh_chapter_page(app);
            }
            return;
        }
        if (key == CAFE_KEY_UP) move_chapter_selection(app, -1);
        else if (key == CAFE_KEY_DOWN) move_chapter_selection(app, 1);
        else {
            uint16_t node = 0xFFFF;
            if (app->route_sel == 6) {
                cafe_after_t a;
                if (cafe_after_get((uint16_t)app->chapter_sel, &a) &&
                    (app->unlock.progress & (1u << a.requires))) node = a.node;
                else { app_notice(app, "该后日谈尚未解锁"); return; }
            } else {
                int seen = 0;
                for (uint16_t i = 0; i < cafe_meta_u32(4); ++i) {
                    cafe_chapter_t c;
                    if (!cafe_chapter_get(i, &c) || c.route != app->route_sel) continue;
                    if (seen == app->chapter_page * 7 + app->chapter_sel) { node = c.node; break; }
                    seen++;
                }
            }
            if (node != 0xFFFF) start_preview(app, node);
        }
        if (app->page == CAFE_PAGE_CHAPTER) refresh_chapter_page(app);
        return;
    }
    if (page == CAFE_PAGE_TRANSITION) {
        if (app->op_pending) {
            op_finish(app, true);
        } else if (app->preview_pending) {
            app->preview_pending = false;
            enter_location(app);
        } else {
            enter_location(app);
        }
        return;
    }
    if (page == CAFE_PAGE_ENDING) {
        show_title(app, "ending_short");
        return;
    }
    if (page == CAFE_PAGE_ABOUT) {
        if (key == CAFE_KEY_UP) cafe_ui_scroll_about(app->ui, 24);
        else if (key == CAFE_KEY_DOWN) cafe_ui_scroll_about(app->ui, -24);
        return;
    }
}

static bool move_long_direction(cafe_app_t *app, uint8_t key)
{
    if (key != CAFE_KEY_UP && key != CAFE_KEY_DOWN) return false;
    const int delta = key == CAFE_KEY_UP ? -1 : 1;
    switch (app->page) {
    case CAFE_PAGE_TITLE:
        move_sel(&app->title_sel, 6, delta);
        cafe_ui_select_row(app->ui, CAFE_LIST_TITLE, app->title_sel);
        return true;
    case CAFE_PAGE_MENU:
        move_sel(&app->menu_sel, 6, delta);
        cafe_ui_select_row(app->ui, CAFE_LIST_MENU, app->menu_sel);
        return true;
    case CAFE_PAGE_CHOICE:
        if (app->choice_count <= 0) return true;
        move_sel(&app->choice_sel, app->choice_count, delta);
        {
            const char *labels[CAFE_CHOICE_MAX];
            for (int i = 0; i < app->choice_count; ++i) labels[i] = app->choice_labels[i];
            cafe_ui_set_choice(app->ui, labels, NULL, app->choice_count, app->choice_sel);
        }
        return true;
    case CAFE_PAGE_SAVE:
    case CAFE_PAGE_LOAD: {
        int *sel = app->page == CAFE_PAGE_SAVE ? &app->save_sel : &app->load_sel;
        move_sel(sel, CAFE_SAVE_SLOTS, delta);
        cafe_ui_select_row(app->ui,
                           app->page == CAFE_PAGE_SAVE ? CAFE_LIST_SAVE : CAFE_LIST_LOAD, *sel);
        return true;
    }
    case CAFE_PAGE_SETTINGS:
        move_sel(&app->settings_sel, 8, delta);
        refresh_settings(app);
        return true;
    case CAFE_PAGE_HISTORY:
        if (app->history_count <= 0) return true;
        move_sel(&app->history_sel, app->history_count, delta);
        cafe_ui_select_row(app->ui, CAFE_LIST_HISTORY, app->history_sel);
        return true;
    case CAFE_PAGE_MUSIC:
        move_music_selection(app, delta);
        return true;
    case CAFE_PAGE_CHAPTER:
        move_chapter_selection(app, delta);
        return true;
    case CAFE_PAGE_RECALL:
        move_recall_selection(app, delta);
        return true;
    case CAFE_PAGE_CG_LIST:
        if (delta < 0) { if (app->cg_sel > 0) app->cg_sel--; }
        else { if (app->cg_sel + 1 < 121) app->cg_sel++; }
        refresh_cg_list(app);
        return true;
    case CAFE_PAGE_CG: {
        cafe_gallery_t g;
        if (cafe_gallery_get((uint16_t)app->cg_sel, &g) && g.variant_count > 0) {
            app->cg_variant = (app->cg_variant + (delta < 0 ? g.variant_count - 1 : 1)) % g.variant_count;
            open_cg_view(app);
        }
        return true;
    }
    case CAFE_PAGE_GALLERY:
        move_sel(&app->gallery_sel, 5, delta);
        refresh_gallery(app);
        return true;
    default:
        return false;
    }
}

static void handle_long(cafe_app_t *app, uint8_t key)
{
    touch(app);
    if (app->modal_open) {
        // 长按 OK 取消覆盖确认，短按 OK 才执行。
        app->modal_open = false;
        cafe_ui_hide_modal(app->ui);
        refresh_slots(app, true);
        return;
    }
    const cafe_page_t page = app->page;
    if (page == CAFE_PAGE_GAME) {
        if (key == CAFE_KEY_OK) {
            open_menu(app);
        } else if (key == CAFE_KEY_DOWN) {
            app->auto_mode = !app->auto_mode;
            app->auto_accum_ms = 0;
            app->fast_forward = false;
            cafe_ui_set_mode(app->ui, app->auto_mode);
        } else if (key == CAFE_KEY_UP) {
            app->fast_forward = true;
            app->auto_mode = false;
            app->ff_start_ms = app->now_ms;
            app->ff_steps = 0;
            app->ff_total_us = 0;
            app->ff_max_us = 0;
            app->ff_scene_start = cafe_image_render_scene_count();
            app->ff_verify_start = cafe_image_verify_count();
            cafe_ui_set_mode(app->ui, false);
        }
        return;
    }
    if (page == CAFE_PAGE_CHOICE) {
        if (key == CAFE_KEY_OK) open_menu(app);
        else (void)move_long_direction(app, key);
        return;
    }
    if (page == CAFE_PAGE_TITLE) {
        if (key == CAFE_KEY_UP || key == CAFE_KEY_DOWN) (void)move_long_direction(app, key);
        return;
    }
    if (page == CAFE_PAGE_START) return;
    if (page == CAFE_PAGE_TRANSITION && app->op_pending && app->op_gallery &&
        key == CAFE_KEY_OK) {
        op_finish(app, true);
        return;
    }
    if (page == CAFE_PAGE_ENDING || page == CAFE_PAGE_TRANSITION) {
        if (key == CAFE_KEY_OK && page == CAFE_PAGE_ENDING) show_title(app, "ending_long");
        return;
    }
    if (key != CAFE_KEY_OK) {
        (void)move_long_direction(app, key);
        return;
    }
    // 二级页面：长按 OK 返回。
    switch (page) {
    case CAFE_PAGE_MENU: close_menu(app); break;
    case CAFE_PAGE_SAVE:
    case CAFE_PAGE_LOAD:
        if (app->return_page == CAFE_PAGE_TITLE) show_title(app, "settings_return_title");
        else open_menu(app);
        break;
    case CAFE_PAGE_SETTINGS:
        if (app->return_page == CAFE_PAGE_TITLE) show_title(app, "settings_return_title");
        else if (app->return_page == CAFE_PAGE_GALLERY) open_gallery(app);
        else open_menu(app);
        break;
    case CAFE_PAGE_HISTORY: open_menu(app); break;
    case CAFE_PAGE_GALLERY: show_title(app, "gallery_long"); break;
    case CAFE_PAGE_CG_LIST: open_gallery(app); break;
    case CAFE_PAGE_CG: set_page(app, CAFE_PAGE_CG_LIST); refresh_cg_list(app); break;
    case CAFE_PAGE_RECALL: open_gallery(app); break;
    case CAFE_PAGE_MUSIC: open_gallery(app); break;

    case CAFE_PAGE_CHAPTER:
        if (app->chapter_routes) open_gallery(app);
        else if (app->route_sel == 6) {
            app->chapter_routes = true;
            refresh_chapter_page(app);
        } else {
            app->chapter_routes = true;
            refresh_chapter_page(app);
        }
        break;
    case CAFE_PAGE_ABOUT: show_title(app, "about_long"); break;
    default: break;
    }
}

static void fault_retry(cafe_app_t *app)
{
    const uint8_t kind = app->fault_kind;
    app->resource_fault = false;
    app->fault_kind = FAULT_NONE;
    app->modal_open = false;
    cafe_ui_hide_modal(app->ui);
    if (kind == FAULT_TITLE) {
        app->title_art_failed = false;
        show_title(app, "fault_title_retry");
    } else if (kind == FAULT_CG) {
        open_cg_view(app);
    } else if (kind == FAULT_OP) {
        app->op_pending = true;
        app->op_gallery = app->fault_op_gallery;
        app->op_auto_resume = app->fault_op_auto_resume;
        const uint16_t retry_frame = app->fault_op_frame;
        const uint32_t retry_elapsed = app->fault_op_elapsed_ms;
        app->op_elapsed_ms = retry_elapsed;
        app->op_start_ms = app->now_ms - retry_elapsed;
        set_page(app, CAFE_PAGE_TRANSITION);
        cafe_ui_art_visible(app->ui, true);
        app->audio_scene = 2;
        (void)cafe_audio_play_op_at(retry_elapsed);
        const uint16_t first = cafe_op_image_id(retry_frame);
        if (first == CAFE_IMAGE_NONE || !cafe_image_render_op(app->bands, first))
            app_fault(app, FAULT_OP, "OP 图像读取/CRC/解码失败");
        else
            cafe_ui_art_invalidate(app->ui);
    } else if (kind == FAULT_AUDIO) {
        if (app->fault_page == CAFE_PAGE_TRANSITION && app->fault_op_pending) {
            /* Audio failure happened during OP: restore the OP, not title BGM. */
            app->op_pending = true;
            app->op_gallery = app->fault_op_gallery;
            app->op_auto_resume = app->fault_op_auto_resume;
            const uint16_t retry_frame = app->fault_op_frame;
            const uint32_t retry_elapsed = app->fault_op_elapsed_ms;
            app->op_frame = retry_frame;
            app->op_elapsed_ms = retry_elapsed;
            app->op_start_ms = app->now_ms - retry_elapsed;
            set_page(app, CAFE_PAGE_TRANSITION);
            cafe_ui_art_visible(app->ui, true);
            app->audio_scene = 2;
            (void)cafe_audio_play_op_at(retry_elapsed);
            const uint16_t first = cafe_op_image_id(retry_frame);
            if (first == CAFE_IMAGE_NONE || !cafe_image_render_op(app->bands, first))
                app_fault(app, FAULT_AUDIO, "OP 图像读取/CRC/解码失败");
            else
                cafe_ui_art_invalidate(app->ui);
        } else if (app->fault_page == CAFE_PAGE_MUSIC) {
            app->audio_scene = 3;
            set_page(app, CAFE_PAGE_MUSIC);
            (void)cafe_audio_play_preview((uint8_t)app->music_sel, false);
        } else {
            set_page(app, app->fault_page);
            audio_scene(app, app->fault_audio_scene);
        }
    } else {
        set_page(app, CAFE_PAGE_GAME);
        show_frame(app, true);
    }
}

static void fault_return_title(cafe_app_t *app)
{
    const uint8_t kind = app->fault_kind;
    app->resource_fault = false;
    app->fault_kind = FAULT_NONE;
    app->modal_open = false;
    cafe_ui_hide_modal(app->ui);
    if (kind == FAULT_TITLE) app->title_art_failed = true;
    show_title(app, "fault_return_title");
}

static void handle_modal_short(cafe_app_t *app, uint8_t key)
{
    if (app->modal_action == MODAL_RESOURCE) {
        if (key == CAFE_KEY_UP) move_sel(&app->modal_sel, 2, -1);
        else if (key == CAFE_KEY_DOWN) move_sel(&app->modal_sel, 2, 1);
        else if (key == CAFE_KEY_OK) {
            if (app->modal_sel == 0) fault_retry(app);
            else fault_return_title(app);
            return;
        }
        static const char *const actions[] = { "重试", "返回标题" };
        cafe_ui_show_modal(app->ui, "资源读取失败", app->fault_message, actions, 2,
                           app->modal_sel);
        return;
    }
    if (key == CAFE_KEY_UP) move_sel(&app->modal_sel, 2, -1);
    else if (key == CAFE_KEY_DOWN) move_sel(&app->modal_sel, 2, 1);
    else if (key == CAFE_KEY_OK) {
        if (app->modal_sel == 1) save_slot(app, app->save_sel);
        app->modal_open = false;
        cafe_ui_hide_modal(app->ui);
        refresh_slots(app, true);
        return;
    }
    cafe_ui_set_rows(app->ui, CAFE_LIST_MODAL,
                     (const char *const[]){ "取消", "覆盖" }, NULL, 2, app->modal_sel);
}

void cafe_app_key_at(cafe_app_t *app, uint8_t key, bool pressed,
                     uint32_t event_ms, uint32_t process_ms)
{
    if (!app) return;
    app->now_ms = process_ms;
    ESP_LOGI(TAG, "key k=%u %s event=%u proc=%u page=%u vm=n%u.%u",
             (unsigned)key, pressed ? "press" : "release", (unsigned)event_ms,
             (unsigned)process_ms, (unsigned)app->page,
             (unsigned)app->vm.node, (unsigned)app->vm.index);
    const uint8_t bit = (uint8_t)(1u << key);
    if (app->fault_swallow_mask & bit) {
        /* Consume the same press/release cycle that observed the fault. */
        if (pressed) (void)cafe_input_press(&app->input, key, event_ms);
        else (void)cafe_input_release(&app->input, key, event_ms, CAFE_LONG_MS);
        if (!pressed) app->fault_swallow_mask &= (uint8_t)~bit;
        return;
    }
    if (!app->resource_fault && cafe_audio_failed()) {
        app_fault(app, FAULT_AUDIO, cafe_audio_failure());
        app->fault_swallow_mask |= bit;
        if (pressed) (void)cafe_input_press(&app->input, key, event_ms);
        else (void)cafe_input_release(&app->input, key, event_ms, CAFE_LONG_MS);
        return;
    }
    touch(app);
    const cafe_input_event_t ev = pressed
        ? cafe_input_press(&app->input, key, event_ms)
        : cafe_input_release(&app->input, key, event_ms, CAFE_LONG_MS);
    if (!pressed && key == CAFE_KEY_UP) {
        ff_finish(app, "release");
    }
    if (ev == CAFE_IN_SHORT) {
        if (app->modal_open) handle_modal_short(app, key);
        else handle_short(app, key);
    } else if (ev == CAFE_IN_LONG) {
        handle_long(app, key);
    }
}

void cafe_app_key(cafe_app_t *app, uint8_t key, bool pressed, uint32_t now_ms)
{
    cafe_app_key_at(app, key, pressed, now_ms, now_ms);
}

void cafe_app_cancel_input(cafe_app_t *app)
{
    if (!app) return;
    cafe_input_abort(&app->input);
    ff_finish(app, "cancel");
    app->auto_mode = false;
    app->auto_accum_ms = 0;
    if (app->ui) cafe_ui_set_mode(app->ui, false);
}

void cafe_app_tick(cafe_app_t *app, uint32_t now_ms, uint32_t elapsed_ms)
{
    if (!app) return;
    app->now_ms = now_ms;
    if (!app->resource_fault && cafe_audio_failed()) {
        app_fault(app, FAULT_AUDIO, cafe_audio_failure());
        app->fault_swallow_mask = 0x07;  // discard any in-flight press/release
        return;
    }
    if (app->resource_fault) return;
    uint8_t key = 0;
    while (cafe_input_tick(&app->input, now_ms, CAFE_LONG_MS, &key) == CAFE_IN_LONG) {
        handle_long(app, key);
    }
    if (app->page == CAFE_PAGE_TRANSITION && app->op_pending) {
        op_tick(app);
        if (app->page != CAFE_PAGE_TRANSITION || app->resource_fault) return;
    }
    if (app->notice_ms && (int32_t)(now_ms - app->notice_ms) >= 0) {
        app->notice_ms = 0;
        cafe_ui_set_notice(app->ui, "");
        if (app->page == CAFE_PAGE_SAVE || app->page == CAFE_PAGE_LOAD ||
            app->page == CAFE_PAGE_SETTINGS) {
            cafe_ui_set_footer(app->ui, app->page, "");
        }
    }
    if (app->page == CAFE_PAGE_GAME && app->fast_forward) {
        // One sentence maximum per cafe_app_tick/input control tick.  There is
        // no per-sentence accumulator and no catch-up loop.
        fast_forward_step(app);
    } else if (app->page == CAFE_PAGE_GAME && app->auto_mode && !cafe_ui_typing(app->ui)) {
        app->auto_accum_ms += elapsed_ms;
        if (app->auto_accum_ms >= CAFE_AUTO_MS) {
            app->auto_accum_ms = 0;
            advance_reading(app, false);
            touch(app);
            if (app->page != CAFE_PAGE_GAME) {
                app->auto_mode = false;
                cafe_ui_set_mode(app->ui, false);
            }
        }
    } else if (app->page == CAFE_PAGE_TRANSITION && app->auto_mode) {
        app->auto_accum_ms += elapsed_ms;
        if (app->auto_accum_ms >= CAFE_AUTO_TRANS_MS) {
            app->auto_accum_ms = 0;
            handle_short(app, CAFE_KEY_OK);
        }
    }
    if (app->progress_dirty) {
        app->save_accum_ms += elapsed_ms;
        if (app->save_accum_ms >= CAFE_PROGRESS_SAVE_MS) save_progress_now(app);
    }
    if (app->unlock_dirty) save_unlock_now(app);
    if (app->settings_dirty) save_settings_now(app);
}

uint32_t cafe_app_idle_ms(const cafe_app_t *app)
{
    if (!app) return 0;
    return app->now_ms - app->last_activity_ms;
}

void cafe_app_flush(cafe_app_t *app)
{
    if (!app) return;
    if (app->resource_fault) return;
    save_progress_now(app);
    save_unlock_now(app);
    save_settings_now(app);
}

void cafe_app_set_battery(cafe_app_t *app, int percent)
{
    if (!app) return;
    app->battery = percent;
    cafe_ui_set_battery(app->ui, percent);
}

bool cafe_app_init(cafe_app_t *app, cafe_ui_t *ui, cafe_bands_t *bands)
{
    if (!app || !ui || !bands) return false;
    memset(app, 0, sizeof(*app));
    app->ui = ui;
    app->bands = bands;
    app->battery = -1;
    app->ending_bit = 0xFF;
    invalidate_art_cache(app);
    app->menu_source = CAFE_PAGE_GAME;
    cafe_input_init(&app->input);
    cafe_store_defaults(&app->settings);
    (void)cafe_store_init();
    (void)cafe_store_load_settings(&app->settings);
    if (!cafe_store_load_unlock(&app->unlock)) {
        memset(&app->unlock, 0, sizeof(app->unlock));
        app->unlock.last_cleared = 0xFF;
    }
    for (int i = 0; i < CAFE_SAVE_SLOTS; ++i) {
        if (!cafe_store_load_slot(i, &app->slots[i])) memset(&app->slots[i], 0, sizeof(app->slots[i]));
    }
    cafe_vm_init(&app->vm, app->unlock.progress);
    cafe_audio_set_volume(app->settings.sound_on ? app->settings.volume : 0);
    checkpoint_progress(app);
    cafe_ui_bind_art(ui, bands);
    cafe_ui_set_brand(ui, TITLE_TEXT, SUBTITLE_TEXT);
    cafe_ui_set_battery(ui, -1);
    cafe_ui_set_font_large(ui, app->settings.font_large);
    cafe_ui_set_panel_transparency(ui, app->settings.panel_opacity);
    cafe_ui_set_notice(ui, "");
    app->page = CAFE_PAGE_START;
    cafe_ui_show_page(ui, CAFE_PAGE_START);
    cafe_ui_art_visible(ui, false);
    app->now_ms = 0;
    app->last_activity_ms = 0;
    ESP_LOGI(TAG, "Cafe Stella 就绪: 节点=%u 包=%08x", (unsigned)cafe_meta_u32(0),
             (unsigned)cafe_pack_source_sha_prefix());
    return true;
}
