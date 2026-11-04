// main/steins0_app.c —— 应用状态机与 NVS 持久化。
#include "steins0_app.h"

#include "bsp_battery.h"
#include "bsp_button.h"

#include "esp_log.h"
#include "nvs.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "steins0";

LV_FONT_DECLARE(steins0_font16);

#define NVS_NS "steins0"
#define KEY_CFG "cfg"
#define KEY_UNLOCK "unlock"
#define KEY_AUTO "auto"
#define NVS_SLOT_FMT "s%02d"
#define AUTOSAVE_MS 6000u
#define UNLOCK_RETRY_MS 5000u
#define NOTICE_MS 2200u
#define TYPE_MS_SLOW 55u
#define TYPE_MS_NORMAL 28u
#define TYPE_MS_FAST 12u
#define FF_MS 55u
#define AUTO_MS_SLOW 1400u
#define AUTO_MS_NORMAL 900u
#define AUTO_MS_FAST 520u

static void set_notice(steins0_app_t *app, const char *text);
static sg0_slot_state_t read_slot_blob(const char *key, sg0_save_t *save);
static sg0_slot_state_t read_auto_blob(sg0_save_t *save);
static void slot_save_request(steins0_app_t *app, int index);
static void phone_open(steins0_app_t *app);
static void phone_back(steins0_app_t *app);
static void phone_move(steins0_app_t *app, int delta);
static void phone_activate(steins0_app_t *app);
static void phone_thread_render(steins0_app_t *app);
static void phone_thread_reply(steins0_app_t *app);
static void refresh_title(steins0_app_t *app);
static void refresh_game_ui(steins0_app_t *app);
static void build_list(steins0_app_t *app);
static void list_back(steins0_app_t *app);
static bool load_op(steins0_app_t *app, uint8_t restore_page);
static bool goto_page(steins0_app_t *app, steins0_page_t page);

// ---------------------------------------------------------------- NVS helpers
static bool nvs_write_blob(const char *ns, const char *key, const void *data, size_t len)
{
    nvs_handle_t handle;
    if (nvs_open(ns, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(handle, key, data, len);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

static bool nvs_read_blob(const char *ns, const char *key, void *data, size_t len)
{
    nvs_handle_t handle;
    if (nvs_open(ns, NVS_READONLY, &handle) != ESP_OK) return false;
    size_t actual = len;
    const esp_err_t err = nvs_get_blob(handle, key, data, &actual);
    nvs_close(handle);
    return err == ESP_OK && actual == len;
}

static sg0_slot_state_t read_slot_status(const char *key, sg0_save_t *save)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NS, NVS_READONLY, &handle) != ESP_OK) return SG0_SLOT_IO;
    size_t actual = 0;
    esp_err_t err = nvs_get_blob(handle, key, NULL, &actual);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return SG0_SLOT_EMPTY;
    }
    if (err != ESP_OK || actual == 0 || actual > SG0_SAVE_BYTES) {
        nvs_close(handle);
        return err == ESP_OK ? SG0_SLOT_CORRUPT : SG0_SLOT_IO;
    }
    uint8_t raw[SG0_SAVE_BYTES];
    size_t len = actual;
    err = nvs_get_blob(handle, key, raw, &len);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return SG0_SLOT_EMPTY;
    if (err != ESP_OK || len != actual) return SG0_SLOT_IO;
    return sg0_save_decode(save, raw, len) ? SG0_SLOT_VALID : SG0_SLOT_CORRUPT;
}

static bool nvs_probe(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NS, NVS_READWRITE, &handle) != ESP_OK) return false;
    nvs_close(handle);
    return true;
}

// ------------------------------------------------------------------ settings
static void load_settings(steins0_app_t *app)
{
    app->settings.speed = 1;
    app->settings.auto_wait = 1;
    uint8_t cfg[4];
    if (nvs_read_blob(NVS_NS, KEY_CFG, cfg, sizeof(cfg)) && cfg[0] == 1 &&
        cfg[1] <= 3 && cfg[2] <= 2) {
        app->settings.speed = cfg[1];
        app->settings.auto_wait = cfg[2];
    }
}

static void save_settings(steins0_app_t *app)
{
    uint8_t cfg[4] = {1, app->settings.speed, app->settings.auto_wait, 0};
    if (!app->nvs_ok) return;
    (void)nvs_write_blob(NVS_NS, KEY_CFG, cfg, sizeof(cfg));
}

// -------------------------------------------------------------------- unlock
static void load_unlock(steins0_app_t *app)
{
    uint8_t raw[SG0_UNLOCK_BYTES];
    sg0_unlock_init(&app->unlock);
    if (nvs_read_blob(NVS_NS, KEY_UNLOCK, raw, sizeof(raw)) &&
        sg0_unlock_decode(&app->unlock, raw, sizeof(raw))) {
        return;
    }
    sg0_unlock_set_chapter(&app->unlock, 0);   // 章节选择至少有一个入口
    app->unlock_dirty = true;
}

static bool write_unlock(steins0_app_t *app)
{
    if (!app->nvs_ok) {
        app->unlock_dirty = true;
        return false;
    }
    uint8_t raw[SG0_UNLOCK_BYTES];
    if (sg0_unlock_encode(&app->unlock, raw, sizeof(raw)) != SG0_UNLOCK_BYTES) return false;
    if (!nvs_write_blob(NVS_NS, KEY_UNLOCK, raw, sizeof(raw))) {
        app->unlock_dirty = true;
        return false;
    }
    app->unlock_dirty = false;
    app->unlock_retry = 0;
    return true;
}

// --------------------------------------------------------------------- slots
static void slot_key(int index, char *out, size_t cap)
{
    snprintf(out, cap, NVS_SLOT_FMT, index);
}

static void load_slots(steins0_app_t *app)
{
    for (int i = 0; i < SG0_SLOT_COUNT; ++i) {
        char key[16];
        slot_key(i, key, sizeof(key));
        sg0_save_t save;
        const sg0_slot_state_t state = read_slot_status(key, &save);
        app->slot_state[i] = (uint8_t)state;
        app->slot_valid[i] = state == SG0_SLOT_VALID;
        if (state == SG0_SLOT_VALID) {
            sg0_save_meta_from(&save, &app->slots[i]);
            if (!sg0_engine_cursor_valid(&app->pack, save.meta.node, save.meta.cursor)) {
                app->slot_valid[i] = false;
                app->slot_state[i] = SG0_SLOT_CORRUPT;
            }
        }
    }
}

static void load_auto(steins0_app_t *app)
{
    sg0_save_t save;
    const sg0_slot_state_t state = read_slot_status(KEY_AUTO, &save);
    app->auto_state = (uint8_t)state;
    app->auto_valid = state == SG0_SLOT_VALID;
    if (state == SG0_SLOT_VALID) {
        sg0_save_meta_from(&save, &app->autosave);
        if (!sg0_engine_cursor_valid(&app->pack, save.meta.node, save.meta.cursor)) {
            app->auto_valid = false;
            app->auto_state = SG0_SLOT_CORRUPT;
        }
    }
}

static void capture_save(steins0_app_t *app, sg0_save_t *save, const char *preview)
{
    memset(save, 0, sizeof(*save));
    save->meta.node = app->state.node;
    save->meta.cursor = app->state.cursor;
    save->meta.page = app->page_index;
    save->meta.mode = app->state.mode;
    save->meta.endings = app->state.endings & 0x3Fu;
    save->meta.flags = app->state.flags;
    memcpy(save->meta.chapters, app->unlock.chapters, SG0_CHAPTER_BYTES);
    memcpy(save->meta.gallery, app->unlock.gallery, SG0_GALLERY_BYTES);
    uint8_t group = 0;
    (void)sg0_engine_prev_group(&app->pack, &app->state, &group);
    save->meta.group = group;
    save->meta.date = sg0_pack_date(&app->pack, app->frame.date);
    const char *src = preview ? preview : app->op_text;
    size_t len = strlen(src);
    if (len >= SG0_SAVE_PREVIEW) {
        size_t cut = SG0_SAVE_PREVIEW - 1;
        while (cut > 0 && (((uint8_t)src[cut]) & 0xC0u) == 0x80u) --cut;
        len = cut;
    }
    memcpy(save->meta.preview, src, len);
    save->meta.preview[len] = 0;
    save->meta.preview_len = (uint8_t)len;
    for (uint16_t i = 0; i < app->state.rine.count && i < SG0_RINE_MAX; ++i) {
        const uint16_t ref = app->state.rine.refs[i];
        sg0_save_rine_set(save, ref, app->state.rine.read[i] != 0);
    }
    save->rine_replied = app->state.rine.replied;
    save->rine_windows = app->state.rine.windows;
    save->pending_valid = app->state.rine.pending_valid;
    save->pending_node = app->state.rine.pending_node;
    save->last_card = app->last_card;
    save->event_valid = 1;
    save->event_node = app->state.node;
    uint16_t event_ordinal = 0;
    if (app->state.event_node == app->state.node) {
        event_ordinal = app->state.event_cursor;
    } else if (!sg0_engine_event_ordinal(&app->pack, app->state.node,
                                        app->state.cursor, &event_ordinal)) {
        event_ordinal = 0;
    }
    save->event_cursor = event_ordinal;
}

static bool write_slot(steins0_app_t *app, int index, const sg0_save_t *save)
{
    if (!app->nvs_ok) return false;
    uint8_t raw[SG0_SAVE_BYTES];
    if (sg0_save_encode(save, raw, sizeof(raw)) != SG0_SAVE_BYTES) return false;
    char key[16];
    slot_key(index, key, sizeof(key));
    return nvs_write_blob(NVS_NS, key, raw, sizeof(raw));
}

static void write_auto(steins0_app_t *app, const sg0_save_t *save)
{
    if (!app->nvs_ok) return;
    uint8_t raw[SG0_SAVE_BYTES];
    if (sg0_save_encode(save, raw, sizeof(raw)) != SG0_SAVE_BYTES) return;
    if (nvs_write_blob(NVS_NS, KEY_AUTO, raw, sizeof(raw))) {
        sg0_save_meta_from(save, &app->autosave);
        app->auto_valid = true;
        app->auto_state = SG0_SLOT_VALID;
    }
}

static sg0_slot_state_t read_slot_blob(const char *key, sg0_save_t *save)
{
    return read_slot_status(key, save);
}

static sg0_slot_state_t read_auto_blob(sg0_save_t *save)
{
    return read_slot_status(KEY_AUTO, save);
}

static void maybe_autosave(steins0_app_t *app, bool force)
{
    if (!app->game_started || app->state.ended || !app->pack_ok || !app->nvs_ok) return;
    if (!force && app->autosave_accum < AUTOSAVE_MS) return;
    app->autosave_accum = 0;
    sg0_save_t save;
    capture_save(app, &save, NULL);
    write_auto(app, &save);
}

// -------------------------------------------------------------------- utility
static void format_date(uint32_t date, char *out, size_t cap)
{
    const uint32_t month = (date / 100u) % 100u;
    const uint32_t day = date % 100u;
    if (month >= 1 && month <= 12 && day >= 1 && day <= 31) {
        snprintf(out, cap, "%02u.%02u", (unsigned)month, (unsigned)day);
    } else {
        snprintf(out, cap, "??.??");
    }
}

static void chapter_line(steins0_app_t *app, char *out, size_t cap)
{
    uint8_t group = 0;
    (void)sg0_engine_prev_group(&app->pack, &app->state, &group);
    const char *title = NULL;
    uint16_t title_len = 0;
    char date[12];
    format_date(app->pack_ok ? sg0_pack_date(&app->pack, app->frame.date) : 0, date,
                sizeof(date));
    if (!sg0_pack_title(&app->pack, group, &title, &title_len)) {
        snprintf(out, cap, "%s", date);
        return;
    }
    size_t n = title_len > 40 ? 40 : title_len;
    while (n > 0 && (((uint8_t)title[n]) & 0xC0u) == 0x80u) --n;
    memcpy(out, title, n);
    out[n] = '\0';
    // 日期接在标题后(标题是 PUA,日期是 ASCII)。
    const size_t used = strlen(out);
    snprintf(out + used, cap > used ? cap - used : 0, "  %s", date);
}

static void push_history(steins0_app_t *app, const char *speaker, const char *text)
{
    sg0_history_t *entry = &app->history[app->history_next];
    snprintf(entry->speaker, sizeof(entry->speaker), "%s", speaker ? speaker : "");
    snprintf(entry->text, sizeof(entry->text), "%s", text ? text : "");
    app->history_next = (app->history_next + 1) % SG0_HISTORY_COUNT;
    if (app->history_count < SG0_HISTORY_COUNT) ++app->history_count;
}

static void build_history_text(steins0_app_t *app)
{
    app->history_buf[0] = '\0';
    size_t used = 0;
    const int start = (app->history_next - app->history_count + SG0_HISTORY_COUNT) %
                      SG0_HISTORY_COUNT;
    for (int i = 0; i < app->history_count; ++i) {
        const sg0_history_t *entry = &app->history[(start + i) % SG0_HISTORY_COUNT];
        char piece[SG0_HISTORY_TEXT + 32];
        if (entry->speaker[0]) {
            snprintf(piece, sizeof(piece), "%s\n%s\n\n", entry->speaker, entry->text);
        } else {
            snprintf(piece, sizeof(piece), "%s\n\n", entry->text);
        }
        const size_t n = strlen(piece);
        if (used + n + 1 >= sizeof(app->history_buf)) break;
        memcpy(app->history_buf + used, piece, n);
        used += n;
        app->history_buf[used] = '\0';
    }
    if (!app->history_count) snprintf(app->history_buf, sizeof(app->history_buf), "还没有历史记录");
}

// ---------------------------------------------------------------- 解锁扫描
static void unlock_at(steins0_app_t *app)
{
    // 章节入口:当前节点内已经到达的 start。
    for (uint16_t i = 0; i < SG0_MAX_STARTS; ++i) {
        sg0_start_t st;
        if (!sg0_pack_start(&app->pack, i, &st)) break;
        if (st.node == app->state.node && st.index <= app->state.cursor) {
            if (!sg0_unlock_has_chapter(&app->unlock, i)) {
                sg0_unlock_set_chapter(&app->unlock, i);
                app->unlock_dirty = true;
            }
        }
    }
    // CG:当前 view 里出现的鉴赏资产。
    sg0_view_t view;
    if (!sg0_pack_view(&app->pack, app->view, &view)) return;
    for (uint8_t l = 0; l < view.layer_count; ++l) {
        sg0_layer_t layer;
        if (!sg0_pack_layer(&app->pack, (uint16_t)(view.layer_first + l), &layer)) continue;
        for (uint8_t g = 0; g < sg0_pack_gallery_count(&app->pack) && g < SG0_GALLERY_BITS; ++g) {
            sg0_gallery_group_t group;
            if (!sg0_pack_gallery_group(&app->pack, g, &group)) continue;
            for (uint8_t k = 0; k < group.count; ++k) {
                uint16_t asset = 0;
                if (!sg0_pack_gallery_entry(&app->pack, (uint16_t)(group.first + k), &asset)) {
                    break;
                }
                if (asset == layer.asset) {
                    if (!sg0_unlock_has_gallery(&app->unlock, g)) {
                        sg0_unlock_set_gallery(&app->unlock, g);
                        app->unlock_dirty = true;
                    }
                }
            }
        }
    }
}

// ------------------------------------------------------------------ rendering
static void truncate_utf8(char *dst, size_t cap, const char *src, size_t len)
{
    if (len >= cap) {
        size_t cut = cap - 1;
        while (cut > 0 && (((uint8_t)src[cut]) & 0xC0u) == 0x80u) --cut;
        len = cut;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void render_page(steins0_app_t *app)
{
    const uint8_t page = app->page_index < app->pages.pages ? app->page_index : 0;
    const uint16_t page_end = app->pages.page_start[page + 1];
    const uint32_t shown = app->typing ? app->type_pos : page_end;
    app->line_count = app->pages.line_count[page];
    for (int i = 0; i < 3; ++i) app->line_buf[i][0] = '\0';
    for (int i = 0; i < app->line_count && i < 3; ++i) {
        uint16_t start = app->pages.line_start[page][i];
        uint16_t end = app->pages.line_start[page][i + 1];
        if (start > page_end) start = page_end;
        if (end > page_end) end = page_end;
        while (start < end && app->op_text[start] == ' ') ++start;
        uint16_t visible = end;
        if (shown <= start) visible = start;
        else if (shown < end) {
            visible = (uint16_t)shown;
            while (visible > start && (((uint8_t)app->op_text[visible]) & 0xC0u) == 0x80u) {
                --visible;
            }
        }
        while (visible > start && app->op_text[visible - 1] == ' ') --visible;
        // visible 可能小于 start（行起点非单调 / end 被夹到 page_end），无符号相减会变成巨大长度，
        // 进而 memcpy 越界把 line_buf 写坏 → LVGL 在 set_text_internal 里死循环。
        if (visible < start) visible = start;
        size_t len = (size_t)(visible - start);
        if (len >= sizeof(app->line_buf[i])) len = sizeof(app->line_buf[i]) - 1;
        truncate_utf8(app->line_buf[i], sizeof(app->line_buf[i]), app->op_text + start, len);
        app->line_buf[i][len] = 0;   // 显式结尾
    }
    const char *lines[3] = {app->line_buf[0], app->line_buf[1], app->line_buf[2]};
    // 注意：包内字符串是“指针+长度”，不保证 NUL 结尾。直接交给 lv_label_set_text 会让
    // copy_text_to_label 一路往后扫 Flash，卡死在 LVGL 内部（真机 WDT 的根因）。
    char speaker_buf[64] = "";
    const char *speaker = speaker_buf;
    {
        const char *raw = "";
        uint16_t slen = 0;
        if (sg0_pack_speaker(&app->pack, app->frame.speaker, &raw, &slen) && raw) {
            truncate_utf8(speaker_buf, sizeof(speaker_buf), raw, slen);
        }
    }
    steins0_ui_game_text(&app->ui, speaker, lines, app->line_count);
    steins0_ui_game_page(&app->ui, page, app->pages.pages);
    steins0_ui_battery(&app->ui, app->battery);
    steins0_ui_choice_hide(&app->ui);
    app->ui.choice_selected = app->choice_sel;
}

static void show_choice(steins0_app_t *app)
{
    sg0_node_t node;
    sg0_choice_t choice;
    if (!sg0_pack_node(&app->pack, app->state.node, &node) || node.choice == SG0_NONE ||
        !sg0_pack_choice(&app->pack, node.choice, &choice)) {
        set_notice(app, "选项数据损坏");
        return;
    }
    const char *heading = "";
    uint16_t hlen = 0;
    (void)sg0_pack_string(&app->pack, choice.heading, &heading, &hlen);
    for (int i = 0; i < choice.count && i < 2; ++i) {
        const char *text = "";
        uint16_t len = 0;
        (void)sg0_pack_string(&app->pack, choice.text[i], &text, &len);
        truncate_utf8(app->choice_text[i], sizeof(app->choice_text[i]), text, len);
        app->choice_ptrs[i] = app->choice_text[i];
    }
    if (app->choice_sel >= choice.count) app->choice_sel = 0;
    app->typing = false;
    app->fast_forward = false;
    app->auto_mode = false;
    steins0_ui_choice(&app->ui, heading, app->choice_ptrs, choice.count, app->choice_sel);
}

static void show_ending(steins0_app_t *app)
{
    const uint8_t id = app->state.ending_id;
    const char *name = "";
    uint16_t len = 0;
    sg0_node_t node;
    if (sg0_pack_node(&app->pack, app->state.node, &node) && node.ending >= 0) {
        sg0_ending_t ending;
        if (sg0_pack_ending(&app->pack, (uint16_t)node.ending, &ending)) {
            (void)sg0_pack_string(&app->pack, ending.name, &name, &len);
        }
    }
    char body[96];
    truncate_utf8(body, sizeof(body), name, len);
    sg0_unlock_set_ending(&app->unlock, id);
    if (id == 5) sg0_unlock_set_ending(&app->unlock, 3);
    if (id == 0) sg0_unlock_set_flag(&app->unlock, SG0_FLAG_PR_COMPLETE);
    app->state.endings |= app->unlock.endings & 0x3Fu;
    app->state.flags |= app->unlock.flags;
    app->unlock_dirty = true;
    (void)write_unlock(app);
    char hint[64];
    snprintf(hint, sizeof(hint), "已记录通关记录 - 短按 OK 返回标题");
    steins0_ui_ending(&app->ui, "ENDING", body, hint);
    goto_page(app, SG0_PAGE_ENDING);
}

static bool goto_page(steins0_app_t *app, steins0_page_t page)
{
    if (!app) return false;
    if (!steins0_ui_show(&app->ui, page)) {
        ESP_LOGW(TAG, "goto_page fail page=%u target=%u", (unsigned)app->page,
                 (unsigned)page);
        /* The UI keeps the old page and shows the existing allocation notice. */
        return false;
    }
    app->page = page;
    return true;
}

static void refresh_game_ui(steins0_app_t *app)
{
    // 未开始的正文页是空白屏(短 OK/UP 全无反应):一律改回标题页。
    if (!app->game_started) {
        refresh_title(app);
        return;
    }
    chapter_line(app, app->chapter_line, sizeof(app->chapter_line));
    steins0_ui_game(&app->ui, app->chapter_line, app->battery);
    {
        char rine[24] = "";
        const uint16_t unread = sg0_engine_unread(&app->pack, &app->state);
        if (app->state.rine.pending_valid) snprintf(rine, sizeof(rine), "来电");
        else if (unread) snprintf(rine, sizeof(rine), "RINE %u", (unsigned)unread);
        steins0_ui_game_rine(&app->ui, rine);
    }
    if (app->state.choice_active) {
        show_choice(app);
    } else {
        render_page(app);
    }
}

static bool load_op(steins0_app_t *app, uint8_t restore_page)
{
    app->scene_ok = true;
    sg0_choice_t choice;
    const sg0_place_t place = sg0_engine_locate(&app->pack, &app->state, &choice);
    if (place == SG0_PLACE_CHOICE) {
        // 保留上一帧画面
        chapter_line(app, app->chapter_line, sizeof(app->chapter_line));
        steins0_ui_game(&app->ui, app->chapter_line, app->battery);
        app->state.choice_active = 1;
        if (app->choice_sel >= choice.count) app->choice_sel = 0;
        show_choice(app);
        goto_page(app, SG0_PAGE_GAME);
        maybe_autosave(app, true);
        return true;
    }
    if (place == SG0_PLACE_END) {
        show_ending(app);
        return true;
    }
    if (place != SG0_PLACE_TEXT) {
        set_notice(app, "剧情连接损坏");
        return false;
    }
    if (!sg0_engine_read(&app->pack, &app->state, app->chunk_buf, sizeof(app->chunk_buf),
                         &app->chunk, &app->chunk_raw_len, &app->frame, &app->raw_text,
                         &app->raw_text_len)) {
        set_notice(app, "正文读取失败");
        return false;
    }
    const uint16_t new_view = app->frame.view;
    if (new_view != app->last_view || app->force_scene) {
        if (!steins0_image_show_scene(&app->ui.image, &app->pack, new_view, true)) {
            app->scene_ok = false;
            set_notice(app, "画面读取失败，请重试");
            return false;
        }
        app->last_view = new_view;
        app->force_scene = false;
    }
    app->view = new_view;
    const size_t clean_len = sg0_text_clean(app->raw_text, app->raw_text_len, app->op_text,
                                            sizeof(app->op_text));
    (void)clean_len;
    if (!sg0_text_layout(app->op_text, 304u * 16u, SG0_TEXT_LINES, &app->pages) ||
        app->pages.pages == 0) {
        set_notice(app, "正文排版失败");
        return false;
    }
    app->page_index = restore_page < app->pages.pages ? restore_page : 0;
    app->type_pos = app->pages.page_start[app->page_index];
    app->type_accum = 0;
    app->typing = app->settings.speed < 3;
    const bool new_op = app->last_history_node != app->state.node ||
                        app->last_history_cursor != app->state.cursor;
    if (new_op) {
        const char *speaker = "";
        uint16_t slen = 0;
        (void)sg0_pack_speaker(&app->pack, app->frame.speaker, &speaker, &slen);
        push_history(app, speaker, app->op_text);
        app->last_history_node = app->state.node;
        app->last_history_cursor = app->state.cursor;
    }
    unlock_at(app);
    chapter_line(app, app->chapter_line, sizeof(app->chapter_line));
    steins0_ui_game(&app->ui, app->chapter_line, app->battery);
    {
        char rine[24] = "";
        const uint16_t unread = sg0_engine_unread(&app->pack, &app->state);
        if (app->state.rine.pending_valid) snprintf(rine, sizeof(rine), "来电");
        else if (unread) snprintf(rine, sizeof(rine), "RINE %u", (unsigned)unread);
        steins0_ui_game_rine(&app->ui, rine);
    }
    uint8_t group = 0;
    (void)sg0_engine_prev_group(&app->pack, &app->state, &group);
    if (group < 14 && group != app->last_card) {
        app->last_card = group;
        uint16_t card_asset = 0xFFFFu;
        if (sg0_pack_card(&app->pack, group, &card_asset)) {
            (void)steins0_image_show_card(&app->ui.image, &app->pack, card_asset, true);
        }
        const char *title = "";
        uint16_t title_len = 0;
        (void)sg0_pack_title(&app->pack, group, &title, &title_len);
        char buf[64];
        size_t n = title_len < sizeof(buf) - 1 ? title_len : sizeof(buf) - 1;
        while (n > 0 && (((uint8_t)title[n]) & 0xC0u) == 0x80u) --n;
        memcpy(buf, title, n);
        buf[n] = 0;
        steins0_ui_card(&app->ui, buf);
        app->card_pending = 1;
        app->card_ms = 3500;
        app->fast_forward = false;
        // 章节卡不是选择;保留 auto,由 card 计时自动关闭后继续阅读。
        goto_page(app, SG0_PAGE_CARD);
        return true;
    }
    render_page(app);
    goto_page(app, SG0_PAGE_GAME);
    maybe_autosave(app, false);
    return true;
}

// ------------------------------------------------------------------ 标题/列表
static void refresh_title(steins0_app_t *app)
{
    app->game_started = false;
    app->auto_mode = false;
    app->fast_forward = false;
    static const char *const labels[6] = {
        "开始游戏", "继续游戏", "读取存档", "章节选择", "鉴赏室", "设置",
    };
    for (int i = 0; i < 6; ++i) {
        snprintf(app->title_text[i], sizeof(app->title_text[i]), "%s", labels[i]);
        app->title_ptrs[i] = app->title_text[i];
    }
    app->title_sel = app->title_sel < 0 ? 0 : (app->title_sel > 5 ? 5 : app->title_sel);
    steins0_ui_title(&app->ui, app->title_ptrs, 6, app->title_sel);
    steins0_ui_title_battery(&app->ui, app->battery);
    if (app->pack_ok && app->title_art_ok) {
        (void)steins0_image_show_title(&app->ui.image, &app->pack,
                                       (uint16_t)sg0_pack_meta_u16(&app->pack, "title_asset",
                                                                   0xFFFFu),
                                       true);
    } else {
        steins0_image_clear(&app->ui.image);
    }
    goto_page(app, SG0_PAGE_TITLE);
}

static int list_total(const steins0_app_t *app)
{
    switch (app->list_kind) {
    case SG0_LIST_MENU: return 8;
    case SG0_LIST_SLOTS_SAVE:
    case SG0_LIST_SLOTS_LOAD: return SG0_SLOT_COUNT;
    case SG0_LIST_SETTINGS: return 5;
    case SG0_LIST_ROUTES: return 14;
    case SG0_LIST_STARTS: {
        int count = 0;
        for (uint16_t i = 0; i < SG0_MAX_STARTS; ++i) {
            sg0_start_t st;
            if (!sg0_pack_start(&app->pack, i, &st)) break;
            if (st.group == app->route_group) ++count;
        }
        return count;
    }
    case SG0_LIST_GALLERY: return (int)sg0_pack_gallery_count(&app->pack);
    case SG0_LIST_CLEAR: return 6;
    default: return 0;
    }
}

static int start_at_group(const steins0_app_t *app, int index)
{
    int seen = 0;
    for (uint8_t i = 0; i < app->start_order_count; ++i) {
        const uint16_t pack_index = app->start_order[i];
        sg0_start_t st;
        if (!sg0_pack_start(&app->pack, pack_index, &st)) continue;
        if (st.group != app->route_group) continue;
        if (seen == index) return (int)pack_index;
        ++seen;
    }
    return -1;
}

static void build_list(steins0_app_t *app)
{
    const int total = list_total(app);
    if (total <= 0) {
        app->list_sel = 0;
        app->list_win = 0;
        steins0_ui_list(&app->ui, "空", app->row_ptrs, app->value_ptrs, 0, 0,
                        "长按 OK 返回");
        return;
    }
    if (app->list_sel < 0) app->list_sel = 0;
    if (app->list_sel >= total) app->list_sel = total - 1;
    if (app->list_sel < app->list_win) app->list_win = app->list_sel;
    if (app->list_sel >= app->list_win + SG0_LIST_ROWS) app->list_win = app->list_sel - SG0_LIST_ROWS + 1;
    if (app->list_win < 0) app->list_win = 0;
    char title[64] = "";
    char footer[96] = "上/下 选择 / OK 确认 / 长按 OK 返回";
    int shown = total - app->list_win;
    if (shown > SG0_LIST_ROWS) shown = SG0_LIST_ROWS;
    for (int i = 0; i < shown; ++i) {
        const int index = app->list_win + i;
        app->row_text[i][0] = '\0';
        app->row_value[i][0] = '\0';
        switch (app->list_kind) {
        case SG0_LIST_MENU: {
            static const char *const labels[8] = {"继续阅读", "手机", "保存进度", "读取进度",
                                                  "历史记录", "设置", "返回标题", "关于本作"};
            snprintf(app->row_text[i], sizeof(app->row_text[i]), "%s", labels[index]);
            break;
        }
        case SG0_LIST_SLOTS_SAVE:
        case SG0_LIST_SLOTS_LOAD: {
            snprintf(title, sizeof(title), "%s %02d/%02d",
                     app->list_kind == SG0_LIST_SLOTS_SAVE ? "保存进度" : "读取进度", index + 1,
                     SG0_SLOT_COUNT);
            snprintf(app->row_value[i], sizeof(app->row_value[i]), "%02d", index + 1);
            if (app->slot_valid[index]) {
                snprintf(app->row_text[i], sizeof(app->row_text[i]), "%s",
                         app->slots[index].preview_len ? app->slots[index].preview : "(无预览)");
            } else if (app->slot_state[index] == SG0_SLOT_IO) {
                snprintf(app->row_text[i], sizeof(app->row_text[i]), "读取失败，可重试");
            } else if (app->slot_state[index] == SG0_SLOT_CORRUPT) {
                snprintf(app->row_text[i], sizeof(app->row_text[i]), "存档损坏/版本不兼容");
            } else {
                snprintf(app->row_text[i], sizeof(app->row_text[i]), "空档位");
            }
            break;
        }
        case SG0_LIST_SETTINGS: {
            static const char *const labels[5] = {"文字速度", "自动间隔", "通关记录",
                                                  "关于本作", "返回"};
            snprintf(app->row_text[i], sizeof(app->row_text[i]), "%s", labels[index]);
            if (index == 0) {
                static const char *const values[4] = {"慢", "普通", "快", "即时"};
                snprintf(app->row_value[i], sizeof(app->row_value[i]), "%s",
                         values[app->settings.speed & 3u]);
            } else if (index == 1) {
                static const char *const values[3] = {"慢", "普通", "快"};
                snprintf(app->row_value[i], sizeof(app->row_value[i]), "%s",
                         values[app->settings.auto_wait % 3u]);
            }
            break;
        }
        case SG0_LIST_ROUTES: {
            const char *label = "";
            uint16_t len = 0;
            (void)sg0_pack_title(&app->pack, (uint8_t)index, &label, &len);
            truncate_utf8(app->row_text[i], sizeof(app->row_text[i]), label, len);
            int starts = 0;
            int unlocked = 0;
            for (uint16_t s = 0; s < SG0_MAX_STARTS; ++s) {
                sg0_start_t st;
                if (!sg0_pack_start(&app->pack, s, &st)) break;
                if (st.group != index) continue;
                ++starts;
                if (sg0_unlock_has_chapter(&app->unlock, s)) ++unlocked;
            }
            snprintf(app->row_value[i], sizeof(app->row_value[i]), "%d/%d", unlocked, starts);
            snprintf(title, sizeof(title), "章节选择");
            break;
        }
        case SG0_LIST_STARTS: {
            const int start_index = start_at_group(app, index);
            if (start_index < 0) break;
            sg0_start_t st;
            (void)sg0_pack_start(&app->pack, (uint16_t)start_index, &st);
            char date[12];
            format_date(sg0_pack_date(&app->pack, st.date), date, sizeof(date));
            snprintf(app->row_text[i], sizeof(app->row_text[i]), "第 %d 节  %s", index + 1, date);
            snprintf(app->row_value[i], sizeof(app->row_value[i]), "%s",
                     sg0_unlock_has_chapter(&app->unlock, (uint16_t)start_index) ? "已阅"
                                                                                : "未读");
            const char *label = "";
            uint16_t len = 0;
            (void)sg0_pack_title(&app->pack, (uint8_t)app->route_group, &label, &len);
            truncate_utf8(title, sizeof(title), label, len);
            break;
        }
        case SG0_LIST_GALLERY: {
            sg0_gallery_group_t group;
            if (!sg0_pack_gallery_group(&app->pack, (uint8_t)index, &group)) break;
            const char *label = "";
            uint16_t len = 0;
            (void)sg0_pack_string(&app->pack, group.label, &label, &len);
            truncate_utf8(app->row_text[i], sizeof(app->row_text[i]), label, len);
            snprintf(app->row_value[i], sizeof(app->row_value[i]), "%u 差分",
                     (unsigned)group.count);
            snprintf(title, sizeof(title), "鉴赏室 %d/%d", app->list_win / SG0_LIST_ROWS + 1,
                     (total + SG0_LIST_ROWS - 1) / SG0_LIST_ROWS);
            break;
        }
        case SG0_LIST_CLEAR: {
            const char *label = "";
            uint16_t len = 0;
            uint16_t str = 0;
            if (sg0_pack_endlist(&app->pack, (uint8_t)index, &str)) {
                (void)sg0_pack_string(&app->pack, str, &label, &len);
            }
            truncate_utf8(app->row_text[i], sizeof(app->row_text[i]), label, len);
            snprintf(app->row_value[i], sizeof(app->row_value[i]), "%s",
                     sg0_unlock_has_ending(&app->unlock, (uint8_t)index) ? "已达成" : "未达成");
            break;
        }
        default:
            break;
        }
        app->row_ptrs[i] = app->row_text[i];
        app->value_ptrs[i] = app->row_value[i];
    }
    for (int i = shown; i < SG0_LIST_ROWS; ++i) {
        app->row_text[i][0] = '\0';
        app->row_value[i][0] = '\0';
        app->row_ptrs[i] = app->row_text[i];
        app->value_ptrs[i] = app->row_value[i];
    }
    if (app->list_kind == SG0_LIST_SLOTS_SAVE || app->list_kind == SG0_LIST_SLOTS_LOAD) {
        const int index = app->list_sel;
        if (app->slot_valid[index]) {
            char date[12];
            format_date(app->slots[index].date, date, sizeof(date));
            const char *label = "";
            uint16_t len = 0;
            (void)sg0_pack_title(&app->pack, app->slots[index].group, &label, &len);
            char chapter[48];
            truncate_utf8(chapter, sizeof(chapter), label, len);
            snprintf(footer, sizeof(footer), "%s  %s", chapter, date);
        } else {
            snprintf(footer, sizeof(footer), "空档位 - OK 保存/读取 · 长按OK返回");
        }
    }
    steins0_ui_list(&app->ui, title[0] ? title : "", app->row_ptrs, app->value_ptrs, shown,
                    app->list_sel - app->list_win, footer);
}

static void list_seed(steins0_app_t *app, sg0_list_kind_t kind)
{
    app->list_sel = 0;
    app->list_win = 0;
    if (kind == SG0_LIST_SLOTS_SAVE || kind == SG0_LIST_SLOTS_LOAD) {
        app->list_sel = app->slot_sel;
    } else if (kind == SG0_LIST_MENU) {
        app->list_sel = app->menu_sel;
    } else if (kind == SG0_LIST_SETTINGS) {
        app->list_sel = app->settings_sel;
    }
}

static void open_list(steins0_app_t *app, sg0_list_kind_t kind, steins0_page_t back)
{
    // 列表套列表:压入上一层;存/读档是叶子列表(返回固定回正文),不进栈。
    const bool nested = back == SG0_PAGE_LIST && kind != SG0_LIST_SLOTS_SAVE &&
                        kind != SG0_LIST_SLOTS_LOAD;
    if (nested) {
        if (app->list_depth < SG0_LIST_STACK) {
            app->list_kind_stack[app->list_depth] = app->list_kind;
            app->list_ret_stack[app->list_depth] = app->list_return;
            ++app->list_depth;
        }
    } else if (back != SG0_PAGE_LIST) {
        app->list_depth = 0;   // 从页面进入列表:父链清空
    }
    app->list_kind = kind;
    app->list_return = back;
    list_seed(app, kind);
    build_list(app);
    goto_page(app, SG0_PAGE_LIST);
}

// ------------------------------------------------------------------ 存/读档
static void do_save(steins0_app_t *app, int index)
{
    if (!app->pack_ok || !app->game_started) return;
    sg0_save_t save;
    capture_save(app, &save, NULL);
    if (!app->nvs_ok) {
        set_notice(app, "保存失败：存储不可用");
        return;
    }
    if (!write_slot(app, index, &save)) {
        set_notice(app, "保存失败：写入未完成");
        build_list(app);
        return;
    }
    sg0_save_meta_from(&save, &app->slots[index]);
    app->slot_valid[index] = true;
    app->slot_state[index] = SG0_SLOT_VALID;
    app->slot_sel = index;
    set_notice(app, "保存成功");
    (void)write_unlock(app);
    refresh_game_ui(app);
    goto_page(app, SG0_PAGE_GAME);
}

static bool apply_save(steins0_app_t *app, const sg0_save_t *save)
{
    sg0_state_t state;
    memset(&state, 0, sizeof(state));
    state.node = save->meta.node;
    state.cursor = save->meta.cursor;
    state.mode = save->meta.mode;
    state.endings = (uint8_t)(save->meta.endings | app->unlock.endings);
    state.flags = (uint8_t)(save->meta.flags | app->unlock.flags);
    state.choice_active = 0;
    if (save->event_valid) {
        state.event_node = save->event_node;
        state.event_cursor = save->event_cursor;
    } else {
        uint16_t ordinal = 0;
        if (!sg0_engine_event_ordinal(&app->pack, save->meta.node, save->meta.cursor,
                                      &ordinal)) {
            return false;
        }
        state.event_node = save->meta.node;
        state.event_cursor = ordinal;
    }
    state.rine.replied = save->rine_replied;
    state.rine.windows = save->rine_windows;
    state.rine.pending_valid = save->pending_valid;
    state.rine.pending_node = save->pending_node;
    state.rine.count = 0;
    for (uint16_t ref = 0; ref < SG0_SAVE_RINE_BITS; ++ref) {
        if (!sg0_save_rine_active(save, ref)) continue;
        if (state.rine.count >= SG0_RINE_MAX) break;
        sg0_rine_msg_t msg;
        if (!sg0_pack_rine_msg(&app->pack, ref, &msg)) continue;
        state.rine.refs[state.rine.count] = ref;
        state.rine.read[state.rine.count] = sg0_save_rine_read(save, ref) ? 1 : 0;
        ++state.rine.count;
    }
    app->last_card = save->last_card;
    app->state = state;
    app->game_started = true;
    app->choice_sel = 0;
    app->fast_forward = false;
    app->auto_mode = false;
    app->typing = false;
    app->last_history_node = 0xFFu;
    app->last_history_cursor = 0xFFFFu;
    app->force_scene = true;
    sg0_unlock_merge_save(&app->unlock, save);
    app->unlock_dirty = true;
    app->history_count = 0;
    app->history_next = 0;
    app->autosave_accum = 0;
    return load_op(app, save->meta.page);
}

static bool preflight_save(steins0_app_t *app, const sg0_save_t *save)
{
    if (!sg0_engine_cursor_valid(&app->pack, save->meta.node, save->meta.cursor)) return false;
    sg0_node_t event_node;
    if (!sg0_pack_node(&app->pack, save->meta.node, &event_node)) return false;
    if (save->event_valid) {
        if (save->event_node != save->meta.node ||
            save->event_cursor > event_node.event_count) return false;
    } else {
        uint16_t ordinal = 0;
        if (!sg0_engine_event_ordinal(&app->pack, save->meta.node, save->meta.cursor,
                                      &ordinal)) return false;
    }
    sg0_state_t state;
    memset(&state, 0, sizeof(state));
    state.node = save->meta.node;
    state.cursor = save->meta.cursor;
    state.mode = save->meta.mode;
    state.endings = save->meta.endings;
    state.flags = save->meta.flags;
    state.event_node = 0xFFu;
    sg0_choice_t choice;
    const sg0_place_t place = sg0_engine_locate(&app->pack, &state, &choice);
    if (place == SG0_PLACE_END) return true;
    if (place == SG0_PLACE_CHOICE) {
        // 选项存档没有新画面;预检上一句所在 view。
        if (state.cursor == 0) return true;
        state.cursor--;
        state.choice_active = 0;
    }
    if (state.choice_active) state.choice_active = 0;
    sg0_chunk_t chunk;
    uint32_t raw_len = 0;
    sg0_frame_t frame;
    const char *text = NULL;
    uint16_t text_len = 0;
    if (!sg0_engine_read(&app->pack, &state, app->chunk_buf, sizeof(app->chunk_buf), &chunk,
                         &raw_len, &frame, &text, &text_len)) {
        return false;
    }
    return steins0_image_validate_view(&app->pack, frame.view);
}

static void do_load(steins0_app_t *app, int index)
{
    char key[16];
    slot_key(index, key, sizeof(key));
    sg0_save_t save;
    const sg0_slot_state_t state = read_slot_blob(key, &save);
    app->slot_state[index] = (uint8_t)state;
    if (state == SG0_SLOT_EMPTY) {
        app->slot_valid[index] = false;
        set_notice(app, "空档位");
        return;
    }
    if (state == SG0_SLOT_IO) {
        set_notice(app, "读取失败，请重试");
        return;
    }
    if (state == SG0_SLOT_CORRUPT) {
        set_notice(app, "存档损坏或版本不兼容");
        app->slot_valid[index] = false;
        return;
    }
    app->slot_valid[index] = true;
    sg0_save_meta_from(&save, &app->slots[index]);
    if (!preflight_save(app, &save)) {
        set_notice(app, "存档画面不可用，未读取");
        return;
    }
    if (!apply_save(app, &save)) {
        set_notice(app, "读取失败：画面或正文不可用");
        return;
    }
    set_notice(app, "读取成功");
}

static void do_load_auto(steins0_app_t *app)
{
    if (!app->auto_valid) {
        set_notice(app, "没有可继续的进度");
        return;
    }
    sg0_save_t save;
    const sg0_slot_state_t state = read_auto_blob(&save);
    app->auto_state = (uint8_t)state;
    if (state == SG0_SLOT_IO) {
        set_notice(app, "自动存档读取失败，请重试");
        return;
    }
    if (state == SG0_SLOT_CORRUPT) {
        set_notice(app, "自动存档损坏或版本不兼容");
        app->auto_valid = false;
        return;
    }
    if (state == SG0_SLOT_EMPTY) {
        set_notice(app, "没有可继续的进度");
        app->auto_valid = false;
        return;
    }
    app->auto_valid = true;
    if (!preflight_save(app, &save)) {
        set_notice(app, "自动存档画面不可用");
        return;
    }
    if (!apply_save(app, &save)) {
        set_notice(app, "继续失败：画面或正文不可用");
        return;
    }
    set_notice(app, "已继续上次进度");
}

// ------------------------------------------------------------------ 标题动作
static void start_new_game(steins0_app_t *app)
{
    if (!app->pack_ok) {
        set_notice(app, "资源包损坏，无法开始");
        return;
    }
    sg0_engine_init(&app->state, app->unlock.endings, app->unlock.flags);
    app->game_started = true;
    app->choice_sel = 0;
    app->typing = false;
    app->auto_mode = false;
    app->fast_forward = false;
    app->last_history_node = 0xFFu;
    app->last_history_cursor = 0xFFFFu;
    app->force_scene = true;
    app->last_view = 0xFFFFu;
    app->last_card = 0xFFu;
    app->history_count = 0;
    app->history_next = 0;
    app->autosave_accum = 0;
    (void)load_op(app, 0);
}

static void start_chapter(steins0_app_t *app, uint16_t start_index)
{
    sg0_start_t start;
    if (!sg0_pack_start(&app->pack, start_index, &start)) return;
    sg0_engine_init(&app->state, app->unlock.endings, app->unlock.flags);
    sg0_engine_enter_chapter(&app->state, &start);
    app->game_started = true;
    app->choice_sel = 0;
    app->typing = false;
    app->auto_mode = false;
    app->fast_forward = false;
    app->last_history_node = 0xFFu;
    app->last_history_cursor = 0xFFFFu;
    app->force_scene = true;
    app->history_count = 0;
    app->history_next = 0;
    app->autosave_accum = 0;
    (void)load_op(app, 0);
}

static void title_activate(steins0_app_t *app, int index)
{
    switch (index) {
    case 0: start_new_game(app); break;
    case 1: do_load_auto(app); break;
    case 2: open_list(app, SG0_LIST_SLOTS_LOAD, SG0_PAGE_TITLE); break;
    case 3: open_list(app, SG0_LIST_ROUTES, SG0_PAGE_TITLE); break;
    case 4: open_list(app, SG0_LIST_GALLERY, SG0_PAGE_TITLE); break;
    case 5:
        app->settings_sel = 0;
        open_list(app, SG0_LIST_SETTINGS, SG0_PAGE_TITLE);
        break;
    default: break;
    }
}

static void open_menu(steins0_app_t *app)
{
    app->auto_mode = false;
    app->fast_forward = false;
    open_list(app, SG0_LIST_MENU, SG0_PAGE_GAME);
}

static void open_history(steins0_app_t *app)
{
    // 历史是只读页:记住来源(正文或菜单),返回时回同一层。
    app->page_return = app->page == SG0_PAGE_LIST ? SG0_PAGE_LIST : SG0_PAGE_GAME;
    build_history_text(app);
    steins0_ui_history(&app->ui, app->history_buf);
    goto_page(app, SG0_PAGE_HISTORY);
}

static void history_back(steins0_app_t *app)
{
    if (app->page_return == SG0_PAGE_LIST) {
        goto_page(app, SG0_PAGE_LIST);
        build_list(app);
        return;
    }
    refresh_game_ui(app);
    goto_page(app, SG0_PAGE_GAME);
}

static void open_about(steins0_app_t *app, steins0_page_t back)
{
    static const char *const about =
        "命运石之门 0 阅读器\n"
        "AI Passport 本地技术验证版\n\n"
        "源：xuanyide01/steins0-rw5 转换快照。\n"
        "原作剧情、图像、标题与字体归权利人所有；本固件不授予原作素材权利，不作分发。\n\n"
        "正文使用原作 PUA 字形编码与源子集字库，未做 OCR 替换。\n\n"
        "操作：UP 推进 / 长按快进；DOWN 历史 / 长按自动；OK 菜单 / 长按菜单。\n"
        "列表：UP/DOWN 选择，OK 确认，长按 OK 返回。";
    app->page_return = back;
    steins0_ui_about(&app->ui, about);
    goto_page(app, SG0_PAGE_ABOUT);
}

static void open_clear_list(steins0_app_t *app)
{
    open_list(app, SG0_LIST_CLEAR, SG0_PAGE_LIST);
}

static void gallery_open(steins0_app_t *app, int group)
{
    sg0_gallery_group_t gg;
    if (!sg0_pack_gallery_group(&app->pack, (uint8_t)group, &gg)) return;
    app->cg_group = group;
    app->cg_variant = 0;
    const char *label = "";
    uint16_t len = 0;
    (void)sg0_pack_string(&app->pack, gg.label, &label, &len);
    truncate_utf8(app->cg_caption, sizeof(app->cg_caption), label, len);
    uint16_t asset = 0;
    if (!sg0_pack_gallery_entry(&app->pack, gg.first, &asset) ||
        !steins0_image_show_cg(&app->ui.image, &app->pack, asset, true)) {
        set_notice(app, "CG 读取失败");
        return;
    }
    char variant[24];
    snprintf(variant, sizeof(variant), "1/%u", (unsigned)gg.count);
    steins0_ui_cg(&app->ui, app->cg_caption, variant);
    sg0_unlock_set_gallery(&app->unlock, (uint8_t)group);
    app->unlock_dirty = true;
    goto_page(app, SG0_PAGE_CG);
}

static void gallery_variant(steins0_app_t *app, int delta)
{
    sg0_gallery_group_t gg;
    if (!sg0_pack_gallery_group(&app->pack, (uint8_t)app->cg_group, &gg) || gg.count == 0) return;
    int v = (app->cg_variant + delta) % gg.count;
    if (v < 0) v += gg.count;
    uint16_t asset = 0;
    if (!sg0_pack_gallery_entry(&app->pack, (uint16_t)(gg.first + v), &asset)) return;
    if (!steins0_image_show_cg(&app->ui.image, &app->pack, asset, true)) {
        set_notice(app, "CG 读取失败");
        return;
    }
    app->cg_variant = v;
    char variant[24];
    snprintf(variant, sizeof(variant), "%d/%u", v + 1, (unsigned)gg.count);
    steins0_ui_cg(&app->ui, app->cg_caption, variant);
}

// ------------------------------------------------------------------- 列表动作
static void list_move(steins0_app_t *app, int delta)
{
    const int total = list_total(app);
    if (total <= 0) return;
    app->list_sel += delta;
    if (app->list_sel < 0) app->list_sel = total - 1;
    if (app->list_sel >= total) app->list_sel = 0;
    if (app->list_kind == SG0_LIST_SLOTS_SAVE || app->list_kind == SG0_LIST_SLOTS_LOAD) {
        app->slot_sel = app->list_sel;
    } else if (app->list_kind == SG0_LIST_MENU) {
        app->menu_sel = app->list_sel;
    } else if (app->list_kind == SG0_LIST_SETTINGS) {
        app->settings_sel = app->list_sel;
    }
    build_list(app);
}

static void slot_save_request(steins0_app_t *app, int index)
{
    if (!app->pack_ok || !app->game_started) return;
    sg0_slot_state_t state = (sg0_slot_state_t)app->slot_state[index];
    char key[16];
    slot_key(index, key, sizeof(key));
    if (state == SG0_SLOT_EMPTY) {
        do_save(app, index);
        return;
    }
    // 已知占用、IO 未知或损坏:重新读一次,只有明确 EMPTY 才直接新建。
    sg0_save_t check;
    state = read_slot_blob(key, &check);
    app->slot_state[index] = (uint8_t)state;
    app->slot_valid[index] = state == SG0_SLOT_VALID;
    if (state == SG0_SLOT_EMPTY) {
        do_save(app, index);
        return;
    }
    if (state == SG0_SLOT_VALID) sg0_save_meta_from(&check, &app->slots[index]);
    app->confirm_action = SG0_CONFIRM_OVERWRITE;
    app->confirm_slot = index;
    app->confirm_return = SG0_PAGE_LIST;
    app->confirm_sel = 1;
    if (state == SG0_SLOT_CORRUPT) {
        snprintf(app->confirm_prompt, sizeof(app->confirm_prompt),
                 "存档损坏或版本不兼容。仍然覆盖吗？");
    } else if (state == SG0_SLOT_IO) {
        snprintf(app->confirm_prompt, sizeof(app->confirm_prompt),
                 "无法读取该槽位。仍然覆盖吗？");
    } else {
        snprintf(app->confirm_prompt, sizeof(app->confirm_prompt), "覆盖这个存档吗？");
    }
    snprintf(app->confirm_yes, sizeof(app->confirm_yes), "覆盖");
    snprintf(app->confirm_no, sizeof(app->confirm_no), "取消");
    steins0_ui_confirm(&app->ui, app->confirm_prompt, app->confirm_yes,
                       app->confirm_no, app->confirm_sel);
    goto_page(app, SG0_PAGE_CONFIRM);
}

static void list_activate(steins0_app_t *app)
{
    const int index = app->list_sel;
    switch (app->list_kind) {
    case SG0_LIST_MENU:
        switch (index) {
        case 0: refresh_game_ui(app); goto_page(app, SG0_PAGE_GAME); break;
        case 1: phone_open(app); break;
        case 2: open_list(app, SG0_LIST_SLOTS_SAVE, SG0_PAGE_LIST); break;
        case 3: open_list(app, SG0_LIST_SLOTS_LOAD, SG0_PAGE_LIST); break;
        case 4: open_history(app); break;
        case 5:
            app->settings_sel = 0;
            open_list(app, SG0_LIST_SETTINGS, SG0_PAGE_LIST);
            break;
        case 6:
            app->confirm_action = SG0_CONFIRM_TITLE;
            app->confirm_slot = -1;
            app->confirm_return = SG0_PAGE_LIST;
            app->confirm_sel = 1;
            snprintf(app->confirm_prompt, sizeof(app->confirm_prompt),
                     "返回标题吗？\n当前进度会写入自动存档。");
            snprintf(app->confirm_yes, sizeof(app->confirm_yes), "返回");
            snprintf(app->confirm_no, sizeof(app->confirm_no), "取消");
            steins0_ui_confirm(&app->ui, app->confirm_prompt, app->confirm_yes,
                               app->confirm_no, app->confirm_sel);
            goto_page(app, SG0_PAGE_CONFIRM);
            break;
        case 7: open_about(app, SG0_PAGE_LIST); break;
        default: break;
        }
        break;
    case SG0_LIST_SLOTS_SAVE:
        slot_save_request(app, index);
        break;
    case SG0_LIST_SLOTS_LOAD:
        do_load(app, index);
        break;
    case SG0_LIST_SETTINGS:
        if (index == 0) app->settings.speed = (uint8_t)((app->settings.speed + 1) & 3u);
        else if (index == 1) app->settings.auto_wait = (uint8_t)((app->settings.auto_wait + 1) % 3u);
        else if (index == 2) { open_clear_list(app); return; }
        else if (index == 3) { open_about(app, SG0_PAGE_LIST); return; }
        else { list_back(app); return; }
        save_settings(app);
        app->list_sel = index;
        app->settings_sel = index;
        build_list(app);
        break;
    case SG0_LIST_ROUTES:
        app->route_group = index;
        open_list(app, SG0_LIST_STARTS, SG0_PAGE_LIST);
        break;
    case SG0_LIST_STARTS: {
        const int start_index = start_at_group(app, index);
        if (start_index < 0) break;
        start_chapter(app, (uint16_t)start_index);
        break;
    }
    case SG0_LIST_GALLERY:
        gallery_open(app, index);
        break;
    case SG0_LIST_CLEAR:
        set_notice(app, "通关记录只读");
        break;
    default:
        break;
    }
}

static void list_back(steins0_app_t *app)
{
    // 列表套列表:回栈里记录的上一层,不再用单个 list_return 猜层级。
    if (app->list_return == SG0_PAGE_LIST && app->list_depth > 0) {
        --app->list_depth;
        app->list_kind = app->list_kind_stack[app->list_depth];
        app->list_return = app->list_ret_stack[app->list_depth];
        list_seed(app, app->list_kind);
        if (app->list_kind == SG0_LIST_ROUTES) app->list_sel = app->route_group;
        build_list(app);
        goto_page(app, SG0_PAGE_LIST);
        return;
    }
    // 存/读档是叶子列表:返回固定回正文(未开始则回标题),保持既有习惯。
    if (app->list_kind == SG0_LIST_SLOTS_SAVE || app->list_kind == SG0_LIST_SLOTS_LOAD) {
        if (app->game_started) {
            refresh_game_ui(app);
            goto_page(app, SG0_PAGE_GAME);
        } else {
            refresh_title(app);
        }
        return;
    }
    if (app->list_return == SG0_PAGE_GAME && app->game_started) {
        refresh_game_ui(app);
        goto_page(app, SG0_PAGE_GAME);
    } else {
        refresh_title(app);   // 未知来源一律回标题,绝不落进未开始的正文页
    }
}

// -------------------------------------------------------------------- 手机
static void contact_name(const steins0_app_t *app, uint8_t id, char *out, size_t cap)
{
    out[0] = '\0';
    for (uint8_t i = 0; i < app->pack.sec[SG0_SEC_RCONTACT].count; ++i) {
        uint8_t cid = 0;
        const char *name = NULL;
        uint16_t name_len = 0;
        if (!sg0_pack_rine_contact(&app->pack, i, &cid, &name, &name_len, NULL, NULL)) continue;
        if (cid != id) continue;
        size_t n = name_len < cap - 1 ? name_len : cap - 1;
        while (n > 0 && (((uint8_t)name[n]) & 0xC0u) == 0x80u) --n;
        memcpy(out, name, n);
        out[n] = '\0';
        return;
    }
    snprintf(out, cap, "未知");
}

static uint16_t rine_count(const steins0_app_t *app, bool sent, bool unread_only)
{
    uint16_t count = 0;
    for (uint16_t i = 0; i < app->state.rine.count; ++i) {
        sg0_rine_msg_t msg;
        if (!sg0_pack_rine_msg(&app->pack, app->state.rine.refs[i], &msg)) continue;
        const bool outgoing = (msg.flags & 1u) != 0;
        if (outgoing != sent) continue;
        if (unread_only && app->state.rine.read[i]) continue;
        ++count;
    }
    return count;
}

static bool thread_key_seen(const steins0_app_t *app, uint8_t count, uint16_t base,
                            uint32_t date, uint8_t sender)
{
    for (uint8_t i = 0; i < count; ++i) {
        if (app->phone_thread_keys[i].base == base &&
            app->phone_thread_keys[i].date == date &&
            app->phone_thread_keys[i].sender == sender) {
            return true;
        }
    }
    return false;
}

static uint8_t build_threads(steins0_app_t *app, bool sent)
{
    uint8_t count = 0;
    for (uint16_t i = app->state.rine.count; i > 0 && count < SG0_PHONE_KEY_MAX; --i) {
        sg0_rine_msg_t msg;
        if (!sg0_pack_rine_msg(&app->pack, app->state.rine.refs[i - 1], &msg)) continue;
        if (((msg.flags & 1u) != 0) != sent) continue;
        const uint32_t date = sg0_pack_date(
            &app->pack, sg0_engine_rine_date(&app->pack, &app->state,
                                             app->state.rine.refs[i - 1]));
        if (thread_key_seen(app, count, msg.base, date, msg.sender)) continue;
        app->phone_thread_keys[count].base = msg.base;
        app->phone_thread_keys[count].date = date;
        app->phone_thread_keys[count].sender = msg.sender;
        app->phone_thread_keys[count].sent = sent ? 1 : 0;
        ++count;
    }
    return count;
}

static bool catalog_text(const steins0_app_t *app, uint16_t ref, char *out, size_t cap,
                         bool *stamp)
{
    sg0_rine_msg_t msg;
    if (!sg0_pack_rine_msg(&app->pack, ref, &msg)) return false;
    if (stamp) *stamp = (msg.flags & 2u) != 0;
    if (msg.flags & 2u) {
        snprintf(out, cap, "动画表情");
        return true;
    }
    const char *text = NULL;
    uint16_t len = 0;
    if (!sg0_pack_string(&app->pack, msg.text, &text, &len)) return false;
    size_t n = len < cap - 1 ? len : cap - 1;
    while (n > 0 && (((uint8_t)text[n]) & 0xC0u) == 0x80u) --n;
    memcpy(out, text, n);
    out[n] = '\0';
    return true;
}

static uint16_t latest_thread_ref(const steins0_app_t *app, bool sent, uint16_t base,
                                  uint32_t date, uint8_t sender)
{
    uint16_t found = 0xFFFFu;
    for (uint16_t i = 0; i < app->state.rine.count; ++i) {
        sg0_rine_msg_t msg;
        if (!sg0_pack_rine_msg(&app->pack, app->state.rine.refs[i], &msg)) continue;
        if (((msg.flags & 1u) != 0) != sent) continue;
        if (msg.base == base &&
            sg0_pack_date(&app->pack,
                          sg0_engine_rine_date(&app->pack, &app->state,
                                               app->state.rine.refs[i])) == date &&
            msg.sender == sender) {
            found = app->state.rine.refs[i];
        }
    }
    return found;
}

static void phone_thread_render(steins0_app_t *app);
static void phone_thread_open(steins0_app_t *app, uint16_t base, uint32_t date, uint8_t sender,
                              bool sent);

static int phone_total(steins0_app_t *app)
{
    switch (app->phone_view) {
    case SG0_PHONE_HOME: return 5;
    case SG0_PHONE_INBOX: return build_threads(app, false);
    case SG0_PHONE_SENT: return build_threads(app, true);
    case SG0_PHONE_CONTACTS: return (int)app->pack.sec[SG0_SEC_RCONTACT].count;
    case SG0_PHONE_CALLS: return app->state.rine.pending_valid ? 2 : 1;
    case SG0_PHONE_REPLY: {
        uint16_t base = 0;
        uint8_t contact = 0, choices = 0;
        uint16_t first = 0, key = 0;
        for (uint8_t i = 0; i < app->pack.sec[SG0_SEC_RREPLY].count; ++i) {
            if (!sg0_pack_rine_reply(&app->pack, i, &base, &contact, &choices, &first, &key)) {
                continue;
            }
            if (base == app->phone_thread_base) return choices;
        }
        return 0;
    }
    default: return 0;
    }
}

static void phone_render(steins0_app_t *app)
{
    const int total = phone_total(app);
    if (app->phone_sel < 0) app->phone_sel = 0;
    if (total > 0 && app->phone_sel >= total) app->phone_sel = total - 1;
    if (app->phone_sel < app->phone_win) app->phone_win = app->phone_sel;
    if (app->phone_sel >= app->phone_win + SG0_LIST_ROWS) {
        app->phone_win = app->phone_sel - SG0_LIST_ROWS + 1;
    }
    const int shown = total - app->phone_win > SG0_LIST_ROWS
                          ? SG0_LIST_ROWS : (total - app->phone_win > 0 ? total - app->phone_win : 0);
    char title[48] = "手机";
    char sub[64] = "RINE";
    char footer[96] = "上/下 选择 / OK 确认 / 长按 OK 返回";
    for (int i = 0; i < SG0_LIST_ROWS; ++i) {
        app->phone_text[i][0] = '\0';
        app->phone_value[i][0] = '\0';
    }
    if (app->phone_view == SG0_PHONE_HOME) {
        const uint16_t inbox = rine_count(app, false, false);
        const uint16_t sent = rine_count(app, true, false);
        const uint16_t unread = sg0_engine_unread(&app->pack, &app->state);
        static const char *const home[5] = {"收信箱", "已发送", "联系人", "当前电话与手机操作",
                                            "返回剧情"};
        for (int i = 0; i < 5; ++i) snprintf(app->phone_text[i], sizeof(app->phone_text[i]),
                                            "%s", home[i]);
        snprintf(app->phone_value[0], sizeof(app->phone_value[0]), "%u条/%u未读",
                 (unsigned)inbox, (unsigned)unread);
        snprintf(app->phone_value[1], sizeof(app->phone_value[1]), "%u条", (unsigned)sent);
        snprintf(app->phone_value[2], sizeof(app->phone_value[2]), "%u位",
                 (unsigned)app->pack.sec[SG0_SEC_RCONTACT].count);
        snprintf(app->phone_value[3], sizeof(app->phone_value[3]), "%s",
                 app->state.rine.pending_valid ? "有待处理来电" : "无");
        snprintf(sub, sizeof(sub), "收信箱 %u / 已发送 %u / 未读 %u", (unsigned)inbox,
                 (unsigned)sent, (unsigned)unread);
    } else if (app->phone_view == SG0_PHONE_INBOX || app->phone_view == SG0_PHONE_SENT) {
        const bool sent = app->phone_view == SG0_PHONE_SENT;
        const uint8_t count = build_threads(app, sent);
        snprintf(title, sizeof(title), "%s", sent ? "已发送" : "收信箱");
        snprintf(sub, sizeof(sub), "%u 组对话 · %u 条消息", (unsigned)count,
                 (unsigned)rine_count(app, sent, false));
        for (int i = 0; i < shown; ++i) {
            const uint16_t base = app->phone_thread_keys[app->phone_win + i].base;
            const uint32_t date = app->phone_thread_keys[app->phone_win + i].date;
            const uint8_t sender = app->phone_thread_keys[app->phone_win + i].sender;
            const uint16_t ref = latest_thread_ref(app, sent, base, date, sender);
            char name[40];
            char preview[48] = "";
            bool is_stamp = false;
            contact_name(app, sender, name, sizeof(name));
            if (ref != 0xFFFFu) catalog_text(app, ref, preview, sizeof(preview), &is_stamp);
            snprintf(app->phone_text[i], sizeof(app->phone_text[i]), "%s: %s", name, preview);
            char day[12];
            format_date(date, day, sizeof(day));
            snprintf(app->phone_value[i], sizeof(app->phone_value[i]), "%s", day);
        }
        if (count == 0) {
            snprintf(footer, sizeof(footer), "暂无内容 · 长按 OK 返回");
        } else {
            snprintf(footer, sizeof(footer), "%d/%d 组 · 长按 OK 返回",
                     app->phone_sel + 1, count);
        }
    } else if (app->phone_view == SG0_PHONE_CONTACTS) {
        snprintf(title, sizeof(title), "联系人");
        snprintf(sub, sizeof(sub), "随剧情推进更新");
        for (int i = 0; i < shown; ++i) {
            uint8_t cid = 0;
            const char *name = NULL;
            const char *number = NULL;
            uint16_t name_len = 0, number_len = 0;
            if (!sg0_pack_rine_contact(&app->pack, (uint8_t)(app->phone_win + i), &cid, &name,
                                       &name_len, &number, &number_len)) {
                continue;
            }
            size_t n = name_len < sizeof(app->phone_text[i]) - 1
                           ? name_len : sizeof(app->phone_text[i]) - 1;
            memcpy(app->phone_text[i], name, n);
            app->phone_text[i][n] = '\0';
            n = number_len < sizeof(app->phone_value[i]) - 1
                    ? number_len : sizeof(app->phone_value[i]) - 1;
            memcpy(app->phone_value[i], number, n);
            app->phone_value[i][n] = '\0';
        }
        snprintf(footer, sizeof(footer), "当前没有来电 · 长按 OK 返回");
    } else if (app->phone_view == SG0_PHONE_CALLS) {
        sg0_node_t node;
        sg0_choice_t choice;
        const char *heading = "当前电话";
        const char *category = "";
        uint16_t len = 0;
        if (app->state.rine.pending_valid &&
            sg0_pack_node(&app->pack, app->state.rine.pending_node, &node) &&
            node.choice != SG0_NONE && sg0_pack_choice(&app->pack, node.choice, &choice)) {
            (void)sg0_pack_string(&app->pack, choice.heading, &heading, &len);
            (void)sg0_pack_string(&app->pack, choice.category, &category, &len);
        }
        snprintf(title, sizeof(title), "%s", heading);
        snprintf(sub, sizeof(sub), "%s", category);
        snprintf(app->phone_text[0], sizeof(app->phone_text[0]), "%s",
                 choice.kind == 1 ? "关闭手机电源" : "接听电话");
        snprintf(app->phone_value[0], sizeof(app->phone_value[0]), "执行");
        snprintf(app->phone_text[1], sizeof(app->phone_text[1]), "暂不处理，继续剧情");
        snprintf(app->phone_value[1], sizeof(app->phone_value[1]), "保留来电");
        snprintf(footer, sizeof(footer), "长按 OK 返回手机");
    } else if (app->phone_view == SG0_PHONE_REPLY) {
        uint16_t base = 0, first = 0, key = 0;
        uint8_t contact = 0, choices = 0;
        uint16_t reply_index = 0xFFFFu;
        for (uint8_t i = 0; i < app->pack.sec[SG0_SEC_RREPLY].count; ++i) {
            if (!sg0_pack_rine_reply(&app->pack, i, &base, &contact, &choices, &first, &key)) {
                continue;
            }
            if (base == app->phone_thread_base) { reply_index = i; break; }
        }
        snprintf(title, sizeof(title), "回复 RINE");
        char name[40];
        contact_name(app, contact, name, sizeof(name));
        snprintf(sub, sizeof(sub), "%s", name);
        for (int i = 0; i < shown && reply_index != 0xFFFFu; ++i) {
            uint16_t selected = 0, seq_first = 0;
            uint8_t seq_count = 0, stamp = 0;
            if (!sg0_pack_rine_choice(&app->pack, (uint16_t)(first + app->phone_win + i),
                                      &selected, &seq_first, &seq_count, &stamp)) {
                continue;
            }
            bool is_stamp = false;
            catalog_text(app, selected, app->phone_text[i], sizeof(app->phone_text[i]),
                         &is_stamp);
            if (is_stamp) snprintf(app->phone_text[i], sizeof(app->phone_text[i]),
                                   "动画表情 %d", app->phone_win + i + 1);
            snprintf(app->phone_value[i], sizeof(app->phone_value[i]), "发送");
        }
        snprintf(footer, sizeof(footer), "OK 发送 / 长按 OK 取消");
    }
    app->phone_ptrs[0] = app->phone_text[0];
    for (int i = 0; i < SG0_LIST_ROWS; ++i) {
        app->phone_ptrs[i] = app->phone_text[i];
        app->phone_value_ptrs[i] = app->phone_value[i];
    }
    steins0_ui_phone(&app->ui, title, sub, "", app->phone_ptrs, app->phone_value_ptrs, shown,
                     app->phone_sel - app->phone_win, footer);
}

static void phone_thread_render(steins0_app_t *app)
{
    const uint16_t ref = app->phone_thread_part < app->phone_thread_count
                             ? app->phone_thread_refs[app->phone_thread_part] : 0xFFFFu;
    const uint8_t sub = app->phone_thread_part < app->phone_thread_count
                            ? app->phone_thread_sub[app->phone_thread_part] : 0;
    char header[96];
    char name[40];
    char date[12];
    contact_name(app, app->phone_thread_sender, name, sizeof(name));
    format_date(app->phone_thread_date, date, sizeof(date));
    snprintf(header, sizeof(header), "%s  %s", name, date);
    char hint[80];
    snprintf(hint, sizeof(hint), "%u/%u  OK 回复 / 长按 OK 返回",
             (unsigned)(app->phone_thread_part + 1), (unsigned)app->phone_thread_count);
    if (ref == 0xFFFFu) {
        steins0_ui_phone_thread(&app->ui, "会话", "", NULL, 0, false, "长按 OK 返回");
        return;
    }
    sg0_rine_msg_t msg;
    if (!sg0_pack_rine_msg(&app->pack, ref, &msg)) return;
    if (msg.flags & 2u) {
        char msg_name[40];
        contact_name(app, msg.sender, msg_name, sizeof(msg_name));
        /* Ensure the lazy thread page (and its stamp canvas) exists before drawing. */
        steins0_ui_phone_thread(&app->ui, header, msg.flags & 1u ? "我" : msg_name, NULL, 0,
                                true, hint);
        if (!app->ui.stamp.canvas ||
            !steins0_image_show_stamp(&app->ui.stamp, &app->pack, msg.stamp, true)) {
            set_notice(app, "贴纸读取失败");
        }
    } else {
        const char *text = NULL;
        uint16_t len = 0;
        char clean[SG0_TEXT_BUFFER];
        char raw[SG0_TEXT_BUFFER];
        sg0_pages_t pages;
        if (!sg0_pack_string(&app->pack, msg.text, &text, &len) || len >= sizeof(raw)) return;
        memcpy(raw, text, len);
        raw[len] = 0;
        text = raw;
        if (!sg0_text_layout(text, 300u * 16u, 3, &pages)) return;
        const uint8_t page = sub < pages.pages ? sub : 0;
        for (int l = 0; l < pages.line_count[page] && l < 3; ++l) {
            uint16_t start = pages.line_start[page][l];
            uint16_t end = pages.line_start[page][l + 1];
            while (start < end && text[start] == ' ') ++start;
            size_t n = end - start;
            if (n >= sizeof(clean)) n = sizeof(clean) - 1;
            memcpy(clean, text + start, n);
            clean[n] = '\0';
            size_t copy = strlen(clean);
            if (copy >= sizeof(app->line_buf[l])) copy = sizeof(app->line_buf[l]) - 1;
            memcpy(app->line_buf[l], clean, copy);
            app->line_buf[l][copy] = 0;
        }
        for (int l = pages.line_count[page]; l < 3; ++l) app->line_buf[l][0] = '\0';
        const char *lines[3] = {app->line_buf[0], app->line_buf[1], app->line_buf[2]};
        char msg_name[40];
        contact_name(app, msg.sender, msg_name, sizeof(msg_name));
        steins0_ui_phone_thread(&app->ui, header, msg.flags & 1u ? "我" : msg_name, lines,
                                pages.line_count[page], false, hint);
    }
    for (uint16_t i = 0; i < app->state.rine.count; ++i) {
        if (app->state.rine.refs[i] == ref) {
            app->state.rine.read[i] = 1;
            break;
        }
    }
    steins0_ui_game_rine(&app->ui, "");
}

static void phone_thread_open(steins0_app_t *app, uint16_t base, uint32_t date, uint8_t sender,
                              bool sent)
{
    app->phone_thread_base = base;
    app->phone_thread_date = date;
    app->phone_thread_sender = sender;
    app->phone_thread_sent = sent ? 1 : 0;
    app->phone_thread_count = 0;
    for (uint16_t i = 0; i < app->state.rine.count && app->phone_thread_count < SG0_PHONE_THREAD_MAX; ++i) {
        sg0_rine_msg_t msg;
        if (!sg0_pack_rine_msg(&app->pack, app->state.rine.refs[i], &msg)) continue;
        if (msg.base != base ||
            sg0_pack_date(&app->pack,
                          sg0_engine_rine_date(&app->pack, &app->state,
                                               app->state.rine.refs[i])) != date) {
            continue;
        }
        sg0_rine_msg_t msg2;
        if (!sg0_pack_rine_msg(&app->pack, app->state.rine.refs[i], &msg2)) continue;
        if (msg2.flags & 2u) {
            app->phone_thread_refs[app->phone_thread_count] = app->state.rine.refs[i];
            app->phone_thread_sub[app->phone_thread_count++] = 0;
        } else {
            const char *text = NULL;
            uint16_t len = 0;
            char clean[SG0_TEXT_BUFFER];
            sg0_pages_t pages;
            if (sg0_pack_string(&app->pack, msg2.text, &text, &len) && len < sizeof(clean)) {
                memcpy(clean, text, len);
                clean[len] = 0;
            } else {
                clean[0] = 0;
            }
            if (sg0_text_layout(clean, 300u * 16u, 3, &pages)) {
                for (uint8_t page = 0; page < pages.pages &&
                     app->phone_thread_count < SG0_PHONE_THREAD_MAX; ++page) {
                    app->phone_thread_refs[app->phone_thread_count] = app->state.rine.refs[i];
                    app->phone_thread_sub[app->phone_thread_count++] = page;
                }
            }
        }
    }
    if (!steins0_ui_show(&app->ui, SG0_PAGE_PHONE_THREAD)) return;
    app->phone_thread_part = 0;
    app->phone_view = SG0_PHONE_THREAD;
    app->page = SG0_PAGE_PHONE_THREAD;
    phone_thread_render(app);
}

static void phone_open(steins0_app_t *app)
{
    if (!steins0_ui_show(&app->ui, SG0_PAGE_PHONE)) return;
    app->phone_view = SG0_PHONE_HOME;
    app->phone_sel = 0;
    app->phone_win = 0;
    app->page = SG0_PAGE_PHONE;
    phone_render(app);
}

static void phone_back(steins0_app_t *app)
{
    if (app->phone_view == SG0_PHONE_THREAD) {
        if (!steins0_ui_show(&app->ui, SG0_PAGE_PHONE)) return;
        app->phone_view = app->phone_thread_sent ? SG0_PHONE_SENT : SG0_PHONE_INBOX;
        app->phone_sel = 0;
        app->phone_win = 0;
        app->page = SG0_PAGE_PHONE;
        phone_render(app);
        return;
    }
    if (app->phone_view == SG0_PHONE_REPLY) {
        if (!steins0_ui_show(&app->ui, SG0_PAGE_PHONE_THREAD)) return;
        app->phone_view = SG0_PHONE_THREAD;
        app->page = SG0_PAGE_PHONE_THREAD;
        phone_thread_render(app);
        return;
    }
    if (app->phone_view == SG0_PHONE_HOME) {
        refresh_game_ui(app);
        goto_page(app, SG0_PAGE_GAME);
        return;
    }
    if (!steins0_ui_show(&app->ui, SG0_PAGE_PHONE)) return;
    app->phone_view = SG0_PHONE_HOME;
    app->phone_sel = 0;
    app->phone_win = 0;
    app->page = SG0_PAGE_PHONE;
    phone_render(app);
}

static void phone_move(steins0_app_t *app, int delta)
{
    const int total = phone_total(app);
    if (total <= 0) return;
    app->phone_sel += delta;
    if (app->phone_sel < 0) app->phone_sel = total - 1;
    if (app->phone_sel >= total) app->phone_sel = 0;
    if (app->phone_view == SG0_PHONE_THREAD) {
        app->phone_thread_part = (uint16_t)((app->phone_thread_part + delta + app->phone_thread_count) %
                                            (app->phone_thread_count ? app->phone_thread_count : 1));
        phone_thread_render(app);
    } else {
        phone_render(app);
    }
}

static void phone_thread_reply(steins0_app_t *app)
{
    if (!sg0_engine_can_reply(&app->pack, &app->state, app->phone_thread_base)) {
        set_notice(app, "这段对话没有可用的回复时机");
        return;
    }
    if (!steins0_ui_show(&app->ui, SG0_PAGE_PHONE)) return;
    app->phone_view = SG0_PHONE_REPLY;
    app->phone_sel = 0;
    app->phone_win = 0;
    app->page = SG0_PAGE_PHONE;
    phone_render(app);
}

static void phone_activate(steins0_app_t *app)
{
    const int index = app->phone_sel;
    if (app->phone_view == SG0_PHONE_HOME) {
        if (index == 0) { app->phone_view = SG0_PHONE_INBOX; app->phone_sel = 0; app->phone_win = 0; phone_render(app); }
        else if (index == 1) { app->phone_view = SG0_PHONE_SENT; app->phone_sel = 0; app->phone_win = 0; phone_render(app); }
        else if (index == 2) { app->phone_view = SG0_PHONE_CONTACTS; app->phone_sel = 0; app->phone_win = 0; phone_render(app); }
        else if (index == 3) {
            if (app->state.rine.pending_valid) {
                app->phone_view = SG0_PHONE_CALLS;
                app->phone_sel = 0;
                app->phone_win = 0;
                phone_render(app);
            } else set_notice(app, "当前没有待处理来电");
        } else phone_back(app);
        return;
    }
    if (app->phone_view == SG0_PHONE_INBOX || app->phone_view == SG0_PHONE_SENT) {
        const bool sent = app->phone_view == SG0_PHONE_SENT;
        const uint8_t count = build_threads(app, sent);
        if (index < 0 || index >= count) return;
        phone_thread_open(app, app->phone_thread_keys[index].base,
                          app->phone_thread_keys[index].date,
                          app->phone_thread_keys[index].sender, sent);
        return;
    }
    if (app->phone_view == SG0_PHONE_CONTACTS) {
        uint8_t cid = 0;
        if (sg0_pack_rine_contact(&app->pack, (uint8_t)index, &cid, NULL, NULL, NULL, NULL) &&
            app->state.rine.pending_valid) {
            app->phone_view = SG0_PHONE_CALLS;
            app->phone_sel = 0;
            app->phone_win = 0;
            phone_render(app);
        } else {
            set_notice(app, "当前剧情没有开放此号码的通话");
        }
        return;
    }
    if (app->phone_view == SG0_PHONE_CALLS) {
        if (!app->state.rine.pending_valid) { phone_back(app); return; }
        if (index == 0) {
            if (!sg0_engine_answer_pending(&app->pack, &app->state, true)) {
                set_notice(app, "来电状态已变化");
                return;
            }
            (void)load_op(app, 0);
            return;
        }
        set_notice(app, "已保留来电，可稍后处理");
        phone_back(app);
        return;
    }
    if (app->phone_view == SG0_PHONE_REPLY) {
        uint16_t base = 0, first = 0, key = 0;
        uint8_t contact = 0, choices = 0;
        uint16_t reply_index = 0xFFFFu;
        for (uint8_t i = 0; i < app->pack.sec[SG0_SEC_RREPLY].count; ++i) {
            if (!sg0_pack_rine_reply(&app->pack, i, &base, &contact, &choices, &first, &key)) continue;
            if (base == app->phone_thread_base) { reply_index = i; break; }
        }
        if (reply_index == 0xFFFFu) return;
        if (!sg0_engine_send_reply(&app->pack, &app->state, reply_index, (uint8_t)index)) {
            set_notice(app, "这条消息已经过了回复时机");
            return;
        }
        // Rebuild the thread from the updated delivery so reply/sequence rows
        // join the existing thread instead of leaving a stale count/part list.
        phone_thread_open(app, base, app->phone_thread_date, app->phone_thread_sender,
                          app->phone_thread_sent != 0);
    }
}

// -------------------------------------------------------------------- notices
static void set_notice(steins0_app_t *app, const char *text)
{
    snprintf(app->notice, sizeof(app->notice), "%s", text ? text : "");
    app->notice_ms = NOTICE_MS;
    steins0_ui_notice(&app->ui, app->notice);
}

// ------------------------------------------------------------------ 游戏动作
static void game_advance(steins0_app_t *app)
{
    if (!app->game_started || app->state.ended || app->state.choice_active) return;
    // 不在这里清 auto:自动 tick 也走本函数。手动 key_click/key_long 负责停 auto。
    if (app->typing) {
        app->typing = false;
        app->type_pos = app->pages.page_start[app->page_index + 1];
        render_page(app);
        return;
    }
    if (app->page_index + 1 < app->pages.pages) {
        app->page_index++;
        app->type_pos = app->pages.page_start[app->page_index];
        app->typing = app->settings.speed < 3;
        app->type_accum = 0;
        render_page(app);
        return;
    }
    if (!sg0_engine_next(&app->pack, &app->state)) return;
    (void)load_op(app, 0);
}

static void game_choice_confirm(steins0_app_t *app)
{
    if (!app->state.choice_active) return;
    if (!sg0_engine_choose(&app->pack, &app->state, (uint8_t)app->choice_sel)) {
        set_notice(app, "选项状态已变化");
        app->state.choice_active = 0;
        (void)load_op(app, 0);
        return;
    }
    app->state.choice_active = 0;
    steins0_ui_choice_hide(&app->ui);
    (void)load_op(app, 0);
}

static void open_or_advance(steins0_app_t *app)
{
    if (app->state.choice_active) {
        game_choice_confirm(app);
    } else {
        game_advance(app);
    }
}

static void card_dismiss(steins0_app_t *app)
{
    if (!app->card_pending) return;
    app->card_pending = 0;
    app->card_ms = 0;
    const uint8_t page = app->page_index;
    app->force_scene = true;
    (void)load_op(app, page);
}

// ---------------------------------------------------------------------- keys
static void key_long(steins0_app_t *app, uint8_t btn)
{
    switch (app->page) {
    case SG0_PAGE_GAME:
        if (app->state.choice_active) {
            open_menu(app);
            return;
        }
        if (btn == BSP_BTN_OK) {
            app->auto_mode = false;
            open_menu(app);
        } else if (btn == BSP_BTN_UP) {
            app->auto_mode = false;
            if (!app->state.ended) app->fast_forward = true;
        } else if (btn == BSP_BTN_DOWN) {
            app->auto_mode = !app->auto_mode;
            app->auto_accum = 0;
            if (app->auto_mode) app->fast_forward = false;
        }
        break;
    case SG0_PAGE_LIST:
        list_back(app);
        break;
    case SG0_PAGE_CG:
        if (btn == BSP_BTN_OK) {
            open_list(app, SG0_LIST_GALLERY, SG0_PAGE_TITLE);
            app->list_sel = app->cg_group;
            build_list(app);
        }
        break;
    case SG0_PAGE_HISTORY:
        if (btn == BSP_BTN_OK) history_back(app);
        break;
    case SG0_PAGE_ABOUT:
        if (btn == BSP_BTN_OK) {
            goto_page(app, app->page_return);
            build_list(app);
        }
        break;
    case SG0_PAGE_ENDING:
        refresh_title(app);
        break;
    case SG0_PAGE_CONFIRM:
        if (btn == BSP_BTN_OK) {
            goto_page(app, app->confirm_return);
            build_list(app);
        }
        break;
    case SG0_PAGE_CARD:
        card_dismiss(app);
        break;
    case SG0_PAGE_PHONE:
    case SG0_PAGE_PHONE_THREAD:
        if (btn == BSP_BTN_OK) phone_back(app);
        break;
    default:
        break;
    }
}

static void key_click(steins0_app_t *app, uint8_t btn)
{
    switch (app->page) {
    case SG0_PAGE_TITLE:
        if (btn == BSP_BTN_UP) {
            app->title_sel = (app->title_sel + 5) % 6;
            refresh_title(app);
        } else if (btn == BSP_BTN_DOWN) {
            app->title_sel = (app->title_sel + 1) % 6;
            refresh_title(app);
        } else {
            title_activate(app, app->title_sel);
        }
        break;
    case SG0_PAGE_GAME:
        app->auto_mode = false;
        app->fast_forward = false;
        if (app->state.choice_active) {
            if (btn == BSP_BTN_UP) {
                app->choice_sel = (app->choice_sel + 1) % 2;
                refresh_game_ui(app);
            } else if (btn == BSP_BTN_DOWN) {
                app->choice_sel = (app->choice_sel + 1) % 2;
                refresh_game_ui(app);
            } else {
                game_choice_confirm(app);
            }
        } else if (btn == BSP_BTN_UP) {
            game_advance(app);
        } else if (btn == BSP_BTN_DOWN) {
            open_history(app);
        } else {
            open_or_advance(app);
        }
        break;
    case SG0_PAGE_LIST:
        if (btn == BSP_BTN_UP) {
            list_move(app, -1);
        } else if (btn == BSP_BTN_DOWN) {
            list_move(app, 1);
        } else {
            list_activate(app);
        }
        break;
    case SG0_PAGE_CG:
        if (btn == BSP_BTN_UP) {
            gallery_variant(app, 1);
        } else if (btn == BSP_BTN_DOWN) {
            gallery_variant(app, -1);
        } else {
            open_list(app, SG0_LIST_GALLERY, SG0_PAGE_TITLE);
            app->list_sel = app->cg_group;
            build_list(app);
        }
        break;
    case SG0_PAGE_HISTORY:
        if (btn == BSP_BTN_UP) steins0_ui_scroll(&app->ui, SG0_PAGE_HISTORY, 19);
        else if (btn == BSP_BTN_DOWN) steins0_ui_scroll(&app->ui, SG0_PAGE_HISTORY, -19);
        else history_back(app);
        break;
    case SG0_PAGE_ABOUT:
        if (btn == BSP_BTN_UP) steins0_ui_scroll(&app->ui, SG0_PAGE_ABOUT, 19);
        else if (btn == BSP_BTN_DOWN) steins0_ui_scroll(&app->ui, SG0_PAGE_ABOUT, -19);
        else {
            goto_page(app, app->page_return);
            build_list(app);
        }
        break;
    case SG0_PAGE_ENDING:
        refresh_title(app);
        break;
    case SG0_PAGE_CARD:
        card_dismiss(app);
        break;
    case SG0_PAGE_PHONE:
        if (btn == BSP_BTN_UP) phone_move(app, -1);
        else if (btn == BSP_BTN_DOWN) phone_move(app, 1);
        else phone_activate(app);
        break;
    case SG0_PAGE_PHONE_THREAD:
        if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
            if (app->phone_thread_count > 0) {
                int part = (int)app->phone_thread_part + (btn == BSP_BTN_UP ? -1 : 1);
                if (part < 0) part = app->phone_thread_count - 1;
                if (part >= app->phone_thread_count) part = 0;
                app->phone_thread_part = (uint16_t)part;
                phone_thread_render(app);
            }
        } else {
            phone_thread_reply(app);
        }
        break;
    case SG0_PAGE_CONFIRM:
        if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
            app->confirm_sel = btn == BSP_BTN_UP ? 0 : 1;
            steins0_ui_confirm(&app->ui, app->confirm_prompt, app->confirm_yes,
                               app->confirm_no, app->confirm_sel);
        } else if (app->confirm_sel == 0) {
            if (app->confirm_action == SG0_CONFIRM_OVERWRITE) {
                do_save(app, app->confirm_slot);
            } else if (app->confirm_action == SG0_CONFIRM_TITLE) {
                maybe_autosave(app, true);
                (void)write_unlock(app);
                app->game_started = false;
                refresh_title(app);
            }
        } else {
            goto_page(app, app->confirm_return);
            build_list(app);
        }
        break;
    default:
        break;
    }
}

void steins0_app_key(steins0_app_t *app, const steins0_key_t *key)
{
    if (!app || !key) return;
    app->idle_ms = 0;
    if (app->page != SG0_PAGE_GAME) {
        app->fast_forward = false;
        app->auto_mode = false;
    }
    switch (key->ev) {
    case BSP_BTN_PRESS:
        app->swallow_click = false;
        break;
    case BSP_BTN_LONG:
        app->swallow_click = true;
        key_long(app, key->btn);
        break;
    case BSP_BTN_CLICK:
        if (app->swallow_click) {
            app->swallow_click = false;
            break;
        }
        key_click(app, key->btn);
        break;
    case BSP_BTN_RELEASE:
        if (key->btn == BSP_BTN_UP) app->fast_forward = false;
        break;
    default:
        break;
    }
}

// --------------------------------------------------------------------- tick
static uint32_t type_delay(const steins0_app_t *app)
{
    switch (app->settings.speed) {
    case 0: return TYPE_MS_SLOW;
    case 2: return TYPE_MS_FAST;
    case 3: return 0;
    default: return TYPE_MS_NORMAL;
    }
}

static void tick_typing(steins0_app_t *app, uint32_t elapsed)
{
    if (!app->typing) return;
    const uint16_t page_end = app->pages.page_start[app->page_index + 1];
    app->type_accum += elapsed;
    const uint32_t delay = type_delay(app);
    bool changed = false;
    while (app->type_accum >= delay && app->type_pos < page_end) {
        app->type_accum -= delay;
        size_t pos = app->type_pos;
        (void)sg0_utf8_next(app->op_text, page_end, &pos);
        app->type_pos = (uint32_t)pos;
        changed = true;
    }
    if (app->type_pos >= page_end) app->typing = false;
    if (changed) render_page(app);
}

void steins0_app_tick(steins0_app_t *app, uint32_t elapsed_ms)
{
    if (!app) return;
    if (app->auto_mode || app->fast_forward) app->idle_ms = 0;
    else if (app->idle_ms < 0xFFFFFFFFu - elapsed_ms) app->idle_ms += elapsed_ms;
    if (app->notice_ms > elapsed_ms) {
        app->notice_ms -= elapsed_ms;
    } else if (app->notice_ms) {
        app->notice_ms = 0;
        steins0_ui_notice_hide(&app->ui);
    }
    app->battery_accum += elapsed_ms;
    if (app->battery_accum >= 30000u) {
        app->battery_accum = 0;
        app->battery = bsp_battery_soc();
        steins0_ui_battery(&app->ui, app->battery);
        steins0_ui_title_battery(&app->ui, app->battery);
    }
    app->autosave_accum += elapsed_ms;
    if (app->unlock_dirty) {
        app->unlock_retry_accum += elapsed_ms;
        if (app->unlock_retry_accum >= UNLOCK_RETRY_MS) {
            app->unlock_retry_accum = 0;
            if (app->unlock_retry < 255) ++app->unlock_retry;
            (void)write_unlock(app);
        }
    }
    if (app->page == SG0_PAGE_CARD) {
        if (app->card_ms > elapsed_ms) app->card_ms -= elapsed_ms;
        else {
            app->card_ms = 0;
            card_dismiss(app);
        }
        return;
    }
    if (app->page != SG0_PAGE_GAME || !app->game_started) return;
    if (app->state.choice_active) return;
    if (app->fast_forward) {
        app->ff_accum += elapsed_ms;
        if (app->ff_accum >= FF_MS) {
            app->ff_accum = 0;
            app->typing = false;
            if (app->page_index + 1 < app->pages.pages) {
                app->page_index++;
                app->type_pos = app->pages.page_start[app->page_index];
                render_page(app);
            } else if (sg0_engine_next(&app->pack, &app->state)) {
                (void)load_op(app, 0);
            }
        }
    } else {
        app->ff_accum = 0;
    }
    tick_typing(app, elapsed_ms);
    if (app->auto_mode && !app->typing && !app->state.choice_active) {
        uint32_t wait = app->settings.auto_wait == 0 ? AUTO_MS_SLOW
                       : app->settings.auto_wait == 2 ? AUTO_MS_FAST : AUTO_MS_NORMAL;
        wait += (uint32_t)strlen(app->op_text) * 18u;
        app->auto_accum += elapsed_ms;
        if (app->auto_accum >= wait) {
            app->auto_accum = 0;
            game_advance(app);
        }
    } else {
        app->auto_accum = 0;
    }
}

uint32_t steins0_app_idle_ms(const steins0_app_t *app)
{
    if (app && (app->auto_mode || app->fast_forward)) return 0;   // 自动/快进不休眠
    return app ? app->idle_ms : 0;
}

void steins0_app_flush(steins0_app_t *app)
{
    if (!app) return;
    maybe_autosave(app, true);
    (void)write_unlock(app);
    save_settings(app);
}

void steins0_app_show_sleeping(steins0_app_t *app)
{
    if (!app) return;
    steins0_ui_notice(&app->ui, "已休眠 - 按任意键唤醒");
}

uint8_t steins0_app_line_count(const steins0_app_t *app)
{
    return app ? app->line_count : 0;
}

uint8_t steins0_app_page_index(const steins0_app_t *app)
{
    return app ? app->page_index : 0;
}

// ---------------------------------------------------------------------- init
bool steins0_app_init(steins0_app_t *app, const uint8_t *pack_data, uint32_t pack_size)
{
    if (!app) return false;
    memset(app, 0, sizeof(*app));
    app->font = &steins0_font16;
    app->page = SG0_PAGE_TITLE;
    app->last_view = 0xFFFFu;
    app->last_history_node = 0xFFu;
    app->last_history_cursor = 0xFFFFu;
    app->last_card = 0xFFu;
    app->state.event_node = 0xFFu;
    app->battery = bsp_battery_soc();
    app->pack_ok = sg0_pack_open(&app->pack, pack_data, pack_size);
    if (!app->pack_ok) ESP_LOGE(TAG, "资源包打开失败(%u 字节)", (unsigned)pack_size);
    app->nvs_ok = nvs_probe();
    if (!app->nvs_ok) ESP_LOGW(TAG, "NVS 不可用:存档只读/不落盘");
    load_settings(app);
    sg0_unlock_init(&app->unlock);
    if (app->pack_ok) {
        load_unlock(app);
        load_slots(app);
        load_auto(app);
        const uint16_t title_asset = sg0_pack_meta_u16(&app->pack, "title_asset", 0xFFFFu);
        app->title_art_ok = title_asset != 0xFFFFu &&
                            steins0_image_validate_asset(&app->pack, title_asset);
    } else {
        sg0_unlock_set_chapter(&app->unlock, 0);
    }
    if (app->pack_ok) {
        // 源章节页按 script 排序(组内);解锁位仍用包内 START 下标。
        app->start_order_count = 0;
        for (uint16_t i = 0; i < SG0_MAX_STARTS; ++i) {
            sg0_start_t st;
            if (!sg0_pack_start(&app->pack, i, &st)) break;
            uint8_t pos = app->start_order_count;
            while (pos > 0) {
                sg0_start_t prev;
                if (!sg0_pack_start(&app->pack, app->start_order[pos - 1], &prev)) break;
                if (prev.script <= st.script) break;
                app->start_order[pos] = app->start_order[pos - 1];
                --pos;
            }
            app->start_order[pos] = i;
            ++app->start_order_count;
        }
    }
    if (!steins0_ui_create(&app->ui, lv_screen_active(), app->pixels, app->stamp_pixels,
                               app->font)) {
        ESP_LOGE(TAG, "界面创建失败");
        return false;
    }
    refresh_title(app);
    if (!app->pack_ok) set_notice(app, "资源包损坏，无法开始游戏");
    else if (!app->nvs_ok) set_notice(app, "存储不可用:存档不会保存");
    return true;
}
