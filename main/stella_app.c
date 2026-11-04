// main/stella_app.c —— 应用状态机与 NVS 持久化。
#include "stella_app.h"

#include "bsp_battery.h"
#include "bsp_button.h"
#include "stella_model.h"

#include "esp_log.h"
#include "nvs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "stella";

LV_FONT_DECLARE(stella_cjk_16);
LV_FONT_DECLARE(stella_cjk_20);
#define STELLA_TITLE_ASSET 0u
#define NVS_NS "stella"
#define NVS_NS_AUTO "stella_auto"
#define KEY_AUTO "auto"
#define KEY_CONFIG "cfg"
#define KEY_UNLOCK "unlock"
#define UNLOCK_RETRY_MAX 5u

static const stella_layout_t *layout_for(const stella_app_t *app)
{
    return app->settings.large ? &STELLA_LAYOUT_20 : &STELLA_LAYOUT_16;
}

static void set_notice(stella_app_t *app, const char *text)
{
    snprintf(app->notice, sizeof(app->notice), "%s", text ? text : "");
    app->notice_ms = 1800;
    stella_ui_notice(&app->ui, app->notice);
}

static bool nvs_write_ns(const char *ns, const char *key, const void *data, size_t len)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(h, key, data, len);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

static bool nvs_read_ns(const char *ns, const char *key, void *data, size_t len)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return false;
    size_t actual = len;
    const esp_err_t err = nvs_get_blob(h, key, data, &actual);
    nvs_close(h);
    return err == ESP_OK && actual == len;
}

static bool nvs_write(const char *key, const void *data, size_t len)
{
    return nvs_write_ns(NVS_NS, key, data, len);
}

static bool nvs_read(const char *key, void *data, size_t len)
{
    return nvs_read_ns(NVS_NS, key, data, len);
}

static bool read_save_key(const char *key, stella_save_t *save)
{
    uint8_t raw[STELLA_SAVE_BYTES];
    return nvs_read(key, raw, sizeof(raw)) && stella_save_decode(save, raw, sizeof(raw));
}

static bool read_auto_key(stella_save_t *save)
{
    uint8_t raw[STELLA_SAVE_BYTES];
    return nvs_read_ns(NVS_NS_AUTO, KEY_AUTO, raw, sizeof(raw)) &&
           stella_save_decode(save, raw, sizeof(raw));
}

static bool write_auto_key(const stella_save_t *save)
{
    uint8_t raw[STELLA_SAVE_BYTES];
    if (stella_save_encode(save, raw, sizeof(raw)) != STELLA_SAVE_BYTES) return false;
    return nvs_write_ns(NVS_NS_AUTO, KEY_AUTO, raw, sizeof(raw));
}

static bool write_save_key(const char *key, const stella_save_t *save)
{
    uint8_t raw[STELLA_SAVE_BYTES];
    if (stella_save_encode(save, raw, sizeof(raw)) != STELLA_SAVE_BYTES) return false;
    return nvs_write(key, raw, sizeof(raw));
}

static void save_cfg(stella_app_t *app)
{
    uint8_t cfg[4] = {1, app->settings.large, app->settings.speed, app->settings.auto_wait};
    (void)nvs_write(KEY_CONFIG, cfg, sizeof(cfg));
}

static void load_cfg(stella_app_t *app)
{
    uint8_t cfg[4];
    app->settings.large = 0;
    app->settings.speed = 1;
    app->settings.auto_wait = 1;
    if (nvs_read(KEY_CONFIG, cfg, sizeof(cfg)) && cfg[0] == 1) {
        app->settings.large = cfg[1] ? 1 : 0;
        app->settings.speed = cfg[2] <= 3 ? cfg[2] : 1;
        app->settings.auto_wait = cfg[3] <= 2 ? cfg[3] : 1;
    }
}

static void save_unlock(stella_app_t *app, bool force)
{
    uint8_t blob[1 + sizeof(stella_unlock_t)];
    blob[0] = 1;
    memcpy(blob + 1, &app->unlock, sizeof(app->unlock));
    if (nvs_write(KEY_UNLOCK, blob, sizeof(blob))) {
        app->unlock_dirty = false;
        app->unlock_retry_ms = 0;
        app->unlock_retry_count = 0;
    } else {
        app->unlock_dirty = true;
        if (force) app->unlock_retry_count = 0;
        else if (app->unlock_retry_count < 255u) app->unlock_retry_count++;
        app->unlock_retry_ms = 0;
    }
}

static void load_unlock(stella_app_t *app)
{
    stella_unlock_init(&app->unlock);
    uint8_t blob[1 + sizeof(stella_unlock_t)];
    if (nvs_read(KEY_UNLOCK, blob, sizeof(blob)) && blob[0] == 1) {
        memcpy(&app->unlock, blob + 1, sizeof(app->unlock));
    }
    app->unlock_dirty = false;
    app->unlock_retry_ms = 0;
    app->unlock_retry_count = 0;
}

static void slot_key(int index, char *key, size_t cap)
{
    snprintf(key, cap, "s%d", index);
}

static bool recoverable_op(const stella_app_t *app, const stella_scene_t *scene,
                           uint32_t op_index, uint16_t page)
{
    if (!scene || op_index >= scene->op_count || page >= STELLA_MAX_PAGES) return false;
    uint16_t n = 0;
    const uint8_t *ops = stella_pack_ops(&app->pack, scene, &n);
    if (!ops || op_index >= n) return false;
    stella_op_t op;
    if (!stella_pack_op(ops, n, (uint16_t)op_index, &op)) return false;
    if (op.type == STELLA_OP_CHOICE) {
        if (page != 0 || op.arg >= n) return false;
        stella_op_t target;
        if (!stella_pack_op(ops, n, op.arg, &target)) return false;
        return target.type == STELLA_OP_TEXT || target.type == STELLA_OP_CHOICE;
    }
    if (op.type != STELLA_OP_TEXT) return false;
    char text[STELLA_TEXT_BYTES];
    if (!stella_pack_text_copy(&app->pack, op.text_off, op.text_len, text, sizeof(text))) {
        return false;
    }
    uint32_t offs[STELLA_MAX_PAGES + 1];
    const int pages = stella_text_pages(text, layout_for(app), offs, STELLA_MAX_PAGES + 1);
    return pages >= 1 && page < pages;
}

static bool save_valid(const stella_app_t *app, const stella_save_t *save)
{
    uint16_t idx;
    stella_scene_t scene;
    if (!save || (save->mode != STELLA_MODE_STORY && save->mode != STELLA_MODE_CHAPTER)) {
        return false;
    }
    if (!stella_pack_find_scene(&app->pack, save->scene, &idx)) return false;
    if (!stella_pack_scene(&app->pack, idx, &scene)) return false;
    return recoverable_op(app, &scene, save->op, save->page);
}

static void set_checkpoint(stella_app_t *app, uint16_t scene, uint32_t op, uint16_t page)
{
    if (!app) return;
    app->checkpoint.scene = scene;
    app->checkpoint.op = op;
    app->checkpoint.page = page;
    app->checkpoint.mode = app->mode;
    app->checkpoint.flags = app->unlock.flags;
    app->checkpoint_valid = save_valid(app, &app->checkpoint);
    if (app->checkpoint_valid) app->progress_dirty = true;
}

static void slot_desc(const stella_app_t *app, const stella_save_t *save, char *out, size_t cap)
{
    uint16_t idx;
    stella_scene_t scene;
    char name[64] = "";
    if (!stella_pack_find_scene(&app->pack, save->scene, &idx) ||
        !stella_pack_scene(&app->pack, idx, &scene)) {
        snprintf(out, cap, "无效存档");
        return;
    }
    stella_pack_name_copy(&app->pack, scene.name, name, sizeof(name));
    snprintf(out, cap, "%s · %u", name, (unsigned)(save->op + 1));
}

static void save_slot(stella_app_t *app, int index)
{
    if (!app->checkpoint_valid) {
        set_notice(app, "当前没有可保存的阅读停点");
        return;
    }
    stella_save_t save = app->checkpoint;
    save.flags = app->unlock.flags;
    save.mode = app->mode;
    if (!save_valid(app, &save)) {
        set_notice(app, "存档位置无效");
        return;
    }
    char key[8];
    slot_key(index, key, sizeof(key));
    if (!write_save_key(key, &save)) {
        set_notice(app, "保存失败");
        return;
    }
    app->slots[index] = save;
    app->slot_valid[index] = true;
    set_notice(app, "已保存");
    app->progress_dirty = false;
}

static void load_save(stella_app_t *app, const stella_save_t *save);

static void save_auto_now(stella_app_t *app)
{
    if (!app->checkpoint_valid || app->ended) return;
    stella_save_t save = app->checkpoint;
    save.flags = app->unlock.flags;
    save.mode = app->mode;
    if (!save_valid(app, &save)) return;
    if (write_auto_key(&save)) {
        app->autosave = save;
        app->have_auto = true;
        app->progress_dirty = false;
    } else {
        app->progress_dirty = true;  // 失败保留旧档并等待下次重试
    }
}

static void load_auto(stella_app_t *app)
{
    if (!app->have_auto || !save_valid(app, &app->autosave)) {
        set_notice(app, "没有可继续的进度");
        return;
    }
    load_save(app, &app->autosave);
}

static void refresh_title(stella_app_t *app)
{
    static const char *labels[7];
    labels[0] = app->have_auto ? "继续阅读" : "开始故事";
    labels[1] = "开始故事";
    labels[2] = stella_unlock_has_flag(&app->unlock, STELLA_FLAG_MAIN_CLEAR)
                    ? "后日谈·日记" : "后日谈（未解锁）";
    labels[3] = "章节选择";
    labels[4] = "读取存档";
    labels[5] = "CG 鉴赏";
    labels[6] = "设置";
    stella_ui_set_title_rows(&app->ui, labels, 7, app->title_sel);
}

static void refresh_menu(stella_app_t *app)
{
    static const char *labels[6] = {
        "返回阅读", "保存进度", "读取进度", "历史记录", "设置", "返回标题",
    };
    stella_ui_set_list(&app->ui, "游戏菜单", labels, NULL, 6, app->menu_sel);
}

static void refresh_settings(stella_app_t *app)
{
    static const char *labels[4] = {"字号", "文字速度", "自动等待", "返回"};
    static const char *large_values[2] = {"16px", "20px"};
    static const char *speed_values[4] = {"慢", "中", "快", "瞬间"};
    static const char *auto_values[3] = {"慢 (2.6s)", "标准 (1.5s)", "快 (0.8s)"};
    const char *values[4] = {
        large_values[app->settings.large ? 1 : 0],
        speed_values[app->settings.speed],
        auto_values[app->settings.auto_wait],
        "",
    };
    stella_ui_set_list(&app->ui, "设置", labels, values, 4, app->settings_sel);
}

static void refresh_slots(stella_app_t *app)
{
    for (int i = 0; i < STELLA_LIST_ROWS; ++i) {
        app->row_text[i][0] = '\0';
        app->row_value[i][0] = '\0';
    }
    snprintf(app->row_text[0], sizeof(app->row_text[0]), "自动存档");
    snprintf(app->row_value[0], sizeof(app->row_value[0]), "%s",
             app->have_auto ? "可读取" : "空");
    for (int i = 0; i < STELLA_SLOT_COUNT; ++i) {
        snprintf(app->row_text[i + 1], sizeof(app->row_text[i + 1]), "存档 %d", i + 1);
        if (app->slot_valid[i]) slot_desc(app, &app->slots[i], app->row_value[i + 1],
                                          sizeof(app->row_value[i + 1]));
        else snprintf(app->row_value[i + 1], sizeof(app->row_value[i + 1]), "空");
    }
    for (int i = 0; i < STELLA_LIST_ROWS; ++i) {
        app->row_ptrs[i] = app->row_text[i];
        app->value_ptrs[i] = app->row_value[i];
    }
    stella_ui_set_list(&app->ui, app->slot_action ? "读取进度" : "保存进度",
                       app->row_ptrs, app->value_ptrs, STELLA_LIST_ROWS, app->slot_sel);
}

static int chapter_count(const stella_app_t *app)
{
    int count = 0;
    for (uint16_t i = 0; i < app->pack.scene_count; ++i) {
        stella_scene_t scene;
        if (stella_pack_scene(&app->pack, i, &scene) && scene.route != STELLA_ROUTE_LEGACY) count++;
    }
    return count;
}

static uint16_t chapter_at(const stella_app_t *app, int index)
{
    int seen = 0;
    for (uint16_t i = 0; i < app->pack.scene_count; ++i) {
        stella_scene_t scene;
        if (!stella_pack_scene(&app->pack, i, &scene) || scene.route == STELLA_ROUTE_LEGACY) continue;
        if (seen++ == index) return i;
    }
    return 0xFFFF;
}

static bool chapter_locked(const stella_app_t *app, const stella_scene_t *scene)
{
    if (scene->flags & STELLA_SCENE_MAIN_CLEAR) {
        if (!stella_unlock_has_flag(&app->unlock, STELLA_FLAG_MAIN_CLEAR)) return true;
    }
    if (scene->flags & STELLA_SCENE_AFTER_CLEAR) {
        if (!stella_unlock_has_flag(&app->unlock, STELLA_FLAG_AFTER_CLEAR)) return true;
    }
    return false;
}

static void refresh_chapters(stella_app_t *app)
{
    const int total = chapter_count(app);
    const int pages = (total + 5) / 6;
    if (app->chapter_page >= pages) app->chapter_page = pages ? pages - 1 : 0;
    for (int i = 0; i < STELLA_LIST_ROWS; ++i) {
        app->row_text[i][0] = '\0';
        app->row_value[i][0] = '\0';
    }
    for (int i = 0; i < 6; ++i) {
        const int index = app->chapter_page * 6 + i;
        if (index >= total) break;
        const uint16_t scene_index = chapter_at(app, index);
        stella_scene_t scene;
        if (scene_index == 0xFFFF || !stella_pack_scene(&app->pack, scene_index, &scene)) continue;
        stella_pack_name_copy(&app->pack, scene.name, app->row_text[i], sizeof(app->row_text[i]));
        if (chapter_locked(app, &scene)) snprintf(app->row_value[i], sizeof(app->row_value[i]), "锁");
        else if (stella_unlock_has_scene(&app->unlock, scene.id)) {
            snprintf(app->row_value[i], sizeof(app->row_value[i]), "已读");
        } else {
            app->row_value[i][0] = 0;
        }
    }
    snprintf(app->row_text[6], sizeof(app->row_text[6]), "上一页");
    snprintf(app->row_text[7], sizeof(app->row_text[7]), "下一页");
    for (int i = 0; i < STELLA_LIST_ROWS; ++i) {
        app->row_ptrs[i] = app->row_text[i];
        app->value_ptrs[i] = app->row_value[i];
    }
    char title[64];
    snprintf(title, sizeof(title), "章节选择 · %d/%d", app->chapter_page + 1, pages ? pages : 1);
    stella_ui_set_list(&app->ui, title, app->row_ptrs, app->value_ptrs,
                       STELLA_LIST_ROWS, app->chapter_sel);
}

static void refresh_gallery(stella_app_t *app)
{
    const int pages = (app->pack.group_count + 5) / 6;
    if (app->gallery_page >= pages) app->gallery_page = pages ? pages - 1 : 0;
    for (int i = 0; i < STELLA_LIST_ROWS; ++i) {
        app->row_text[i][0] = '\0';
        app->row_value[i][0] = '\0';
    }
    for (int i = 0; i < 6; ++i) {
        const int index = app->gallery_page * 6 + i;
        if (index >= (int)app->pack.group_count) break;
        stella_group_t group;
        if (!stella_pack_group(&app->pack, (uint16_t)index, &group)) continue;
        stella_pack_name_copy(&app->pack, group.label, app->row_text[i], sizeof(app->row_text[i]));
        snprintf(app->row_value[i], sizeof(app->row_value[i]), "%s",
                 stella_unlock_has_gallery(&app->unlock, (uint16_t)index) ? "已阅" : "未阅");
    }
    snprintf(app->row_text[6], sizeof(app->row_text[6]), "上一页");
    snprintf(app->row_text[7], sizeof(app->row_text[7]), "下一页");
    for (int i = 0; i < STELLA_LIST_ROWS; ++i) {
        app->row_ptrs[i] = app->row_text[i];
        app->value_ptrs[i] = app->row_value[i];
    }
    char title[64];
    snprintf(title, sizeof(title), "CG 鉴赏 · %d/%d", app->gallery_page + 1, pages ? pages : 1);
    stella_ui_set_list(&app->ui, title, app->row_ptrs, app->value_ptrs,
                       STELLA_LIST_ROWS, app->gallery_sel);
}

static void build_history(stella_app_t *app)
{
    app->ui.history_buf[0] = '\0';
    size_t used = 0;
    for (int i = 0; i < app->history_count; ++i) {
        const int index = (app->history_count < STELLA_HISTORY_COUNT)
                              ? i : (app->history_next + i) % STELLA_HISTORY_COUNT;
        const stella_history_t *h = &app->history[index];
        char line[STELLA_HISTORY_TEXT + 48];
        if (h->speaker[0]) snprintf(line, sizeof(line), "%s\n%s\n\n", h->speaker, h->text);
        else snprintf(line, sizeof(line), "%s\n\n", h->text);
        const size_t len = strlen(line);
        if (used + len + 1 >= sizeof(app->ui.history_buf)) break;
        memcpy(app->ui.history_buf + used, line, len);
        used += len;
        app->ui.history_buf[used] = '\0';
    }
    stella_ui_set_history(&app->ui, app->ui.history_buf);
}

static void push_history(stella_app_t *app)
{
    if (app->last_hist_scene == app->scene.id && app->last_hist_op == app->op) return;
    stella_history_t *h = &app->history[app->history_next];
    size_t n = strlen(app->speaker);
    if (n >= sizeof(h->speaker)) n = sizeof(h->speaker) - 1;
    memcpy(h->speaker, app->speaker, n);
    h->speaker[n] = 0;
    n = strlen(app->op_text);
    if (n >= sizeof(h->text)) n = sizeof(h->text) - 1;
    memcpy(h->text, app->op_text, n);
    h->text[n] = 0;
    app->last_hist_scene = app->scene.id;
    app->last_hist_op = app->op;
    app->history_next = (app->history_next + 1) % STELLA_HISTORY_COUNT;
    if (app->history_count < STELLA_HISTORY_COUNT) app->history_count++;
}

static void show_page_text(stella_app_t *app)
{
    const uint32_t start = app->page_off[app->page_no];
    const uint32_t end = app->page_off[app->page_no + 1];
    size_t len = end > start ? end - start : 0;
    if (len >= sizeof(app->page_text)) len = sizeof(app->page_text) - 1;
    memcpy(app->page_text, app->op_text + start, len);
    app->page_text[len] = '\0';
    app->typing = app->settings.speed != 3;
    app->typing_pos = 0;
    app->shown[0] = '\0';
    stella_ui_set_text(&app->ui, app->typing ? "" : app->page_text);
    app->auto_accum = 0;
}

static void show_ending(stella_app_t *app, const char *label)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s\n\n阅读完毕\n\nOK 返回标题", label ? label : "阅读完毕");
    stella_ui_set_ending(&app->ui, buf);
    app->page = STELLA_PAGE_ENDING;
    stella_ui_show(&app->ui, app->page);
    app->ended = true;
    app->checkpoint_valid = false;
    app->progress_dirty = false;
    app->have_auto = false;
}

static bool process_current(stella_app_t *app);

static bool enter_scene(stella_app_t *app, uint16_t scene_id, uint32_t op, uint16_t page)
{
    uint16_t index;
    stella_scene_t scene;
    if (!stella_pack_find_scene(&app->pack, scene_id, &index) ||
        !stella_pack_scene(&app->pack, index, &scene)) {
        set_notice(app, "场景不存在");
        return false;
    }
    uint16_t count = 0;
    app->ops = stella_pack_ops(&app->pack, &scene, &count);
    if (!app->ops || count == 0 || op >= count) {
        set_notice(app, "剧本读取失败");
        return false;
    }
    app->scene_index = index;
    app->scene = scene;
    app->op_count = count;
    app->op = (uint16_t)op;
    app->page_no = page;
    app->page_count = 1;
    app->at_choice = false;
    app->ended = false;
    app->scene_image_failed = false;
    app->op_text[0] = 0;
    app->page_text[0] = 0;
    app->speaker[0] = 0;
    app->chapter[0] = 0;
    app->scene_asset_bg = STELLA_NONE;
    app->scene_body_count = 0;
    memset(app->scene_bodies, 0, sizeof(app->scene_bodies));
    const bool shown = process_current(app);
    if (shown) {
        stella_unlock_set_scene(&app->unlock, scene_id);
        save_unlock(app, false);
    }
    if (app->page != STELLA_PAGE_ENDING && app->page_no > 0 && app->page_count > 0) {
        if (app->page_no >= app->page_count) app->page_no = app->page_count - 1;
        show_page_text(app);
        if (!app->scene_image_failed) set_checkpoint(app, app->scene.id, app->op, app->page_no);
    }
    return true;
}
static bool process_current(stella_app_t *app)
{
    if (app->op >= app->op_count) {
        if (app->scene.next != STELLA_NONE) {
            return enter_scene(app, app->scene.next, 0, 0);
        }
        set_notice(app, "本幕已结束");
        return false;
    }
    stella_op_t op;
    if (!stella_pack_op(app->ops, app->op_count, app->op, &op)) {
        set_notice(app, "剧本指令无效");
        return false;
    }
    if (op.type == STELLA_OP_TEXT) {
        app->at_choice = false;
        app->scene_image_failed = false;
        stella_pack_text_copy(&app->pack, op.text_off, op.text_len, app->op_text,
                              sizeof(app->op_text));
        stella_pack_name_copy(&app->pack, op.speaker, app->speaker, sizeof(app->speaker));
        stella_pack_name_copy(&app->pack, app->scene.name, app->chapter, sizeof(app->chapter));
        app->page_count = (uint16_t)stella_text_pages(app->op_text, layout_for(app),
                                                      app->page_off, STELLA_MAX_PAGES + 1);
        if (app->page_no >= app->page_count) app->page_no = app->page_count - 1;
        app->scene_asset_bg = op.bg;
        app->scene_body_count = op.body_count;
        memcpy(app->scene_bodies, op.body, sizeof(app->scene_bodies));
        const bool image_ok = stella_image_show_scene(&app->ui.image, &app->pack, op.bg,
                                                       op.body, op.body_count);
        if (!image_ok) {
            app->scene_image_failed = true;
            stella_image_clear(&app->ui.image, STELLA_IMAGE_THEME_TOP, STELLA_IMAGE_THEME_BOTTOM);
            set_notice(app, "图片解码失败");
        }
        stella_ui_set_game(&app->ui, app->speaker, "", app->settings.large != 0,
                           app->chapter, app->battery);
        app->page = STELLA_PAGE_GAME;
        stella_ui_show(&app->ui, app->page);
        stella_ui_hide_choices(&app->ui);
        show_page_text(app);
        if (image_ok) {
            push_history(app);
            set_checkpoint(app, app->scene.id, app->op, app->page_no);
        }
        return image_ok;
    } else if (op.type == STELLA_OP_CHOICE) {
        // 进入选项立即停下自动/快进；只有显式 OK 才能确认，tick 不得代选。
        app->auto_mode = false;
        app->fast_forward = false;
        app->ff_accum = 0;
        app->auto_accum = 0;
        app->typing = false;
        app->scene_image_failed = false;
        char option[STELLA_TEXT_BYTES];
        stella_pack_text_copy(&app->pack, op.text_off, op.text_len, option, sizeof(option));
        // 无论上一帧是正文、另一条线还是旧档，都必须重建本次选项自己的提示句与画面。
        app->op_text[0] = 0;
        app->page_text[0] = 0;
        app->speaker[0] = 0;
        app->scene_asset_bg = STELLA_NONE;
        app->scene_body_count = 0;
        memset(app->scene_bodies, 0, sizeof(app->scene_bodies));
        bool prompt_found = false;
        for (int k = (int)app->op - 1; k >= 0; --k) {
            stella_op_t prev;
            if (!stella_pack_op(app->ops, app->op_count, (uint16_t)k, &prev)) break;
            if (prev.type != STELLA_OP_TEXT) continue;
            stella_pack_text_copy(&app->pack, prev.text_off, prev.text_len, app->page_text,
                                  sizeof(app->page_text));
            stella_pack_name_copy(&app->pack, prev.speaker, app->speaker, sizeof(app->speaker));
            stella_pack_name_copy(&app->pack, app->scene.name, app->chapter,
                                  sizeof(app->chapter));
            app->scene_asset_bg = prev.bg;
            app->scene_body_count = prev.body_count;
            memcpy(app->scene_bodies, prev.body, sizeof(app->scene_bodies));
            prompt_found = true;
            break;
        }
        bool image_ok = true;
        if (prompt_found) {
            image_ok = stella_image_show_scene(&app->ui.image, &app->pack, app->scene_asset_bg,
                                               app->scene_bodies, app->scene_body_count);
            if (!image_ok) {
                app->scene_image_failed = true;
                stella_image_clear(&app->ui.image, STELLA_IMAGE_THEME_TOP, STELLA_IMAGE_THEME_BOTTOM);
                set_notice(app, "图片解码失败");
            }
            stella_ui_set_game(&app->ui, app->speaker, app->page_text,
                               app->settings.large != 0, app->chapter, app->battery);
            stella_ui_set_page(&app->ui, app->page_no, app->page_count);
        } else {
            app->page_text[0] = 0;
            app->speaker[0] = 0;
        }
        app->at_choice = true;
        app->choice_target = op.arg;
        app->page = STELLA_PAGE_GAME;
        stella_ui_show(&app->ui, app->page);
        stella_ui_show_choices(&app->ui, app->page_text, option);
        if (op.arg >= app->op_count) {
            set_notice(app, "选项目标无效");
            return false;
        }
        if (image_ok) {
            set_checkpoint(app, app->scene.id, app->op, 0);
        }
        return image_ok;
    } else if (op.type == STELLA_OP_END) {
        char label[128];
        stella_pack_text_copy(&app->pack, op.text_off, op.text_len, label, sizeof(label));
        app->at_choice = false;
        app->scene_image_failed = false;
        if (app->mode == STELLA_MODE_STORY) {
            if (app->scene.id == 21) {
                stella_unlock_set_flag(&app->unlock, STELLA_FLAG_MAIN_CLEAR | STELLA_FLAG_MAIN_END);
            } else if (app->scene.id == 107) {
                stella_unlock_set_flag(&app->unlock, STELLA_FLAG_AFTER_CLEAR | STELLA_FLAG_AFTER_END);
            } else if (app->scene.id == 109) {
                stella_unlock_set_flag(&app->unlock, STELLA_FLAG_EXTRA_END);
            } else if (app->scene.id == 108) {
                stella_unlock_set_flag(&app->unlock, STELLA_FLAG_MAIN_END);
            }
            save_unlock(app, false);
        }
        show_ending(app, label);
        return true;
    }
    return false;
}

static void advance(stella_app_t *app, bool confirm_choice)
{
    if (app->ended) return;
    if (app->at_choice) {
        if (!confirm_choice) return;
        app->op = app->choice_target;
        app->page_no = 0;
        app->at_choice = false;
        stella_ui_hide_choices(&app->ui);
        (void)process_current(app);
        return;
    }
    if (app->typing) {
        app->typing = false;
        stella_ui_set_text(&app->ui, app->page_text);
        app->auto_accum = 0;
        return;
    }
    if (app->page_no + 1 < app->page_count) {
        app->page_no++;
        show_page_text(app);
        if (!app->scene_image_failed) set_checkpoint(app, app->scene.id, app->op, app->page_no);
        return;
    }
    app->op++;
    app->page_no = 0;
    process_current(app);
}

static void open_menu(stella_app_t *app)
{
    app->return_page = STELLA_PAGE_GAME;
    app->menu_sel = 0;
    refresh_menu(app);
    app->page = STELLA_PAGE_MENU;
    stella_ui_show(&app->ui, app->page);
}

static void open_settings(stella_app_t *app, stella_page_t from)
{
    app->return_page = from;
    app->settings_sel = 0;
    refresh_settings(app);
    app->page = STELLA_PAGE_SETTINGS;
    stella_ui_show(&app->ui, app->page);
}

static void open_slots(stella_app_t *app, int action, stella_page_t from)
{
    app->slot_action = action;
    app->slot_sel = 0;
    app->slots_return = from;
    refresh_slots(app);
    app->page = STELLA_PAGE_SLOTS;
    stella_ui_show(&app->ui, app->page);
}

static void open_chapters(stella_app_t *app)
{
    app->chapter_sel = 0;
    app->chapter_page = 0;
    refresh_chapters(app);
    app->page = STELLA_PAGE_CHAPTERS;
    stella_ui_show(&app->ui, app->page);
}

static void open_gallery(stella_app_t *app)
{
    app->gallery_sel = 0;
    app->gallery_page = 0;
    refresh_gallery(app);
    app->page = STELLA_PAGE_GALLERY;
    stella_ui_show(&app->ui, app->page);
}

static void show_title(stella_app_t *app)
{
    stella_image_show_title(&app->ui.image, &app->pack, STELLA_TITLE_ASSET);
    app->checkpoint_valid = false;
    app->progress_dirty = false;
    app->at_choice = false;
    app->title_sel = 0;
    refresh_title(app);
    app->page = STELLA_PAGE_TITLE;
    stella_ui_show(&app->ui, app->page);
}

static void open_cg(stella_app_t *app, uint16_t group_index)
{
    stella_group_t group;
    if (!stella_pack_group(&app->pack, group_index, &group) || group.count == 0) {
        set_notice(app, "该组没有图片");
        return;
    }
    stella_gentry_t entry;
    if (!stella_pack_gentry(&app->pack, group.first, &entry)) return;
    if (!stella_image_show_cg(&app->ui.image, &app->pack, entry.asset)) {
        app->cg_decode_failed = true;
        set_notice(app, "图片解码失败");
        return;
    }
    app->cg_decode_failed = false;
    app->cg_group = group_index;
    app->cg_variant = 0;
    stella_unlock_set_gallery(&app->unlock, group_index);
    save_unlock(app, false);
    char label[64] = "";
    stella_pack_name_copy(&app->pack, group.label, label, sizeof(label));
    char caption[96];
    snprintf(caption, sizeof(caption), "%s · %u/%u", label, 1u, (unsigned)group.count);
    stella_ui_set_cg_caption(&app->ui, caption);
    app->page = STELLA_PAGE_CG;
    stella_ui_show(&app->ui, app->page);
}

static void cycle_cg(stella_app_t *app, int delta)
{
    stella_group_t group;
    if (app->cg_decode_failed) {
        set_notice(app, "图片解码失败");
        return;
    }
    if (!stella_pack_group(&app->pack, app->cg_group, &group) || group.count == 0) return;
    int v = (int)app->cg_variant + delta;
    if (v < 0) v = group.count - 1;
    if (v >= group.count) v = 0;
    stella_gentry_t entry;
    if (!stella_pack_gentry(&app->pack, group.first + (uint16_t)v, &entry)) return;
    if (!stella_image_show_cg(&app->ui.image, &app->pack, entry.asset)) {
        app->cg_decode_failed = true;
        set_notice(app, "图片解码失败");
        return;
    }
    app->cg_variant = (uint16_t)v;
    char label[64] = "";
    stella_pack_name_copy(&app->pack, group.label, label, sizeof(label));
    char caption[96];
    snprintf(caption, sizeof(caption), "%s · %u/%u", label, (unsigned)(app->cg_variant + 1),
             (unsigned)group.count);
    stella_ui_set_cg_caption(&app->ui, caption);
}

static void confirm_save(stella_app_t *app)
{
    const int slot = app->slot_pending;
    app->page = STELLA_PAGE_SLOTS;
    stella_ui_show(&app->ui, app->page);
    if (slot >= 0) save_slot(app, slot);
    refresh_slots(app);
}

static void enter_confirm(stella_app_t *app, const char *prompt, int slot)
{
    app->confirm_return = STELLA_PAGE_SLOTS;
    app->confirm_sel = 0;
    app->slot_pending = slot;
    stella_ui_set_confirm(&app->ui, prompt, "覆盖", "取消", 0);
    app->page = STELLA_PAGE_CONFIRM;
    stella_ui_show(&app->ui, app->page);
}

static void activate_list(stella_app_t *app)
{
    switch (app->page) {
    case STELLA_PAGE_TITLE:
        switch (app->title_sel) {
        case 0:
            if (app->have_auto) {
                load_auto(app);
            } else {
                app->mode = STELLA_MODE_STORY;
                app->checkpoint_valid = false;
                if (!enter_scene(app, (uint16_t)stella_pack_meta_int(
                                      &app->pack, "initial", 1), 0, 0)) {
                    set_notice(app, "无法开始");
                }
            }
            break;
        case 1:
            app->mode = STELLA_MODE_STORY;
            app->checkpoint_valid = false;
            if (!enter_scene(app, (uint16_t)stella_pack_meta_int(&app->pack, "initial", 1),
                             0, 0)) set_notice(app, "无法开始");
            break;
        case 2:
            if (stella_unlock_has_flag(&app->unlock, STELLA_FLAG_MAIN_CLEAR)) {
                app->mode = STELLA_MODE_STORY;
                app->checkpoint_valid = false;
                if (!enter_scene(app, (uint16_t)stella_pack_meta_int(
                                      &app->pack, "after_initial", 101), 0, 0)) {
                    set_notice(app, "无法开始");
                }
            } else {
                set_notice(app, "请先完成正篇");
            }
            break;
        case 3: open_chapters(app); break;
        case 4: open_slots(app, 1, STELLA_PAGE_TITLE); break;
        case 5: open_gallery(app); break;
        case 6: open_settings(app, STELLA_PAGE_TITLE); break;
        default: break;
        }
        break;
    case STELLA_PAGE_MENU:
        switch (app->menu_sel) {
        case 0: app->page = STELLA_PAGE_GAME; stella_ui_show(&app->ui, app->page); break;
        case 1: open_slots(app, 0, STELLA_PAGE_MENU); break;
        case 2: open_slots(app, 1, STELLA_PAGE_MENU); break;
        case 3:
            build_history(app);
            app->history_return = STELLA_PAGE_MENU;
            app->page = STELLA_PAGE_HISTORY;
            stella_ui_show(&app->ui, app->page);
            break;
        case 4: open_settings(app, STELLA_PAGE_MENU); break;
        case 5: save_auto_now(app); show_title(app); break;
        default: break;
        }
        break;
    case STELLA_PAGE_SETTINGS:
        switch (app->settings_sel) {
        case 0:
            app->settings.large = app->settings.large ? 0 : 1;
            save_cfg(app);
            refresh_settings(app);
            // 阅读中改字号：立即按新字号重排当前句，而不是等下一句。
            if (app->return_page == STELLA_PAGE_MENU && app->scene.id != 0 &&
                app->op < app->op_count) {
                stella_op_t op;
                if (stella_pack_op(app->ops, app->op_count, app->op, &op) &&
                    op.type == STELLA_OP_TEXT) {
                    app->page_count = (uint16_t)stella_text_pages(
                        app->op_text, layout_for(app), app->page_off, STELLA_MAX_PAGES + 1);
                    if (app->page_no >= app->page_count) app->page_no = app->page_count - 1;
                    stella_ui_set_game(&app->ui, app->speaker, app->page_text,
                                       app->settings.large != 0, app->chapter, app->battery);
                    stella_ui_set_page(&app->ui, app->page_no, app->page_count);
                    show_page_text(app);
                    if (!app->scene_image_failed) {
                        set_checkpoint(app, app->scene.id, app->op, app->page_no);
                    }
                }
            }
            break;
        case 1:
            app->settings.speed = (uint8_t)((app->settings.speed + 1) % 4);
            save_cfg(app);
            refresh_settings(app);
            break;
        case 2:
            app->settings.auto_wait = (uint8_t)((app->settings.auto_wait + 1) % 3);
            save_cfg(app);
            refresh_settings(app);
            break;
        case 3:
            app->page = app->return_page;
            if (app->page == STELLA_PAGE_MENU) refresh_menu(app);
            else if (app->page == STELLA_PAGE_TITLE) refresh_title(app);
            stella_ui_show(&app->ui, app->page);
            break;
        default: break;
        }
        break;
    case STELLA_PAGE_SLOTS: {
        const int sel = app->slot_sel;
        if (sel == 0) {
            if (app->slot_action == 0) set_notice(app, "自动存档不可手动保存");
            else if (app->have_auto) load_save(app, &app->autosave);
            else set_notice(app, "自动存档为空");
            break;
        }
        const int slot = sel - 1;
        if (slot < 0 || slot >= STELLA_SLOT_COUNT) break;
        if (app->slot_action == 0) {
            if (app->slot_valid[slot]) {
                char prompt[64];
                snprintf(prompt, sizeof(prompt), "覆盖存档 %d？", slot + 1);
                enter_confirm(app, prompt, slot);
            } else {
                save_slot(app, slot);
                refresh_slots(app);
            }
        } else if (app->slot_valid[slot]) {
            load_save(app, &app->slots[slot]);
        } else {
            set_notice(app, "空存档");
        }
        break;
    }
    case STELLA_PAGE_CHAPTERS: {
        const int sel = app->chapter_sel;
        const int total = chapter_count(app);
        if (sel == 6) {
            if (app->chapter_page > 0) app->chapter_page--;
            refresh_chapters(app);
            break;
        }
        if (sel == 7) {
            const int pages = (total + 5) / 6;
            if (app->chapter_page + 1 < pages) app->chapter_page++;
            refresh_chapters(app);
            break;
        }
        const int index = app->chapter_page * 6 + sel;
        if (index >= total) break;
        const uint16_t scene_index = chapter_at(app, index);
        stella_scene_t scene;
        if (scene_index == 0xFFFF || !stella_pack_scene(&app->pack, scene_index, &scene)) break;
        if (chapter_locked(app, &scene)) { set_notice(app, "尚未解锁"); break; }
        app->mode = STELLA_MODE_CHAPTER;
        app->checkpoint_valid = false;
        if (!enter_scene(app, scene.id, 0, 0)) set_notice(app, "无法开始");
        break;
    }
    case STELLA_PAGE_GALLERY: {
        const int sel = app->gallery_sel;
        if (sel == 6) {
            if (app->gallery_page > 0) app->gallery_page--;
            refresh_gallery(app);
            break;
        }
        if (sel == 7) {
            const int pages = (app->pack.group_count + 5) / 6;
            if (app->gallery_page + 1 < pages) app->gallery_page++;
            refresh_gallery(app);
            break;
        }
        const int index = app->gallery_page * 6 + sel;
        if (index < (int)app->pack.group_count) open_cg(app, (uint16_t)index);
        break;
    }
    case STELLA_PAGE_CONFIRM:
        if (app->confirm_sel == 0) confirm_save(app);
        else { app->page = STELLA_PAGE_SLOTS; stella_ui_show(&app->ui, app->page); }
        break;
    default:
        break;
    }
}

static void list_move(stella_app_t *app, int delta)
{
    int count = 0;
    int *sel = NULL;
    switch (app->page) {
    case STELLA_PAGE_TITLE: count = 7; sel = &app->title_sel; break;
    case STELLA_PAGE_MENU: count = 6; sel = &app->menu_sel; break;
    case STELLA_PAGE_SETTINGS: count = 4; sel = &app->settings_sel; break;
    case STELLA_PAGE_SLOTS: count = STELLA_LIST_ROWS; sel = &app->slot_sel; break;
    case STELLA_PAGE_CHAPTERS: count = STELLA_LIST_ROWS; sel = &app->chapter_sel; break;
    case STELLA_PAGE_GALLERY: count = STELLA_LIST_ROWS; sel = &app->gallery_sel; break;
    case STELLA_PAGE_CONFIRM: count = 2; sel = &app->confirm_sel; break;
    default: return;
    }
    int v = *sel + delta;
    if (v < 0) v = 0;
    if (v >= count) v = count - 1;
    *sel = v;
    if (app->page == STELLA_PAGE_TITLE) refresh_title(app);
    else if (app->page == STELLA_PAGE_MENU) refresh_menu(app);
    else if (app->page == STELLA_PAGE_SETTINGS) refresh_settings(app);
    else if (app->page == STELLA_PAGE_SLOTS) stella_ui_list_select(&app->ui, v);
    else if (app->page == STELLA_PAGE_CHAPTERS) stella_ui_list_select(&app->ui, v);
    else if (app->page == STELLA_PAGE_GALLERY) stella_ui_list_select(&app->ui, v);
    else if (app->page == STELLA_PAGE_CONFIRM) stella_ui_confirm_select(&app->ui, v);
}

static void go_back(stella_app_t *app)
{
    switch (app->page) {
    case STELLA_PAGE_GAME:
        open_menu(app);
        break;
    case STELLA_PAGE_MENU:
        app->page = STELLA_PAGE_GAME;
        stella_ui_show(&app->ui, app->page);
        break;
    case STELLA_PAGE_SETTINGS:
        app->page = app->return_page;
        if (app->page == STELLA_PAGE_MENU) refresh_menu(app);
        else if (app->page == STELLA_PAGE_TITLE) refresh_title(app);
        stella_ui_show(&app->ui, app->page);
        break;
    case STELLA_PAGE_SLOTS:
        app->page = app->slots_return;
        if (app->page == STELLA_PAGE_TITLE) refresh_title(app);
        else if (app->page == STELLA_PAGE_MENU) refresh_menu(app);
        stella_ui_show(&app->ui, app->page);
        break;
    case STELLA_PAGE_CHAPTERS:
    case STELLA_PAGE_GALLERY:
        show_title(app);
        break;
    case STELLA_PAGE_HISTORY:
        app->page = app->history_return;
        if (app->page == STELLA_PAGE_MENU) refresh_menu(app);
        stella_ui_show(&app->ui, app->page);
        break;
    case STELLA_PAGE_CONFIRM:
        app->page = STELLA_PAGE_SLOTS;
        stella_ui_show(&app->ui, app->page);
        break;
    case STELLA_PAGE_CG:
        app->page = STELLA_PAGE_GALLERY;
        stella_ui_show(&app->ui, app->page);
        break;
    case STELLA_PAGE_ENDING:
        show_title(app);
        break;
    default:
        break;
    }
}

bool stella_app_init(stella_app_t *app, const uint8_t *pack_data, uint32_t pack_size)
{
    if (!app || !pack_data) return false;
    memset(app, 0, sizeof(*app));
    app->long_btn = 0xFF;
    if (!stella_pack_open(&app->pack, pack_data, pack_size)) {
        ESP_LOGE(TAG, "资源包校验失败");
        return false;
    }
    load_cfg(app);
    load_unlock(app);
    app->battery = bsp_battery_soc();
    app->title_sel = 0;
    app->battery_ms = 30000;

    for (int i = 0; i < STELLA_SLOT_COUNT; ++i) {
        char key[8];
        slot_key(i, key, sizeof(key));
        if (read_save_key(key, &app->slots[i]) && save_valid(app, &app->slots[i])) {
            app->slot_valid[i] = true;
        }
    }
    stella_save_t auto_tmp;
    if (read_auto_key(&auto_tmp) && save_valid(app, &auto_tmp)) {
        app->autosave = auto_tmp;
        app->have_auto = true;
    } else {
        // 旧版本把 auto 放在 stella 命名空间；只迁移一次，之后只读写 stella_auto。
        uint8_t raw[STELLA_SAVE_BYTES];
        if (nvs_read(KEY_AUTO, raw, sizeof(raw)) &&
            stella_save_decode(&auto_tmp, raw, sizeof(raw)) && save_valid(app, &auto_tmp)) {
            app->autosave = auto_tmp;
            app->have_auto = true;
            (void)write_auto_key(&auto_tmp);
        }
    }

    if (!stella_ui_create(&app->ui, lv_screen_active(), app->ui_pixels, &stella_cjk_16,
                          &stella_cjk_20)) {
        ESP_LOGE(TAG, "界面创建失败");
        return false;
    }
    stella_image_show_title(&app->ui.image, &app->pack, STELLA_TITLE_ASSET);
    refresh_title(app);
    app->page = STELLA_PAGE_TITLE;
    stella_ui_show(&app->ui, app->page);
    ESP_LOGI(TAG, "就绪: %ux%u, pack=%u B, scenes=%u, groups=%u, assets=%u",
             (unsigned)STELLA_SCREEN_W, (unsigned)STELLA_SCREEN_H, (unsigned)pack_size,
             (unsigned)app->pack.scene_count, (unsigned)app->pack.group_count,
             (unsigned)app->pack.asset_count);
    return true;
}

void stella_app_key(stella_app_t *app, const stella_key_t *key)
{
    if (!app || !key) return;
    app->idle_ms = 0;
    const uint8_t ev = key->ev;
    const uint8_t btn = key->btn;

    if (ev == BSP_BTN_PRESS) {
        app->long_btn = 0xFF;
        if (app->auto_mode) app->auto_mode = false;
        return;
    }
    if (ev == BSP_BTN_RELEASE) {
        if (btn == BSP_BTN_UP) app->fast_forward = false;
        return;
    }
    // 短按的真实序列是 PRESS -> RELEASE -> 延迟 CLICK；长按后若驱动仍给 CLICK，
    // 用 long_btn 吞掉，避免一次长按推进两句。
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
        app->auto_accum = 0;
        return;
    }

    if (app->page == STELLA_PAGE_GAME) {
        if (app->at_choice) {
            // 待选时只有 OK 确认；长 OK 进菜单，菜单/存档/读档/返回均保留 at_choice。
            if (ev == BSP_BTN_CLICK && btn == BSP_BTN_OK) advance(app, true);
            else if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) open_menu(app);
            return;
        }
        if (ev == BSP_BTN_LONG) {
            if (btn == BSP_BTN_UP) {
                app->fast_forward = true;
                app->ff_accum = 0;
                // 快进一律整句瞬显：不等打字机。
                if (app->typing) {
                    app->typing = false;
                    app->typing_pos = strlen(app->page_text);
                    stella_ui_set_text(&app->ui, app->page_text);
                }
                return;
            }
            if (btn == BSP_BTN_DOWN) { app->auto_mode = true; app->auto_accum = 0; return; }
            if (btn == BSP_BTN_OK) {
                open_menu(app);
                app->suppress_click = true;   // 吞掉驱动在 LONG 后补发的 CLICK
                return;
            }
            return;
        }
        if (ev != BSP_BTN_CLICK) return;
        if (btn == BSP_BTN_UP) { app->fast_forward = false; advance(app, false); }
        else if (btn == BSP_BTN_DOWN) {
            build_history(app);
            app->history_return = STELLA_PAGE_GAME;
            app->page = STELLA_PAGE_HISTORY;
            stella_ui_show(&app->ui, app->page);
        } else if (btn == BSP_BTN_OK) {
            if (app->suppress_click) {
                app->suppress_click = false;   // 长按后的补发 CLICK，不推进
                return;
            }
            advance(app, false);               // 短按 OK = 下一句
        }
        return;
    }

    if (app->page == STELLA_PAGE_HISTORY) {
        if (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG) {
            if (btn == BSP_BTN_UP) stella_ui_history_scroll(&app->ui, -24);
            else if (btn == BSP_BTN_DOWN) stella_ui_history_scroll(&app->ui, 24);
            else go_back(app);
        }
        return;
    }
    if (app->page == STELLA_PAGE_CG) {
        if (ev == BSP_BTN_CLICK) {
            if (btn == BSP_BTN_UP) cycle_cg(app, 1);
            else if (btn == BSP_BTN_DOWN) cycle_cg(app, -1);
            else go_back(app);
        } else if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            go_back(app);
        }
        return;
    }
    if (app->page == STELLA_PAGE_ENDING) {
        if (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG) show_title(app);
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

void stella_app_tick(stella_app_t *app, uint32_t elapsed_ms)
{
    if (!app) return;
    if (app->auto_mode || app->fast_forward) app->idle_ms = 0;
    else app->idle_ms += elapsed_ms;
    app->progress_ms += elapsed_ms;
    app->battery_ms += elapsed_ms;
    if (app->notice_ms) {
        app->notice_ms = app->notice_ms > elapsed_ms ? app->notice_ms - elapsed_ms : 0;
        if (!app->notice_ms) stella_ui_notice_hide(&app->ui);
    }
    if (app->battery_ms >= 30000) {
        app->battery_ms = 0;
        const int soc = bsp_battery_soc();
        if (soc != app->battery) {
            app->battery = soc;
            stella_ui_set_battery(&app->ui, soc);
        }
    }
    if (app->page == STELLA_PAGE_GAME) {
        if (app->at_choice) {
            app->auto_mode = false;
            app->fast_forward = false;
            app->ff_accum = 0;
            app->auto_accum = 0;
        }
        if (app->fast_forward && app->typing) {
            // 快进期间任何残留打字立即补满。
            app->typing = false;
            app->typing_pos = strlen(app->page_text);
            stella_ui_set_text(&app->ui, app->page_text);
        }
        if (app->typing) {
            uint32_t step = 1;
            if (app->settings.speed == 1) step = 2;
            else if (app->settings.speed == 2) step = 4;
            const size_t len = strlen(app->page_text);
            while (step-- > 0 && app->typing_pos < len) {
                app->typing_pos = stella_utf8_next(app->page_text, len, app->typing_pos);
            }
            if (app->typing_pos >= len) {
                app->typing = false;
                stella_ui_set_text(&app->ui, app->page_text);
                app->auto_accum = 0;
            } else {
                snprintf(app->shown, sizeof(app->shown), "%.*s", (int)app->typing_pos,
                         app->page_text);
                stella_ui_set_text(&app->ui, app->shown);
            }
        } else if (app->fast_forward) {
            // 快进：每个 tick 立刻推进，不等待、不累积。
            app->ff_accum = 0;
            advance(app, false);
        } else if (app->auto_mode) {
            static const uint32_t waits[3] = {2600, 1500, 800};
            app->auto_accum += elapsed_ms;
            if (app->auto_accum >= waits[app->settings.auto_wait]) {
                app->auto_accum = 0;
                advance(app, false);
            }
        }
    }
    if (app->progress_dirty && app->progress_ms >= 2000) {
        app->progress_ms = 0;
        save_auto_now(app);
    }
    if (app->unlock_dirty && app->unlock_retry_count < UNLOCK_RETRY_MAX) {
        app->unlock_retry_ms += elapsed_ms;
        if (app->unlock_retry_ms >= 2000) {
            app->unlock_retry_ms = 0;
            save_unlock(app, false);
        }
    } else if (!app->unlock_dirty) {
        app->unlock_retry_ms = 0;
        app->unlock_retry_count = 0;
    } else {
        app->unlock_retry_ms = 0;  // 次数用尽；下一次显式 flush 才强制重试
    }
}

uint32_t stella_app_idle_ms(const stella_app_t *app)
{
    if (!app) return 0;
    if (app->auto_mode || app->fast_forward) return 0;
    if (app->auto_mode || app->fast_forward) return 0;   // 自动/快进不休眠
    return app->idle_ms;
}

void stella_app_flush(stella_app_t *app)
{
    if (!app) return;
    // IO 恢复后的睡前 flush 必须强制一次 unlock；不能被普通重试上限挡住。
    if (app->unlock_dirty) save_unlock(app, true);
    // 自动进度只取最后一个成功显示的阅读停点，与当前是否停在历史/设置页无关。
    save_auto_now(app);
}

void stella_app_show_sleeping(stella_app_t *app)
{
    if (!app) return;
    stella_ui_set_ending(&app->ui, "设备即将休眠\n\n按任意键唤醒");
    app->page = STELLA_PAGE_ENDING;
    stella_ui_show(&app->ui, app->page);
}

static bool preflight_save(stella_app_t *app, const stella_save_t *save)
{
    if (!save_valid(app, save)) return false;
    uint16_t idx, n = 0;
    stella_scene_t scene;
    if (!stella_pack_find_scene(&app->pack, save->scene, &idx) ||
        !stella_pack_scene(&app->pack, idx, &scene)) return false;
    const uint8_t *ops = stella_pack_ops(&app->pack, &scene, &n);
    if (!ops || save->op >= n) return false;
    stella_op_t op;
    if (!stella_pack_op(ops, n, (uint16_t)save->op, &op)) return false;
    if (op.type == STELLA_OP_TEXT) {
        return stella_image_validate_scene(&app->pack, op.bg, op.body, op.body_count);
    }
    if (op.type == STELLA_OP_CHOICE) {
        for (int k = (int)save->op - 1; k >= 0; --k) {
            stella_op_t prev;
            if (!stella_pack_op(ops, n, (uint16_t)k, &prev)) break;
            if (prev.type != STELLA_OP_TEXT) continue;
            return stella_image_validate_scene(&app->pack, prev.bg, prev.body,
                                               prev.body_count);
        }
        return true;
    }
    return false;
}

static void load_save(stella_app_t *app, const stella_save_t *save)
{
    if (!save || !save_valid(app, save)) {
        set_notice(app, "存档无效");
        return;
    }
    // 先完整预检图片再改 app 状态；坏图不得先破坏当前正常进度。
    if (!preflight_save(app, save)) {
        set_notice(app, "存档图片损坏");
        return;
    }
    const uint8_t old_mode = app->mode;
    app->mode = save->mode;
    if (enter_scene(app, save->scene, save->op, save->page)) {
        if (app->page != STELLA_PAGE_ENDING) {
            app->page = STELLA_PAGE_GAME;
            stella_ui_show(&app->ui, app->page);
        }
    } else {
        app->mode = old_mode;
    }
}
