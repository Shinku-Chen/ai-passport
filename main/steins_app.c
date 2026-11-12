// main/steins_app.c —— 《命运石之门》阅读器的应用状态机。
//
// 按键只认 CLICK / LONG / RELEASE 三种事件:长按之后的抬手不会再触发一次 CLICK,
// 长按也不会吞掉下一次独立短按。所有 LVGL 写入都在 bsp_lvgl_lock 之外由输入任务
// 串行调用(main.c 负责加锁),本层不自己做锁。
#include "steins_app.h"
#include "steins_mobile.h"

#include <stdio.h>
#include <string.h>

#include "bsp_battery.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "nvs.h"

#ifndef ESP_ERR_INVALID_SIZE
#define ESP_ERR_INVALID_SIZE 0x1105
#endif

static const char *TAG = "steins";

// ABOUT 正文有 3 段/约 450 字节，128 字节的 LIST_TEXT 会截断半篇。
#define STEINS_ABOUT_TEXT 1024

// ---------------------------------------------------------------- 常量

#define STEINS_NVS_NS "steins"
#define STEINS_KEY_UNLOCK "unlock"
#define STEINS_KEY_SETTINGS "cfg"
#define STEINS_KEY_AUTOSAVE "sauto"

#define STEINS_AUTO_BASE_MS 1400u
#define STEINS_AUTO_PER_CHAR_MS 30u
#define STEINS_FF_MS 70u
#define STEINS_NOTICE_MS 1800u
#define STEINS_AUTOSAVE_MS 2000u
#define STEINS_BATTERY_MS 5000u

// 界面文案键(与 tools/steins_pack.py 的 UI_TEXT 一一对应)
#define UI_TITLE_NEW "title.new"
#define UI_TITLE_CONTINUE "title.continue"
#define UI_TITLE_LOAD "title.load"
#define UI_TITLE_GALLERY "title.gallery"
#define UI_TITLE_CHAPTER "title.chapter"
#define UI_TITLE_CONFIG "title.config"
#define UI_TITLE_ABOUT "title.about"
#define UI_MENU_RESUME "menu.resume"
#define UI_MENU_SAVE "menu.save"
#define UI_MENU_LOAD "menu.load"
#define UI_MENU_PHONE "menu.phone"
#define UI_MENU_HISTORY "menu.history"
#define UI_MENU_AUTO_ON "menu.auto_on"
#define UI_MENU_AUTO_OFF "menu.auto_off"
#define UI_MENU_CHAPTER "menu.chapter"
#define UI_MENU_GALLERY "menu.gallery"
#define UI_MENU_TITLE "menu.title"
#define UI_SAVE_TITLE "save.title"
#define UI_LOAD_TITLE "load.title"
#define UI_SAVE_EMPTY "save.empty"
#define UI_SAVE_OVERWRITE "save.overwrite"
#define UI_SAVE_DONE "save.done"
#define UI_SAVE_FAIL "save.fail"
#define UI_LOAD_FAIL "load.fail"
#define UI_YES "common.yes"
#define UI_NO "common.no"
#define UI_HISTORY_TITLE "history.title"
#define UI_SETTINGS_TITLE "settings.title"
#define UI_SETTINGS_FONT "settings.font"
#define UI_SETTINGS_TEXT_SPEED "settings.text_speed"
#define UI_SETTINGS_AUTO_SPEED "settings.auto_speed"
#define UI_SETTINGS_OPACITY "settings.opacity"
#define UI_CHAPTER_TITLE "chapter.title"
#define UI_CHAPTER_LOCKED "chapter.locked"
#define UI_GALLERY_TITLE "gallery.title"
#define UI_GALLERY_VARIANT "gallery.variant"
#define UI_GALLERY_EMPTY "gallery.empty"
#define UI_ENDING_REACHED "ending.reached"
#define UI_ENDING_TRUE "ending.true"
#define UI_ABOUT_TITLE "about.title"
#define UI_ABOUT_BODY "about.body"
#define UI_IMAGE_FAIL "image.fail"
#define UI_CHOICE_CONFIRM "choice.confirm"
#define UI_AUTOSAVE "save.autosave"
#define UI_WORLDLINE "worldline"
#define UI_APP_TITLE "app.title"

enum {
    ACT_NONE = 0,
    ACT_OVERWRITE_SAVE,
};

// 标题菜单行动作(菜单行按"有没有自动存档"动态拼,所以用动作而不是下标分发)
enum {
    TITLE_ACT_NEW = 0,
    TITLE_ACT_CONTINUE,
    TITLE_ACT_LOAD,
    TITLE_ACT_GALLERY,
    TITLE_ACT_CHAPTER,
    TITLE_ACT_CONFIG,
    TITLE_ACT_ABOUT,
};

#define STEINS_SETTINGS_BYTES 8
#define STEINS_CHECKPOINT_BYTES STEINS_CHK_MAX_BYTES

static uint8_t s_checkpoint_buf[STEINS_CHECKPOINT_BYTES];

// 按键事件(bsp_btn_ev_t):PRESS=0 CLICK=1 DOUBLE=2 LONG=3 RELEASE=4
#define KEY_CLICK 1
#define KEY_LONG 3
#define KEY_RELEASE 4
#define BTN_UP 0
#define BTN_DOWN 1
#define BTN_OK 2

// ---------------------------------------------------------------- 前置声明
static void render_game(steins_app_t *app);
static void render_list(steins_app_t *app);
static void render_title(steins_app_t *app);
static void render_mail(steins_app_t *app);
static void render_choice(steins_app_t *app);
static void render_confirm(steins_app_t *app);
static void fill_menu(steins_app_t *app);
static void fill_slots(steins_app_t *app);
static void fill_chapters(steins_app_t *app);
static void fill_gallery(steins_app_t *app);
static void fill_settings(steins_app_t *app);
static void fill_history(steins_app_t *app);
static void open_menu(steins_app_t *app);
static void open_saves(steins_app_t *app, bool saving);
static void open_history(steins_app_t *app);
static void open_phone_home(steins_app_t *app);
static void open_phone_list(steins_app_t *app, bool sent);
static void open_phone_detail(steins_app_t *app, uint16_t id, bool sent);
static void open_phone_replies(steins_app_t *app);
static void open_phone_calls(steins_app_t *app);
static void open_phone_contacts(steins_app_t *app);
static bool open_phone_presentation(steins_app_t *app);
static bool phone_mail_meta(steins_app_t *app, uint16_t id, steins_mail_t *mail);
static void nav_push(steins_app_t *app, steins_page_t page, int sel, int kind);
static void render_phone_mail(steins_app_t *app);
static void fill_phone_home(steins_app_t *app);
static void fill_phone_list(steins_app_t *app, bool sent);
static void fill_phone_replies(steins_app_t *app);
static void fill_phone_calls(steins_app_t *app);
static void send_phone_reply(steins_app_t *app, uint16_t child);
static bool mobile_clip_valid(steins_app_t *app, uint16_t node);
static bool render_phone_clip(steins_app_t *app, uint16_t node, uint32_t frame);
static void phone_clip_key(steins_app_t *app, const steins_key_t *key);
static void open_settings(steins_app_t *app);
static void open_chapters(steins_app_t *app);
static void open_gallery(steins_app_t *app);
static void render_cg(steins_app_t *app);
static void open_confirm(steins_app_t *app, uint8_t action, int slot);
static void go_title(steins_app_t *app);
static void return_back(steins_app_t *app);
static bool text_fully_shown(const steins_app_t *app);
static bool render_current(steins_app_t *app, bool autosave);
static bool apply_position(steins_app_t *app, const steins_pos_t *pos);
static bool node_of(steins_app_t *app, uint16_t index, steins_node_t *out);

// ---------------------------------------------------------------- 小工具

// 显式截断复制:比 snprintf("%s") 更清楚,也不会被 -Wformat-truncation 误报。
static void copy_text(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0u) return;
    const size_t len = src ? strlen(src) : 0u;
    const size_t take = len < cap - 1u ? len : cap - 1u;
    if (take) memcpy(dst, src, take);
    dst[take] = '\0';
}

static void ui_text(steins_app_t *app, const char *key, char *out, size_t cap)
{
    const uint16_t index = steins_pack_ui(&app->pack, key);
    if (index == STEINS_NONE) {
        copy_text(out, cap, key);
        return;
    }
    if (steins_pack_str_utf8(&app->pack, index, out, cap) < 0) {
        copy_text(out, cap, key);
    }
}

// pack 里缺键时用调用方给的兜底文案:绝不把键名(如 "menu.phone")画到屏幕上。
static void ui_text_fb(steins_app_t *app, const char *key, const char *fallback, char *out,
                       size_t cap)
{
    if (steins_pack_ui(&app->pack, key) == STEINS_NONE) {
        copy_text(out, cap, fallback);
        return;
    }
    ui_text(app, key, out, cap);
    if (strcmp(out, key) == 0) copy_text(out, cap, fallback);
}

static void set_list_hint(steins_app_t *app, const char *text)
{
    copy_text(app->list_hint, sizeof(app->list_hint), text);
}

static void set_notice(steins_app_t *app, const char *key)
{
    ui_text(app, key, app->notice, sizeof(app->notice));
    app->notice_ms = STEINS_NOTICE_MS;
}

static void set_notice_text(steins_app_t *app, const char *text)
{
    copy_text(app->notice, sizeof(app->notice), text ? text : "");
    app->notice_ms = text && text[0] ? STEINS_NOTICE_MS : 0;
}

static void set_page(steins_app_t *app, steins_page_t page)
{
    app->page = page;
    steins_ui_show(&app->ui, page);
}

static void nav_push(steins_app_t *app, steins_page_t page, int sel, int kind)
{
    if (app->nav_depth < STEINS_NAV_MAX) {
        app->nav[app->nav_depth].page = page;
        app->nav[app->nav_depth].sel = (int16_t)sel;
        app->nav[app->nav_depth].kind = (uint8_t)kind;
        ++app->nav_depth;
    }
}

static bool nav_pop(steins_app_t *app, steins_page_t *page_out, int *sel_out, int *kind_out)
{
    if (app->nav_depth <= 0) return false;
    --app->nav_depth;
    if (page_out) *page_out = app->nav[app->nav_depth].page;
    if (sel_out) *sel_out = app->nav[app->nav_depth].sel;
    if (kind_out) *kind_out = app->nav[app->nav_depth].kind;
    return true;
}

// ---------------------------------------------------------------- 存储

static esp_err_t storage_get_ex(const char *key, void *out, size_t cap, size_t *len_out)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(STEINS_NVS_NS, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    size_t len = cap;
    err = nvs_get_blob(handle, key, out, &len);
    if (err == ESP_OK && (len == 0u || len > cap)) err = ESP_ERR_INVALID_SIZE;
    nvs_close(handle);
    if (err == ESP_OK && len_out) *len_out = len;
    return err;
}

static bool storage_get(const char *key, void *out, size_t cap, size_t *len_out)
{
    return storage_get_ex(key, out, cap, len_out) == ESP_OK;
}

static bool storage_set(const char *key, const void *data, size_t len)
{
    nvs_handle_t handle;
    if (nvs_open(STEINS_NVS_NS, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "NVS 不可用,写入 %s 跳过", key);
        return false;
    }
    esp_err_t err = nvs_set_blob(handle, key, data, len);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "写入 %s 失败: %s", key, esp_err_to_name(err));
        return false;
    }
    return true;
}

static void slot_key(int slot, char *out, size_t cap)
{
    if (slot < 0) snprintf(out, cap, "%s", STEINS_KEY_AUTOSAVE);
    else snprintf(out, cap, "sv%02d", slot);
}

// v2 主键；旧 v1 的 svNN 保留为 16 B 兼容镜像（迁移读取/旧工具可读），
// 不是第二套 checkpoint codec，也不参与手机状态。
static void slot_v2_key(int slot, char *out, size_t cap)
{
    if (slot < 0) snprintf(out, cap, "%s", STEINS_KEY_AUTOSAVE);
    else snprintf(out, cap, "sv%02dv2", slot);
}

// 把当前 pos/全局结局同步进 checkpoint；手机字段由 engine/UI 保持。
static void checkpoint_sync_pos(steins_app_t *app)
{
    app->chk.node = app->pos.node;
    app->chk.cursor = app->pos.cursor;
    app->chk.mode = app->pos.mode;
    app->chk.endings = 0;
    for (int i = 0; i < STEINS_MAX_ENDINGS && i < 16; ++i) {
        if (steins_unlock_has_ending(&app->unlock, (uint8_t)i)) {
            app->chk.endings |= (uint16_t)(1u << i);
        }
    }
}

// 读取一个栏位：NOT_FOUND 是空槽，其他 NVS 错误是 IO_ERROR，解码失败是 CORRUPT。
// 共享存档 scratch:steins_checkpoint_t 有 3520 B,放进栈会直接打穿 main 任务
// 的 3584 B 栈(firstboot 2026-11-02 实测 Stack protection fault)。
// 合同:调用方必须持 LVGL 锁且单线程;同一调用链内不得嵌套两个持有者;
// 内容只在下一次 checkpoint_read() 之前有效。绝不写入 app->chk。
static steins_checkpoint_t s_chk_scratch;
// 标题图只在进入标题页时解一次(菜单光标移动也会走 render_title,别每键都解 5.5KB JPEG)。
static bool s_title_art_drawn;

static steins_slot_state_t checkpoint_read(steins_app_t *app, int slot, steins_checkpoint_t *out)
{
    if (!app || !app->storage_ok) return STEINS_SLOT_IO_ERROR;
    char key[16];
    if (slot < 0) slot_key(slot, key, sizeof(key));
    else slot_v2_key(slot, key, sizeof(key));
    size_t len = 0;
    esp_err_t err = storage_get_ex(key, s_checkpoint_buf, sizeof(s_checkpoint_buf), &len);
    if (err == ESP_ERR_NVS_NOT_FOUND && slot >= 0) {
        slot_key(slot, key, sizeof(key));
        err = storage_get_ex(key, s_checkpoint_buf, sizeof(s_checkpoint_buf), &len);
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) return STEINS_SLOT_EMPTY;
    if (err != ESP_OK) return STEINS_SLOT_IO_ERROR;
    // out 就是 scratch 时直接解码进去(失败只污染 scratch,不会伤调用方 live 状态);
    // 否则解码到 scratch,仅 VALID 时再拷出,保持"失败不改 out"的既有语义。
    steins_checkpoint_t *tmp = (out == &s_chk_scratch) ? out : &s_chk_scratch;
    bool migrated = false;
    if (!steins_checkpoint_save_decode(tmp, s_checkpoint_buf, len, &migrated)) {
        return STEINS_SLOT_CORRUPT;
    }
    if (out && tmp != out) *out = *tmp;
    return STEINS_SLOT_VALID;
}

static bool checkpoint_write(steins_app_t *app, int slot)
{
    if (!app || !app->reading) return false;
    checkpoint_sync_pos(app);
    const size_t len = steins_checkpoint_save_encode(&app->chk, s_checkpoint_buf,
                                                     sizeof(s_checkpoint_buf));
    if (len == 0u) return false;
    char key[16];
    if (slot < 0) slot_key(slot, key, sizeof(key));
    else slot_v2_key(slot, key, sizeof(key));
    const bool ok = storage_set(key, s_checkpoint_buf, len);
    if (ok) {
        app->checkpoint_dirty = false;
        app->checkpoint_ms = 0;
        if (slot >= 0 && slot < STEINS_SLOTS) app->slot_state[slot] = STEINS_SLOT_VALID;
        // 旧 16 B 头镜像：只用于旧测试/旧工具/迁移识别；失败不回滚 v2。
        if (slot >= 0) {
            uint8_t legacy[STEINS_SAVE_BYTES];
            steins_pos_t pos = { app->chk.node, app->chk.cursor, app->chk.mode, 0 };
            if (steins_save_encode(&pos, legacy, sizeof(legacy)) > 0u) {
                char old_key[12];
                slot_key(slot, old_key, sizeof(old_key));
                (void)storage_set(old_key, legacy, sizeof(legacy));
            }
        }
    } else {
        app->checkpoint_dirty = true;
    }
    return ok;
}

// 兼容旧调用名；成功才返回 true，调用方负责设置“已保存”提示。
static bool save_write(steins_app_t *app, int slot)
{
    return checkpoint_write(app, slot);
}

static bool checkpoint_apply(steins_app_t *app, const steins_checkpoint_t *chk, bool migrated)
{
    if (!app || !chk) return false;
    steins_node_t node;
    const bool clip = mobile_clip_valid(app, chk->node);
    if (!clip && !node_of(app, chk->node, &node)) return false;
    if (!clip && chk->cursor > node.count) return false;
    app->chk = *chk;
    app->migrated_v1 = migrated;
    app->pos.node = chk->node;
    app->pos.cursor = chk->cursor;
    app->pos.mode = chk->mode;
    app->pos.flags = 0;
    app->reading = true;
    app->checkpoint_dirty = false;
    app->checkpoint_ms = 0;
    // 章节档不得点亮全局 story 结局；story 档只增不减地合并。
    if (chk->mode == STEINS_MODE_STORY) {
        for (int i = 0; i < STEINS_MAX_ENDINGS && i < 16; ++i) {
            if ((chk->endings & (uint16_t)(1u << i)) &&
                steins_unlock_set_ending(&app->unlock, (uint8_t)i)) {
                app->unlock_dirty = true;
            }
        }
    }
    if (clip) {
        app->nav_depth = 0;
        return render_phone_clip(app, chk->node, chk->cursor);
    }
    return apply_position(app, &app->pos);
}

static void settings_save(steins_app_t *app)
{
    uint8_t buf[STEINS_SETTINGS_BYTES];
    memset(buf, 0, sizeof(buf));
    buf[0] = 'S';
    buf[1] = 'C';
    buf[2] = 1;
    buf[3] = app->text_speed;
    buf[4] = app->auto_speed;
    buf[5] = app->box_opacity;
    buf[6] = app->font_size;
    buf[7] = (uint8_t)steins_crc16(buf, 7u);
    (void)storage_set(STEINS_KEY_SETTINGS, buf, sizeof(buf));
}

static void settings_load(steins_app_t *app)
{
    uint8_t buf[STEINS_SETTINGS_BYTES];
    size_t len = 0;
    if (!storage_get(STEINS_KEY_SETTINGS, buf, sizeof(buf), &len)) return;
    if (len != STEINS_SETTINGS_BYTES || buf[0] != 'S' || buf[1] != 'C' || buf[2] != 1) return;
    if (buf[7] != (uint8_t)steins_crc16(buf, 7u)) return;
    if (buf[3] <= 2u) app->text_speed = buf[3];
    if (buf[4] <= 2u) app->auto_speed = buf[4];
    if (buf[5] <= 2u) app->box_opacity = buf[5];
    if (buf[6] == 16u) app->font_size = 16;
}

static void unlock_save(steins_app_t *app)
{
    uint8_t buf[STEINS_UNLOCK_BYTES];
    if (steins_unlock_encode(&app->unlock, buf, sizeof(buf)) == 0u) return;
    if (storage_set(STEINS_KEY_UNLOCK, buf, sizeof(buf))) app->unlock_dirty = false;
}

static void unlock_load(steins_app_t *app)
{
    uint8_t buf[STEINS_UNLOCK_BYTES];
    size_t len = 0;
    steins_unlock_init(&app->unlock);
    if (!storage_get(STEINS_KEY_UNLOCK, buf, sizeof(buf), &len)) return;
    steins_unlock_t loaded;
    if (steins_unlock_decode(&loaded, buf, len)) app->unlock = loaded;
}

// ---------------------------------------------------------------- 包里查找

static bool node_of(steins_app_t *app, uint16_t index, steins_node_t *out)
{
    return steins_pack_node(&app->pack, index, out);
}

static bool frame_at(steins_app_t *app, uint16_t node_index, uint32_t cursor,
                     steins_frame_t *out)
{
    steins_node_t node;
    if (!node_of(app, node_index, &node)) return false;
    uint32_t at = 0;
    for (uint8_t i = 0; i < node.chunk_count; ++i) {
        uint16_t chunk_index = 0;
        steins_chunk_t chunk;
        if (!steins_pack_node_chunk(&app->pack, &node, i, &chunk_index)) return false;
        if (!steins_pack_chunk(&app->pack, chunk_index, &chunk)) return false;
        if (cursor < at + chunk.count) {
            return steins_pack_frame(&app->pack, chunk.first_frame + (cursor - at), out);
        }
        at += chunk.count;
    }
    return false;
}

static int ending_index(steins_app_t *app, uint16_t string_index)
{
    uint16_t ids[STEINS_MAX_ENDINGS];
    int count = 0;
    const uint32_t nodes = steins_pack_count(&app->pack, STEINS_SEC_NODE);
    for (uint32_t i = 0; i < nodes && count < STEINS_MAX_ENDINGS; ++i) {
        steins_node_t node;
        if (!node_of(app, (uint16_t)i, &node)) continue;
        if (node.ending == STEINS_NONE) continue;
        bool seen = false;
        for (int k = 0; k < count; ++k) {
            if (ids[k] == node.ending) {
                seen = true;
                break;
            }
        }
        if (!seen) ids[count++] = node.ending;
    }
    for (int k = 0; k < count; ++k) {
        if (ids[k] == string_index) return k;
    }
    return -1;
}

static int chapter_number_from_title(const char *title)
{
    int number = 0;
    int digits = 0;
    while (digits < 3 && title[digits] >= '0' && title[digits] <= '9') {
        number = number * 10 + (title[digits] - '0');
        ++digits;
    }
    return digits ? number : 0;
}

// ---------------------------------------------------------------- 画面

static void draw_view(steins_app_t *app)
{
    app->scene_ok = steins_image_show_view(&app->ui.image, &app->pack, app->view_index,
                                           &app->scene_error);
    if (!app->scene_ok) {
        app->drawn_view = STEINS_NONE;   // 保持缓存失效,下次同资源再试
        return;
    }
    app->drawn_view = app->view_index;

    // 鉴赏解锁:该图在正文画面里实际显示过才算解锁。
    steins_view_t view;
    if (!steins_pack_view(&app->pack, app->view_index, &view)) return;
    const uint32_t groups = steins_pack_count(&app->pack, STEINS_SEC_GAL);
    for (uint32_t g = 0; g < groups && g < STEINS_MAX_GALLERY; ++g) {
        steins_gal_t gal;
        if (!steins_pack_gal(&app->pack, (uint16_t)g, &gal)) continue;
        bool hit = false;
        for (uint16_t e = 0; e < gal.count && !hit; ++e) {
            uint16_t asset = STEINS_NONE;
            if (!steins_pack_gentry(&app->pack, (uint16_t)(gal.first + e), &asset)) continue;
            for (uint8_t l = 0; l < view.count; ++l) {
                steins_layer_t layer;
                if (!steins_pack_layer(&app->pack, (uint16_t)(view.layer_first + l),
                                       &layer)) {
                    continue;
                }
                if (layer.asset == asset) {
                    hit = true;
                    break;
                }
            }
        }
        if (hit && steins_unlock_set_gallery(&app->unlock, (uint8_t)g)) {
            app->unlock_dirty = true;
        }
    }
}

// ---------------------------------------------------------------- 渲染:标题

static void render_title(steins_app_t *app)
{
    char worldline[24];
    char wl_label[24];
    char wl[48];
    const int initial_node = steins_pack_meta_int(&app->pack, "initial", 0);
    steins_worldline_text(steins_worldline_rate(&app->pack, (uint16_t)initial_node, 0), worldline,
                          sizeof(worldline));
    ui_text(app, UI_WORLDLINE, wl_label, sizeof(wl_label));
    snprintf(wl, sizeof(wl), "%s %s", wl_label, worldline);

    const bool can_continue = checkpoint_read(app, -1, &s_chk_scratch) == STEINS_SLOT_VALID;
    const char *keys[STEINS_UI_TITLE_ROWS];
    uint8_t actions[STEINS_UI_TITLE_ROWS];
    char texts[STEINS_UI_TITLE_ROWS][48];
    const char *rows[STEINS_UI_TITLE_ROWS];
    int count = 0;
    actions[count] = TITLE_ACT_NEW;
    keys[count++] = UI_TITLE_NEW;
    if (can_continue) {
        actions[count] = TITLE_ACT_CONTINUE;   // 没有自动存档就不列"继续阅读"
        keys[count++] = UI_TITLE_CONTINUE;
    }
    actions[count] = TITLE_ACT_LOAD;
    keys[count++] = UI_TITLE_LOAD;
    actions[count] = TITLE_ACT_GALLERY;
    keys[count++] = UI_TITLE_GALLERY;
    actions[count] = TITLE_ACT_CHAPTER;
    keys[count++] = UI_TITLE_CHAPTER;
    actions[count] = TITLE_ACT_CONFIG;
    keys[count++] = UI_TITLE_CONFIG;
    actions[count] = TITLE_ACT_ABOUT;
    keys[count++] = UI_TITLE_ABOUT;
    app->title_count = count;
    if (app->sel >= count) app->sel = count - 1;
    for (int i = 0; i < count; ++i) {
        app->title_action[i] = actions[i];
        ui_text(app, keys[i], texts[i], sizeof(texts[i]));
        rows[i] = texts[i];
    }
    char name[32];
    ui_text(app, UI_APP_TITLE, name, sizeof(name));
    steins_ui_title(&app->ui, name, wl, rows, count, app->sel);

    // 标题画面:META asset_title_bg(=13) 铺满 320x180 画布;失败留黑并下次重试。
    if (!s_title_art_drawn) {
        const int bg = steins_pack_meta_int(&app->pack, "asset_title_bg", -1);
        if (bg >= 0) {
            steins_image_error_t err;
            s_title_art_drawn = steins_image_show_asset(&app->ui.image, &app->pack,
                                                       (uint16_t)bg, true, &err);
        }
    }
}

// ---------------------------------------------------------------- 渲染:正文

static void format_worldline(const steins_app_t *app, uint16_t node, uint32_t cursor,
                             char *out, size_t cap)
{
    steins_worldline_text(steins_worldline_rate(&app->pack, node, cursor), out, cap);
}

// 渲染用文本 scratch:只用于“拼好一帧立刻交给 LVGL”。lv_label_set_text 会复制内容
// (本文件 steins_ui.c 全用 set_text,无 set_text_static),所以函数返回后即可复用。
// 合同:仅持 LVGL 锁的渲染路径使用;不得跨渲染保留指针;不与 s_chk_scratch 混用。
static char s_render_text[STEINS_TEXT_BYTES + 64];

static void render_game(steins_app_t *app)
{
    char *revealed = s_render_text;
    if (text_fully_shown(app)) {
        copy_text(revealed, sizeof(s_render_text), app->page_data.text);
    } else {
        const int n = app->page_data.offsets[app->shown < 0 ? 0 : app->shown];
        snprintf(revealed, sizeof(s_render_text), "%.*s", n, app->page_data.text);
    }

    char date[64];
    copy_text(date, sizeof(date), app->date);
    char battery[20] = "--";
    if (app->battery_percent >= 0) snprintf(battery, sizeof(battery), "%d%%", app->battery_percent);

    char status[48];
    if (app->notice_ms > 0) {
        copy_text(status, sizeof(status), app->notice);
    } else if (app->auto_mode) {
        ui_text(app, UI_MENU_AUTO_ON, status, sizeof(status));
    } else if (app->fast_forward) {
        snprintf(status, sizeof(status), ">>");
    } else {
        status[0] = 0;
    }
    // 页码独立成一格 label（不再与提示/状态复用同一行）。
    char page[24];
    if (app->page_count > 1) {
        snprintf(page, sizeof(page), "%d/%d", app->page_index + 1, app->page_count);
    } else {
        page[0] = '\0';
    }

    char hint[64];
    if (!app->scene_ok) {
        ui_text(app, UI_IMAGE_FAIL, hint, sizeof(hint));
    } else {
        hint[0] = 0;   // 好画面不显示提示行（正文占满 184..238）
    }
    steins_ui_game(&app->ui, date, battery, app->speaker, revealed, status, page, hint,
                   !app->scene_ok);
}

// ---------------------------------------------------------------- 渲染:选择

static void render_mail(steins_app_t *app)
{
    char page[16];
    snprintf(page, sizeof(page), "%d/%d", app->mail_page + 1, app->mail_pages);
    char hint[48];
    ui_text_fb(app, UI_CHOICE_CONFIRM, "确定", hint, sizeof(hint));
    app->phone_mail_mode = false;
    steins_ui_mail(&app->ui, app->choice_tag, app->mail_data.text, page, hint);
    set_page(app, STEINS_PAGE_MAIL);
}

static void render_choice(steins_app_t *app)
{
    const char *rows[STEINS_CHOICE_MAX];
    for (int i = 0; i < app->choice_count; ++i) rows[i] = app->choice_rows[i];
    char hint[48];
    ui_text_fb(app, UI_CHOICE_CONFIRM, "确定", hint, sizeof(hint));
    steins_ui_choice(&app->ui, app->choice_tag, app->choice_heading, rows,
                     app->choice_count, app->choice_sel, hint);
    set_page(app, STEINS_PAGE_CHOICE);
}

// ---------------------------------------------------------------- 渲染:列表

static void render_list(steins_app_t *app)
{
    const char *subs[STEINS_UI_LIST_ROWS];
    for (int i = 0; i < STEINS_UI_LIST_ROWS; ++i) {
        app->list_main_ptr[i] = app->list_main[i];
        app->list_sub_ptr[i] = app->list_sub[i];
        subs[i] = app->list_sub_ptr[i];
    }
    steins_ui_list_font(&app->ui, app->list_story_font);
    // 失败提示优先显示:否则列表页上的 set_notice_text 永远看不见,按 OK 像没反应。
    const char *hint = app->list_hint;
    if (app->notice_ms > 0 && app->notice[0]) hint = app->notice;
    steins_ui_list(&app->ui, app->list_title, app->list_main_ptr, subs, app->list_count,
                   app->sel, hint);
}

static int window_first(const steins_app_t *app)
{
    return app->sel >= STEINS_UI_LIST_ROWS ? app->sel - STEINS_UI_LIST_ROWS + 1 : 0;
}

static void list_clear(steins_app_t *app)
{
    for (int i = 0; i < STEINS_UI_LIST_ROWS; ++i) {
        app->list_main[i][0] = '\0';
        app->list_sub[i][0] = '\0';
    }
}

// ---------------------------------------------------------------- 正文装载

static bool mobile_sync_at(steins_app_t *app, uint16_t node, uint32_t cursor)
{
    if (!app->mobile_ready) return true;
    app->chk.node = node;
    app->chk.cursor = cursor;
    if (!steins_mobile_enter(&app->mobile, &app->mobile_content, app->mobile_user, node)) {
        return false;
    }
    if (!steins_mobile_sync(&app->mobile, &app->chk)) return false;
    return true;
}

static bool load_frame_at(steins_app_t *app, uint16_t node_index, uint32_t cursor)
{
    if (!mobile_sync_at(app, node_index, cursor)) return false;
    steins_frame_t frame;
    if (!frame_at(app, node_index, cursor, &frame)) return false;
    app->pos.node = node_index;
    app->pos.cursor = cursor;
    app->view_index = frame.view;

    if (steins_pack_str_utf8(&app->pack, frame.text, app->text, sizeof(app->text)) < 0) {
        app->text[0] = '\0';
    }
    if (steins_pack_str_utf8(&app->pack, frame.speaker, app->speaker,
                             sizeof(app->speaker)) < 0) {
        app->speaker[0] = '\0';
    }
    if (steins_pack_str_utf8(&app->pack, frame.aux, app->date, sizeof(app->date)) < 0) {
        app->date[0] = '\0';
    }
    char title[160];
    if (steins_pack_str_utf8(&app->pack, frame.title, title, sizeof(title)) < 0) title[0] = '\0';

    bool title_changed = false;
    if (strcmp(title, app->title) != 0) {
        title_changed = true;
        copy_text(app->title, sizeof(app->title), title);
        const int number = chapter_number_from_title(title);
        if (number >= 1 && number <= 11) {
            const uint32_t chapter_count = steins_pack_count(&app->pack, STEINS_SEC_CHAPTER);
            for (uint32_t i = 0; i < chapter_count && i < STEINS_MAX_CHAPTERS; ++i) {
                steins_chapter_t chapter;
                if (!steins_pack_chapter(&app->pack, (uint16_t)i, &chapter)) continue;
                char name[96];
                if (steins_pack_str_utf8(&app->pack, chapter.name, name, sizeof(name)) < 0) {
                    continue;
                }
                if (strcmp(name, title) == 0) {
                    if (steins_unlock_set_chapter(&app->unlock, (uint8_t)i)) {
                        app->unlock_dirty = true;
                    }
                    app->chapter_card_asset = steins_chapter_card_asset(&app->pack, title);
                    break;
                }
            }
        } else {
            app->chapter_card_asset = STEINS_NONE;
        }
    }

    if (app->drawn_view != app->view_index) draw_view(app);

    app->page_count = steins_page_build(app->text, app->font_size, STEINS_TEXT_W,
                                        STEINS_TEXT_LINES, 0, &app->page_data);
    if (app->page_count <= 0) {
        app->page_count = 1;
        app->page_data.pages = 1;
        app->page_data.page = 0;
        app->page_data.chars = 0;
        app->page_data.text[0] = '\0';
    }
    app->page_index = app->page_data.page;
    app->shown = app->fast_forward ? app->page_data.chars : 0;
    app->type_ms = 0;
    app->auto_ms = 0;

    set_page(app, STEINS_PAGE_GAME);
    render_game(app);

    // 源在章节开头插入标题卡(takeChapterCard + CHAPTER_CARD_HOLD_MS)。
    if (title_changed && app->chapter_card_asset != STEINS_NONE) {
        char hint[32];
        ui_text_fb(app, UI_CHOICE_CONFIRM, "确定", hint, sizeof(hint));
        steins_ui_cg(&app->ui, app->title, hint);
        app->cg_asset = app->chapter_card_asset;
        app->cg_valid = steins_image_show_asset(&app->ui.image, &app->pack,
                                               app->chapter_card_asset, true,
                                               &app->scene_error);
        if (app->cg_valid) {
            app->chapter_card_active = true;
            app->chapter_card_ms = 0;
            set_page(app, STEINS_PAGE_CG);
        } else {
            app->chapter_card_active = false;
        }
    } else {
        app->chapter_card_active = false;
    }
    return true;
}

static void dismiss_chapter_card(steins_app_t *app)
{
    if (!app->chapter_card_active) return;
    app->chapter_card_active = false;
    set_page(app, STEINS_PAGE_GAME);
    render_game(app);
}

static bool render_current(steins_app_t *app, bool autosave)
{
    steins_node_t node;
    if (!node_of(app, app->pos.node, &node)) return false;
    if (!mobile_sync_at(app, app->pos.node, app->pos.cursor)) return false;
    // The graph materializes the initial phone choices, but their skipped
    // branches do not contain the source's final key-phone prompt.
    steins_mobile_frame_t boundary;
    const bool keyphone = app->mobile_ready &&
        steins_mobile_keyphone_boundary(&app->mobile, &app->chk, &boundary);
    if (!keyphone && app->pos.cursor < node.count) {
        if (!load_frame_at(app, app->pos.node, app->pos.cursor)) return false;
        if (autosave) {
            app->progress_dirty = true;
            app->progress_ms = 0;
        }
        return true;
    }
    steins_node_t choice_node = node;
    if (keyphone && !node_of(app, boundary.node, &choice_node)) return false;
    const uint16_t choice_record = choice_node.choice;
    if (choice_record != STEINS_NONE) {
        steins_choice_t choice;
        if (!steins_pack_choice(&app->pack, choice_record, &choice)) return false;
        app->choice_record = choice_record;
        app->choice_kind = choice.kind;
        app->choice_count = choice.count > STEINS_CHOICE_MAX ? STEINS_CHOICE_MAX : choice.count;
        if (app->choice_sel >= app->choice_count) app->choice_sel = 0;
        if (app->choice_sel < 0) app->choice_sel = 0;
        // heading 就是邮件正文(发件人+标题+正文):实测最长 509 B,必须完整读出来。
        // 转码失败时 str_utf8 也会给出合法的 NUL 结尾前缀,不会留下旧内容。
        if (steins_pack_str_utf8(&app->pack, choice.heading, app->choice_heading,
                                 sizeof(app->choice_heading)) < 0) {
            ESP_LOGW(TAG, "choice %u heading 超出 %u B,已按前缀显示",
                     (unsigned)node.choice, (unsigned)sizeof(app->choice_heading));
        }
        if (steins_pack_str_utf8(&app->pack, choice.category, app->choice_tag,
                                 sizeof(app->choice_tag)) < 0) {
            app->choice_tag[0] = 0;
        }
        for (int i = 0; i < app->choice_count; ++i) {
            steins_citem_t item;
            if (!steins_pack_citem(&app->pack, (uint16_t)(choice.first_item + i), &item)) {
                app->choice_rows[i][0] = '\0';
                continue;
            }
            if (steins_pack_str_utf8(&app->pack, item.text, app->choice_rows[i],
                                     sizeof(app->choice_rows[i])) < 0) {
                app->choice_rows[i][0] = '\0';
            }
            // Source keyPhoneBoundary() rewrites the final no-call/no-send
            // labels, while preserving every other item text verbatim.
            if (keyphone && item.action == STEINS_ACT_NOSEND) {
                copy_text(app->choice_rows[i], sizeof(app->choice_rows[i]),
                          "确认不发送，继续剧情");
            } else if (keyphone && item.action == STEINS_ACT_NOCALL) {
                copy_text(app->choice_rows[i], sizeof(app->choice_rows[i]),
                          "确认不拨打，继续剧情");
            }
        }
        if (keyphone) {
            copy_text(app->choice_heading, sizeof(app->choice_heading),
                      "最后确认本段手机操作");
            copy_text(app->choice_tag, sizeof(app->choice_tag),
                      "即将超过电话操作时限");
        }
        app->auto_mode = false;
        app->fast_forward = false;
        // 正文超过一屏(邮件)时先给可翻页的阅读页,读完了再回到选项;
        // 阅读页上的任何按键都不会确认选项。
        app->mail_pages = steins_page_build(app->choice_heading, app->font_size,
                                            STEINS_TEXT_W, STEINS_MAIL_LINES, 0,
                                            &app->mail_data);
        if (app->mail_pages <= 0) {
            // 正文超出分页工作区:不假装读完,仍给选择页,并在提示里说明。
            ESP_LOGW(TAG, "choice %u 正文超出分页工作区,直接给选项", (unsigned)node.choice);
            app->mail_pages = 1;
            app->mail_pending_choice = false;
            render_choice(app);
        } else if (app->mail_pages > 1) {
            app->mail_page = 0;
            app->mail_pending_choice = true;
            render_mail(app);
        } else {
            app->mail_pending_choice = false;
            render_choice(app);
        }
        if (autosave) {
            app->progress_dirty = true;
            app->progress_ms = 0;
        }
        return true;
    }
    if (node.ending != STEINS_NONE) {
        if (steins_pack_str_utf8(&app->pack, node.ending, app->ending_name,
                                 sizeof(app->ending_name)) < 0) {
            app->ending_name[0] = '\0';
        }
        // 源只在 story 模式把结局写进全局进度(progressFromState 以
        // progression==='story' 为前提);章节回顾可以读到结局,但不点亮全局位。
        bool changed = false;
        if (app->pos.mode == STEINS_MODE_STORY) {
            const int index = ending_index(app, node.ending);
            if (index >= 0 && steins_unlock_set_ending(&app->unlock, (uint8_t)index)) {
                changed = true;
            }
            static const char TRUE_END[] = "\xE7\x9C\x9F\xE7\xBB\x93\xE5\xB1\x80";
            if (strcmp(app->ending_name, TRUE_END) == 0 &&
                !steins_unlock_true_end(&app->unlock)) {
                steins_unlock_set_true_end(&app->unlock);
                changed = true;
            }
        }
        if (changed) app->unlock_dirty = true;
        app->progress_dirty = true;
        app->auto_mode = false;
        app->fast_forward = false;

        char collected[96];
        int unlocked = 0;
        for (int i = 0; i < STEINS_MAX_ENDINGS; ++i) {
            if (steins_unlock_has_ending(&app->unlock, (uint8_t)i)) ++unlocked;
        }
        char label[24];
        ui_text(app, UI_ENDING_REACHED, label, sizeof(label));
        snprintf(collected, sizeof(collected), "%s %d", app->ending_name, unlocked);
        char hint[32];
        ui_text_fb(app, "end.new_game", "OK 返回标题", hint, sizeof(hint));
        steins_ui_ending(&app->ui, label, collected, hint);
        set_page(app, STEINS_PAGE_ENDING);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- 推进

static bool text_fully_shown(const steins_app_t *app)
{
    return app->shown >= app->page_data.chars;
}

static void push_history(steins_app_t *app)
{
    if (app->text[0] == '\0') return;
    if (app->history_count >= STEINS_HISTORY_MAX) {
        memmove(app->history[0], app->history[1],
                sizeof(app->history[0]) * (STEINS_HISTORY_MAX - 1));
        app->history_count = STEINS_HISTORY_MAX - 1;
    }
    copy_text(app->history[app->history_count], STEINS_LIST_TEXT, app->text);
    ++app->history_count;
}

static void advance(steins_app_t *app)
{
    if (app->page != STEINS_PAGE_GAME) return;
    if (!text_fully_shown(app)) {
        app->shown = app->page_data.chars;
        render_game(app);
        return;
    }
    steins_node_t node;
    if (!node_of(app, app->pos.node, &node)) return;
    if (app->page_index + 1 < app->page_count) {
        push_history(app);
        steins_page_build(app->text, app->font_size, STEINS_TEXT_W, STEINS_TEXT_LINES,
                          app->page_index + 1, &app->page_data);
        app->page_index = app->page_data.page;
        app->shown = 0;
        app->type_ms = 0;
        render_game(app);
        return;
    }
    push_history(app);
    app->pos.cursor += 1u;
    if (!render_current(app, true)) {
        ESP_LOGW(TAG, "节点 %u 游标 %u 之后无法继续", (unsigned)app->pos.node,
                 (unsigned)app->pos.cursor);
    }
}

// ---------------------------------------------------------------- 手机页

static bool mobile_clip_valid(steins_app_t *app, uint16_t node)
{
    if (!app || !app->mobile_ready || node < STEINS_MOB_CLIP_NODE_BASE) return false;
    uint32_t frames = 0;
    return app->mobile_content.clip_frames &&
           app->mobile_content.clip_frames(app->mobile_user, node, &frames) && frames > 0;
}

static bool render_phone_clip(steins_app_t *app, uint16_t node, uint32_t frame)
{
    if (!mobile_clip_valid(app, node)) return false;
    const uint8_t clip_index = (uint8_t)(node - STEINS_MOB_CLIP_NODE_BASE);
    steins_clip_t clip;
    if (!steins_pack_clip(&app->pack, clip_index, &clip) || clip.frame_count == 0) return false;
    if (frame >= clip.frame_count) frame = clip.frame_count - 1;
    steins_cframe_t cf;
    if (!steins_pack_cframe(&app->pack, (uint16_t)(clip.first_frame + frame), &cf)) return false;
    char speaker[64] = {0};
    char *text = s_render_text;   // 与 render_game 共用渲染 scratch;两者不会嵌套调用
    (void)steins_pack_str_utf8(&app->pack, cf.speaker, speaker, sizeof(speaker));
    (void)steins_pack_str_utf8(&app->pack, cf.text, text, STEINS_TEXT_BYTES);
    char title[96];
    snprintf(title, sizeof(title), "通话 %u/%u", (unsigned)(frame + 1),
             (unsigned)clip.frame_count);
    char page[16];
    snprintf(page, sizeof(page), "%u/%u", (unsigned)(frame + 1),
             (unsigned)clip.frame_count);
    app->phone_clip_node = node;
    app->phone_clip_frame = frame;
    app->phone_clip_count = clip.frame_count;
    app->phone_mail_mode = false;
    steins_ui_mail(&app->ui, speaker[0] ? speaker : title, text, page,
                   "上/下切换通话帧 · 末帧确定返回");
    set_page(app, STEINS_PAGE_PHONE_CLIP);
    return true;
}

static void phone_clip_key(steins_app_t *app, const steins_key_t *key)
{
    if (key->ev == KEY_LONG) {
        return_back(app);
        return;
    }
    if (key->ev != KEY_CLICK) return;
    if (key->btn == BTN_UP && app->phone_clip_frame > 0) {
        (void)render_phone_clip(app, app->phone_clip_node, app->phone_clip_frame - 1);
        return;
    }
    if (key->btn == BTN_DOWN &&
        app->phone_clip_frame + 1 < app->phone_clip_count) {
        (void)render_phone_clip(app, app->phone_clip_node, app->phone_clip_frame + 1);
        return;
    }
    if (key->btn == BTN_OK) {
        steins_mobile_pos_t land;
        if (app->mobile_ready &&
            steins_mobile_return_call(&app->mobile, &app->chk, &land)) {
            steins_pos_t pos = { land.node, land.cursor, app->pos.mode, 0 };
            (void)apply_position(app, &pos);
        } else {
            return_back(app);
        }
    }
}

static bool phone_mail_meta(steins_app_t *app, uint16_t id, steins_mail_t *mail)
{
    return steins_pack_mail(&app->pack, id, mail);
}

static uint16_t phone_id_at(steins_app_t *app, bool sent, int index)
{
    uint16_t id = 0xFFFFu;
    if (index < 0) return id;
    (void)(sent ? steins_mobile_sent_at(&app->chk, (uint16_t)index, &id)
                : steins_mobile_inbox_at(&app->chk, (uint16_t)index, &id));
    return id;
}

static void fill_phone_home(steins_app_t *app)
{
    list_clear(app);
    app->list_count = 4;
    copy_text(app->list_title, sizeof(app->list_title), "手机");
    copy_text(app->list_main[0], STEINS_LIST_TEXT, "收信箱");
    snprintf(app->list_sub[0], sizeof(app->list_sub[0]), "%u 封 / %u 未读",
             (unsigned)steins_mobile_inbox_count(&app->chk),
             (unsigned)steins_mobile_unread_count(&app->chk));
    copy_text(app->list_main[1], STEINS_LIST_TEXT, "已发送");
    snprintf(app->list_sub[1], sizeof(app->list_sub[1]), "%u 封",
             (unsigned)steins_mobile_sent_count(&app->chk));
    copy_text(app->list_main[2], STEINS_LIST_TEXT, "联系人");
    copy_text(app->list_sub[2], sizeof(app->list_sub[2]), "按剧情开放");
    copy_text(app->list_main[3], STEINS_LIST_TEXT, "当前电话/发信");
    if (app->chk.keyphone_present) copy_text(app->list_sub[3], sizeof(app->list_sub[3]), "有可操作项");
    else if (app->chk.call_present) copy_text(app->list_sub[3], sizeof(app->list_sub[3]), "有来电");
    else if (app->chk.pres_count) copy_text(app->list_sub[3], sizeof(app->list_sub[3]), "有手机屏演示");
    else copy_text(app->list_sub[3], sizeof(app->list_sub[3]), "本段没有可操作项");
}

static void open_phone_home(steins_app_t *app)
{
    nav_push(app, app->page, app->sel, 0);
    app->phone_sel = 0;
    app->sel = 0;
    app->phone_return_to_game = true;
    set_list_hint(app, "上/下选择 · OK 打开 · 长按返回");
    set_page(app, STEINS_PAGE_PHONE_HOME);
    fill_phone_home(app);
    render_list(app);
}

static void fill_phone_list(steins_app_t *app, bool sent)
{
    list_clear(app);
    app->list_count = sent ? (int)steins_mobile_sent_count(&app->chk)
                           : (int)steins_mobile_inbox_count(&app->chk);
    if (app->list_count <= 0) {
        copy_text(app->list_main[0], STEINS_LIST_TEXT,
                  sent ? "暂无已发送邮件" : "暂无收到的邮件");
        app->list_count = 1;
        return;
    }
    const int first = window_first(app);
    for (int i = 0; i < STEINS_UI_LIST_ROWS; ++i) {
        const int index = first + i;
        if (index >= app->list_count) continue;
        const uint16_t id = phone_id_at(app, sent, index);
        steins_mail_t mail;
        if (!phone_mail_meta(app, id, &mail)) {
            snprintf(app->list_main[i], STEINS_LIST_TEXT, "邮件 %u", (unsigned)id);
            continue;
        }
        char sender[48] = {0};
        char title[96] = {0};
        (void)steins_pack_str_utf8(&app->pack, mail.sender, sender, sizeof(sender));
        (void)steins_pack_str_utf8(&app->pack, mail.title, title, sizeof(title));
        snprintf(app->list_main[i], STEINS_LIST_TEXT, "%s", sender[0] ? sender : "手机邮件");
        copy_text(app->list_sub[i], sizeof(app->list_sub[i]), title);
    }
}

static void open_phone_list(steins_app_t *app, bool sent)
{
    nav_push(app, app->page, app->sel, 0);
    app->phone_sel = 0;
    app->phone_mail_sent = sent;
    app->sel = 0;
    copy_text(app->list_title, sizeof(app->list_title), sent ? "已发送" : "收信箱");
    copy_text(app->list_hint, sizeof(app->list_hint), "上/下选择 · 确定阅读 · 长按返回");
    set_page(app, sent ? STEINS_PAGE_PHONE_SENT : STEINS_PAGE_PHONE_INBOX);
    fill_phone_list(app, sent);
    render_list(app);
}

static bool phone_can_reply_ui(steins_app_t *app, uint16_t id)
{
    if (app->mobile_ready) return steins_mobile_can_reply(&app->mobile, &app->chk, id);
    if (steins_checkpoint_replied(&app->chk, id)) return false;
    const int16_t channel = steins_mobile_open_channel(&app->chk, id);
    const bool deferred = app->chk.deferred_present && app->chk.deferred_id == id;
    if (channel < 0 && !deferred) return false;
    steins_mail_t mail;
    if (!phone_mail_meta(app, id, &mail)) return false;
    uint16_t succ[16];
    return steins_pack_mail_succ(&app->pack, &mail, succ, 16, true) > 0;
}

static bool open_phone_presentation(steins_app_t *app)
{
    if (!app->mobile_ready) return false;
    uint16_t id = 0;
    uint8_t kind = 0, owner = 0;
    if (!steins_mobile_plot_presentation(&app->chk, &id, &kind, &owner)) return false;
    steins_mail_t mail;
    if (!phone_mail_meta(app, id, &mail)) return false;
    nav_push(app, app->page, app->sel, 0);
    app->phone_mail = id;
    app->phone_mail_sent = false;
    app->phone_mail_mode = true;
    app->phone_presentation_pending = true;
    app->phone_mail_page = 0;
    snprintf(app->phone_title, sizeof(app->phone_title), "手机屏演示 · %s",
             kind == STEINS_MOB_PRES_SEND ? "发送" :
             kind == STEINS_MOB_PRES_DRAFT ? "草稿" : "阅读");
    app->phone_body[0] = 0;
    if (mail.body != 0xFFFFu) {
        (void)steins_pack_str_utf8(&app->pack, mail.body, app->phone_body,
                                   sizeof(app->phone_body));
    }
    app->phone_reply_count = 0;
    app->phone_mail_pages = steins_page_build(app->phone_body, app->font_size, STEINS_TEXT_W,
                                              STEINS_MAIL_LINES, 0, &app->mail_data);
    render_phone_mail(app);
    return true;
}

static void render_phone_mail(steins_app_t *app)
{
    if (app->phone_mail_pages <= 0) {
        copy_text(app->phone_body, sizeof(app->phone_body), "邮件正文无法分页");
        app->phone_mail_pages = 1;
        app->phone_mail_page = 0;
        app->mail_data.text[0] = 0;
    } else {
        (void)steins_page_build(app->phone_body, app->font_size, STEINS_TEXT_W,
                                STEINS_MAIL_LINES, app->phone_mail_page,
                                &app->mail_data);
    }
    char page[16];
    snprintf(page, sizeof(page), "%d/%d", app->phone_mail_page + 1, app->phone_mail_pages);
    char hint[64];
    if (app->phone_mail_page + 1 < app->phone_mail_pages) {
        copy_text(hint, sizeof(hint), "上/下翻页 · 确定在末页继续");
    } else if (app->phone_presentation_pending) {
        copy_text(hint, sizeof(hint), "确定完成演示 · 长按返回");
    } else if (!app->phone_mail_sent && app->phone_reply_count > 0) {
        copy_text(hint, sizeof(hint), "确定选择回复 · 长按返回");
    } else {
        copy_text(hint, sizeof(hint), "确定返回 · 长按返回");
    }
    app->phone_mail_mode = true;
    steins_ui_mail(&app->ui, app->phone_title, app->mail_data.text, page, hint);
    set_page(app, STEINS_PAGE_MAIL);
}

static void open_phone_detail(steins_app_t *app, uint16_t id, bool sent)
{
    steins_mail_t mail;
    if (!phone_mail_meta(app, id, &mail)) {
        set_notice_text(app, "邮件内容不可用");
        render_list(app);
        return;
    }
    nav_push(app, app->page, app->sel, 0);
    app->phone_mail = id;
    app->phone_mail_sent = sent;
    app->phone_mail_page = 0;
    app->phone_mail_pages = 0;
    char sender[48] = {0};
    char title[96] = {0};
    (void)steins_pack_str_utf8(&app->pack, mail.sender, sender, sizeof(sender));
    (void)steins_pack_str_utf8(&app->pack, mail.title, title, sizeof(title));
    if (sent) snprintf(app->phone_title, sizeof(app->phone_title), "%s", title[0] ? title : sender);
    else snprintf(app->phone_title, sizeof(app->phone_title), "%s%s%s", sender,
                  title[0] ? " · " : "", title);
    app->phone_body[0] = 0;
    if (mail.body != 0xFFFFu) {
        (void)steins_pack_str_utf8(&app->pack, mail.body, app->phone_body,
                                   sizeof(app->phone_body));
    }
    app->phone_reply_count = 0;
    if (!sent) {
        if (app->mobile_ready) {
            steins_mobile_pos_t land;
            if (!steins_mobile_mark_read(&app->mobile, &app->chk, id, &land)) {
                set_notice_text(app, "邮件已读状态更新失败");
            } else if (land.node != app->chk.node || land.cursor != app->chk.cursor) {
                steins_pos_t pos = { land.node, land.cursor, app->pos.mode, 0 };
                (void)apply_position(app, &pos);
            }
        } else {
            (void)steins_checkpoint_set_unread(&app->chk, id, false);
            (void)steins_checkpoint_set_read(&app->chk, id, true);
        }
        app->checkpoint_dirty = true;
        if (phone_can_reply_ui(app, id)) {
            uint16_t succ[16];
            app->phone_reply_count = steins_pack_mail_succ(&app->pack, &mail, succ, 16, true);
            for (uint8_t i = 0; i < app->phone_reply_count; ++i) app->phone_reply_ids[i] = succ[i];
        }
    }
    app->phone_mail_pages = steins_page_build(app->phone_body, app->font_size, STEINS_TEXT_W,
                                              STEINS_MAIL_LINES, 0, &app->mail_data);
    render_phone_mail(app);
}

static void fill_phone_replies(steins_app_t *app)
{
    list_clear(app);
    app->list_count = app->phone_reply_count;
    if (app->list_count <= 0) {
        copy_text(app->list_main[0], STEINS_LIST_TEXT, "没有可回复的邮件");
        app->list_count = 1;
        return;
    }
    const int first = window_first(app);
    for (int i = 0; i < STEINS_UI_LIST_ROWS; ++i) {
        const int index = first + i;
        if (index >= app->list_count) continue;
        const uint16_t id = app->phone_reply_ids[index];
        steins_mail_t mail;
        if (phone_mail_meta(app, id, &mail)) {
            char title[96] = {0};
            (void)steins_pack_str_utf8(&app->pack, mail.title, title, sizeof(title));
            copy_text(app->list_main[i], STEINS_LIST_TEXT, title[0] ? title : "回复");
        } else {
            snprintf(app->list_main[i], STEINS_LIST_TEXT, "回复 %u", (unsigned)id);
        }
    }
}

static void open_phone_replies(steins_app_t *app)
{
    if (app->phone_reply_count == 0) {
        set_notice_text(app, "这封邮件现在不能回复");
        render_phone_mail(app);
        return;
    }
    nav_push(app, app->page, app->sel, 0);
    app->sel = 0;
    copy_text(app->list_title, sizeof(app->list_title), "选择回复");
    copy_text(app->list_hint, sizeof(app->list_hint), "上/下选择 · 确定发送 · 长按取消");
    set_page(app, STEINS_PAGE_PHONE_REPLY);
    fill_phone_replies(app);
    render_list(app);
}

static void send_phone_reply(steins_app_t *app, uint16_t child)
{
    const uint16_t parent = app->phone_mail;
    if (app->mobile_ready) {
        steins_mobile_pos_t land;
        if (!steins_mobile_send_reply(&app->mobile, &app->chk, parent, child, &land)) {
            set_notice_text(app, "这封邮件现在不能回复");
            render_phone_mail(app);
            return;
        }
    } else {
        if (steins_checkpoint_replied(&app->chk, parent)) return;
        if (child >= STEINS_MAIL_IDS) return;
        (void)steins_checkpoint_set_unread(&app->chk, parent, false);
        (void)steins_checkpoint_set_read(&app->chk, parent, true);
        bool in_sent = false;
        for (uint16_t i = 0; i < app->chk.sent_count; ++i)
            if (app->chk.sent[i] == child) in_sent = true;
        if (!in_sent && !steins_checkpoint_push_sent(&app->chk, child)) return;
        (void)steins_checkpoint_set_replied(&app->chk, parent, true);
    }
    app->checkpoint_dirty = true;
    open_phone_detail(app, child, true);
}

// Source keyPhoneActions exposes only call/send while the decision is pending.
// Keep the original item indices so both phone entry points use the same answer.
static uint8_t keyphone_calls(steins_app_t *app, uint8_t *indices, uint8_t cap)
{
    if (!app->mobile_ready || !app->chk.keyphone_present ||
        !app->mobile_content.choice_items) return 0;
    steins_mobile_choice_item_t items[STEINS_CHOICE_MAX];
    const uint8_t count = app->mobile_content.choice_items(app->mobile_user,
        app->chk.keyphone_root, items, STEINS_CHOICE_MAX);
    uint8_t found = 0;
    for (uint8_t i = 0; i < count && i < STEINS_CHOICE_MAX && found < cap; ++i) {
        if (items[i].action == STEINS_MOB_ACT_CALL ||
            items[i].action == STEINS_MOB_ACT_SEND) indices[found++] = i;
    }
    return found;
}

static void fill_phone_calls(steins_app_t *app)
{
    list_clear(app);
    int count = 0;
    uint8_t indices[STEINS_CHOICE_MAX];
    const uint8_t key_count = keyphone_calls(app, indices, STEINS_CHOICE_MAX);
    if (key_count > 0) {
        steins_node_t root;
        steins_choice_t choice;
        if (node_of(app, app->chk.keyphone_root, &root) &&
            steins_pack_choice(&app->pack, root.choice, &choice)) {
            for (uint8_t i = 0; i < key_count && count < STEINS_UI_LIST_ROWS; ++i) {
                steins_citem_t item;
                if (!steins_pack_citem(&app->pack,
                    (uint16_t)(choice.first_item + indices[i]), &item)) continue;
                if (steins_pack_str_utf8(&app->pack, item.label,
                    app->list_main[count], STEINS_LIST_TEXT) <= 0) {
                    (void)steins_pack_str_utf8(&app->pack, item.text,
                        app->list_main[count], STEINS_LIST_TEXT);
                }
                ++count;
            }
        }
    } else if (app->chk.call_present) {
        steins_callsite_t site;
        if (steins_pack_call_site(&app->pack, app->chk.call_site, &site)) {
            for (uint8_t i = 0; i < site.item_count && count < STEINS_UI_LIST_ROWS; ++i) {
                steins_callitem_t item;
                if (!steins_pack_call_item(&app->pack,
                                           (uint16_t)(site.first_item + i), &item)) continue;
                char text[STEINS_LIST_TEXT] = {0};
                (void)steins_pack_str_utf8(&app->pack, item.text, text, sizeof(text));
                snprintf(app->list_main[count], STEINS_LIST_TEXT, "%s", text);
                copy_text(app->list_sub[count], sizeof(app->list_sub[count]),
                          item.clip != 0xFFu ? "通话片段" : "接听");
                ++count;
            }
        }
    }
    if (count == 0) {
        copy_text(app->list_main[0], STEINS_LIST_TEXT, "本段没有可用电话");
        copy_text(app->list_sub[0], sizeof(app->list_sub[0]), "随剧情推进更新");
        count = 1;
    }
    app->list_count = count;
}

static void open_phone_calls(steins_app_t *app)
{
    nav_push(app, app->page, app->sel, 0);
    app->sel = 0;
    copy_text(app->list_title, sizeof(app->list_title), "电话／发信");
    copy_text(app->list_hint, sizeof(app->list_hint), "上/下选择 · 确定接入 · 长按返回");
    set_page(app, STEINS_PAGE_PHONE_CALLS);
    fill_phone_calls(app);
    render_list(app);
}

static void open_phone_contacts(steins_app_t *app)
{
    nav_push(app, app->page, app->sel, 0);
    app->sel = 0;
    list_clear(app);
    copy_text(app->list_title, sizeof(app->list_title), "联系人");
    copy_text(app->list_main[0], STEINS_LIST_TEXT, "冈部");
    copy_text(app->list_sub[0], sizeof(app->list_sub[0]), "手机主人");
    copy_text(app->list_main[1], STEINS_LIST_TEXT, "萌郁");
    copy_text(app->list_main[2], STEINS_LIST_TEXT, "FB");
    copy_text(app->list_main[3], STEINS_LIST_TEXT, "菲利斯");
    app->list_count = 4;
    set_list_hint(app, "本段联系人随剧情开放 · 长按返回");
    set_page(app, STEINS_PAGE_PHONE_CONTACTS);
    render_list(app);
}

// ---------------------------------------------------------------- 页面打开

static void go_title(steins_app_t *app)
{
    s_title_art_drawn = false;   // 画布可能是上一帧剧情图,进标题必须重画
    app->nav_depth = 0;
    app->auto_mode = false;
    app->fast_forward = false;
    app->sel = 0;
    if (app->reading) (void)checkpoint_write(app, -1);
    if (app->unlock_dirty) unlock_save(app);
    set_page(app, STEINS_PAGE_TITLE);
    render_title(app);
}

static void open_menu(steins_app_t *app)
{
    nav_push(app, app->page, app->sel, 0);
    list_clear(app);
    app->list_story_font = false;
    app->list_title[0] = '\0';
    set_list_hint(app, "上/下选择 · OK 确定 · 长按返回");
    app->sel = 0;
    set_page(app, STEINS_PAGE_MENU);
    fill_menu(app);
    render_list(app);
}

#define MENU_COUNT 10

static void menu_key(const steins_app_t *app, int index, char *out, size_t cap)
{
    switch (index) {
    case 0: ui_text((steins_app_t *)app, UI_MENU_RESUME, out, cap); break;
    case 1: ui_text((steins_app_t *)app, UI_MENU_SAVE, out, cap); break;
    case 2: ui_text((steins_app_t *)app, UI_MENU_LOAD, out, cap); break;
    case 3: ui_text_fb((steins_app_t *)app, UI_MENU_PHONE, "手机", out, cap); break;
    case 4: ui_text((steins_app_t *)app, UI_MENU_HISTORY, out, cap); break;
    case 5:
        ui_text((steins_app_t *)app, app->auto_mode ? UI_MENU_AUTO_ON : UI_MENU_AUTO_OFF,
                out, cap);
        break;
    case 6: ui_text((steins_app_t *)app, UI_MENU_CHAPTER, out, cap); break;
    case 7: ui_text((steins_app_t *)app, UI_MENU_GALLERY, out, cap); break;
    case 8: ui_text((steins_app_t *)app, UI_SETTINGS_TITLE, out, cap); break;
    default: ui_text((steins_app_t *)app, UI_MENU_TITLE, out, cap); break;
    }
}

static void fill_menu(steins_app_t *app)
{
    list_clear(app);
    app->list_count = MENU_COUNT;
    const int first = window_first(app);
    for (int i = 0; i < STEINS_UI_LIST_ROWS; ++i) {
        const int index = first + i;
        if (index < MENU_COUNT) menu_key(app, index, app->list_main[i], STEINS_LIST_TEXT);
    }
}

// ---------------------------------------------------------------- 存读档

#define STEINS_SLOT_RAW 30
static char s_slot_main[STEINS_SLOT_RAW][28];
static char s_slot_sub[STEINS_SLOT_RAW][72];

static bool saving_mode(const steins_app_t *app)
{
    return app->nav_depth > 0 && app->nav[app->nav_depth - 1].kind == 1;
}

static void format_slot(steins_app_t *app, int slot)
{
    steins_checkpoint_t *chk = &s_chk_scratch;
    const steins_slot_state_t state = checkpoint_read(app, slot, chk);
    app->slot_state[slot] = state;
    if (state != STEINS_SLOT_VALID) {
        snprintf(s_slot_main[slot], sizeof(s_slot_main[slot]), "%02d", slot + 1);
        if (state == STEINS_SLOT_EMPTY) {
            ui_text(app, UI_SAVE_EMPTY, s_slot_sub[slot], sizeof(s_slot_sub[slot]));
        } else if (state == STEINS_SLOT_CORRUPT) {
            copy_text(s_slot_sub[slot], sizeof(s_slot_sub[slot]), "存档损坏");
        } else {
            copy_text(s_slot_sub[slot], sizeof(s_slot_sub[slot]), "读取失败");
        }
        return;
    }
    char worldline[24];
    format_worldline(app, chk->node, chk->cursor, worldline, sizeof(worldline));
    {
        char *dst = s_slot_main[slot];
        const size_t cap = sizeof(s_slot_main[slot]);
        dst[0] = (char)('0' + (slot + 1) / 10 % 10);
        dst[1] = (char)('0' + (slot + 1) % 10);
        dst[2] = ' ';
        size_t used = 3;
        for (size_t i = 0; worldline[i] && used + 1u < cap; ++i) dst[used++] = worldline[i];
        dst[used] = '\0';
    }
    steins_frame_t frame;
    char preview[64];
    preview[0] = '\0';
    if (frame_at(app, chk->node, chk->cursor, &frame)) {
        char raw[STEINS_LIST_TEXT];
        if (steins_pack_str_utf8(&app->pack, frame.text, raw, sizeof(raw)) >= 0) {
            steins_text_prefix(raw, 12, preview, sizeof(preview));
        }
    }
    copy_text(s_slot_sub[slot], sizeof(s_slot_sub[slot]), preview);
}

static void fill_slots(steins_app_t *app)
{
    list_clear(app);
    app->list_count = STEINS_SLOT_RAW;
    const int first = window_first(app);
    for (int i = 0; i < STEINS_UI_LIST_ROWS; ++i) {
        const int slot = first + i;
        if (slot >= STEINS_SLOT_RAW) continue;
        copy_text(app->list_main[i], STEINS_LIST_TEXT, s_slot_main[slot]);
        copy_text(app->list_sub[i], sizeof(app->list_sub[i]), s_slot_sub[slot]);
    }
}

static void open_saves(steins_app_t *app, bool saving)
{
    nav_push(app, app->page, app->sel, saving ? 1 : 0);
    app->list_story_font = true;
    set_list_hint(app, saving ? "OK 存档 · 长按返回" : "OK 读档 · 长按返回");
    app->sel = 0;
    for (int i = 0; i < STEINS_SLOT_RAW; ++i) {
        app->slot_state[i] = checkpoint_read(app, i, NULL);
        format_slot(app, i);
    }
    ui_text(app, saving ? UI_SAVE_TITLE : UI_LOAD_TITLE, app->list_title,
            sizeof(app->list_title));
    set_page(app, STEINS_PAGE_SAVES);
    fill_slots(app);
    render_list(app);
}

// ---------------------------------------------------------------- 历史/设置

static void fill_history(steins_app_t *app)
{
    list_clear(app);
    app->list_count = app->history_count;
    const int first = window_first(app);
    for (int i = 0; i < STEINS_UI_LIST_ROWS; ++i) {
        const int slot = first + i;
        if (slot >= 0 && slot < app->history_count) {
            copy_text(app->list_main[i], STEINS_LIST_TEXT, app->history[slot]);
        }
    }
}

static void open_history(steins_app_t *app)
{
    nav_push(app, app->page, app->sel, 0);
    app->list_story_font = true;
    app->list_count = app->history_count;
    ui_text(app, UI_HISTORY_TITLE, app->list_title, sizeof(app->list_title));
    set_list_hint(app, "OK 返回 · 长按返回");
    app->sel = app->history_count > 0 ? app->history_count - 1 : 0;
    set_page(app, STEINS_PAGE_HISTORY);
    list_clear(app);
    const int first = window_first(app);
    for (int i = 0; i < STEINS_UI_LIST_ROWS; ++i) {
        const int slot = first + i;
        if (slot < app->history_count) {
            copy_text(app->list_main[i], STEINS_LIST_TEXT, app->history[slot]);
        }
    }
    render_list(app);
}

static void fill_settings(steins_app_t *app)
{
    char label[32];
    char value[16];
    list_clear(app);
    app->list_count = 4;
    ui_text(app, UI_SETTINGS_FONT, label, sizeof(label));
    copy_text(app->list_main[0], STEINS_LIST_TEXT, label);
    snprintf(app->list_sub[0], sizeof(app->list_sub[0]), "16px");
    ui_text(app, UI_SETTINGS_TEXT_SPEED, label, sizeof(label));
    copy_text(app->list_main[1], STEINS_LIST_TEXT, label);
    snprintf(value, sizeof(value), "%u", (unsigned)(app->text_speed + 1));
    snprintf(app->list_sub[1], sizeof(app->list_sub[1]), "%s", value);
    ui_text(app, UI_SETTINGS_AUTO_SPEED, label, sizeof(label));
    copy_text(app->list_main[2], STEINS_LIST_TEXT, label);
    snprintf(value, sizeof(value), "%u", (unsigned)(app->auto_speed + 1));
    snprintf(app->list_sub[2], sizeof(app->list_sub[2]), "%s", value);
    ui_text(app, UI_SETTINGS_OPACITY, label, sizeof(label));
    copy_text(app->list_main[3], STEINS_LIST_TEXT, label);
    snprintf(value, sizeof(value), "%u", (unsigned)(app->box_opacity + 1));
    snprintf(app->list_sub[3], sizeof(app->list_sub[3]), "%s", value);
}

static void open_settings(steins_app_t *app)
{
    nav_push(app, app->page, app->sel, 0);
    app->list_story_font = false;
    set_list_hint(app, "OK 切换 · 长按返回");
    app->sel = 0;
    ui_text(app, UI_SETTINGS_TITLE, app->list_title, sizeof(app->list_title));
    fill_settings(app);
    set_page(app, STEINS_PAGE_SETTINGS);
    render_list(app);
}

// ---------------------------------------------------------------- 章节/鉴赏

static void fill_chapters(steins_app_t *app)
{
    list_clear(app);
    const uint32_t count = steins_pack_count(&app->pack, STEINS_SEC_CHAPTER);
    app->list_count = (int)count;
    const int first = window_first(app);
    for (int i = 0; i < STEINS_UI_LIST_ROWS; ++i) {
        const int slot = first + i;
        if (slot < 0 || (uint32_t)slot >= count) continue;
        steins_chapter_t chapter;
        if (!steins_pack_chapter(&app->pack, (uint16_t)slot, &chapter)) continue;
        if (steins_pack_str_utf8(&app->pack, chapter.name, app->list_main[i],
                                 STEINS_LIST_TEXT) < 0) {
            continue;
        }
        if (!steins_unlock_has_chapter(&app->unlock, (uint8_t)slot)) {
            ui_text(app, UI_CHAPTER_LOCKED, app->list_sub[i], sizeof(app->list_sub[i]));
        }
    }
}

static void open_chapters(steins_app_t *app)
{
    nav_push(app, app->page, app->sel, 0);
    app->list_story_font = false;
    set_list_hint(app, "上/下选择 · OK 进入 · 长按返回");
    app->sel = 0;
    ui_text(app, UI_CHAPTER_TITLE, app->list_title, sizeof(app->list_title));
    set_page(app, STEINS_PAGE_CHAPTERS);
    fill_chapters(app);
    render_list(app);
}

static void fill_gallery(steins_app_t *app)
{
    list_clear(app);
    const uint32_t count = steins_pack_count(&app->pack, STEINS_SEC_GAL);
    app->list_count = (int)count;
    const int first = window_first(app);
    for (int i = 0; i < STEINS_UI_LIST_ROWS; ++i) {
        const int slot = first + i;
        if (slot < 0 || (uint32_t)slot >= count) continue;
        steins_gal_t gal;
        if (!steins_pack_gal(&app->pack, (uint16_t)slot, &gal)) continue;
        if (steins_pack_str_utf8(&app->pack, gal.label, app->list_main[i], 24) < 0) continue;
        const bool unlocked = steins_unlock_has_gallery(&app->unlock, (uint8_t)slot);
        snprintf(app->list_sub[i], sizeof(app->list_sub[i]), "%u%s", (unsigned)gal.count,
                 unlocked ? "" : " -");
    }
}

static void open_gallery(steins_app_t *app)
{
    nav_push(app, app->page, app->sel, 0);
    app->list_story_font = false;
    set_list_hint(app, "上/下选择 · OK 打开 · 长按返回");
    app->sel = 0;
    ui_text(app, UI_GALLERY_TITLE, app->list_title, sizeof(app->list_title));
    set_page(app, STEINS_PAGE_GALLERY);
    fill_gallery(app);
    render_list(app);
}

static void render_cg(steins_app_t *app)
{
    const uint32_t count = steins_pack_count(&app->pack, STEINS_SEC_GAL);
    if (app->gallery_sel < 0 || (uint32_t)app->gallery_sel >= count) return;
    steins_gal_t gal;
    if (!steins_pack_gal(&app->pack, (uint16_t)app->gallery_sel, &gal)) return;
    if (gal.count == 0) return;
    if (app->gallery_variant >= (int)gal.count) app->gallery_variant = 0;
    uint16_t asset = STEINS_NONE;
    if (!steins_pack_gentry(&app->pack, (uint16_t)(gal.first + app->gallery_variant),
                            &asset)) {
        return;
    }
    char label[32];
    if (steins_pack_str_utf8(&app->pack, gal.label, label, sizeof(label)) < 0) label[0] = '\0';
    char caption[64];
    if (gal.count > 1) {
        snprintf(caption, sizeof(caption), "%s %d/%d", label, app->gallery_variant + 1,
                 (int)gal.count);
    } else {
        copy_text(caption, sizeof(caption), label);
    }
    char hint[32];
    ui_text(app, UI_GALLERY_VARIANT, hint, sizeof(hint));
    set_page(app, STEINS_PAGE_CG);
    app->cg_asset = asset;
    app->cg_valid = steins_image_show_asset(&app->ui.image, &app->pack, asset, true,
                                            &app->scene_error);
    if (!app->cg_valid) {
        char fail[40];
        ui_text(app, UI_IMAGE_FAIL, fail, sizeof(fail));
        steins_ui_cg(&app->ui, caption, fail);
    } else {
        steins_ui_cg(&app->ui, caption, hint);
    }
}

static void open_about(steins_app_t *app)
{
    nav_push(app, app->page, app->sel, 0);
    app->list_story_font = false;
    set_list_hint(app, "OK 返回 · 长按返回");
    app->sel = 0;
    list_clear(app);
    static char about_text[STEINS_ABOUT_TEXT];
    char about_title[STEINS_LIST_TEXT] = "";
    ui_text(app, UI_ABOUT_TITLE, about_title, sizeof(about_title));
    const uint16_t index = steins_pack_ui(&app->pack, UI_ABOUT_BODY);
    if (index != STEINS_NONE) {
        (void)steins_pack_str_utf8(&app->pack, index, about_text, sizeof(about_text));
    }
    app->list_count = 1;
    set_page(app, STEINS_PAGE_ABOUT);
    steins_ui_about(&app->ui, about_title, about_text);
}

// ---------------------------------------------------------------- 确认弹窗

static void render_confirm(steins_app_t *app)
{
    char yes[16];
    char no[16];
    ui_text(app, UI_YES, yes, sizeof(yes));
    ui_text(app, UI_NO, no, sizeof(no));
    char hint[48];
    snprintf(hint, sizeof(hint), "%s%s / %s%s", app->confirm_sel == 0 ? "> " : "", yes,
             app->confirm_sel == 1 ? "> " : "", no);
    steins_ui_confirm(&app->ui, app->confirm_text, hint);
    set_page(app, STEINS_PAGE_CONFIRM);
}

static void open_confirm(steins_app_t *app, uint8_t action, int slot)
{
    nav_push(app, app->page, app->sel, 2);
    app->pending_action = action;
    app->pending_slot = slot;
    // 默认落在"取消"上:误按一次确定不会覆盖旧档,要覆盖得主动移到"确定"。
    app->confirm_sel = 1;
    ui_text(app, UI_SAVE_OVERWRITE, app->confirm_text, sizeof(app->confirm_text));
    render_confirm(app);
}

// ---------------------------------------------------------------- 位置应用

static bool apply_position(steins_app_t *app, const steins_pos_t *pos)
{
    if (mobile_clip_valid(app, pos->node)) {
        app->pos = *pos;
        app->chk.node = pos->node;
        app->chk.cursor = pos->cursor;
        app->chk.mode = pos->mode;
        app->reading = true;
        app->nav_depth = 0;
        return render_phone_clip(app, pos->node, pos->cursor);
    }
    steins_node_t node;
    if (!node_of(app, pos->node, &node)) return false;
    if (pos->cursor > node.count) return false;
    app->pos = *pos;
    app->chk.node = pos->node;
    app->chk.cursor = pos->cursor;
    app->chk.mode = pos->mode;
    app->reading = true;
    app->drawn_view = STEINS_NONE;
    app->title[0] = '\0';
    app->auto_mode = false;
    app->fast_forward = false;
    app->choice_sel = 0;
    app->history_count = 0;
    app->nav_depth = 0;
    return render_current(app, false);
}

static void start_story(steins_app_t *app)
{
    const int initial = steins_pack_meta_int(&app->pack, "initial", 0);
    steins_pos_t pos;
    pos.node = (uint16_t)initial;
    pos.cursor = 0;
    pos.mode = STEINS_MODE_STORY;
    pos.flags = 0;
    steins_checkpoint_init(&app->chk);
    app->chk.node = pos.node;
    app->chk.cursor = pos.cursor;
    app->chk.mode = pos.mode;
    app->chk.last_node = 0xFFFFu;
    app->chk.last_at = 0xFFFFFFFFu;
    if (app->mobile_ready) steins_mobile_seek(&app->chk, pos.node, pos.cursor);
    app->reading = true;
    app->checkpoint_dirty = false;
    app->drawn_view = STEINS_NONE;
    app->title[0] = '\0';
    app->nav_depth = 0;
    app->choice_sel = 0;
    app->auto_mode = false;
    app->fast_forward = false;
    if (!apply_position(app, &pos)) {
        ESP_LOGE(TAG, "初始节点无效");
        copy_text(app->text, sizeof(app->text), "pack broken");
        set_page(app, STEINS_PAGE_GAME);
        render_game(app);
    }
}

// ---------------------------------------------------------------- 按键:各页

static void return_back(steins_app_t *app)
{
    steins_page_t back = STEINS_PAGE_GAME;
    int sel = 0;
    int kind = 0;
    if (!nav_pop(app, &back, &sel, &kind)) {
        go_title(app);
        return;
    }
    app->sel = sel;
    switch (back) {
    case STEINS_PAGE_GAME:
        set_page(app, back);
        render_game(app);
        break;
    case STEINS_PAGE_CHOICE:
        render_choice(app);
        break;
    case STEINS_PAGE_TITLE:
        set_page(app, back);
        render_title(app);
        break;
    case STEINS_PAGE_MENU:
        set_page(app, back);
        fill_menu(app);
        render_list(app);
        break;
    case STEINS_PAGE_SAVES:
        set_page(app, back);
        fill_slots(app);
        render_list(app);
        break;
    case STEINS_PAGE_CHAPTERS:
        set_page(app, back);
        fill_chapters(app);
        render_list(app);
        break;
    case STEINS_PAGE_GALLERY:
        set_page(app, back);
        fill_gallery(app);
        render_list(app);
        break;
    case STEINS_PAGE_SETTINGS:
        set_page(app, back);
        fill_settings(app);
        render_list(app);
        break;
    case STEINS_PAGE_PHONE_HOME:
        set_page(app, back);
        fill_phone_home(app);
        set_list_hint(app, "上/下选择 · OK 打开 · 长按返回");   // 不要残留子页的提示
        render_list(app);
        break;
    case STEINS_PAGE_PHONE_INBOX:
        set_page(app, back);
        fill_phone_list(app, false);
        render_list(app);
        break;
    case STEINS_PAGE_PHONE_SENT:
        set_page(app, back);
        fill_phone_list(app, true);
        render_list(app);
        break;
    case STEINS_PAGE_PHONE_REPLY:
        set_page(app, back);
        fill_phone_replies(app);
        render_list(app);
        break;
    case STEINS_PAGE_PHONE_CALLS:
        set_page(app, back);
        fill_phone_calls(app);
        render_list(app);
        break;
    case STEINS_PAGE_PHONE_CONTACTS:
        set_page(app, back);
        render_list(app);
        break;
    case STEINS_PAGE_PHONE_CLIP:
        set_page(app, back);
        render_list(app);
        break;
    default:
        set_page(app, back);
        render_list(app);
        break;
    }
}

static void title_key(steins_app_t *app, const steins_key_t *key)
{
    if (key->ev == KEY_CLICK) {
        if (key->btn == BTN_UP || key->btn == BTN_DOWN) {
            // 上下键只挪高亮：render_title 会重新读 NVS 自动存档 + 重拼整页，
            // 每次按键都走一遍会明显卡顿。
            if (key->btn == BTN_UP && app->sel > 0) --app->sel;
            else if (key->btn == BTN_DOWN && app->sel + 1 < app->title_count) ++app->sel;
            else return;
            steins_ui_title_select(&app->ui, app->sel);
            return;
        }
        if (key->btn == BTN_OK) {
            // 菜单行是按"有没有自动存档"动态拼的,这里按当前行的 key 分发,
            // 不再假设固定下标(否则没有存档时"读取"会错位)。
            switch (app->title_action[app->sel]) {
            case TITLE_ACT_NEW:
                start_story(app);
                return;
            case TITLE_ACT_CONTINUE: {
                if (checkpoint_read(app, -1, &s_chk_scratch) == STEINS_SLOT_VALID &&
                    checkpoint_apply(app, &s_chk_scratch, false)) return;
                open_saves(app, false);
                return;
            }
            case TITLE_ACT_LOAD: open_saves(app, false); return;
            case TITLE_ACT_GALLERY: open_gallery(app); return;
            case TITLE_ACT_CHAPTER: open_chapters(app); return;
            case TITLE_ACT_CONFIG: open_settings(app); return;
            default: open_about(app); return;
            }
        }
        render_title(app);
    }
}

static void game_key(steins_app_t *app, const steins_key_t *key)
{
    if (key->ev == KEY_CLICK) {
        if (key->btn == BTN_DOWN) {
            open_history(app);
            return;
        }
        advance(app);
    } else if (key->ev == KEY_LONG) {
        if (key->btn == BTN_UP) {
            app->fast_forward = true;
            app->shown = app->page_data.chars;
            app->ff_ms = 0;
            render_game(app);
        } else if (key->btn == BTN_DOWN) {
            app->auto_mode = !app->auto_mode;
            app->auto_ms = 0;
            render_game(app);
        } else {
            open_menu(app);
        }
    } else if (key->ev == KEY_RELEASE && key->btn == BTN_UP) {
        app->fast_forward = false;
        render_game(app);
    }
}

static void phone_mail_key(steins_app_t *app, const steins_key_t *key)
{
    if (key->ev == KEY_LONG) {
        if (app->phone_presentation_pending) {
            steins_mobile_finish_presentation(&app->mobile, &app->chk);
            app->phone_presentation_pending = false;
            app->checkpoint_dirty = true;
        }
        return_back(app);
        while (app->page == STEINS_PAGE_PHONE_HOME || app->page == STEINS_PAGE_MENU) {
            return_back(app);
        }
        return;
    }
    if (key->ev == KEY_CLICK && key->btn == BTN_UP) {
        if (app->phone_mail_page > 0) --app->phone_mail_page;
        render_phone_mail(app);
        return;
    }
    if (key->ev == KEY_CLICK && key->btn == BTN_DOWN) {
        if (app->phone_mail_page + 1 < app->phone_mail_pages) ++app->phone_mail_page;
        render_phone_mail(app);
        return;
    }
    if (key->ev == KEY_CLICK && key->btn == BTN_OK) {
        if (app->phone_presentation_pending) {
            steins_mobile_finish_presentation(&app->mobile, &app->chk);
            app->phone_presentation_pending = false;
            app->checkpoint_dirty = true;
            return_back(app);
            while (app->page == STEINS_PAGE_PHONE_HOME || app->page == STEINS_PAGE_MENU) {
                return_back(app);
            }
            return;
        }
        if (app->phone_mail_page + 1 < app->phone_mail_pages) {
            ++app->phone_mail_page;
            render_phone_mail(app);
        } else if (!app->phone_mail_sent && app->phone_reply_count > 0) {
            open_phone_replies(app);
        } else {
            return_back(app);
        }
    }
}

static void mail_key(steins_app_t *app, const steins_key_t *key)
{
    if (key->ev == KEY_CLICK && key->btn == BTN_UP) {
        if (app->mail_page > 0) --app->mail_page;
        steins_page_build(app->choice_heading, app->font_size, STEINS_TEXT_W,
                          STEINS_MAIL_LINES, app->mail_page, &app->mail_data);
        app->mail_page = app->mail_data.page;
        render_mail(app);
        return;
    }
    if (key->ev == KEY_CLICK && key->btn == BTN_DOWN) {
        if (app->mail_page + 1 < app->mail_pages) ++app->mail_page;
        steins_page_build(app->choice_heading, app->font_size, STEINS_TEXT_W,
                          STEINS_MAIL_LINES, app->mail_page, &app->mail_data);
        app->mail_page = app->mail_data.page;
        render_mail(app);
        return;
    }
    if (key->ev == KEY_CLICK || key->ev == KEY_LONG) {
        // 确定/长按都只是"读完正文,回到选项",绝不代替玩家做选择。
        app->mail_pending_choice = false;
        render_choice(app);
    }
}

static void choice_key(steins_app_t *app, const steins_key_t *key)
{
    if (key->ev == KEY_LONG) {
        if (key->btn == BTN_OK) open_menu(app);
        return;
    }
    if (key->ev != KEY_CLICK) return;
    if (key->btn == BTN_UP) {
        if (app->choice_sel > 0) --app->choice_sel;
        render_choice(app);
    } else if (key->btn == BTN_DOWN) {
        if (app->choice_sel + 1 < app->choice_count) ++app->choice_sel;
        render_choice(app);
    } else {
        // 显式确认才生效。手机事件机可用时由 choose 统一处理 incoming/sent/open
        // 并返回 land；只有旧 v3 包/未初始化时才走原有图内落点。
        steins_choice_t choice;
        steins_citem_t item;
        if (!steins_pack_choice(&app->pack, app->choice_record, &choice)) return;
        if (!steins_pack_citem(&app->pack, (uint16_t)(choice.first_item + app->choice_sel),
                               &item)) {
            return;
        }
        steins_pos_t pos;
        if (app->mobile_ready) {
            steins_mobile_pos_t land;
            steins_mobile_frame_t boundary;
            const bool keyphone = steins_mobile_keyphone_boundary(
                &app->mobile, &app->chk, &boundary);
            const bool chosen = keyphone
                ? steins_mobile_answer_key_phone(&app->mobile, &app->chk,
                    (uint8_t)app->choice_sel, &land)
                : steins_mobile_choose(&app->mobile, &app->chk, app->pos.node,
                    (uint8_t)app->choice_sel, 0, &land);
            if (!chosen) {
                set_notice_text(app, "手机选择状态无效，未推进");
                return;
            }
            pos.node = land.node;
            pos.cursor = land.cursor;
        } else {
            pos.node = item.to;
            pos.cursor = 0;
        }
        pos.mode = app->pos.mode;
        pos.flags = 0;
        app->choice_sel = 0;
        if (!apply_position(app, &pos)) ESP_LOGE(TAG, "选项落点无效");
        app->progress_dirty = true;
    }
}

static void list_move(steins_app_t *app, int delta)
{
    const int next = app->sel + delta;
    if (next < 0 || next >= app->list_count) return;
    app->sel = next;
    switch (app->page) {
    case STEINS_PAGE_SAVES: fill_slots(app); break;
    case STEINS_PAGE_CHAPTERS: fill_chapters(app); break;
    case STEINS_PAGE_GALLERY: fill_gallery(app); break;
    case STEINS_PAGE_MENU: fill_menu(app); break;
    case STEINS_PAGE_HISTORY: fill_history(app); break;
    case STEINS_PAGE_PHONE_INBOX: fill_phone_list(app, false); break;
    case STEINS_PAGE_PHONE_SENT: fill_phone_list(app, true); break;
    case STEINS_PAGE_PHONE_REPLY: fill_phone_replies(app); break;
    default: break;
    }
    render_list(app);
}

static void list_key(steins_app_t *app, const steins_key_t *key)
{
    if (key->ev == KEY_LONG) {
        if (key->btn == BTN_OK) return_back(app);
        return;
    }
    if (key->ev != KEY_CLICK) return;
    if (key->btn == BTN_UP) {
        list_move(app, -1);
        return;
    }
    if (key->btn == BTN_DOWN) {
        list_move(app, +1);
        return;
    }
    // 短 OK = 执行
    switch (app->page) {
    case STEINS_PAGE_PHONE_HOME:
        if (app->sel == 0) open_phone_list(app, false);
        else if (app->sel == 1) open_phone_list(app, true);
        else if (app->sel == 2) open_phone_contacts(app);
        else if (!open_phone_presentation(app)) open_phone_calls(app);
        break;
    case STEINS_PAGE_PHONE_INBOX:
    case STEINS_PAGE_PHONE_SENT: {
        const uint16_t id = phone_id_at(app, app->page == STEINS_PAGE_PHONE_SENT, app->sel);
        if (id != 0xFFFFu) {
            open_phone_detail(app, id, app->page == STEINS_PAGE_PHONE_SENT);
        } else {
            set_notice_text(app, "没有可读的内容");
            render_list(app);
        }
        break;
    }
    case STEINS_PAGE_PHONE_REPLY:
        if (app->sel < app->phone_reply_count) send_phone_reply(app, app->phone_reply_ids[app->sel]);
        break;
    case STEINS_PAGE_PHONE_CALLS: {
        uint8_t indices[STEINS_CHOICE_MAX];
        const uint8_t key_count = keyphone_calls(app, indices, STEINS_CHOICE_MAX);
        if (key_count > 0) {
            steins_mobile_pos_t land;
            if (app->sel < 0 || app->sel >= key_count ||
                !steins_mobile_answer_key_phone(&app->mobile, &app->chk,
                    indices[app->sel], &land)) {
                set_notice_text(app, "手机选择状态无效，未推进");
                render_list(app);
                break;
            }
            const steins_pos_t pos = { land.node, land.cursor, app->pos.mode, 0 };
            if (!apply_position(app, &pos)) ESP_LOGE(TAG, "选项落点无效");
            app->progress_dirty = true;
            break;
        }
        uint16_t site = 0;
        uint8_t count = 0;
        if (!app->mobile_ready ||
            !steins_mobile_call_current(&app->mobile, &app->chk, &site, &count) ||
            app->sel >= count) {
            set_notice_text(app, "本段没有可用电话");
            render_list(app);
            break;
        }
        steins_mobile_pos_t land;
        if (!steins_mobile_take_call(&app->mobile, &app->chk, (uint8_t)app->sel, &land)) {
            set_notice_text(app, "接听状态无效，未推进");
            render_list(app);
            break;
        }
        steins_pos_t pos = { land.node, land.cursor, app->pos.mode, 0 };
        if (!apply_position(app, &pos)) {
            set_notice_text(app, "通话片段落点无效");
            render_list(app);
        }
        break;
    }
    case STEINS_PAGE_PHONE_CONTACTS:
        set_notice_text(app, "联系人随剧情开放");
        render_list(app);
        break;
    case STEINS_PAGE_MENU:
        switch (app->sel) {
        case 0: return_back(app); break;
        case 1: open_saves(app, true); break;
        case 2: open_saves(app, false); break;
        case 3: open_phone_home(app); break;
        case 4: open_history(app); break;
        case 5:
            app->auto_mode = !app->auto_mode;
            app->auto_ms = 0;
            fill_menu(app);
            render_list(app);
            break;
        case 6: {
            steins_node_t node;
            if (!node_of(app, app->pos.node, &node)) return;
            uint32_t target = node.count;
            for (uint8_t i = 0; i < node.break_count; ++i) {
                uint32_t cursor = 0;
                if (!steins_pack_break(&app->pack, (uint16_t)(node.break_first + i), &cursor)) {
                    continue;
                }
                if (cursor > app->pos.cursor) {
                    target = cursor;
                    break;
                }
            }
            steins_pos_t pos = app->pos;
            pos.cursor = target;
            app->nav_depth = 0;
            if (!apply_position(app, &pos)) ESP_LOGW(TAG, "跳章落点无效");
            break;
        }
        case 7: open_gallery(app); break;
        case 8: open_settings(app); break;
        default: go_title(app); break;
        }
        break;
    case STEINS_PAGE_SAVES:
        if (saving_mode(app)) {
            open_confirm(app, ACT_OVERWRITE_SAVE, app->sel);
        } else {
            const steins_slot_state_t state = checkpoint_read(app, app->sel, NULL);
            app->slot_state[app->sel] = state;
            if (state == STEINS_SLOT_VALID) {
                // 枚举不能当布尔:IO_ERROR(3)/CORRUPT(2) 都是 truthy,
                // 只有 VALID 才允许 apply;否则坏档/IO 错误会被当成成功读到。
                const steins_slot_state_t st = checkpoint_read(app, app->sel, &s_chk_scratch);
                if (st != STEINS_SLOT_VALID ||
                    !checkpoint_apply(app, &s_chk_scratch, false)) {
                    set_notice(app, UI_LOAD_FAIL);
                    render_list(app);
                }
            } else if (state == STEINS_SLOT_EMPTY) {
                set_notice_text(app, "这个栏位还没有存档");
                copy_text(app->list_hint, sizeof(app->list_hint), app->notice);
                render_list(app);
            } else if (state == STEINS_SLOT_CORRUPT) {
                set_notice_text(app, "存档损坏，无法读取");
                copy_text(app->list_hint, sizeof(app->list_hint), app->notice);
                render_list(app);
            } else {
                set_notice_text(app, "存档读取失败，请重试");
                copy_text(app->list_hint, sizeof(app->list_hint), app->notice);
                render_list(app);
            }
        }
        break;
    case STEINS_PAGE_SETTINGS:
        if (app->sel == 0) set_notice_text(app, "字号 16px 未开放");
        else if (app->sel == 1) app->text_speed = (uint8_t)((app->text_speed + 1) % 3);
        else if (app->sel == 2) app->auto_speed = (uint8_t)((app->auto_speed + 1) % 3);
        else if (app->sel == 3) {
            app->box_opacity = (uint8_t)((app->box_opacity + 1) % 3);
            steins_ui_set_box_opacity(&app->ui, app->box_opacity);
        }
        settings_save(app);
        fill_settings(app);
        render_list(app);
        break;
    case STEINS_PAGE_CHAPTERS: {
        steins_chapter_t chapter;
        // 未到达的章节不能从章节选择进入(序章除外:它就是新游戏的起点)。
        if (app->sel > 0 && !steins_unlock_has_chapter(&app->unlock, (uint8_t)app->sel)) {
            set_notice(app, UI_CHAPTER_LOCKED);
            copy_text(app->list_hint, sizeof(app->list_hint), app->notice);
            render_list(app);
            break;
        }
        if (!steins_pack_chapter(&app->pack, (uint16_t)app->sel, &chapter)) break;
        steins_pos_t pos;
        pos.node = chapter.node;
        pos.cursor = chapter.cursor;
        pos.mode = STEINS_MODE_CHAPTER;
        pos.flags = 0;
        steins_checkpoint_init(&app->chk);
        app->chk.node = pos.node;
        app->chk.cursor = pos.cursor;
        app->chk.mode = pos.mode;
        if (app->mobile_ready) steins_mobile_seek(&app->chk, pos.node, pos.cursor);
        else {
            app->chk.last_node = pos.node;
            app->chk.last_at = pos.cursor == 0u ? 0xFFFFFFFFu : pos.cursor - 1u;
        }
        app->chk.opening_done = 1u;
        app->reading = true;
        app->nav_depth = 0;
        if (apply_position(app, &pos)) {
            if (steins_unlock_set_chapter(&app->unlock, (uint8_t)app->sel)) {
                app->unlock_dirty = true;
            }
        }
        break;
    }
    case STEINS_PAGE_GALLERY:
        if (!steins_unlock_has_gallery(&app->unlock, (uint8_t)app->sel)) {
            char hint[32];
            ui_text(app, UI_GALLERY_EMPTY, hint, sizeof(hint));
            copy_text(app->list_hint, sizeof(app->list_hint), hint);
            render_list(app);
            break;
        }
        app->gallery_sel = app->sel;
        app->gallery_variant = 0;
        render_cg(app);
        break;
    default:
        break;
    }
}

static void confirm_key(steins_app_t *app, const steins_key_t *key)
{
    if (key->ev == KEY_LONG) {
        return_back(app);
        return;
    }
    if (key->ev != KEY_CLICK) return;
    if (key->btn == BTN_UP) {
        app->confirm_sel = 0;
        render_confirm(app);
        return;
    }
    if (key->btn == BTN_DOWN) {
        app->confirm_sel = 1;
        render_confirm(app);
        return;
    }
    if (app->confirm_sel != 0) {
        return_back(app);
        return;
    }
    if (app->pending_action == ACT_OVERWRITE_SAVE) {
        const int slot = app->pending_slot;
        const bool ok = save_write(app, slot);
        set_notice(app, ok ? UI_SAVE_DONE : UI_SAVE_FAIL);
        app->pending_action = ACT_NONE;
        if (slot >= 0 && slot < STEINS_SLOT_RAW) format_slot(app, slot);
        return_back(app);
        // 保存结果提示不能被列表刷新覆盖:回到存档页后再写一次提示行。
        set_notice(app, ok ? UI_SAVE_DONE : UI_SAVE_FAIL);
        if (app->page == STEINS_PAGE_SAVES) {
            copy_text(app->list_hint, sizeof(app->list_hint), app->notice);
            render_list(app);
        }
    }
}

static void cg_key(steins_app_t *app, const steins_key_t *key)
{
    if (key->ev == KEY_LONG) {
        return_back(app);
        return;
    }
    if (key->ev != KEY_CLICK) return;
    steins_gal_t gal;
    if (!steins_pack_gal(&app->pack, (uint16_t)app->gallery_sel, &gal) || gal.count == 0) {
        return_back(app);
        return;
    }
    if (key->btn == BTN_UP) {
        app->gallery_variant = (app->gallery_variant + (int)gal.count - 1) % (int)gal.count;
        render_cg(app);
    } else if (key->btn == BTN_DOWN) {
        app->gallery_variant = (app->gallery_variant + 1) % (int)gal.count;
        render_cg(app);
    } else {
        return_back(app);
    }
}

void steins_app_key(steins_app_t *app, const steins_key_t *key)
{
    if (!app || !key) return;
    app->idle_ms = 0;
    // 章节标题卡期间,任意短按/长按只用来收起卡片(源 CHAPTER_CARD_HOLD_MS 语义),
    // 不能让这次按键落到底下的鉴赏页去触发“返回标题”。
    if (app->chapter_card_active) {
        if (key->ev == KEY_CLICK || key->ev == KEY_LONG) dismiss_chapter_card(app);
        return;
    }
    // 提示只在真正的按键动作上清除:PRESS/RELEASE 属于同一次按键,不能把刚写下的
    // “已保存”提示清掉(那样列表刷新后就再也看不到存盘结果了)。
    if ((key->ev == KEY_CLICK || key->ev == KEY_LONG) && app->notice_ms > 0) {
        app->notice_ms = 0;
        app->notice[0] = '\0';
    }
    switch (app->page) {
    case STEINS_PAGE_TITLE: title_key(app, key); break;
    case STEINS_PAGE_GAME: game_key(app, key); break;
    case STEINS_PAGE_CHOICE: choice_key(app, key); break;
    case STEINS_PAGE_MAIL:
        if (app->phone_mail_mode) phone_mail_key(app, key);
        else mail_key(app, key);
        break;
    case STEINS_PAGE_PHONE_HOME:
    case STEINS_PAGE_PHONE_INBOX:
    case STEINS_PAGE_PHONE_SENT:
    case STEINS_PAGE_PHONE_CALLS:
    case STEINS_PAGE_PHONE_CONTACTS:
    case STEINS_PAGE_PHONE_REPLY:
    case STEINS_PAGE_PHONE_CLIP: 
        if (app->page == STEINS_PAGE_PHONE_CLIP) phone_clip_key(app, key);
        else list_key(app, key);
        break;
    case STEINS_PAGE_CG: cg_key(app, key); break;
    case STEINS_PAGE_CONFIRM: confirm_key(app, key); break;
    case STEINS_PAGE_ABOUT:
        // 只读页:短/长 OK 都返回;上下键滚正文(长文案 200px 装不下)。
        if (key->btn == BTN_OK && (key->ev == KEY_CLICK || key->ev == KEY_LONG)) {
            return_back(app);
            break;
        }
        if (key->ev == KEY_CLICK && key->btn == BTN_UP) {
            (void)steins_ui_about_scroll(&app->ui, -1);
            break;
        }
        if (key->ev == KEY_CLICK && key->btn == BTN_DOWN) {
            (void)steins_ui_about_scroll(&app->ui, 1);
            break;
        }
        break;
    case STEINS_PAGE_HISTORY:
        // 只读页:短 OK 就是返回(修复前只有长 OK 能退,屏幕又没有提示)。
        if (key->ev == KEY_CLICK && key->btn == BTN_OK) return_back(app);
        else list_key(app, key);
        break;
    case STEINS_PAGE_ENDING:
        if ((key->ev == KEY_CLICK || key->ev == KEY_LONG) && key->btn == BTN_OK) go_title(app);
        break;
    default: list_key(app, key); break;
    }
}

// ---------------------------------------------------------------- 时钟

static uint32_t typewriter_interval(uint8_t speed)
{
    switch (speed) {
    case 0: return 42u;
    case 2: return 10u;
    default: return 22u;
    }
}

static uint32_t auto_delay(const steins_app_t *app)
{
    const int length = app->page_data.chars;
    uint32_t base;
    switch (app->auto_speed) {
    case 0: base = 2600u; break;
    case 2: base = 800u; break;
    default: base = 1500u; break;
    }
    const int extra = length > 60 ? length - 60 : 0;
    return base + (uint32_t)extra * STEINS_AUTO_PER_CHAR_MS;
}

void steins_app_tick(steins_app_t *app, uint32_t elapsed_ms)
{
    if (!app) return;
    if (app->auto_mode || app->fast_forward) app->idle_ms = 0;
    else app->idle_ms += elapsed_ms;

    if (app->notice_ms > 0) {
        if (elapsed_ms >= app->notice_ms) {
            app->notice_ms = 0;
            app->notice[0] = '\0';
        } else {
            app->notice_ms -= elapsed_ms;
        }
    }

    app->battery_ms += elapsed_ms;
    if (app->battery_ms >= STEINS_BATTERY_MS) {
        app->battery_ms = 0;
        const int soc = bsp_battery_soc();
        if (soc != app->battery_percent) {
            app->battery_percent = soc;
            if (app->page == STEINS_PAGE_GAME) render_game(app);
        }
    }

    if (app->checkpoint_dirty && app->reading) {
        app->checkpoint_ms += elapsed_ms;
        if (app->checkpoint_ms >= STEINS_AUTOSAVE_MS) {
            app->checkpoint_ms = 0;
            (void)checkpoint_write(app, -1);
        }
    }

    if (app->unlock_dirty || app->progress_dirty) {
        app->save_ms += elapsed_ms;
        if (app->save_ms >= STEINS_AUTOSAVE_MS) {
            app->save_ms = 0;
            if (app->unlock_dirty) unlock_save(app);
            if (app->progress_dirty) {
                if (save_write(app, -1)) app->progress_dirty = false;
            }
        }
    }

    if (app->chapter_card_active) {
        app->chapter_card_ms += elapsed_ms;
        if (app->chapter_card_ms >= 2400u) dismiss_chapter_card(app);
        return;
    }

    if (app->page != STEINS_PAGE_GAME) return;

    if (!text_fully_shown(app)) {
        app->type_ms += elapsed_ms;
        const uint32_t step = app->fast_forward ? 1u : typewriter_interval(app->text_speed);
        if (app->type_ms >= step) {
            const int reveal = (int)(app->type_ms / step);
            app->type_ms = 0;
            app->shown += reveal > 0 ? reveal : 1;
            if (app->shown > app->page_data.chars) app->shown = app->page_data.chars;
            render_game(app);
        }
        return;
    }
    if (app->fast_forward) {
        app->ff_ms += elapsed_ms;
        if (app->ff_ms >= STEINS_FF_MS) {
            app->ff_ms = 0;
            advance(app);
        }
        return;
    }
    if (app->auto_mode) {
        app->auto_ms += elapsed_ms;
        if (app->auto_ms >= auto_delay(app)) {
            app->auto_ms = 0;
            advance(app);
        }
    }
}

uint32_t steins_app_idle_ms(const steins_app_t *app)
{
    // 自动阅读/快进期间不得调暗/熄屏/深睡：有效空闲恒为 0。
    if (app && (app->auto_mode || app->fast_forward)) return 0u;
    return app ? app->idle_ms : 0u;
}

void steins_app_flush(steins_app_t *app)
{
    if (!app) return;
    if (app->unlock_dirty) unlock_save(app);
    if (app->reading) {
        if (save_write(app, -1)) app->progress_dirty = false;
    }
}

void steins_app_show_sleeping(steins_app_t *app)
{
    (void)app;
}

// ---------------------------------------------------------------- 初始化

extern const lv_font_t steins_story_16;
extern const lv_font_t steins_ui_16;
extern const lv_font_t steins_ui_18;

bool steins_app_init(steins_app_t *app)
{
    if (!app) return false;
    memset(app, 0, sizeof(*app));
    app->battery_percent = STEINS_BATTERY_NONE;
    app->battery_ms = STEINS_BATTERY_MS;
    app->storage_ok = true;
    app->drawn_view = STEINS_NONE;
    app->pending_slot = -1;
    steins_checkpoint_init(&app->chk);
    app->reading = false;
    app->font_size = 16;
    app->text_speed = 1;
    app->auto_speed = 1;
    app->box_opacity = 1;

    if (!steins_pack_open_embedded(&app->pack)) {
        ESP_LOGE(TAG, "资源包校验失败(段落越界或 magic 不符)");
        return false;
    }
    if (steins_mobile_pack_content(&app->pack, &app->mobile_ctx, &app->mobile_content,
                                   &app->mobile_user)) {
        steins_mobile_init(&app->mobile);
        steins_mobile_set_dynamic_boundaries(&app->mobile, false);
        app->mobile_ready = true;
    }

    lv_obj_t *screen = lv_screen_active();
    if (!steins_ui_create(&app->ui, screen, app->canvas, &steins_story_16, &steins_ui_16,
                          &steins_ui_18)) {
        ESP_LOGE(TAG, "界面初始化失败");
        return false;
    }
    settings_load(app);
    unlock_load(app);
    steins_ui_set_box_opacity(&app->ui, app->box_opacity);
    go_title(app);
    ESP_LOGI(TAG, "资源包就绪: %u 字节, %u 节点, %u 帧, %u 图",
             (unsigned)steins_pack_total(&app->pack),
             (unsigned)steins_pack_count(&app->pack, STEINS_SEC_NODE),
             (unsigned)steins_pack_count(&app->pack, STEINS_SEC_FRAME),
             (unsigned)steins_pack_count(&app->pack, STEINS_SEC_ASSET));
    return true;
}
