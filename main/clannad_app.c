// main/clannad_app.c —— 应用状态机、按键语义、存读档、通关记录与自动档。
#include "clannad_app.h"

#include "bsp_button.h"
#include "clannad_text.h"

#include "lvgl.h"

LV_FONT_DECLARE(clannad_cjk_16);

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "clannad_app";

#define NVS_NS "clannad"
#define NVS_AUTO_NS "clannad_auto"
#define KEY_CFG "cfg"
#define KEY_PROG "prog"
#define KEY_AUTO "auto"

#define AUTOSAVE_TEXTS 18
#define PROGRESS_WRITE_MS 2000u
#define PROGRESS_ACC_MS 500u

static uint8_t s_save_raw[CLAN_VM_SAVE_RAW_MAX];
static uint8_t s_save_packed[CLAN_VM_SAVE_PACKED_MAX];
static uint8_t s_tmp[CLAN_VM_PROGRESS_MAX];

// ---------------------------------------------------------------- utilities
static bool nvs_write_blob(const char *ns, const char *key, const void *data, size_t len)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(h, key, data, len);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

// The exact NVS status matters for slots: a transient read error must not look
// like an empty slot. Keep the boolean wrapper for progress/config reads, but
// preserve ESP_ERR_NVS_NOT_FOUND separately for slots and autosave.
static esp_err_t nvs_read_blob_ex(const char *ns, const char *key, void *data,
                                  size_t cap, size_t *out_len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t len = 0;
    err = nvs_get_blob(h, key, NULL, &len);
    if (err == ESP_OK) {
        if (len == 0 || len > cap) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            err = nvs_get_blob(h, key, data, &len);
            if (err == ESP_OK && out_len) *out_len = len;
        }
    }
    nvs_close(h);
    return err;
}

static bool nvs_read_blob(const char *ns, const char *key, void *data, size_t cap,
                          size_t *out_len)
{
    return nvs_read_blob_ex(ns, key, data, cap, out_len) == ESP_OK;
}

static void slot_key(int index, char *key, size_t cap)
{
    snprintf(key, cap, "s%02d", index);
}

static void set_notice(clannad_app_t *app, const char *text)
{
    if (!app) return;
    snprintf(app->notice, sizeof(app->notice), "%s", text ? text : "");
    app->notice_ms = text && text[0] ? CLAN_NOTICE_MS : 0;
    clannad_ui_notice(&app->ui, app->notice);
}

// 复制并保证 UTF-8 完整；超长时在码位边界截断。
static void copy_utf8(char *dst, size_t cap, const char *src, size_t len)
{
    if (!dst || cap == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    if (len > cap - 1) {
        len = cap - 1;
        while (len && (((uint8_t)src[len] & 0xC0u) == 0x80u)) len--;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static int typewriter_ms(const clannad_app_t *app)
{
    switch (app->settings.speed) {
    case 0: return 60;
    case 2: return 12;
    case 3: return 0;
    default: return 30;
    }
}

static uint32_t auto_wait_ms(const clannad_app_t *app)
{
    const int base = app->settings.auto_speed == 0 ? 2600 :
                     (app->settings.auto_speed == 2 ? 800 : 1500);
    const int chars = clannad_utf8_count(app->text, app->text_len);
    const int extra = chars > 60 ? (chars - 60) * 55 : 0;
    return (uint32_t)base + (uint32_t)(extra > 5000 ? 5000 : extra);
}

// ---------------------------------------------------------------- progress
static void update_progress_flags(clannad_app_t *app)
{
    app->light_count = app->progress_valid
                           ? clannad_progress_light_count(app->progress, app->progress_len)
                           : 0;
    app->after_unlocked = app->progress_valid &&
                          clannad_progress_after_unlocked(app->progress, app->progress_len);
    app->true_ending = app->progress_valid &&
                       clannad_progress_true_ending(app->progress, app->progress_len);
}

static void progress_load(clannad_app_t *app)
{
    size_t len = 0;
    app->progress_valid = false;
    app->progress_len = 0;
    if (nvs_read_blob(NVS_NS, KEY_PROG, s_tmp, sizeof(s_tmp), &len) &&
        clannad_vm_progress_validate(s_tmp, len)) {
        memcpy(app->progress, s_tmp, len);
        app->progress_len = (uint16_t)len;
        app->progress_valid = true;
    }
    update_progress_flags(app);
}

static void progress_sync(clannad_app_t *app)
{
    if (!app || !app->game_active || app->vm.mode != CLAN_MODE_STORY) return;
    const size_t len = clannad_vm_progress_encode(&app->vm, s_tmp, sizeof(s_tmp));
    if (!len || !clannad_vm_progress_validate(s_tmp, len)) return;
    if (app->progress_valid && app->progress_len == len &&
        memcmp(app->progress, s_tmp, len) == 0) {
        return;
    }
    memcpy(app->progress, s_tmp, len);
    app->progress_len = (uint16_t)len;
    app->progress_valid = true;
    app->progress_dirty = true;
    update_progress_flags(app);
}

static bool progress_write(clannad_app_t *app, bool force)
{
    if (!app || !app->progress_dirty) return true;
    if (!app->storage_ok) return false;
    if (!force && app->progress_ms < PROGRESS_WRITE_MS) return false;
    if (app->progress_len == 0 || !app->progress_valid) return false;
    if (nvs_write_blob(NVS_NS, KEY_PROG, app->progress, app->progress_len)) {
        app->progress_dirty = false;
        app->progress_ms = 0;
        return true;
    }
    app->progress_ms = 0;
    set_notice(app, "通关记录保存失败，稍后重试");
    ESP_LOGW(TAG, "progress write failed");
    return false;
}

// ---------------------------------------------------------------- save I/O
static bool save_pack(const uint8_t *raw, size_t raw_len, size_t *packed_len)
{
    uint32_t out = 0;
    if (!clannad_deflate_raw(raw, (uint32_t)raw_len, s_save_packed,
                             sizeof(s_save_packed), &out)) {
        return false;
    }
    *packed_len = out;
    return true;
}

static bool save_unpack(const uint8_t *packed, size_t packed_len, size_t *raw_len)
{
    uint32_t out = 0;
    if (!clannad_inflate_raw(packed, (uint32_t)packed_len, s_save_raw,
                             sizeof(s_save_raw), &out)) {
        return false;
    }
    *raw_len = out;
    return true;
}

static uint16_t current_page_index(const clannad_app_t *app)
{
    return app->page == CLAN_PAGE_CHOICE ? 0 : (uint16_t)app->page_index;
}

static bool encode_current(clannad_app_t *app, uint16_t page, uint16_t choice_sel,
                           size_t *raw_len)
{
    *raw_len = clannad_vm_save_encode(&app->vm, page, choice_sel,
                                      (uint32_t)(esp_timer_get_time() / 1000000),
                                      clannad_vm_chapter_name(&app->vm),
                                      s_save_raw, sizeof(s_save_raw));
    return *raw_len > 0;
}

static void snapshot_push(clannad_app_t *app)
{
    size_t len = 0;
    if (!encode_current(app, current_page_index(app),
                        (uint16_t)app->choice_sel, &len)) {
        return;
    }
    const int slot = app->snapshot_head;
    memcpy(app->snapshot[slot], s_save_raw, len);
    app->snapshot_len[slot] = (uint16_t)len;
    app->snapshot_page[slot] = (uint16_t)app->page_index;
    app->snapshot_choice[slot] = (uint16_t)app->choice_sel;
    app->snapshot_head = (app->snapshot_head + 1) % CLAN_SNAPSHOTS;
    if (app->snapshot_count < CLAN_SNAPSHOTS) app->snapshot_count++;
}

static bool snapshot_previous(clannad_app_t *app)
{
    if (!app->snapshot_count) return false;
    app->snapshot_head = (app->snapshot_head + CLAN_SNAPSHOTS - 1) % CLAN_SNAPSHOTS;
    app->snapshot_count--;
    const int slot = app->snapshot_head;
    if (!clannad_vm_save_validate(&app->vm, app->snapshot[slot],
                                  app->snapshot_len[slot], NULL)) {
        return false;
    }
    if (!clannad_vm_save_apply(&app->vm, app->snapshot[slot], app->snapshot_len[slot])) {
        return false;
    }
    progress_sync(app);
    app->cache_valid = false;
    app->page_index = app->snapshot_page[slot];
    app->choice_sel = app->snapshot_choice[slot];
    return true;
}

static bool reading_checkpoint_valid(const clannad_app_t *app)
{
    return app && app->game_active && !app->vm.ended &&
           (app->frame.type == CLAN_FRAME_TEXT || app->frame.type == CLAN_FRAME_CHOICE);
}

static void autosave_now(clannad_app_t *app)
{
    if (!app || !app->storage_ok || !reading_checkpoint_valid(app)) return;
    // The visible page may be MENU/HISTORY/SETTINGS/SAVE/LOAD/title. The
    // checkpoint is the VM text/choice position, not the page that happens to
    // be on screen, so page_index/choice_sel are intentionally reused here.
    size_t raw_len = 0;
    if (!encode_current(app, current_page_index(app),
                        (uint16_t)app->choice_sel, &raw_len)) {
        return;
    }
    if (!clannad_vm_save_validate(&app->vm, s_save_raw, raw_len, NULL)) {
        return;
    }
    size_t packed_len = 0;
    if (!save_pack(s_save_raw, raw_len, &packed_len)) {
        app->auto_dirty = true;
        return;
    }
    if (nvs_write_blob(NVS_AUTO_NS, KEY_AUTO, s_save_packed, packed_len)) {
        app->auto_save_acc = 0;
        app->auto_dirty = false;
        app->auto_retry_ms = 0;
    } else {
        app->auto_dirty = true;
        ESP_LOGW(TAG, "autosave write failed");
    }
}

// ---------------------------------------------------------------- text UI
static void push_history(clannad_app_t *app)
{
    const int slot = app->history_head;
    copy_utf8(app->history[slot].speaker, sizeof(app->history[slot].speaker),
              app->speaker, strlen(app->speaker));
    copy_utf8(app->history[slot].text, sizeof(app->history[slot].text),
              app->text, app->text_len);
    app->history_head = (app->history_head + 1) % CLAN_HISTORY;
    if (app->history_count < CLAN_HISTORY) app->history_count++;
}

static void layout_page(clannad_app_t *app)
{
    const uint16_t start = app->page_offsets[app->page_index];
    const uint16_t end = app->page_offsets[app->page_index + 1];
    const uint16_t reveal = app->shown_bytes < end ? app->shown_bytes : end;
    clannad_text_layout(app->text, start, end, reveal, CLAN_TEXT_UNITS_PER_LINE,
                        app->line);
}

static void refresh_game(clannad_app_t *app)
{
    layout_page(app);
    snprintf(app->page_mark, sizeof(app->page_mark), "%d/%d",
             app->page_index + 1, app->page_count);
    clannad_ui_game(&app->ui, app->speaker, app->line[0], app->line[1], app->line[2],
                    clannad_vm_chapter_name(&app->vm),
                    app->page_count > 1 ? app->page_mark : "");
}

static void set_text_frame(clannad_app_t *app, const clannad_frame_t *frame)
{
    copy_utf8(app->speaker, sizeof(app->speaker), frame->speaker,
              strlen(frame->speaker ? frame->speaker : ""));
    const size_t len = strlen(frame->text ? frame->text : "");
    copy_utf8(app->text, sizeof(app->text), frame->text, len);
    app->text_len = (uint16_t)strlen(app->text);
    app->page_count = clannad_text_pages(app->text, CLAN_TEXT_UNITS_PER_LINE,
                                        CLAN_TEXT_LINES, app->page_offsets,
                                        CLAN_TEXT_MAX_PAGES + 1);
    if (app->page_count > CLAN_TEXT_MAX_PAGES) app->page_count = CLAN_TEXT_MAX_PAGES;
    app->page_index = 0;
    app->shown_bytes = app->page_offsets[0];
    app->typing = app->settings.speed != 3;
    app->type_ms = 0;
    app->auto_armed = false;
    app->auto_ms = 0;
    push_history(app);
    layout_page(app);
}

static void complete_page(clannad_app_t *app)
{
    app->shown_bytes = app->page_offsets[app->page_index + 1];
    app->typing = false;
    app->type_ms = 0;
    app->auto_armed = false;
    app->auto_ms = 0;
    refresh_game(app);
}

static void show_title(clannad_app_t *app);

static void show_ending(clannad_app_t *app)
{
    app->page = CLAN_PAGE_ENDING;
    clannad_ui_row_t *rows = app->rows;
    memset(rows, 0, sizeof(app->rows));
    snprintf(rows[0].main, sizeof(rows[0].main), "光玉 %ld / 13",
             (long)app->light_count);
    snprintf(rows[0].sub, sizeof(rows[0].sub), "%s",
             app->vm.mode == CLAN_MODE_STORY ? "通关记录已保存"
                                             : "章节阅读不计入通关记录");
    snprintf(rows[1].main, sizeof(rows[1].main), "返回标题");
    snprintf(rows[1].sub, sizeof(rows[1].sub), "确定返回");
    clannad_ui_show(&app->ui, CLAN_UI_LIST);
    clannad_ui_list(&app->ui, "本篇阅读完毕", rows, 2, app->list_sel, 0,
                    "确定返回标题 · 长按确定");
    app->list_sel = 0;
    app->list_top = 0;
}

static void show_error(clannad_app_t *app, const char *message)
{
    app->page = CLAN_PAGE_ERROR;
    clannad_ui_show(&app->ui, CLAN_UI_MESSAGE);
    clannad_ui_message(&app->ui, "暂时无法继续", message ? message : "",
                       "确定返回标题");
    ESP_LOGE(TAG, "error page: %s", message ? message : "");
}

static void refresh_page(clannad_app_t *app);

static void run_frame(clannad_app_t *app)
{
    app->frame = clannad_vm_run(&app->vm);
    if (app->frame.type == CLAN_FRAME_TEXT) {
        app->page = CLAN_PAGE_GAME;
        set_text_frame(app, &app->frame);
        progress_sync(app);
        refresh_page(app);
    } else if (app->frame.type == CLAN_FRAME_CHOICE) {
        app->page = CLAN_PAGE_CHOICE;
        app->choice_count = app->frame.choice_count;
        for (uint8_t i = 0; i < app->choice_count; ++i) {
            snprintf(app->choice_text[i], sizeof(app->choice_text[i]), "%s",
                     app->frame.choice_text[i] ? app->frame.choice_text[i] : "");
            app->choice_value[i] = app->frame.choice_value[i];
        }
        app->choice_sel = 0;
        app->choice_top = 0;
        progress_sync(app);
        refresh_page(app);
    } else if (app->frame.type == CLAN_FRAME_END) {
        app->page = CLAN_PAGE_ENDING;
        app->typing = false;
        app->fast_forward = false;
        progress_sync(app);
        (void)progress_write(app, true);
        app->list_sel = 0;
        show_ending(app);
    } else {
        show_error(app, app->frame.text ? app->frame.text : app->vm.error);
    }
    // 场景切换或积累若干句后写自动档（节流见 tick）。
    if (app->vm.chapter != app->last_scene) {
        app->last_scene = app->vm.chapter;
        autosave_now(app);
    }
}

static void advance_text(clannad_app_t *app)
{
    if (app->typing) {
        complete_page(app);
        return;
    }
    if (app->page_index + 1 < app->page_count) {
        app->page_index++;
        app->shown_bytes = app->page_offsets[app->page_index];
        app->typing = app->settings.speed != 3;
        app->type_ms = 0;
        app->auto_armed = false;
        app->auto_ms = 0;
        refresh_game(app);
        return;
    }
    snapshot_push(app);
    if (!clannad_vm_next(&app->vm)) {
        show_error(app, "剧情游标越界");
        return;
    }
    app->auto_save_acc++;
    if (app->auto_save_acc >= AUTOSAVE_TEXTS) autosave_now(app);
    run_frame(app);
}

static void choose_current(clannad_app_t *app, uint8_t index)
{
    if (app->page != CLAN_PAGE_CHOICE || index >= app->choice_count) return;
    snapshot_push(app);
    if (!clannad_vm_choose(&app->vm, index)) {
        show_error(app, app->vm.error);
        return;
    }
    app->choice_sel = 0;
    app->choice_top = 0;
    run_frame(app);
}

// ---------------------------------------------------------------- images
static uint16_t meta_asset(clannad_app_t *app, const char *key)
{
    return (uint16_t)clannad_pack_meta_int(&app->pack, key, 0);
}

static bool draw_scene_image(clannad_app_t *app)
{
    if (app->cache_valid && app->cache_bg == app->vm.background &&
        app->cache_count == app->vm.body_count &&
        memcmp(app->cache_bodies, app->vm.bodies, sizeof(app->cache_bodies)) == 0) {
        return true;
    }
    const bool ok = clannad_image_show_scene(&app->ui.image, &app->pack,
                                             app->vm.background, app->vm.bodies,
                                             app->vm.body_count);
    if (ok) {
        app->cache_bg = app->vm.background;
        app->cache_bodies[0] = app->vm.bodies[0];
        app->cache_bodies[1] = app->vm.bodies[1];
        app->cache_bodies[2] = app->vm.bodies[2];
        app->cache_count = app->vm.body_count;
        app->cache_valid = true;
        app->image_failed = false;
    } else {
        app->cache_valid = false;
        app->image_failed = true;
        set_notice(app, "图片读取失败，按确定或上键重试");
    }
    return ok;
}

static void draw_title_art(clannad_app_t *app)
{
    // The title page is transparent over the art canvas. Clear the whole
    // canvas first so a failed/partial decode cannot keep zeroed BSS or a
    // previous frame after returning to the title.
    clannad_image_clear(&app->ui.image);
    const uint16_t bg = meta_asset(app, app->true_ending ? "title_after" : "title_before");
    const uint16_t logo = meta_asset(app, "logo");
    const uint16_t subtitle = meta_asset(app, "subtitle");
    bool ok = clannad_image_show_asset(&app->ui.image, &app->pack, bg, 0, CLAN_TITLE_Y,
                                       CLAN_SCREEN_W, CLAN_TITLE_H, false);
    if (ok) ok = clannad_image_show_asset(&app->ui.image, &app->pack, logo, 4, 6,
                                          180, 41, true);
    if (ok) ok = clannad_image_show_asset(&app->ui.image, &app->pack, subtitle, 10, 52,
                                          130, 25, true);
    app->cache_valid = false;
    app->image_failed = !ok;
    app->title_art_for_ending = app->true_ending;
    if (!ok) set_notice(app, "标题图片读取失败");
}

static void draw_cg(clannad_app_t *app)
{
    clannad_gallery_group_t group;
    if (!clannad_pack_gallery_group(&app->pack, (uint16_t)app->gallery_group, &group)) {
        show_error(app, "CG 目录无效");
        return;
    }
    if (app->gallery_variant >= group.count) app->gallery_variant = 0;
    clannad_gallery_entry_t entry;
    if (!clannad_pack_gallery_entry(&app->pack,
                                    (uint16_t)(group.first_entry + app->gallery_variant),
                                    &entry)) {
        show_error(app, "CG 资源无效");
        return;
    }
    const bool ok = clannad_image_show_cg(&app->ui.image, &app->pack, entry.asset);
    if (!ok) set_notice(app, "CG 读取失败，按确定或上键重试");
    app->cache_valid = false;
    app->image_failed = !ok;
    char caption[96];
    char hint[64];
    snprintf(caption, sizeof(caption), "%s · %u/%u",
             clannad_pack_gallery_label(&app->pack, (uint16_t)app->gallery_group),
             (unsigned)(app->gallery_variant + 1), (unsigned)group.count);
    snprintf(hint, sizeof(hint), "上/下差分 · 确定返回");
    clannad_ui_cg(&app->ui, caption, hint);
}

// ---------------------------------------------------------------- pages
static void open_menu(clannad_app_t *app)
{
    if (app->page != CLAN_PAGE_GAME && app->page != CLAN_PAGE_CHOICE) return;
    app->menu_return = app->page;
    app->page = CLAN_PAGE_MENU;
    app->list_sel = 0;
    app->list_top = 0;
    refresh_page(app);
}

static void open_history(clannad_app_t *app)
{
    // 正文/待选的下键与菜单第 5 项都走这里；菜单进入后返回菜单。
    if (app->page != CLAN_PAGE_GAME && app->page != CLAN_PAGE_CHOICE &&
        app->page != CLAN_PAGE_MENU) {
        return;
    }
    app->return_page = app->page;
    app->page = CLAN_PAGE_HISTORY;
    app->list_sel = 0;
    app->list_top = 0;
    refresh_page(app);
}

static void open_settings(clannad_app_t *app)
{
    app->return_page = (app->page == CLAN_PAGE_MENU) ? CLAN_PAGE_MENU : CLAN_PAGE_TITLE;
    app->page = CLAN_PAGE_SETTINGS;
    app->list_sel = 0;
    app->list_top = 0;
    refresh_page(app);
}

static void open_about(clannad_app_t *app)
{
    // 不自带返回字段：关于页只从设置进入，返回固定 SETTINGS（见 go_back/
    // activate_list）。写共享 return_page 会覆盖设置页自己的来源，把设置页锁死。
    app->page = CLAN_PAGE_ABOUT;
    app->list_sel = 0;
    app->list_top = 0;
    refresh_page(app);
}

static void open_gallery(clannad_app_t *app)
{
    app->page = CLAN_PAGE_GALLERY;
    app->list_sel = 0;
    app->list_top = 0;
    refresh_page(app);
}

static void open_cg(clannad_app_t *app, int group, int variant)
{
    app->gallery_group = group;
    app->gallery_variant = variant;
    app->page = CLAN_PAGE_CG;
    clannad_ui_show(&app->ui, CLAN_UI_CG);
    draw_cg(app);
}

static void open_routes(clannad_app_t *app)
{
    app->page = CLAN_PAGE_ROUTES;
    app->list_sel = 0;
    app->list_top = 0;
    refresh_page(app);
}

static void open_chapters(clannad_app_t *app, int route)
{
    app->route_sel = route;
    app->page = CLAN_PAGE_CHAPTERS;
    app->list_sel = 0;
    app->list_top = 0;
    refresh_page(app);
}

static clannad_slot_state_t slot_probe(clannad_app_t *app, int index)
{
    app->slot_ok[index] = false;
    app->slot_state[index] = CLAN_SLOT_IO_ERROR;
    if (!app->storage_ok) return CLAN_SLOT_IO_ERROR;
    char key[8];
    slot_key(index, key, sizeof(key));
    size_t packed_len = 0;
    const esp_err_t err = nvs_read_blob_ex(NVS_NS, key, s_save_packed,
                                            sizeof(s_save_packed), &packed_len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        app->slot_state[index] = CLAN_SLOT_EMPTY;
        return CLAN_SLOT_EMPTY;
    }
    if (err != ESP_OK) {
        app->slot_state[index] = CLAN_SLOT_IO_ERROR;
        return CLAN_SLOT_IO_ERROR;
    }
    size_t raw_len = 0;
    if (!save_unpack(s_save_packed, packed_len, &raw_len)) {
        app->slot_state[index] = CLAN_SLOT_CORRUPT;
        return CLAN_SLOT_CORRUPT;
    }
    clannad_save_info_t info;
    if (!clannad_vm_save_validate(&app->vm, s_save_raw, raw_len, &info)) {
        app->slot_state[index] = CLAN_SLOT_CORRUPT;
        return CLAN_SLOT_CORRUPT;
    }
    app->slot_info[index] = info;
    app->slot_ok[index] = true;
    app->slot_state[index] = CLAN_SLOT_VALID;
    return CLAN_SLOT_VALID;
}

static void open_slots(clannad_app_t *app, int action)
{
    app->slot_action = action;
    app->return_page = (app->page == CLAN_PAGE_MENU) ? CLAN_PAGE_MENU : CLAN_PAGE_TITLE;
    app->page = action ? CLAN_PAGE_SAVE : CLAN_PAGE_LOAD;
    app->list_sel = 0;
    app->list_top = 0;
    // A failed read is not an empty slot: keep EMPTY, CORRUPT and IO_ERROR
    // distinct so SAVE can never silently overwrite an existing slot.
    for (int i = 0; i < CLAN_SAVE_SLOTS; ++i) {
        if (app->storage_ok) {
            (void)slot_probe(app, i);
        } else {
            app->slot_ok[i] = false;
            app->slot_state[i] = CLAN_SLOT_IO_ERROR;
        }
    }
    refresh_page(app);
}

static void open_confirm_save(clannad_app_t *app, int slot)
{
    app->pending_slot = slot;
    app->confirm_sel = 1;   // 默认停在“取消”，覆盖要显式再确认
    app->page = CLAN_PAGE_CONFIRM;
    refresh_page(app);
}

// ---------------------------------------------------------------- list rows
static void rows_menu(clannad_app_t *app, clannad_ui_row_t rows[], int *count,
                      int top)
{
    static const char *const LABELS[] = {"返回游戏", "保存进度", "读取进度", "上一句",
                                        "历史记录", "自动播放", "设置", "返回标题"};
    const int total = (int)(sizeof(LABELS) / sizeof(LABELS[0]));
    memset(rows, 0, sizeof(clannad_ui_row_t) * CLAN_UI_LIST_ROWS);
    *count = 0;
    for (int i = 0; i < CLAN_UI_LIST_ROWS; ++i) {
        const int index = top + i;
        if (index >= total) break;
        snprintf(rows[i].main, sizeof(rows[i].main), "%s", LABELS[index]);
        if (index == 5) {
            snprintf(rows[i].value, sizeof(rows[i].value), "%s",
                     app->auto_mode ? "开" : "关");
        }
        if (index == 0) {
            snprintf(rows[i].sub, sizeof(rows[i].sub), "回到当前正文");
        }
        (*count)++;
    }
}

static void rows_settings(clannad_app_t *app, clannad_ui_row_t rows[], int *count)
{
    static const char *const SPEEDS[] = {"慢速", "标准", "快速", "瞬间"};
    static const char *const AUTOS[] = {"慢速", "标准", "快速"};
    memset(rows, 0, sizeof(clannad_ui_row_t) * CLAN_UI_LIST_ROWS);
    *count = 5;
    snprintf(rows[0].main, sizeof(rows[0].main), "文字速度");
    snprintf(rows[0].value, sizeof(rows[0].value), "%s", SPEEDS[app->settings.speed & 3]);
    snprintf(rows[1].main, sizeof(rows[1].main), "自动等待");
    snprintf(rows[1].value, sizeof(rows[1].value), "%s",
             AUTOS[app->settings.auto_speed % 3]);
    snprintf(rows[2].main, sizeof(rows[2].main), "自动播放");
    snprintf(rows[2].value, sizeof(rows[2].value), "%s", app->auto_mode ? "开" : "关");
    snprintf(rows[3].main, sizeof(rows[3].main), "操作说明");
    snprintf(rows[4].main, sizeof(rows[4].main), "返回");
    snprintf(rows[0].sub, sizeof(rows[0].sub), "字号固定 16px（完整字库）");
}

static void rows_history(clannad_app_t *app, clannad_ui_row_t rows[], int *count,
                         int top)
{
    memset(rows, 0, sizeof(clannad_ui_row_t) * CLAN_UI_LIST_ROWS);
    *count = 0;
    const int total = app->history_count;
    for (int i = 0; i < CLAN_UI_LIST_ROWS; ++i) {
        const int index = top + i;
        if (index >= total) break;
        const int slot = (app->history_head - 1 - index + CLAN_HISTORY * 2) % CLAN_HISTORY;
        snprintf(rows[i].main, sizeof(rows[i].main), "%s",
                 app->history[slot].speaker[0] ? app->history[slot].speaker : "旁白");
        copy_utf8(rows[i].sub, sizeof(rows[i].sub), app->history[slot].text,
                  strlen(app->history[slot].text));
        (*count)++;
    }
}

static void rows_gallery(clannad_app_t *app, clannad_ui_row_t rows[], int *count,
                         int top)
{
    memset(rows, 0, sizeof(clannad_ui_row_t) * CLAN_UI_LIST_ROWS);
    *count = 0;
    const int total = (int)app->pack.gallery_group_count;
    for (int i = 0; i < CLAN_UI_LIST_ROWS; ++i) {
        const int index = top + i;
        if (index >= total) break;
        clannad_gallery_group_t group;
        if (!clannad_pack_gallery_group(&app->pack, (uint16_t)index, &group)) break;
        snprintf(rows[i].main, sizeof(rows[i].main), "%s",
                 clannad_pack_gallery_label(&app->pack, (uint16_t)index));
        snprintf(rows[i].value, sizeof(rows[i].value), "%u 张", (unsigned)group.count);
        (*count)++;
    }
}

static void rows_routes(clannad_app_t *app, clannad_ui_row_t rows[], int *count,
                        int top)
{
    memset(rows, 0, sizeof(clannad_ui_row_t) * CLAN_UI_LIST_ROWS);
    *count = 0;
    const int total = (int)app->pack.route_count;
    for (int i = 0; i < CLAN_UI_LIST_ROWS; ++i) {
        const int index = top + i;
        if (index >= total) break;
        clannad_route_t route;
        if (!clannad_pack_route(&app->pack, (uint16_t)index, &route)) break;
        snprintf(rows[i].main, sizeof(rows[i].main), "%s",
                 clannad_pack_route_name(&app->pack, (uint16_t)index));
        snprintf(rows[i].value, sizeof(rows[i].value), "%u 章",
                 (unsigned)route.chapter_count);
        (*count)++;
    }
}

static int chapter_total_for_route(const clannad_app_t *app, int route)
{
    clannad_route_t info;
    if (!clannad_pack_route(&app->pack, (uint16_t)route, &info)) return 0;
    return (int)info.chapter_count;
}

static void rows_chapters(clannad_app_t *app, clannad_ui_row_t rows[], int *count,
                          int top)
{
    memset(rows, 0, sizeof(clannad_ui_row_t) * CLAN_UI_LIST_ROWS);
    *count = 0;
    clannad_route_t route;
    if (!clannad_pack_route(&app->pack, (uint16_t)app->route_sel, &route)) return;
    for (int i = 0; i < CLAN_UI_LIST_ROWS; ++i) {
        const int index = top + i;
        if (index >= (int)route.chapter_count) break;
        clannad_chapter_t chapter;
        if (!clannad_pack_chapter(&app->pack,
                                  (uint16_t)(route.first_chapter + index), &chapter)) {
            break;
        }
        snprintf(rows[i].main, sizeof(rows[i].main), "%s",
                 clannad_pack_chapter_name(&app->pack,
                                           (uint16_t)(route.first_chapter + index)));
        snprintf(rows[i].sub, sizeof(rows[i].sub), "从本章入口开始阅读");
        (*count)++;
    }
}

static void rows_about(clannad_app_t *app, clannad_ui_row_t rows[], int *count,
                       int top)
{
    (void)app;
    static const char *const LINES[] = {
        "上键：下一句 / 长按快进",
        "下键：历史 / 长按自动播放",
        "确定：菜单 / 选项确认",
        "标题：上/下选择，确定进入",
        "列表：上/下滚动，长按确定返回",
        "存档：30 个独立栏位，覆盖需确认",
        "周目记录：光玉与 After Story 独立保存",
        "本作仅含当前手表源的全部剧情、CG 与表情；",
        "标题曲等音频未包含在此单固件中。",
        "原作 © Key / Visual Arts。个人研究移植。",
    };
    memset(rows, 0, sizeof(clannad_ui_row_t) * CLAN_UI_LIST_ROWS);
    *count = 0;
    const int total = (int)(sizeof(LINES) / sizeof(LINES[0]));
    for (int i = 0; i < CLAN_UI_LIST_ROWS; ++i) {
        const int index = top + i;
        if (index >= total) break;
        snprintf(rows[i].main, sizeof(rows[i].main), "%s", LINES[index]);
        (*count)++;
    }
}

static void rows_slots(clannad_app_t *app, clannad_ui_row_t rows[], int *count,
                       int top)
{
    memset(rows, 0, sizeof(clannad_ui_row_t) * CLAN_UI_LIST_ROWS);
    *count = 0;
    for (int i = 0; i < CLAN_UI_LIST_ROWS; ++i) {
        const int index = top + i;
        if (index >= CLAN_SAVE_SLOTS) break;
        const clannad_slot_state_t state = app->slot_state[index];
        const char *main = "空栏位";
        if (state == CLAN_SLOT_VALID) main = app->slot_info[index].chapter_name;
        else if (state == CLAN_SLOT_CORRUPT) main = "存档损坏";
        else if (state == CLAN_SLOT_IO_ERROR) main = "读取失败";
        snprintf(rows[i].main, sizeof(rows[i].main), "%02d  %s", index + 1, main);
        if (state == CLAN_SLOT_VALID) {
            copy_utf8(rows[i].sub, sizeof(rows[i].sub),
                      app->slot_info[index].preview[0] ? app->slot_info[index].preview
                                                       : "章节开头",
                      strlen(app->slot_info[index].preview[0]
                                 ? app->slot_info[index].preview
                                 : "章节开头"));
            snprintf(rows[i].value, sizeof(rows[i].value), "%s",
                     app->slot_info[index].mode == CLAN_MODE_STORY ? "周目" : "章节");
        } else if (state == CLAN_SLOT_CORRUPT) {
            snprintf(rows[i].sub, sizeof(rows[i].sub), "%s", "按确定重试读取");
            snprintf(rows[i].value, sizeof(rows[i].value), "%s", "损坏");
        } else if (state == CLAN_SLOT_IO_ERROR) {
            snprintf(rows[i].sub, sizeof(rows[i].sub), "%s", "按确定重试读取");
            snprintf(rows[i].value, sizeof(rows[i].value), "%s", "不可读");
        } else {
            snprintf(rows[i].sub, sizeof(rows[i].sub), "%s",
                     app->slot_action ? "按确定保存到这里" : "按确定读取这个栏位");
            snprintf(rows[i].value, sizeof(rows[i].value), "%s", "空");
        }
        (*count)++;
    }
}

static void refresh_page(clannad_app_t *app)
{
    clannad_ui_row_t *rows = app->rows;
    int count = 0;
    char title[96];
    char hint[96];
    switch (app->page) {
    case CLAN_PAGE_TITLE: {
        const char *labels[CLAN_UI_TITLE_ROWS] = {
            "新游戏", "继续", "读取", "After Story", "CG 鉴赏", "章节 / 路线", "设置"};
        char progress[64];
        snprintf(progress, sizeof(progress), "光玉 %ld / 13", (long)app->light_count);
        clannad_ui_show(&app->ui, CLAN_UI_TITLE);
        clannad_ui_title(&app->ui, labels, CLAN_UI_TITLE_ROWS, app->title_sel,
                         progress, "上/下 · 确定");
        break;
    }
    case CLAN_PAGE_GAME:
        clannad_ui_show(&app->ui, CLAN_UI_GAME);
        (void)draw_scene_image(app);
        refresh_game(app);
        break;
    case CLAN_PAGE_CHOICE: {
        clannad_ui_show(&app->ui, CLAN_UI_CHOICE);
        (void)draw_scene_image(app);
        const char *visible[CLAN_UI_CHOICE_ROWS] = {0};
        if (app->choice_top > (int)app->choice_count - 1 && app->choice_count) {
            app->choice_top = (int)app->choice_count - 1;
        }
        for (int i = 0; i < CLAN_UI_CHOICE_ROWS; ++i) {
            const int index = app->choice_top + i;
            if (index < (int)app->choice_count) visible[i] = app->choice_text[index];
        }
        clannad_ui_choice(&app->ui, "选择下一步", visible,
                          (int)app->choice_count - app->choice_top, app->choice_sel,
                          app->choice_top,
                          "上/下选择 · 确定 · 长按确定菜单");
        break;
    }
    case CLAN_PAGE_MENU:
        rows_menu(app, rows, &count, app->list_top);
        clannad_ui_show(&app->ui, CLAN_UI_LIST);
        clannad_ui_list(&app->ui, "阅读菜单", rows, count, app->list_sel,
                        app->list_top, "上/下 · 确定 · 长按返回");
        break;
    case CLAN_PAGE_SETTINGS:
        rows_settings(app, rows, &count);
        clannad_ui_show(&app->ui, CLAN_UI_LIST);
        clannad_ui_list(&app->ui, "设置", rows, count, app->list_sel, app->list_top,
                        "上/下 · 确定切换 · 长按返回");
        break;
    case CLAN_PAGE_HISTORY:
        rows_history(app, rows, &count, app->list_top);
        clannad_ui_show(&app->ui, CLAN_UI_LIST);
        clannad_ui_list(&app->ui, "历史记录", rows, count, app->list_sel,
                        app->list_top, "上/下滚动 · 确定返回");
        break;
    case CLAN_PAGE_GALLERY:
        rows_gallery(app, rows, &count, app->list_top);
        clannad_ui_show(&app->ui, CLAN_UI_LIST);
        clannad_ui_list(&app->ui, "CG 鉴赏", rows, count, app->list_sel,
                        app->list_top, "上/下 · 确定查看 · 长按返回");
        break;
    case CLAN_PAGE_CG:
        clannad_ui_show(&app->ui, CLAN_UI_CG);
        draw_cg(app);
        break;
    case CLAN_PAGE_ROUTES:
        rows_routes(app, rows, &count, app->list_top);
        clannad_ui_show(&app->ui, CLAN_UI_LIST);
        clannad_ui_list(&app->ui, "选择路线", rows, count, app->list_sel,
                        app->list_top, "上/下 · 确定 · 长按返回");
        break;
    case CLAN_PAGE_CHAPTERS:
        rows_chapters(app, rows, &count, app->list_top);
        clannad_ui_show(&app->ui, CLAN_UI_LIST);
        clannad_ui_list(&app->ui, "选择章节", rows, count, app->list_sel,
                        app->list_top, "上/下 · 确定开始 · 长按返回");
        break;
    case CLAN_PAGE_ABOUT:
        rows_about(app, rows, &count, app->list_top);
        clannad_ui_show(&app->ui, CLAN_UI_LIST);
        clannad_ui_list(&app->ui, "操作说明", rows, count, app->list_sel,
                        app->list_top, "上/下滚动 · 长按返回");
        break;
    case CLAN_PAGE_SAVE:
    case CLAN_PAGE_LOAD:
        rows_slots(app, rows, &count, app->list_top);
        clannad_ui_show(&app->ui, CLAN_UI_LIST);
        snprintf(title, sizeof(title), "%s", app->slot_action ? "保存进度" : "读取进度");
        snprintf(hint, sizeof(hint), "%d 个栏位 · 第 %d-%d 行 · 长按返回",
                 CLAN_SAVE_SLOTS, app->list_top + 1,
                 app->list_top + (count ? count : 1));
        clannad_ui_list(&app->ui, title, rows, count, app->list_sel, app->list_top,
                        hint);
        break;
    case CLAN_PAGE_CONFIRM: {
        memset(rows, 0, sizeof(app->rows));
        snprintf(rows[0].main, sizeof(rows[0].main), "覆盖存档 %02d",
                 app->pending_slot + 1);
        const char *preview = app->pending_slot >= 0 &&
                                      app->pending_slot < CLAN_SAVE_SLOTS &&
                                      app->slot_ok[app->pending_slot]
                                  ? app->slot_info[app->pending_slot].preview
                                  : "";
        copy_utf8(rows[0].sub, sizeof(rows[0].sub), preview, strlen(preview));
        snprintf(rows[1].main, sizeof(rows[1].main), "取消");
        clannad_ui_show(&app->ui, CLAN_UI_LIST);
        clannad_ui_list(&app->ui, "覆盖存档？", rows, 2, app->confirm_sel, 0,
                        "上/下 · 确定 · 长按取消");
        break;
    }
    case CLAN_PAGE_ENDING:
        clannad_ui_show(&app->ui, CLAN_UI_LIST);
        break;
    case CLAN_PAGE_ERROR:
        clannad_ui_show(&app->ui, CLAN_UI_MESSAGE);
        break;
    default:
        break;
    }
    if (app->notice_ms) clannad_ui_notice(&app->ui, app->notice);
}

// ---------------------------------------------------------------- actions
static void show_title(clannad_app_t *app)
{
    if (app->game_active) {
        autosave_now(app);
        progress_sync(app);
        (void)progress_write(app, true);
    }
    app->game_active = false;
    app->typing = false;
    app->fast_forward = false;
    app->auto_mode = false;
    app->page = CLAN_PAGE_TITLE;
    app->title_sel = 0;
    app->cache_valid = false;
    draw_title_art(app);
    update_progress_flags(app);
    refresh_page(app);
}

static void start_story(clannad_app_t *app, uint16_t scene, bool story)
{
    if (!clannad_vm_new(&app->vm, scene, story ? CLAN_MODE_STORY : CLAN_MODE_CHAPTER)) {
        show_error(app, "剧情入口不存在");
        return;
    }
    if (app->progress_valid) {
        // The original wraps both startNew (story) and selectBrowse (chapter)
        // in withProgress(). Chapter runs still must not write progress back.
        (void)clannad_vm_progress_apply(&app->vm, app->progress, app->progress_len);
    }
    app->cache_valid = false;
    app->image_failed = false;
    app->history_count = 0;
    app->history_head = 0;
    app->snapshot_count = 0;
    app->snapshot_head = 0;
    app->page_index = 0;
    app->choice_sel = 0;
    app->choice_top = 0;
    app->list_sel = 0;
    app->list_top = 0;
    app->typing = false;
    app->auto_mode = false;
    app->fast_forward = false;
    app->last_scene = 0;
    app->auto_save_acc = 0;
    app->auto_dirty = false;
    app->auto_retry_ms = 0;
    app->game_active = true;
    run_frame(app);
    // 正文页没有独立提示行（面板 72px 已被 3 行正文占满，page_mark 仅 40px），
    // 开场用一次性提示告诉用户三键语义；图片/存档失败已有自己的提示优先。
    if (app->page == CLAN_PAGE_GAME && !app->notice_ms) {
        set_notice(app, "OK 菜单 · DOWN 历史 · UP 下一句");
    }
}

static bool save_slot(clannad_app_t *app, int index)
{
    if (!app->storage_ok) {
        set_notice(app, "存储不可用，未写入存档");
        return false;
    }
    size_t raw_len = 0;
    if (!encode_current(app, current_page_index(app), (uint16_t)app->choice_sel,
                        &raw_len)) {
        set_notice(app, "保存失败：进度编码错误");
        return false;
    }
    size_t packed_len = 0;
    if (!save_pack(s_save_raw, raw_len, &packed_len)) {
        set_notice(app, "保存失败：压缩错误");
        return false;
    }
    char key[8];
    slot_key(index, key, sizeof(key));
    if (!nvs_write_blob(NVS_NS, key, s_save_packed, packed_len)) {
        set_notice(app, "保存失败，请重试（原存档未改动）");
        return false;
    }
    clannad_save_info_t info;
    if (clannad_vm_save_validate(&app->vm, s_save_raw, raw_len, &info)) {
        app->slot_info[index] = info;
        app->slot_ok[index] = true;
        app->slot_state[index] = CLAN_SLOT_VALID;
    } else {
        app->slot_ok[index] = false;
        app->slot_state[index] = CLAN_SLOT_CORRUPT;
    }
    char msg[48];
    snprintf(msg, sizeof(msg), "已保存到存档 %02d", index + 1);
    set_notice(app, msg);
    app->page = app->return_page == CLAN_PAGE_MENU ? CLAN_PAGE_MENU : CLAN_PAGE_GAME;
    if (app->page == CLAN_PAGE_GAME && app->vm.ended) app->page = CLAN_PAGE_ENDING;
    refresh_page(app);
    return true;
}

static bool load_slot_blob(clannad_app_t *app, const uint8_t *packed, size_t packed_len,
                           const char *what)
{
    size_t raw_len = 0;
    if (!save_unpack(packed, packed_len, &raw_len)) {
        set_notice(app, "存档损坏，当前进度未改动");
        return false;
    }
    clannad_save_info_t info;
    if (!clannad_vm_save_validate(&app->vm, s_save_raw, raw_len, &info)) {
        set_notice(app, "存档校验失败，当前进度未改动");
        return false;
    }
    if (!clannad_vm_save_apply(&app->vm, s_save_raw, raw_len)) {
        set_notice(app, "存档恢复失败，当前进度未改动");
        return false;
    }
    if (app->vm.mode == CLAN_MODE_STORY && app->progress_valid) {
        // 全局通关记录优先：读旧档不回退已取得的光玉/After Story。
        (void)clannad_vm_progress_apply(&app->vm, app->progress, app->progress_len);
    }
    app->cache_valid = false;
    app->image_failed = false;
    app->history_count = 0;
    app->history_head = 0;
    app->snapshot_count = 0;
    app->snapshot_head = 0;
    app->typing = false;
    app->auto_mode = false;
    app->fast_forward = false;
    app->choice_sel = info.choice_sel;
    app->last_scene = 0;
    app->auto_dirty = false;
    app->auto_retry_ms = 0;
    app->game_active = true;
    run_frame(app);
    if (app->frame.type == CLAN_FRAME_TEXT) {
        app->page_index = info.page < (uint16_t)app->page_count ? info.page : 0;
        app->shown_bytes = app->page_offsets[app->page_index + 1];
        app->typing = false;
        refresh_page(app);
    } else if (app->frame.type == CLAN_FRAME_CHOICE) {
        app->choice_sel = info.choice_sel < app->choice_count ? info.choice_sel : 0;
        refresh_page(app);
    }
    set_notice(app, what);
    return true;
}

static bool load_slot(clannad_app_t *app, int index)
{
    if (!app->storage_ok) {
        set_notice(app, "存储不可用");
        return false;
    }
    char key[8];
    slot_key(index, key, sizeof(key));
    size_t packed_len = 0;
    const esp_err_t err = nvs_read_blob_ex(NVS_NS, key, s_save_packed,
                                            sizeof(s_save_packed), &packed_len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        app->slot_state[index] = CLAN_SLOT_EMPTY;
        app->slot_ok[index] = false;
        set_notice(app, "这个栏位还没有存档");
        return false;
    }
    if (err != ESP_OK) {
        app->slot_state[index] = CLAN_SLOT_IO_ERROR;
        app->slot_ok[index] = false;
        set_notice(app, "存档读取失败，当前进度未改动");
        return false;
    }
    return load_slot_blob(app, s_save_packed, packed_len, "已读取存档");
}

static bool load_autosave(clannad_app_t *app)
{
    if (!app->storage_ok) {
        set_notice(app, "存储不可用");
        return false;
    }
    size_t packed_len = 0;
    const esp_err_t err = nvs_read_blob_ex(NVS_AUTO_NS, KEY_AUTO, s_save_packed,
                                            sizeof(s_save_packed), &packed_len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        set_notice(app, "没有可继续的自动存档");
        return false;
    }
    if (err != ESP_OK) {
        set_notice(app, "自动存档读取失败，请重试");
        return false;
    }
    return load_slot_blob(app, s_save_packed, packed_len, "已继续上次进度");
}

static void settings_toggle(clannad_app_t *app, int index)
{
    switch (index) {
    case 0:
        app->settings.speed = (uint8_t)((app->settings.speed + 1) & 3);
        break;
    case 1:
        app->settings.auto_speed = (uint8_t)((app->settings.auto_speed + 1) % 3);
        break;
    case 2:
        app->settings.auto_enabled = app->settings.auto_enabled ? 0 : 1;
        app->auto_mode = app->settings.auto_enabled != 0;
        break;
    case 3:
        open_about(app);
        return;
    default:
        app->page = app->return_page;
        refresh_page(app);
        return;
    }
    if (app->page == CLAN_PAGE_SETTINGS) {
        (void)nvs_write_blob(NVS_NS, KEY_CFG, &app->settings, sizeof(app->settings));
        refresh_page(app);
    }
}

static void activate_list(clannad_app_t *app)
{
    const int sel = app->list_sel;
    switch (app->page) {
    case CLAN_PAGE_TITLE:
        if (app->title_sel == 0) start_story(app, (uint16_t)clannad_pack_meta_int(
                                                   &app->pack, "flow_scene", 414), true);
        else if (app->title_sel == 1) (void)load_autosave(app);
        else if (app->title_sel == 2) open_slots(app, 0);
        else if (app->title_sel == 3) {
            if (!app->after_unlocked) {
                set_notice(app, "先完成学园篇对应路线并取得光玉");
            } else {
                start_story(app, 6800, true);
            }
        } else if (app->title_sel == 4) open_gallery(app);
        else if (app->title_sel == 5) open_routes(app);
        else open_settings(app);
        break;
    case CLAN_PAGE_MENU:
        if (sel == 0) {
            app->page = app->menu_return;
            refresh_page(app);
        } else if (sel == 1) open_slots(app, 1);
        else if (sel == 2) open_slots(app, 0);
        else if (sel == 3) {
            if (snapshot_previous(app)) run_frame(app);
            else set_notice(app, "已经是第一句");
        } else if (sel == 4) open_history(app);
        else if (sel == 5) {
            app->auto_mode = !app->auto_mode;
            app->settings.auto_enabled = app->auto_mode ? 1 : 0;
            app->auto_ms = 0;
            refresh_page(app);
        } else if (sel == 6) open_settings(app);
        else if (sel == 7) show_title(app);
        break;
    case CLAN_PAGE_SAVE:
    case CLAN_PAGE_LOAD: {
        const int index = sel;
        if (index < 0 || index >= CLAN_SAVE_SLOTS) break;
        // Re-read on activation. This lets a transient IO error recover, while
        // still refusing to treat an unreadable slot as empty.
        const clannad_slot_state_t state = slot_probe(app, index);
        if (app->slot_action == 0) {
            if (state == CLAN_SLOT_VALID) {
                (void)load_slot(app, index);
            } else if (state == CLAN_SLOT_EMPTY) {
                set_notice(app, "这个栏位还没有存档");
            } else if (state == CLAN_SLOT_CORRUPT) {
                set_notice(app, "存档损坏，无法读取");
            } else {
                set_notice(app, "存档读取失败，请重试");
            }
        } else if (state == CLAN_SLOT_EMPTY) {
            (void)save_slot(app, index);
        } else if (state == CLAN_SLOT_VALID) {
            open_confirm_save(app, index);
        } else if (state == CLAN_SLOT_CORRUPT) {
            set_notice(app, "存档损坏，未覆盖；按确定重试");
        } else {
            set_notice(app, "存档不可读，保存失败；按确定重试");
        }
        refresh_page(app);
        break;
    }
    case CLAN_PAGE_SETTINGS:
        settings_toggle(app, sel);
        break;
    case CLAN_PAGE_GALLERY:
        if (sel < (int)app->pack.gallery_group_count) open_cg(app, sel, 0);
        break;
    case CLAN_PAGE_CG:
        open_gallery(app);
        break;
    case CLAN_PAGE_ROUTES:
        if (sel < (int)app->pack.route_count) open_chapters(app, sel);
        break;
    case CLAN_PAGE_CHAPTERS: {
        clannad_route_t route;
        if (!clannad_pack_route(&app->pack, (uint16_t)app->route_sel, &route)) break;
        const int index = sel;
        if (index < (int)route.chapter_count) {
            clannad_chapter_t chapter;
            if (clannad_pack_chapter(&app->pack,
                                     (uint16_t)(route.first_chapter + index),
                                     &chapter)) {
                start_story(app, chapter.scene_id, false);
            }
        }
        break;
    }
    case CLAN_PAGE_HISTORY:
        app->page = app->return_page;
        refresh_page(app);
        break;
    case CLAN_PAGE_ABOUT:
        app->page = CLAN_PAGE_SETTINGS;
        refresh_page(app);
        break;
    case CLAN_PAGE_CONFIRM:
        if (app->confirm_sel == 0) (void)save_slot(app, app->pending_slot);
        else {
            app->page = CLAN_PAGE_SAVE;
            refresh_page(app);
        }
        break;
    case CLAN_PAGE_ENDING:
        show_title(app);
        break;
    case CLAN_PAGE_ERROR:
        show_title(app);
        break;
    default:
        break;
    }
}

static int list_count(const clannad_app_t *app)
{
    switch (app->page) {
    case CLAN_PAGE_MENU: return 8;
    case CLAN_PAGE_SETTINGS: return 5;
    case CLAN_PAGE_SAVE:
    case CLAN_PAGE_LOAD: return CLAN_SAVE_SLOTS;
    case CLAN_PAGE_HISTORY: return app->history_count;
    case CLAN_PAGE_GALLERY: return (int)app->pack.gallery_group_count;
    case CLAN_PAGE_ROUTES: return (int)app->pack.route_count;
    case CLAN_PAGE_CHAPTERS: return chapter_total_for_route(app, app->route_sel);
    case CLAN_PAGE_ABOUT: return 10;
    case CLAN_PAGE_CONFIRM: return 2;
    case CLAN_PAGE_ENDING: return 2;
    default: return 0;
    }
}

static void list_move(clannad_app_t *app, int delta)
{
    if (app->page == CLAN_PAGE_TITLE) {
        app->title_sel += delta;
        if (app->title_sel < 0) app->title_sel = 0;
        if (app->title_sel >= CLAN_UI_TITLE_ROWS) app->title_sel = CLAN_UI_TITLE_ROWS - 1;
        refresh_page(app);
        return;
    }
    if (app->page == CLAN_PAGE_CHOICE) {
        int sel = app->choice_sel + delta;
        if (sel < 0) sel = 0;
        if (sel >= (int)app->choice_count) sel = (int)app->choice_count - 1;
        if (sel < app->choice_top) app->choice_top = sel;
        if (sel >= app->choice_top + CLAN_UI_CHOICE_ROWS) {
            app->choice_top = sel - CLAN_UI_CHOICE_ROWS + 1;
        }
        app->choice_sel = sel;
        refresh_page(app);
        return;
    }
    if (app->page == CLAN_PAGE_CG) {
        clannad_gallery_group_t group;
        if (!clannad_pack_gallery_group(&app->pack, (uint16_t)app->gallery_group, &group)) {
            return;
        }
        const int variant = (int)app->gallery_variant + delta;
        app->gallery_variant = group.count ? (variant + group.count) % group.count : 0;
        draw_cg(app);
        return;
    }
    if (app->page == CLAN_PAGE_CONFIRM) {
        app->confirm_sel = (app->confirm_sel + delta) & 1;
        refresh_page(app);
        return;
    }
    const int total = list_count(app);
    if (total <= 0) return;
    int sel = app->list_sel + delta;
    if (sel < 0) sel = 0;
    if (sel >= total) sel = total - 1;
    if (sel < app->list_top) app->list_top = sel;
    if (sel >= app->list_top + CLAN_UI_LIST_ROWS) {
        app->list_top = sel - CLAN_UI_LIST_ROWS + 1;
    }
    app->list_sel = sel;
    refresh_page(app);
}

static void go_back(clannad_app_t *app)
{
    switch (app->page) {
    case CLAN_PAGE_GAME:
    case CLAN_PAGE_CHOICE:
    case CLAN_PAGE_TITLE:
        return;
    case CLAN_PAGE_MENU:
        app->page = app->menu_return;
        refresh_page(app);
        return;
    case CLAN_PAGE_SAVE:
    case CLAN_PAGE_LOAD:
        app->page = app->return_page;
        refresh_page(app);
        return;
    case CLAN_PAGE_SETTINGS:
        app->page = app->return_page == CLAN_PAGE_SETTINGS ? CLAN_PAGE_TITLE
                                                           : app->return_page;
        refresh_page(app);
        return;
    case CLAN_PAGE_ABOUT:
        app->page = CLAN_PAGE_SETTINGS;
        refresh_page(app);
        return;
    case CLAN_PAGE_HISTORY:
        app->page = app->return_page;
        refresh_page(app);
        return;
    case CLAN_PAGE_GALLERY:
    case CLAN_PAGE_ROUTES:
        show_title(app);
        return;
    case CLAN_PAGE_CHAPTERS:
        app->page = CLAN_PAGE_ROUTES;
        refresh_page(app);
        return;
    case CLAN_PAGE_CG:
        open_gallery(app);
        return;
    case CLAN_PAGE_CONFIRM:
        app->page = CLAN_PAGE_SAVE;
        refresh_page(app);
        return;
    case CLAN_PAGE_ENDING:
    case CLAN_PAGE_ERROR:
        show_title(app);
        return;
    default:
        return;
    }
}

// ---------------------------------------------------------------- keys/tick
void clannad_app_key(clannad_app_t *app, const clannad_key_t *key)
{
    if (!app || !key) return;
    app->idle_ms = 0;
    const uint8_t ev = key->ev;
    const uint8_t btn = key->btn;

    if (ev == BSP_BTN_PRESS) {
        app->long_btn = 0xFF;
        if (app->auto_mode) {
            app->auto_mode = false;
            app->settings.auto_enabled = 0;
            app->auto_armed = false;
        }
        return;
    }
    if (ev == BSP_BTN_RELEASE) {
        if (btn == BSP_BTN_UP) app->fast_forward = false;
        return;
    }
    if (ev == BSP_BTN_CLICK && app->long_btn == btn) {
        app->long_btn = 0xFF;
        return;
    }
    if (ev == BSP_BTN_LONG) app->long_btn = btn;
    if (app->fast_forward && !(btn == BSP_BTN_UP && ev == BSP_BTN_LONG)) {
        app->fast_forward = false;
    }
    if (app->auto_mode && (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG)) {
        app->auto_mode = false;
        app->settings.auto_enabled = 0;
        app->auto_armed = false;
        return;
    }

    if (app->page == CLAN_PAGE_GAME) {
        if (ev == BSP_BTN_LONG) {
            if (btn == BSP_BTN_UP) {
                app->fast_forward = true;
                // 快进一开就把当前页打满（自动阅读仍逐字）。
                if (app->typing) complete_page(app);
                return;
            }
            if (btn == BSP_BTN_DOWN) {
                app->auto_mode = true;
                app->auto_armed = false;
                return;
            }
            if (btn == BSP_BTN_OK) {
                open_menu(app);
                return;
            }
            return;
        }
        if (ev != BSP_BTN_CLICK) return;
        if (btn == BSP_BTN_UP) {
            if (app->image_failed) {
                if (draw_scene_image(app)) set_notice(app, "");
                refresh_game(app);
                return;
            }
            advance_text(app);
        } else if (btn == BSP_BTN_DOWN) {
            open_history(app);
        } else if (btn == BSP_BTN_OK) {
            // 短按 OK 与上键一致：推进下一句；长按 OK 才进菜单。
            advance_text(app);
        }
        return;
    }
    if (app->page == CLAN_PAGE_CHOICE) {
        if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            open_menu(app);
            return;
        }
        if (ev == BSP_BTN_LONG) return;
        if (ev != BSP_BTN_CLICK) return;
        if (btn == BSP_BTN_UP) list_move(app, -1);
        else if (btn == BSP_BTN_DOWN) list_move(app, 1);
        else if (btn == BSP_BTN_OK) choose_current(app, (uint8_t)app->choice_sel);
        return;
    }
    if (app->page == CLAN_PAGE_CG) {
        if (ev == BSP_BTN_CLICK && btn == BSP_BTN_UP) list_move(app, 1);
        else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_DOWN) list_move(app, -1);
        else if ((ev == BSP_BTN_CLICK && btn == BSP_BTN_OK) ||
                 (ev == BSP_BTN_LONG && btn == BSP_BTN_OK)) {
            open_gallery(app);
        }
        return;
    }
    if (ev == BSP_BTN_LONG) {
        if (btn == BSP_BTN_OK) go_back(app);
        return;
    }
    if (ev != BSP_BTN_CLICK) return;
    if (btn == BSP_BTN_UP) list_move(app, -1);
    else if (btn == BSP_BTN_DOWN) list_move(app, 1);
    else if (btn == BSP_BTN_OK) activate_list(app);
}

void clannad_app_tick(clannad_app_t *app, uint32_t elapsed_ms)
{
    if (!app) return;
    if (app->auto_mode || app->fast_forward) app->idle_ms = 0;
    else app->idle_ms += elapsed_ms;
    if (app->auto_dirty) {
        app->auto_retry_ms += elapsed_ms;
        if (app->auto_retry_ms >= 2000u) {
            app->auto_retry_ms = 0;
            autosave_now(app);
        }
    }
    app->notice_ms = app->notice_ms > elapsed_ms ? app->notice_ms - elapsed_ms : 0;
    if (!app->notice_ms && app->notice[0]) {
        app->notice[0] = 0;
        clannad_ui_notice(&app->ui, "");
    }
    app->progress_ms += elapsed_ms;
    if (app->progress_dirty) (void)progress_write(app, false);

    if (app->page == CLAN_PAGE_GAME && !app->typing) {
        // 自动播放遇选项/菜单停止（page 变化时自动停止）。
        if (app->auto_mode) {
            app->auto_ms += elapsed_ms;
            if (app->auto_ms >= auto_wait_ms(app)) {
                app->auto_ms = 0;
                advance_text(app);
            }
        }
    }
    if (app->page == CLAN_PAGE_GAME && app->typing) {
        const int interval = typewriter_ms(app);
        if (interval == 0) {
            complete_page(app);
        } else {
            app->type_ms += elapsed_ms;
            const uint16_t end = app->page_offsets[app->page_index + 1];
            while (app->type_ms >= (uint32_t)interval && app->typing) {
                app->type_ms -= (uint32_t)interval;
                if (app->shown_bytes >= end) {
                    complete_page(app);
                    break;
                }
                app->shown_bytes = clannad_utf8_next(app->text, app->text_len,
                                                     app->shown_bytes);
                if (app->shown_bytes > end) app->shown_bytes = end;
                refresh_game(app);
            }
        }
    }
    if (app->fast_forward && app->page == CLAN_PAGE_GAME) {
        app->ff_ms += elapsed_ms;
        if (app->ff_ms >= 60) {
            app->ff_ms = 0;
            if (app->typing) complete_page(app);
            else advance_text(app);
        }
    }
}

uint32_t clannad_app_idle_ms(const clannad_app_t *app)
{
    if (app && (app->auto_mode || app->fast_forward)) return 0;   // 自动/快进不休眠
    return app ? app->idle_ms : 0;
}

void clannad_app_flush(clannad_app_t *app)
{
    if (!app || !app->game_active) return;
    progress_sync(app);
    (void)progress_write(app, true);
    autosave_now(app);
}

void clannad_app_show_sleeping(clannad_app_t *app)
{
    if (!app) return;
    clannad_ui_show(&app->ui, CLAN_UI_MESSAGE);
    clannad_ui_message(&app->ui, "休息一下", "按任意键唤醒后回到标题。", "");
}

// ---------------------------------------------------------------- init
static void load_settings(clannad_app_t *app)
{
    app->settings.speed = 1;
    app->settings.auto_speed = 1;
    app->settings.auto_enabled = 0;
    uint8_t raw[3] = {0};
    size_t len = 0;
    if (nvs_read_blob(NVS_NS, KEY_CFG, raw, sizeof(raw), &len) && len == 3) {
        if (raw[0] <= 3) app->settings.speed = raw[0];
        if (raw[1] <= 2) app->settings.auto_speed = raw[1];
    }
    app->auto_mode = false;
}

bool clannad_app_init(clannad_app_t *app, const uint8_t *pack_data, uint32_t pack_size,
                      char *why, size_t cap)
{
    if (!app || !pack_data) return false;
    memset(app, 0, sizeof(*app));
    if (!clannad_vm_init(&app->vm, pack_data, pack_size, app->raw, sizeof(app->raw),
                         why, cap)) {
        return false;
    }
    app->pack = app->vm.pack;
    if (!clannad_ui_create(&app->ui, lv_screen_active(), app->canvas,
                           &clannad_cjk_16)) {
        if (why && cap) snprintf(why, cap, "UI create failed");
        return false;
    }
    app->storage_ok = true;
    load_settings(app);
    progress_load(app);
    app->cache_valid = false;
    app->cache_bg = 0xFFFF;
    app->last_scene = 0;
    app->page = CLAN_PAGE_TITLE;
    draw_title_art(app);
    refresh_page(app);
    ESP_LOGI(TAG, "CLANNAD pack %u bytes, scenes %u, assets %u",
             (unsigned)pack_size, app->pack.scene_count, app->pack.asset_count);
    return true;
}

int clannad_app_ui_rows(const clannad_app_t *app, char out[][96], int max)
{
    if (!app || !out || max <= 0) return 0;
    int n = 0;
    if (app->page == CLAN_PAGE_TITLE) {
        for (int i = 0; i < CLAN_UI_TITLE_ROWS && n < max; ++i) {
            snprintf(out[n++], 96, "%s", app->ui.title_main[i]);
        }
    } else if (app->page == CLAN_PAGE_CHOICE) {
        for (int i = 0; i < CLAN_UI_CHOICE_ROWS && n < max; ++i) {
            if (i >= (int)app->choice_count) break;
            copy_utf8(out[n++], 96, app->choice_text[i], strlen(app->choice_text[i]));
        }
    } else {
        for (int i = 0; i < CLAN_UI_LIST_ROWS && n < max; ++i) {
            snprintf(out[n++], 96, "%s", clannad_ui_row_main(&app->ui, i));
        }
    }
    return n;
}
