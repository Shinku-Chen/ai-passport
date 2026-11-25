// main/nine/nine_app.c — 9-nine- 春色春恋春熙风 reader.
//
// Page flow, key semantics and persistence live here; the exact graph semantics
// (conditions, effects, ordered nexts, choice fallback, permanent key merge and
// the 500-hop limit) live in nine_vm.c. No pointer into the decompressed text
// cache survives a call: text is copied before any other pack access.
#include "nine_app.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "gal_text.h"
#include "nine_image.h"
#include "nine_media.h"
#include "esp_log.h"
#include "lvgl.h"
#ifndef NINE_APP_HOST
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif
#include <stdio.h>
#include <string.h>

static const char *TAG = "nine";
static void toggle_auto(nine_app_t *app, bool on);
LV_FONT_DECLARE(nine_cjk_16);
LV_FONT_DECLARE(nine_cjk_20);

// Embedded by target_add_binary_data() in main/CMakeLists.txt. Host tests
// provide the same byte ranges from files instead of linker symbols.
#ifdef NINE_APP_HOST
const uint8_t *nine_host_script(size_t *size);
const uint8_t *nine_host_images(size_t *size);
static const uint8_t *script_data(size_t *size) { return nine_host_script(size); }
static const uint8_t *images_data(size_t *size) { return nine_host_images(size); }
#else
extern const uint8_t nine_script_bin_start[] __asm__("_binary_nine_script_bin_start");
extern const uint8_t nine_script_bin_end[] __asm__("_binary_nine_script_bin_end");
extern const uint8_t nine_images_bin_start[] __asm__("_binary_nine_images_bin_start");
extern const uint8_t nine_images_bin_end[] __asm__("_binary_nine_images_bin_end");
static const uint8_t *script_data(size_t *size)
{
    *size = (size_t)(nine_script_bin_end - nine_script_bin_start);
    return nine_script_bin_start;
}
static const uint8_t *images_data(size_t *size)
{
    *size = (size_t)(nine_images_bin_end - nine_images_bin_start);
    return nine_images_bin_start;
}
#endif
#define NINE_NS "nine3"
#define NINE_KEY_PERM "perm"
#define NINE_KEY_AUTO "auto"
#define NINE_KEY_BG "tbg"
#define NINE_KEY_SLOT "s%02d"
#define NINE_AUTO_WAIT_MS 1200u
#define NINE_PROGRESS_MS 1500u
#define NINE_PREVIEW_MS 2000u
#define NINE_BATTERY_MS 10000u

static const char *const TITLE_MENU[] = {
    "开始游戏", "继续游戏", "读档", "设置", "鉴赏", "主视觉", "关于",
};
static const int TITLE_MENU_COUNT = (int)(sizeof(TITLE_MENU) / sizeof(TITLE_MENU[0]));
static const char *const QUICK_MENU[] = {
    "返回剧情", "存档", "读档", "历史文本", "设置", "返回标题",
};
static const int QUICK_MENU_COUNT = (int)(sizeof(QUICK_MENU) / sizeof(QUICK_MENU[0]));
static const char *const GALLERY_MENU[] = { "CG 鉴赏", "剧情鉴赏", "音乐鉴赏", "OP 鉴赏" };
static const int GALLERY_MENU_COUNT = (int)(sizeof(GALLERY_MENU) / sizeof(GALLERY_MENU[0]));
// ---------------------------------------------------------------- 小工具
static uint32_t speed_ms(const nine_app_t *app)
{
    static const uint32_t SPEED_MS[4] = { 60, 35, 15, 0 };
    const uint8_t idx = app->settings.text_speed > 3 ? 1 : app->settings.text_speed;
    return SPEED_MS[idx];
}
static const char *speed_label(const nine_app_t *app)
{
    static const char *const LABEL[4] = { "慢", "中", "快", "瞬间" };
    const uint8_t idx = app->settings.text_speed > 3 ? 1 : app->settings.text_speed;
    return LABEL[idx];
}
// 16px 档 18 个全角/行 × 4 行;20px 档 15 个全角 × 3 行(320×240 横屏,300px 文本宽)。
static void layout_for(const nine_app_t *app, int *units, int *lines)
{
    if (app->settings.font_large) {
        *units = 30;
        *lines = GAL_TEXT_LINES_LARGE;
    } else {
        *units = 36;
        *lines = GAL_TEXT_LINES_SMALL;
    }
}
static void copy_str(char *dst, size_t cap, const char *src)
{
    if (!cap) return;
    size_t n = src ? strlen(src) : 0;
    if (n >= cap) n = cap - 1;
    if (n) memcpy(dst, src, n);
    dst[n] = 0;
}
static void show_notice(nine_app_t *app, const char *text)
{
    copy_str(app->notice, sizeof(app->notice), text ? text : "");
    app->notice_ms = text && text[0] ? NINE_PREVIEW_MS : 0;
    gal_ui_set_notice(&app->ui, app->notice);
}
static void move_selection(int *sel, int count, int delta)
{
    if (count <= 0) return;
    int next = *sel + delta;
    if (next < 0) next = 0;
    if (next >= count) next = count - 1;
    *sel = next;
}
static int window_first(int sel, int visible)
{
    if (visible <= 0) return 0;
    return (sel / visible) * visible;
}
static bool nvs_read(nine_app_t *app, const char *key, void *buf, size_t *len)
{
    if (!app->nvs_ok || !len) return false;
    size_t size = *len;
    const esp_err_t err = nvs_get_blob(app->nvs, key, buf, &size);
    if (err != ESP_OK) return false;
    *len = size;
    return true;
}
static bool nvs_write(nine_app_t *app, const char *key, const void *buf, size_t len)
{
    if (!app->nvs_ok) return false;
    if (nvs_set_blob(app->nvs, key, buf, len) != ESP_OK) return false;
    return nvs_commit(app->nvs) == ESP_OK;
}
static void slot_key(int slot, char *out, size_t cap)
{
    snprintf(out, cap, NINE_KEY_SLOT, slot + 1);
}
// ---------------------------------------------------------------- 永久进度
// progress is the permanent story record: it is written only for PERSISTENT_KEYS
// effects and never cleared by New Game. Manual slots carry a snapshot, but a
// normal load merges the newest permanent record (engine.js does the same).
static void perm_sync(nine_app_t *app)
{
    if (!app->pack_ok) return;
    // A chapter preview must never write (or roll back) the permanent record.
    if (app->vm.state.preview) return;
    uint8_t tmp[NINE_PERM_BYTES];
    const size_t n = nine_vm_progress_encode(&app->vm, tmp, sizeof(tmp));
    if (!n) return;
    if (n == app->perm_len && !memcmp(tmp, app->perm_buffer, n)) return;
    if (nvs_write(app, NINE_KEY_PERM, tmp, n)) {
        memset(app->perm_buffer, 0, sizeof(app->perm_buffer));
        memcpy(app->perm_buffer, tmp, n);
        app->perm_len = n;
    }
}
static void auto_write(nine_app_t *app)
{
    if (!app->pack_ok || !app->has_session) return;
    if (app->vm.state.preview) return;   // chapter preview never moves persistent progress
    const size_t n = nine_vm_save_encode(&app->vm, (uint16_t)app->text_page,
                                         app->save_buffer, sizeof(app->save_buffer));
    if (n && nvs_write(app, NINE_KEY_AUTO, app->save_buffer, n)) app->dirty_auto = false;
}
// ---------------------------------------------------------------- slots
static void slot_scan(nine_app_t *app)
{
    for (int i = 0; i < NINE_SAVE_SLOTS; ++i) {
        char key[8];
        slot_key(i, key, sizeof(key));
        uint8_t buf[NINE_SLOT_BYTES];
        size_t len = sizeof(buf);
        app->slots[i].used = false;
        if (!nvs_read(app, key, buf, &len)) continue;
        if (!nine_vm_save_peek(&app->pack, buf, len, &app->slots[i].node,
                               &app->slots[i].index, &app->slots[i].page,
                               &app->slots[i].preview)) {
            // A present but unreadable record is visible as "损坏", never as empty.
            app->slots[i].used = true;
            app->slots[i].corrupt = true;
            continue;
        }
        app->slots[i].used = true;
        app->slots[i].corrupt = false;
    }
}
// ---------------------------------------------------------------- 标题/主视觉
static int progress_value(const nine_app_t *app, const char *name)
{
    const int key = nine_pack_find_key(&app->pack, name);
    return key >= 0 ? app->vm.state.progress[key] : 0;
}
static int title_bg_unlocked(const nine_app_t *app)
{
    if (!app->pack_ok) return 1;
    // Match the pinned source UI rule (index.ux isTitleBgUnlocked): visual 2
    // needs any loop/true clear; visual 3 needs both. The transient titleBg
    // record is the last milestone (n40 writes 1, n77 writes 2) and must not
    // revoke a permanent unlock.
    const bool loop_cleared = progress_value(app, "loop1") || progress_value(app, "loop_hk");
    const bool true_cleared = progress_value(app, "true") ||
                              progress_value(app, "cf_hk") ||
                              progress_value(app, "clear_true");
    int unlocked = 1;
    if (loop_cleared || true_cleared) unlocked = 2;
    if (loop_cleared && true_cleared) unlocked = 3;
    return unlocked;
}
static void apply_title_bg(nine_app_t *app, bool force)
{
    if (!app->images_ok) { nine_image_clear(0x0000); app->title_bg_applied = false; return; }
    const int unlocked = title_bg_unlocked(app);
    if (app->bg_sel >= unlocked) app->bg_sel = unlocked - 1;
    if (app->bg_sel < 0) app->bg_sel = 0;
    uint32_t image = NINE_NONE;
    bool art_ok = false;
    if (nine_image_title((unsigned)app->bg_sel, &image)) {
        if (!force && app->title_bg_applied && app->title_bg_image == image) return;
        art_ok = nine_image_show_asset_full(image);
    }
    if (art_ok) {
        app->title_bg_image = image;
        app->title_bg_applied = true;
        ++app->title_bg_decodes;
    } else {
        app->title_bg_applied = false;
        nine_image_clear(0x0000);
        show_notice(app, "标题图像读取失败");
    }
    app->have_visual = false;
    app->last_visual = NINE_NONE;
}
// ---------------------------------------------------------------- 分页与正文
static void paginate(nine_app_t *app)
{
    int units = 36, lines = 4;
    layout_for(app, &units, &lines);
    int pages = gal_text_pages(app->text, units, lines, app->page_offsets,
                               GAL_TEXT_MAX_PAGES + 1);
    if (pages < 1) pages = 1;
    if (pages > GAL_TEXT_MAX_PAGES) pages = GAL_TEXT_MAX_PAGES;
    app->page_count = pages;
    if (app->text_page >= pages) app->text_page = pages - 1;
    if (app->text_page < 0) app->text_page = 0;
}
static void page_slice(nine_app_t *app)
{
    const uint32_t begin = app->page_offsets[app->text_page];
    uint32_t end = app->page_offsets[app->text_page + 1];
    if (end < begin) end = begin;
    size_t len = end - begin;
    if (len >= sizeof(app->page_text)) len = sizeof(app->page_text) - 1;
    memcpy(app->page_text, app->text + begin, len);
    app->page_text[len] = '\0';
}
static void history_add(nine_app_t *app)
{
    nine_history_t *h = &app->history;
    if (h->count < (int)NINE_HISTORY_MAX) {
        const int slot = (h->head + h->count) % NINE_HISTORY_MAX;
        copy_str(h->rows[slot].speaker, sizeof(h->rows[slot].speaker), app->speaker);
        copy_str(h->rows[slot].text, sizeof(h->rows[slot].text), app->text);
        ++h->count;
    } else {
        copy_str(h->rows[h->head].speaker, sizeof(h->rows[h->head].speaker), app->speaker);
        copy_str(h->rows[h->head].text, sizeof(h->rows[h->head].text), app->text);
        h->head = (h->head + 1) % NINE_HISTORY_MAX;
    }
}
// ---------------------------------------------------------------- 视图
static const char *chapter_of(nine_app_t *app, uint16_t node)
{
    nine_node_t n;
    if (!nine_pack_node(&app->pack, node, &n) || n.chapter == NINE_NONE) return "";
    nine_chapter_t c;
    if (!nine_pack_chapter(&app->pack, n.chapter, &c)) return "";
    const char *s = nine_pack_string(&app->pack, c.title);
    return s ? s : "";
}
static void retry_visual(nine_app_t *app, const nine_frame_t *frame)
{
    if (frame->visual == NINE_NONE) { app->view_error = NINE_VIEW_OK; return; }
    if (app->images_ok && nine_image_show_visual(frame->visual)) {
        app->last_visual = frame->visual;
        app->have_visual = true;
        app->view_error = NINE_VIEW_OK;
    } else {
        app->view_error = NINE_VIEW_IMAGE_ERROR;
        show_notice(app, "图像解码失败");
    }
}
static bool enter_text(nine_app_t *app, const nine_frame_t *frame)
{
    // load_slot/continue_game request an exact sub-page; every normal entry
    // starts at page 0. paginate() then clamps a stale/out-of-range page.
    const int requested_page = app->restore_text_page;
    app->restore_text_page = -1;
    const char *text = nine_pack_text(&app->pack, frame);
    if (!text) {
        app->text[0] = 0;
        app->speaker[0] = 0;
        app->text_page = 0;
        app->page_count = 1;
        app->page_text[0] = 0;
        app->view_error = NINE_VIEW_TEXT_ERROR;
        show_notice(app, "正文读取失败");
        return false;
    }
    copy_str(app->text, sizeof(app->text), text);
    const char *speaker = nine_pack_string(&app->pack, frame->speaker);
    copy_str(app->speaker, sizeof(app->speaker), speaker ? speaker : "");
    copy_str(app->chapter, sizeof(app->chapter), chapter_of(app, app->vm.state.node));
    app->text_page = requested_page >= 0 ? requested_page : 0;
    paginate(app);
    page_slice(app);
    history_add(app);
    if (frame->visual != NINE_NONE && (!app->have_visual || app->last_visual != frame->visual)) {
        if (!app->images_ok || !nine_image_show_visual(frame->visual)) {
            app->view_error = NINE_VIEW_IMAGE_ERROR;
            show_notice(app, "图像解码失败");
            return false;
        }
        app->last_visual = frame->visual;
        app->have_visual = true;
    }
    app->view_error = NINE_VIEW_OK;
    return true;
}
static void render_text(nine_app_t *app, bool full_page)
{
    gal_ui_set_chapter(&app->ui, app->chapter);
    gal_ui_set_mode(&app->ui, app->auto_mode);
    const bool same = app->typing_set && !full_page &&
                      app->typing_node == app->vm.state.node &&
                      app->typing_index == app->vm.state.index &&
                      app->typing_page == app->text_page &&
                      app->typing_large == (app->settings.font_large != 0);
    if (same) return;
    gal_ui_set_text(&app->ui, app->speaker, app->page_text,
                    (app->fast_forward || full_page) ? 0 : speed_ms(app), NULL, NULL);
    {
        char hud[24];
        snprintf(hud, sizeof(hud), "%d / %d", app->text_page + 1, app->page_count);
        gal_ui_set_page_hud(&app->ui, hud);
    }
    app->typing_node = app->vm.state.node;
    app->typing_index = app->vm.state.index;
    app->typing_page = app->text_page;
    app->typing_large = app->settings.font_large != 0;
    app->typing_set = true;
}
// ---------------------------------------------------------------- 选项/结局
static bool render_choice(nine_app_t *app, const nine_view_t *view)
{
    static gal_option_t options[NINE_CHOICE_MAX];
    static char prompts[NINE_CHOICE_MAX][48];
    const char *labels[NINE_CHOICE_MAX];
    int count = 0;
    for (int i = 0; i < (int)view->choice_count && i < (int)NINE_CHOICE_MAX; ++i) {
        nine_edge_t e;
        if (!nine_pack_edge(&app->pack, true, view->choices[i], &e)) return false;
        const char *label = nine_pack_string(&app->pack, e.label);
        copy_str(prompts[i], sizeof(prompts[i]), label ? label : "");
        options[i].label = prompts[i];
        options[i].flag_set = 0;
        options[i].require_flag = 0xFF;
        labels[i] = prompts[i];
        ++count;
    }
    if (!count) return false;
    if (app->choice_sel >= count) app->choice_sel = count - 1;
    if (app->choice_sel < 0) app->choice_sel = 0;
    gal_ui_set_choice(&app->ui, app->page_text, options, count, app->choice_sel);
    (void)labels;
    app->page = GAL_PAGE_CHOICE;
    gal_ui_show_page(&app->ui, GAL_PAGE_CHOICE);
    return true;
}
static int pick_ending(nine_app_t *app)
{
    int best = -1;
    for (uint32_t i = 0; i < app->pack.ending_count; ++i) {
        nine_ending_t e;
        if (!nine_pack_ending(&app->pack, i, &e)) continue;
        const int value = app->vm.state.progress[e.key];
        if (!value) continue;
        const char *name = nine_pack_key(&app->pack, e.key);
        if (name && !strcmp(name, "true")) return (int)i;   // true end wins
        if (best < 0) best = (int)i;
    }
    return best;
}
static void render_ending(nine_app_t *app)
{
    const int index = pick_ending(app);
    char eyebrow[24] = "BRANCH END";
    char title[64] = "剧情回溯";
    char body[192] = "";
    if (index >= 0) {
        nine_ending_t e;
        if (nine_pack_ending(&app->pack, (uint32_t)index, &e)) {
            const char *name = nine_pack_string(&app->pack, e.name);
            const char *desc = nine_pack_string(&app->pack, e.description);
            const char *key = nine_pack_key(&app->pack, e.key);
            copy_str(title, sizeof(title), name ? name : "");
            snprintf(body, sizeof(body), "%s\n\n%s", desc ? desc : "", "香坂春风线 · 完");
            if (key && !strcmp(key, "true")) snprintf(eyebrow, sizeof(eyebrow), "TRUE END");
        }
    } else {
        snprintf(body, sizeof(body), "已到达剧情终点。");
    }
    gal_ui_set_ending(&app->ui, eyebrow, title, body);
    gal_ui_set_mode(&app->ui, false);
    app->page = GAL_PAGE_ENDING;
    app->fast_forward = false;
    app->auto_mode = false;
    gal_ui_show_page(&app->ui, GAL_PAGE_ENDING);
}
// ---------------------------------------------------------------- 存档窗口
static void render_slots(nine_app_t *app, bool loading)
{
    const int first = window_first(app->slot_sel, NINE_WINDOW_SLOTS);
    static char labels[NINE_WINDOW_SLOTS][32];
    static char values[NINE_WINDOW_SLOTS][64];
    const char *label_ptrs[NINE_WINDOW_SLOTS];
    const char *value_ptrs[NINE_WINDOW_SLOTS];
    int count = 0;
    for (int i = 0; i < NINE_WINDOW_SLOTS; ++i) {
        const int slot = first + i;
        if (slot >= NINE_SAVE_SLOTS) break;
        snprintf(labels[i], sizeof(labels[i]), "存档 %02d", slot + 1);
        label_ptrs[i] = labels[i];
        const nine_slot_t *info = &app->slots[slot];
        if (!info->used) {
            snprintf(values[i], sizeof(values[i]), "空栏位");
        } else if (info->corrupt) {
            snprintf(values[i], sizeof(values[i]), "记录损坏");
        } else {
            char chapter[40];
            nine_node_t node;
            if (nine_pack_node(&app->pack, info->node, &node) && node.chapter != NINE_NONE) {
                nine_chapter_t c;
                if (nine_pack_chapter(&app->pack, node.chapter, &c)) {
                    const char *s = nine_pack_string(&app->pack, c.title);
                    copy_str(chapter, sizeof(chapter), s ? s : "");
                } else {
                    snprintf(chapter, sizeof(chapter), "章节");
                }
            } else {
                snprintf(chapter, sizeof(chapter), "章节");
            }
            snprintf(values[i], sizeof(values[i]), "%s · %u", chapter,
                     (unsigned)info->index + 1);
        }
        value_ptrs[i] = values[i];
        ++count;
    }
    const gal_list_t list = loading ? GAL_LIST_LOAD : GAL_LIST_SAVE;
    gal_ui_set_page_title(&app->ui, loading ? GAL_PAGE_LOAD : GAL_PAGE_SAVE,
                          loading ? "读取存档" : "保存存档");
    gal_ui_set_rows(&app->ui, list, label_ptrs, value_ptrs, count,
                    app->slot_sel - first);
    for (int i = 0; i < count; ++i) {
        const int slot = first + i;
        const bool usable = !loading || (app->slots[slot].used && !app->slots[slot].corrupt);
        gal_ui_set_row_enabled(&app->ui, list, i, usable);
    }
    static char footer[64];
    const int last = first + count;
    snprintf(footer, sizeof(footer), "%d-%d / %d · 长按确定返回", first + 1, last,
             NINE_SAVE_SLOTS);
    gal_ui_set_footer(&app->ui, loading ? GAL_PAGE_LOAD : GAL_PAGE_SAVE, footer);
    const nine_slot_t *sel = &app->slots[app->slot_sel];
    static char preview[128];
    if (sel->used && !sel->corrupt) {
        snprintf(preview, sizeof(preview), "存档 %02d\n%s", app->slot_sel + 1,
                 sel->preview ? "章节试读存档" : "剧情存档");
    } else if (sel->corrupt) {
        snprintf(preview, sizeof(preview), "存档 %02d\n记录校验失败", app->slot_sel + 1);
    } else {
        snprintf(preview, sizeof(preview), "存档 %02d\n空栏位", app->slot_sel + 1);
    }
    gal_ui_set_save_preview(&app->ui, preview);
    app->page = loading ? GAL_PAGE_LOAD : GAL_PAGE_SAVE;
    gal_ui_show_page(&app->ui, app->page);
}
// ---------------------------------------------------------------- 渲染各页
static void render_settings(nine_app_t *app)
{
    static const char *const LABELS_READER[7] = {
        "文字速度", "文字大小", "自动阅读", "对话透明度", "恢复默认", "返回标题", "返回",
    };
    static const char *const LABELS_TITLE[6] = {
        "文字速度", "文字大小", "自动阅读", "对话透明度", "恢复默认", "返回",
    };
    const bool reader = app->return_page == GAL_PAGE_MENU;
    static char transparency[8];
    snprintf(transparency, sizeof(transparency), "%u%%",
             (unsigned)app->settings.panel_transparency);
    const char *values_reader[7] = {
        speed_label(app), app->settings.font_large ? "大" : "小",
        app->settings.auto_play ? "开" : "关", transparency, "", "", "",
    };
    const char *values_title[6] = {
        speed_label(app), app->settings.font_large ? "大" : "小",
        app->settings.auto_play ? "开" : "关", transparency, "", "",
    };
    if (reader) {
        gal_ui_set_rows(&app->ui, GAL_LIST_SETTINGS, LABELS_READER, values_reader, 7,
                        app->settings_sel);
    } else {
        gal_ui_set_rows(&app->ui, GAL_LIST_SETTINGS, LABELS_TITLE, values_title, 6,
                        app->settings_sel);
    }
    app->page = GAL_PAGE_SETTINGS;
    gal_ui_show_page(&app->ui, GAL_PAGE_SETTINGS);
}
static void render_history(nine_app_t *app)
{
    const int first = window_first(app->history_sel, NINE_WINDOW_HISTORY);
    static char text[NINE_WINDOW_HISTORY][320];
    const char *labels[NINE_WINDOW_HISTORY];
    int count = 0;
    for (int i = 0; i < NINE_WINDOW_HISTORY; ++i) {
        const int row = first + i;
        if (row >= app->history.count) break;
        const int slot = (app->history.head + row) % NINE_HISTORY_MAX;
        const char *speaker = app->history.rows[slot].speaker;
        if (speaker[0]) {
            snprintf(text[i], sizeof(text[i]), "%s\n%s", speaker, app->history.rows[slot].text);
        } else {
            snprintf(text[i], sizeof(text[i]), "旁白\n%s", app->history.rows[slot].text);
        }
        labels[i] = text[i];
        ++count;
    }
    gal_ui_set_rows(&app->ui, GAL_LIST_HISTORY, labels, NULL, count,
                    count ? app->history_sel - first : 0);
    static char footer[64];
    snprintf(footer, sizeof(footer), "%d / %d · 长按确定返回",
             app->history.count ? app->history_sel + 1 : 0, app->history.count);
    gal_ui_set_footer(&app->ui, GAL_PAGE_HISTORY, footer);
    app->page = GAL_PAGE_HISTORY;
    gal_ui_show_page(&app->ui, GAL_PAGE_HISTORY);
}
static void render_gallery(nine_app_t *app)
{
    static char values[4][24];
    const char *value_ptrs[4];
    snprintf(values[0], sizeof(values[0]), "%u 组", (unsigned)app->pack.gallery_groups);
    snprintf(values[1], sizeof(values[1]), "%u 章", (unsigned)app->pack.chapter_count);
    snprintf(values[2], sizeof(values[2]), "%u 曲", (unsigned)nine_media_track_count());
    snprintf(values[3], sizeof(values[3]), "%s", app->gallery_op_error ? "失败" : "OP");
    for (int i = 0; i < GALLERY_MENU_COUNT; ++i) value_ptrs[i] = values[i];
    gal_ui_set_rows(&app->ui, GAL_LIST_GALLERY, GALLERY_MENU, value_ptrs,
                    GALLERY_MENU_COUNT, app->gallery_sel);
    // Title BGM belongs to the title area only; returning to title resumes it.
    nine_media_stop();
    nine_image_set_visible(false);
    app->page = GAL_PAGE_GALLERY;
    gal_ui_show_page(&app->ui, GAL_PAGE_GALLERY);
}
static void render_cg_groups(nine_app_t *app)
{
    const int first = window_first(app->cg_group, NINE_WINDOW_LIST);
    static char labels[NINE_WINDOW_LIST][32];
    static char values[NINE_WINDOW_LIST][24];
    const char *label_ptrs[NINE_WINDOW_LIST];
    const char *value_ptrs[NINE_WINDOW_LIST];
    int count = 0;
    uint32_t index = 0;
    int group = -1;
    // Group boundaries are the source gallery group ids, repeated per variant.
    for (uint32_t i = 0; i < app->pack.gallery_count; ++i) {
        nine_gallery_record_t g;
        if (!nine_pack_gallery(&app->pack, i, &g)) break;
        if ((int)g.group != group) {
            group = (int)g.group;
            if (group >= first && count < NINE_WINDOW_LIST) {
                const char *name = nine_pack_string(&app->pack, g.name_str);
                snprintf(labels[count], sizeof(labels[count]), "%s",
                         name ? name : "CG");
                label_ptrs[count] = labels[count];
                uint32_t n = 0;
                for (uint32_t j = i; j < app->pack.gallery_count; ++j) {
                    nine_gallery_record_t x;
                    if (!nine_pack_gallery(&app->pack, j, &x) || (int)x.group != group) break;
                    ++n;
                }
                snprintf(values[count], sizeof(values[count]), "%u 枚", (unsigned)n);
                value_ptrs[count] = values[count];
                ++count;
            }
        }
        ++index;
    }
    (void)index;
    gal_ui_set_page_title(&app->ui, GAL_PAGE_RECALL, "CG 鉴赏");
    gal_ui_set_rows(&app->ui, GAL_LIST_RECALL, label_ptrs, value_ptrs, count,
                    app->cg_group - first);
    static char footer[64];
    snprintf(footer, sizeof(footer), "%d / %u 组 · 长按确定返回", app->cg_group + 1,
             (unsigned)app->pack.gallery_groups);
    gal_ui_set_footer(&app->ui, GAL_PAGE_RECALL, footer);
    app->page = GAL_PAGE_RECALL;
    gal_ui_show_page(&app->ui, GAL_PAGE_RECALL);
}
static void render_cg_view(nine_app_t *app)
{
    uint32_t first = 0, total = 0;
    bool have_first = false;
    for (uint32_t i = 0; i < app->pack.gallery_count; ++i) {
        nine_gallery_record_t g;
        if (!nine_pack_gallery(&app->pack, i, &g)) break;
        if ((int)g.group == app->cg_group) {
            if (!have_first) { first = i; have_first = true; }
            ++total;
        }
    }
    if (!total) { show_notice(app, "该组没有可显示的 CG"); return; }
    if (app->cg_variant < 0) app->cg_variant = (int)total - 1;
    if (app->cg_variant >= (int)total) app->cg_variant = 0;
    nine_gallery_record_t g;
    if (!nine_pack_gallery(&app->pack, first + (uint32_t)app->cg_variant, &g)) {
        show_notice(app, "CG 记录损坏");
        return;
    }
    const char *name = nine_pack_string(&app->pack, g.name_str);
    (void)name;
    bool art_ok = false;
    if (app->images_ok) art_ok = nine_image_show_asset_full(g.image);
    if (!art_ok) {
        nine_image_clear(0x0000);
        show_notice(app, "CG 图像读取失败");
    }
    app->have_visual = false;
    app->last_visual = NINE_NONE;
    // Pure CG view has no permanent text/status overlay covering the art.
    gal_ui_set_cg(&app->ui, "", "", "", NULL);
    app->page = GAL_PAGE_CG;
    gal_ui_show_page(&app->ui, GAL_PAGE_CG);
}
static void render_chapters(nine_app_t *app)
{
    const int count = (int)app->pack.chapter_count;
    const int first = window_first(app->chapter_sel, NINE_WINDOW_CHAPTER);
    static char labels[NINE_WINDOW_CHAPTER][48];
    static char values[NINE_WINDOW_CHAPTER][24];
    const char *label_ptrs[NINE_WINDOW_CHAPTER];
    const char *value_ptrs[NINE_WINDOW_CHAPTER];
    int shown = 0;
    for (int i = 0; i < NINE_WINDOW_CHAPTER; ++i) {
        const int index = first + i;
        if (index >= count) break;
        nine_chapter_t c;
        if (!nine_pack_chapter(&app->pack, (uint32_t)index, &c)) break;
        const char *title = nine_pack_string(&app->pack, c.title);
        copy_str(labels[i], sizeof(labels[i]), title ? title : "");
        snprintf(values[i], sizeof(values[i]), "第 %02d 章", index + 1);
        label_ptrs[i] = labels[i];
        value_ptrs[i] = values[i];
        ++shown;
    }
    gal_ui_set_rows(&app->ui, GAL_LIST_CHAPTER, label_ptrs, value_ptrs, shown,
                    app->chapter_sel - first);
    nine_chapter_t sel;
    if (nine_pack_chapter(&app->pack, (uint32_t)app->chapter_sel, &sel)) {
        const char *preview = nine_pack_string(&app->pack, sel.preview);
        const char *title = nine_pack_string(&app->pack, sel.title);
        static char info[192];
        snprintf(info, sizeof(info), "%s\n%s", title ? title : "",
                 preview ? preview : "从这一章开头开始试读;试读不写入永久进度。");
        gal_ui_set_chapter_info(&app->ui, info);
    }
    static char footer[64];
    snprintf(footer, sizeof(footer), "%d / %d 章 · 长按确定返回", app->chapter_sel + 1, count);
    gal_ui_set_footer(&app->ui, GAL_PAGE_CHAPTER, footer);
    app->page = GAL_PAGE_CHAPTER;
    gal_ui_show_page(&app->ui, GAL_PAGE_CHAPTER);
}
static void render_music(nine_app_t *app)
{
    static char labels[8][48];
    static char values[8][24];
    const char *label_ptrs[8];
    const char *value_ptrs[8];
    const uint32_t count = nine_media_track_count() > 8 ? 8 : nine_media_track_count();
    const uint32_t current = nine_media_current_track();
    for (uint32_t i = 0; i < count; ++i) {
        nine_media_track_t info;
        const char *name = "曲目";
        if (nine_media_track_info(i, &info) && info.name) name = info.name;
        copy_str(labels[i], sizeof(labels[i]), !strcmp(name, "title") ? "标题曲" :
                 !strcmp(name, "opening") ? "片头曲" : name);
        snprintf(values[i], sizeof(values[i]), "%s",
                 current == i ? "播放中" : "确定播放");
        label_ptrs[i] = labels[i];
        value_ptrs[i] = values[i];
    }
    if (app->music_sel >= (int)count) app->music_sel = count ? (int)count - 1 : 0;
    gal_ui_set_rows(&app->ui, GAL_LIST_MUSIC, label_ptrs, value_ptrs, (int)count,
                    app->music_sel);
    app->page = GAL_PAGE_MUSIC;
    gal_ui_show_page(&app->ui, GAL_PAGE_MUSIC);
}
static void render_about(nine_app_t *app)
{
    static char text[512];
    snprintf(text, sizeof(text),
             "%s\n\n离线移植版:全部剧情、选项、结局与画面来自固定来源快照。\n"
             "来源:%s\n"
             "操作:短按 OK/上/下推进;长按 OK 菜单;长按上快进;长按下自动。\n"
             "本作仅在本机运行,不含联网功能。",
             nine_pack_title(&app->pack), "xuanyide01/9-nine-rw5 @ 6a8907a5");
    gal_ui_set_about(&app->ui, "", "", text);
    app->page = GAL_PAGE_ABOUT;
    gal_ui_show_page(&app->ui, GAL_PAGE_ABOUT);
}
static void render_menu(nine_app_t *app)
{
    gal_ui_set_rows(&app->ui, GAL_LIST_MENU, QUICK_MENU, NULL, QUICK_MENU_COUNT,
                    app->menu_sel);
    app->page = GAL_PAGE_MENU;
    gal_ui_show_page(&app->ui, GAL_PAGE_MENU);
}
static void play_title_music(nine_app_t *app)
{
    if (!app->media_ok) return;
    for (uint32_t i = 0; i < nine_media_track_count(); ++i) {
        nine_media_track_t info;
        if (!nine_media_track_info(i, &info) || !info.name || strcmp(info.name, "title")) continue;
        if (nine_media_current_track() != i || !nine_media_is_playing()) {
            if (nine_media_play(i)) ++app->title_music_starts;
        }
        return;
    }
}
static void render_title(nine_app_t *app)
{
    const bool was_title = app->page == GAL_PAGE_TITLE;
    static char values[7][24];
    static const char *value_ptrs[7];
    int used_slots = 0;
    for (int i = 0; i < NINE_SAVE_SLOTS; ++i) {
        if (app->slots[i].used && !app->slots[i].corrupt) ++used_slots;
    }
    values[0][0] = 0;
    copy_str(values[1], sizeof(values[1]), app->has_session ? "有进度" : "无进度");
    snprintf(values[2], sizeof(values[2]), "%d 槽", used_slots);
    values[3][0] = 0;
    values[4][0] = 0;
    snprintf(values[5], sizeof(values[5]), "%d / %d", app->bg_sel + 1, title_bg_unlocked(app));
    snprintf(values[6], sizeof(values[6]), "v0.1.0");
    for (int i = 0; i < TITLE_MENU_COUNT; ++i) value_ptrs[i] = values[i];
    gal_ui_set_rows(&app->ui, GAL_LIST_TITLE, TITLE_MENU, value_ptrs, TITLE_MENU_COUNT,
                    app->title_sel);
    bool any_slot = used_slots > 0;
    gal_ui_set_row_enabled(&app->ui, GAL_LIST_TITLE, 1, app->has_session);
    gal_ui_set_row_enabled(&app->ui, GAL_LIST_TITLE, 2, any_slot);
    apply_title_bg(app, !was_title);
    play_title_music(app);
    gal_ui_set_brand(&app->ui, "", "");
    app->page = GAL_PAGE_TITLE;
    gal_ui_show_page(&app->ui, GAL_PAGE_TITLE);
}
// ---------------------------------------------------------------- OP
static void finish_op(nine_app_t *app, bool advance)
{
    if (!app->opening) return;
    app->opening = false;
    app->restore_text_page = -1;
    nine_media_stop();
    app->page = GAL_PAGE_GAME;
    app->have_visual = false;
    if (advance) {
        const nine_view_t *v = nine_vm_advance(&app->vm);
        if (v && v->kind == NINE_VM_ERROR) {
            show_notice(app, v->error ? v->error : "剧情跳转失败");
        }
    }
    app->dirty_auto = true;
}
static bool find_opening_track(uint32_t *track)
{
    if (!track) return false;
    for (uint32_t i = 0; i < nine_media_track_count(); ++i) {
        nine_media_track_t info;
        if (nine_media_track_info(i, &info) && info.name && !strcmp(info.name, "opening")) {
            *track = i;
            return true;
        }
    }
    return false;
}
static bool start_op(nine_app_t *app)
{
    uint32_t track = NINE_NONE;
    (void)find_opening_track(&track);
    app->opening = true;
    app->fast_forward = false;
    app->auto_mode = false;
    gal_ui_set_mode(&app->ui, false);
    gal_ui_set_transition(&app->ui, "", "", "");
    app->page = GAL_PAGE_TRANSITION;
    gal_ui_show_page(&app->ui, GAL_PAGE_TRANSITION);
    if (track == NINE_NONE || !nine_media_play_op(track)) {
        // A missing OP must not strand the story: advance the marker instead.
        app->opening = false;
        app->page = GAL_PAGE_GAME;
        const nine_view_t *v = nine_vm_advance(&app->vm);
        (void)v;
        return false;
    }
    return true;
}
// Gallery OP is a separate playback mode: it never advances the VM, never
// syncs permanent progress and never queues an NVS write. Failure/skip/natural
// end all return to the same selected Gallery row.
#ifndef NINE_APP_HOST
static void op_diag(const char *phase)
{
    nine_media_stats_t m;
    nine_image_stats_t i;
    nine_media_stats(&m);
    nine_image_stats(&i);
    ESP_LOGI(TAG, "op_diag %s media blocks=%u samples=%u write_gap_max=%ums write_max=%ums wall=%ums frames=%u frame_gap_max=%ums end=%u image renders=%u crc=%u jpeg=%u flush=%u heap=%u largest=%u stack=%u",
             phase, (unsigned)m.blocks, (unsigned)m.samples,
             (unsigned)(m.write_gap_max_us / 1000), (unsigned)(m.write_max_us / 1000),
             (unsigned)m.wall_ms, (unsigned)m.frames,
             (unsigned)(m.frame_gap_max_us / 1000), (unsigned)m.end_reason,
             (unsigned)i.renders, (unsigned)i.crc_checks, (unsigned)i.jpeg_decodes,
             (unsigned)bsp_lvgl_flush_count(), (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
}
#else
static void op_diag(const char *phase) { (void)phase; }
#endif

static void gallery_op_fail(nine_app_t *app, const char *message)
{
    app->gallery_op = false;
    app->opening = false;
    nine_media_stop();
    nine_image_clear(0);
    nine_image_set_visible(false);
    app->gallery_op_error = true;
    app->gallery_sel = 3;
    op_diag("fail");
    render_gallery(app);
    (void)message;
    ESP_LOGE(TAG, "gallery_op failed: %s", message ? message : "unknown");
}
static void gallery_op_finish(nine_app_t *app, const char *reason)
{
    if (!app->gallery_op) return;
    app->gallery_op = false;
    nine_media_stop();
    nine_image_clear(0);
    nine_image_set_visible(false);
    app->gallery_op_error = false;
    app->gallery_sel = 3;
    op_diag(reason ? reason : "natural");
    render_gallery(app);
    (void)reason;
    ESP_LOGI(TAG, "gallery_op end: %s", reason ? reason : "natural");
}
static void start_gallery_op(nine_app_t *app)
{
    if (!app->media_ok) { gallery_op_fail(app, "音频不可用"); return; }
    uint32_t track = NINE_NONE;
    if (!find_opening_track(&track)) { gallery_op_fail(app, "缺少 OP 资源"); return; }
    app->gallery_op = true;
    app->opening = false;
    app->gallery_op_error = false;
    app->fast_forward = false;
    if (app->auto_mode) {
        app->auto_mode = false;
        gal_ui_set_mode(&app->ui, false);
    }
    gal_ui_set_transition(&app->ui, "", "", "");
    app->page = GAL_PAGE_TRANSITION;
    gal_ui_show_page(&app->ui, GAL_PAGE_TRANSITION);
    nine_image_set_visible(true);
    if (!nine_media_play_op(track)) {
        gallery_op_fail(app, "OP 播放失败");
        return;
    }
    op_diag("start");
    ESP_LOGI(TAG, "gallery_op start: track %u", (unsigned)track);
}
static void handle_gallery_op(nine_app_t *app, const nine_key_t *key)
{
    if (!app->gallery_op) return;
    if (nine_media_failed()) { gallery_op_fail(app, "OP 播放失败"); return; }
    if ((key->ev == BSP_BTN_CLICK || key->ev == BSP_BTN_LONG) && key->btn == BSP_BTN_OK) {
        gallery_op_finish(app, key->ev == BSP_BTN_LONG ? "back" : "skip");
    }
}
// ---------------------------------------------------------------- 核心渲染
static void render_game(nine_app_t *app);
static void goto_game(nine_app_t *app)
{
    app->page = GAL_PAGE_GAME;
    render_game(app);
}
static void render_game(nine_app_t *app)
{
    if (!app->pack_ok) {
        gal_ui_set_notice(&app->ui, "资源包校验失败,无法读取剧情");
        gal_ui_show_page(&app->ui, GAL_PAGE_START);
        app->page = GAL_PAGE_START;
        return;
    }
    const nine_view_t *v = nine_vm_locate(&app->vm);
    if (!v || v->kind == NINE_VM_ERROR) {
        app->restore_text_page = -1;
        copy_str(app->error, sizeof(app->error), v && v->error ? v->error : "剧情读取失败");
        show_notice(app, app->error);
        gal_ui_show_page(&app->ui, GAL_PAGE_GAME);
        app->page = GAL_PAGE_GAME;
        return;
    }
    if (v->kind != NINE_VM_TEXT) app->restore_text_page = -1;
    if (v->kind == NINE_VM_END) {
        perm_sync(app);
        render_ending(app);
        return;
    }
    if (v->kind == NINE_VM_OP) {
        if (!app->opening) (void)start_op(app);
        if (!app->opening) render_game(app);
        return;
    }
    if (v->kind == NINE_VM_CHOICE) {
        app->fast_forward = false;
        // Node effects at the choice boundary may write permanent keys (n40).
        perm_sync(app);
        gal_ui_show_page(&app->ui, GAL_PAGE_GAME);
        app->page = GAL_PAGE_GAME;
        if (!render_choice(app, v)) {
            show_notice(app, "选项数据损坏");
        }
        return;
    }
    nine_frame_t frame;
    if (!nine_vm_current_frame(&app->vm, &frame)) {
        show_notice(app, "剧情帧读取失败");
        return;
    }
    if (app->last_node != app->vm.state.node || app->last_index != app->vm.state.index ||
        app->last_frame != v->frame) {
        (void)enter_text(app, &frame);
        app->last_node = app->vm.state.node;
        app->last_index = app->vm.state.index;
        app->last_frame = v->frame;
    } else if (app->view_error == NINE_VIEW_IMAGE_ERROR) {
        retry_visual(app, &frame);
    } else if (app->view_error == NINE_VIEW_TEXT_ERROR) {
        (void)enter_text(app, &frame);
    }
    app->page = GAL_PAGE_GAME;
    render_text(app, app->fast_forward);
    gal_ui_show_page(&app->ui, GAL_PAGE_GAME);
    perm_sync(app);
}
// ---------------------------------------------------------------- 推进
static void advance_reading(nine_app_t *app, bool fast)
{
    if (!app->pack_ok) return;
    if (app->opening) { finish_op(app, true); render_game(app); return; }
    if (app->view_error != NINE_VIEW_OK) {
        // Missing text/image must not be skipped by moving the VM forward.
        app->fast_forward = false;
        if (app->auto_mode) toggle_auto(app, false);
        show_notice(app, app->view_error == NINE_VIEW_IMAGE_ERROR
                              ? "图像读取失败,无法继续" : "正文读取失败,无法继续");
        return;
    }
    const nine_view_t *v = nine_vm_locate(&app->vm);
    if (!v || v->kind == NINE_VM_ERROR) {
        show_notice(app, v && v->error ? v->error : "剧情读取失败");
        return;
    }
    if (v->kind != NINE_VM_TEXT) {
        if (v->kind == NINE_VM_END) render_ending(app);
        return;
    }
    // 短按必须真的推进:当前页若仍在打字,先补全,然后照样翻到下一页/下一句。
    if (gal_ui_typing(&app->ui)) {
        gal_ui_show_full_text(&app->ui);
        app->auto_wait_ms = 0;
    }
    if (app->text_page + 1 < app->page_count) {
        app->text_page++;
        page_slice(app);
        render_text(app, fast);
        app->dirty_auto = true;
        return;
    }
    const nine_view_t *next = nine_vm_advance(&app->vm);
    app->text_page = 0;
    app->restore_text_page = -1;
    if (!next || next->kind == NINE_VM_ERROR) {
        app->fast_forward = false;
        show_notice(app, next && next->error ? next->error : "剧情跳转失败");
        return;
    }
    if (next->kind == NINE_VM_CHOICE) app->choice_sel = 0;
    render_game(app);
    app->dirty_auto = true;
}
// ---------------------------------------------------------------- 页面键处理
static void open_slots(nine_app_t *app, bool loading)
{
    app->slot_sel = 0;
    if (loading) {
        for (int i = 0; i < NINE_SAVE_SLOTS; ++i) {
            if (app->slots[i].used && !app->slots[i].corrupt) { app->slot_sel = i; break; }
        }
    }
    render_slots(app, loading);
}
static void save_slot(nine_app_t *app, int slot)
{
    if (slot < 0 || slot >= NINE_SAVE_SLOTS) return;
    const size_t n = nine_vm_save_encode(&app->vm, (uint16_t)app->text_page,
                                         app->save_buffer, sizeof(app->save_buffer));
    if (!n) { show_notice(app, "存档编码失败"); return; }
    char key[8];
    slot_key(slot, key, sizeof(key));
    if (!nvs_write(app, key, app->save_buffer, n)) {
        show_notice(app, "保存失败:存储不可用");
        return;
    }
    if (!nine_vm_save_peek(&app->pack, app->save_buffer, n, &app->slots[slot].node,
                           &app->slots[slot].index, &app->slots[slot].page,
                           &app->slots[slot].preview)) {
        show_notice(app, "保存后校验失败");
        return;
    }
    app->slots[slot].used = true;
    app->slots[slot].corrupt = false;
    show_notice(app, "已保存");
    render_slots(app, false);
}
static void load_slot(nine_app_t *app, int slot)
{
    if (slot < 0 || slot >= NINE_SAVE_SLOTS) return;
    char key[8];
    slot_key(slot, key, sizeof(key));
    uint8_t buf[NINE_SLOT_BYTES];
    size_t len = sizeof(buf);
    if (!nvs_read(app, key, buf, &len)) { show_notice(app, "该栏位无法读取"); return; }
    uint16_t page = 0;
    // A preview snapshot carries an older permanent record. Keep the newest one
    // in memory so returning to the title / New Game cannot regress unlocks.
    int32_t keep_progress[NINE_KEY_MAX];
    uint8_t keep_set[NINE_KEY_MAX / 8];
    memcpy(keep_progress, app->vm.state.progress, sizeof(keep_progress));
    memcpy(keep_set, app->vm.state.progress_set, sizeof(keep_set));
    if (!nine_vm_save_decode(&app->vm, buf, len, &page)) {
        // decode validates CRC and bounds before touching live state.
        show_notice(app, "存档数据校验失败");
        return;
    }
    if (app->vm.state.preview) {
        memcpy(app->vm.state.progress, keep_progress, sizeof(keep_progress));
        memcpy(app->vm.state.progress_set, keep_set, sizeof(keep_set));
    }
    app->text_page = page;
    app->restore_text_page = (int)page;
    app->has_session = true;
    app->last_frame = NINE_NONE;
    app->have_visual = false;
    app->view_error = NINE_VIEW_OK;
    app->dirty_auto = true;
    show_notice(app, "已读取");
    const nine_view_t *v = nine_vm_locate(&app->vm);
    if (!v || v->kind == NINE_VM_ERROR) {
        show_notice(app, v && v->error ? v->error : "存档指向无效位置");
        return;
    }
    goto_game(app);
}
static void new_game(nine_app_t *app)
{
    if (!app->pack_ok) { show_notice(app, "资源包校验失败"); return; }
    if (!nine_vm_start(&app->vm, NINE_NODE_NONE, false)) {
        show_notice(app, "剧情起点读取失败");
        return;
    }
    app->has_session = true;
    app->history.count = 0;
    app->history.head = 0;
    app->text_page = 0;
    app->restore_text_page = -1;
    app->last_frame = NINE_NONE;
    app->have_visual = false;
    app->view_error = NINE_VIEW_OK;
    app->auto_mode = false;
    app->fast_forward = false;
    app->opening = false;
    app->dirty_auto = true;
    nine_media_stop();
    goto_game(app);
    auto_write(app);
}
static void continue_game(nine_app_t *app)
{
    if (!app->has_session) { show_notice(app, "没有可继续的进度"); return; }
    uint8_t buf[NINE_SLOT_BYTES];
    size_t len = sizeof(buf);
    if (!nvs_read(app, NINE_KEY_AUTO, buf, &len)) { show_notice(app, "进度记录不存在"); return; }
    uint16_t page = 0;
    if (!nine_vm_save_decode(&app->vm, buf, len, &page)) {
        show_notice(app, "进度校验失败");
        return;
    }
    app->text_page = page;
    app->restore_text_page = (int)page;
    app->last_frame = NINE_NONE;
    app->have_visual = false;
    app->view_error = NINE_VIEW_OK;
    app->opening = false;
    nine_media_stop();
    goto_game(app);
}
static void handle_title(nine_app_t *app, const nine_key_t *key)
{
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        move_selection(&app->title_sel, TITLE_MENU_COUNT,
                       key->btn == BSP_BTN_UP ? -1 : 1);
        render_title(app);
        return;
    }
    if (key->ev != BSP_BTN_CLICK || key->btn != BSP_BTN_OK) return;
    if (!app->pack_ok && app->title_sel != 3 && app->title_sel != 6) {
        show_notice(app, "资源包校验失败");
        return;
    }
    switch (app->title_sel) {
    case 0: new_game(app); break;
    case 1: continue_game(app); break;
    case 2:
        app->return_page = GAL_PAGE_TITLE;
        open_slots(app, true);
        break;
    case 3:
        app->return_page = GAL_PAGE_TITLE;
        app->settings_sel = 0;
        render_settings(app);
        break;
    case 4:
        app->gallery_sel = 0;
        render_gallery(app);
        break;
    case 5: {
        const int unlocked = title_bg_unlocked(app);
        app->bg_sel = (app->bg_sel + 1) % unlocked;
        if (app->nvs_ok) {
            const uint8_t v = (uint8_t)app->bg_sel;
            (void)nvs_write(app, NINE_KEY_BG, &v, sizeof(v));
        }
        render_title(app);
        break;
    }
    default: render_about(app); break;
    }
}
static void toggle_auto(nine_app_t *app, bool on)
{
    if (app->auto_mode == on) return;
    app->auto_mode = on;
    app->auto_wait_ms = 0;
    app->settings.auto_play = on ? 1 : 0;
    (void)gal_settings_store(&app->settings);
    if (on) {
        gal_ui_show_full_text(&app->ui);
        app->fast_forward = false;
    }
    gal_ui_set_mode(&app->ui, on);
}
static void handle_game(nine_app_t *app, const nine_key_t *key)
{
    if (app->opening) {
        if (key->ev == BSP_BTN_CLICK || key->ev == BSP_BTN_LONG) {
            finish_op(app, true);
            render_game(app);
        }
        return;
    }
    // 短按 OK/上/下等价:推进正文。自动阅读中的手动按键仍然推进,
    // 只关闭自动,不吞掉这次操作。
    if (key->ev == BSP_BTN_CLICK &&
        (key->btn == BSP_BTN_OK || key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        if (app->fast_forward) app->fast_forward = false;
        if (app->auto_mode) toggle_auto(app, false);
        advance_reading(app, false);
        return;
    }
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        app->suppress_click[BSP_BTN_OK] = true;
        app->fast_forward = false;
        if (app->auto_mode) toggle_auto(app, false);
        app->menu_sel = 0;
        app->return_page = GAL_PAGE_GAME;
        render_menu(app);
        return;
    }
    if (key->btn == BSP_BTN_UP) {
        if (key->ev == BSP_BTN_LONG) {
            app->suppress_click[BSP_BTN_UP] = true;
            if (app->auto_mode) toggle_auto(app, false);
            app->fast_forward = true;
            // 长按阈值到达即推进一整句:跳过本句剩余分页与打字/自动等待。
            if (gal_ui_typing(&app->ui)) gal_ui_show_full_text(&app->ui);
            advance_reading(app, true);
            return;
        }
        if (key->ev == BSP_BTN_RELEASE) {
            app->fast_forward = false;
            return;
        }
    }
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_DOWN) {
        app->suppress_click[BSP_BTN_DOWN] = true;
        toggle_auto(app, !app->auto_mode);
        return;
    }
}
static void handle_choice(nine_app_t *app, const nine_key_t *key)
{
    const nine_view_t *v = nine_vm_locate(&app->vm);
    if (!v || v->kind != NINE_VM_CHOICE) return;
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        // The pinned source choice panel has 打开菜单. Open the same in-game
        // menu as the reading page; the VM and choice_sel stay untouched, so
        // the return path renders the exact choice again.
        app->suppress_click[BSP_BTN_OK] = true;
        app->fast_forward = false;
        if (app->auto_mode) toggle_auto(app, false);
        app->menu_sel = 0;
        app->return_page = GAL_PAGE_CHOICE;
        render_menu(app);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        move_selection(&app->choice_sel, v->choice_count,
                       key->btn == BSP_BTN_UP ? -1 : 1);
        render_choice(app, v);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_OK) {
        const nine_view_t *next = nine_vm_choose(&app->vm, (uint32_t)app->choice_sel);
        app->text_page = 0;
        app->restore_text_page = -1;
        app->last_frame = NINE_NONE;
        if (!next || next->kind == NINE_VM_ERROR) {
            show_notice(app, next && next->error ? next->error : "选项跳转失败");
            return;
        }
        app->page = GAL_PAGE_GAME;
        app->choice_sel = 0;
        render_game(app);
        app->dirty_auto = true;
        perm_sync(app);
        return;
    }
}
static void handle_menu(nine_app_t *app, const nine_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        goto_game(app);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        move_selection(&app->menu_sel, QUICK_MENU_COUNT,
                       key->btn == BSP_BTN_UP ? -1 : 1);
        render_menu(app);
        return;
    }
    if (key->ev != BSP_BTN_CLICK || key->btn != BSP_BTN_OK) return;
    switch ((nine_menu_action_t)app->menu_sel) {
    case NINE_MENU_SAVE:
        app->return_page = GAL_PAGE_MENU;
        open_slots(app, false);
        break;
    case NINE_MENU_LOAD:
        app->return_page = GAL_PAGE_MENU;
        open_slots(app, true);
        break;
    case NINE_MENU_HISTORY:
        app->history_sel = app->history.count ? app->history.count - 1 : 0;
        render_history(app);
        break;
    case NINE_MENU_SETTINGS:
        app->return_page = GAL_PAGE_MENU;
        app->settings_sel = 0;
        render_settings(app);
        break;
    case NINE_MENU_TITLE:
        app->auto_mode = false;
        app->fast_forward = false;
        app->title_sel = 0;
        render_title(app);
        break;
    default:
        goto_game(app);
        break;
    }
}
static void handle_slots(nine_app_t *app, const nine_key_t *key, bool loading)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        if (app->return_page == GAL_PAGE_MENU) render_menu(app);
        else render_title(app);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        move_selection(&app->slot_sel, NINE_SAVE_SLOTS,
                       key->btn == BSP_BTN_UP ? -1 : 1);
        render_slots(app, loading);
        return;
    }
    if (key->ev != BSP_BTN_CLICK || key->btn != BSP_BTN_OK) return;
    if (loading) {
        load_slot(app, app->slot_sel);
        return;
    }
    if (app->slots[app->slot_sel].used) {
        app->modal = NINE_MODAL_OVERWRITE;
        app->modal_action = 0;
        app->modal_slot = app->slot_sel;
        static const char *const ACTIONS[2] = { "取消", "确认覆盖" };
        gal_ui_show_modal(&app->ui, GAL_MODAL_CONFIRM, "覆盖该存档?",
                          "原记录会被新进度替换。", ACTIONS, 2, 0);
        return;
    }
    save_slot(app, app->slot_sel);
}
static void handle_modal(nine_app_t *app, const nine_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        gal_ui_hide_modal(&app->ui);
        app->modal = NINE_MODAL_NONE;
        render_slots(app, false);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        move_selection(&app->modal_action, 2, key->btn == BSP_BTN_UP ? -1 : 1);
        {
            static const char *const ACTIONS[2] = { "取消", "确认覆盖" };
            gal_ui_show_modal(&app->ui, GAL_MODAL_CONFIRM, "覆盖该存档?",
                              "原记录会被新进度替换。", ACTIONS, 2, app->modal_action);
        }
        return;
    }
    if (key->ev != BSP_BTN_CLICK || key->btn != BSP_BTN_OK) return;
    const int slot = app->modal_slot;
    const int confirm = app->modal_action;
    gal_ui_hide_modal(&app->ui);
    app->modal = NINE_MODAL_NONE;
    if (confirm) save_slot(app, slot);
    else render_slots(app, false);
}
static void handle_settings(nine_app_t *app, const nine_key_t *key)
{
    const bool reader = app->return_page == GAL_PAGE_MENU;
    const int count = reader ? 7 : 6;
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        if (reader) render_menu(app);
        else render_title(app);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        move_selection(&app->settings_sel, count, key->btn == BSP_BTN_UP ? -1 : 1);
        render_settings(app);
        return;
    }
    if (key->ev != BSP_BTN_CLICK || key->btn != BSP_BTN_OK) return;
    switch (app->settings_sel) {
    case 0:
        app->settings.text_speed = (uint8_t)((app->settings.text_speed + 1) & 3);
        break;
    case 1:
        app->settings.font_large = app->settings.font_large ? 0 : 1;
        gal_ui_set_font_size(&app->ui, app->settings.font_large != 0);
        app->text_page = 0;
        app->restore_text_page = -1;
        app->last_frame = NINE_NONE;
        if (app->text[0]) { paginate(app); page_slice(app); }
        break;
    case 2:
        toggle_auto(app, !app->auto_mode);
        break;
    case 3:
        app->settings.panel_transparency = gal_transparency_next(app->settings.panel_transparency);
        gal_ui_set_panel_transparency(&app->ui, app->settings.panel_transparency);
        break;
    case 4:
        gal_settings_reset(&app->settings);
        gal_ui_set_font_size(&app->ui, app->settings.font_large != 0);
        gal_ui_set_panel_transparency(&app->ui, app->settings.panel_transparency);
        app->auto_mode = false;
        gal_ui_set_mode(&app->ui, false);
        app->text_page = 0;
        app->restore_text_page = -1;
        app->last_frame = NINE_NONE;
        if (app->text[0]) { paginate(app); page_slice(app); }
        break;
    case 5: {
        // Title settings row 5 = 返回; reader row 5 = 返回标题.
        app->auto_mode = false;
        app->fast_forward = false;
        app->title_sel = 0;
        app->last_frame = NINE_NONE;
        render_title(app);
        return;
    }
    default:
        if (reader) render_menu(app);
        else render_title(app);
        return;
    }
    (void)gal_settings_store(&app->settings);
    render_settings(app);
}
static void handle_history(nine_app_t *app, const nine_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        render_menu(app);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        move_selection(&app->history_sel, app->history.count,
                       key->btn == BSP_BTN_UP ? -1 : 1);
        render_history(app);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_OK) render_menu(app);
}
static void handle_gallery(nine_app_t *app, const nine_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        render_title(app);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        move_selection(&app->gallery_sel, GALLERY_MENU_COUNT,
                       key->btn == BSP_BTN_UP ? -1 : 1);
        render_gallery(app);
        return;
    }
    if (key->ev != BSP_BTN_CLICK || key->btn != BSP_BTN_OK) return;
    switch (app->gallery_sel) {
    case 0:
        app->cg_group = 0;
        app->cg_page = 0;
        app->cg_variant = 0;
        render_cg_groups(app);
        break;
    case 1:
        app->chapter_sel = 0;
        render_chapters(app);
        break;
    case 2:
        app->music_sel = 0;
        render_music(app);
        break;
    default:
        start_gallery_op(app);
        break;
    }
}
static void handle_cg_groups(nine_app_t *app, const nine_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        render_gallery(app);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        move_selection(&app->cg_group, (int)app->pack.gallery_groups,
                       key->btn == BSP_BTN_UP ? -1 : 1);
        render_cg_groups(app);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_OK) {
        app->cg_variant = 0;
        app->cg_page = 0;
        render_cg_view(app);
        return;
    }
}
static void handle_cg_view(nine_app_t *app, const nine_key_t *key)
{
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        app->cg_variant += key->btn == BSP_BTN_UP ? -1 : 1;
        render_cg_view(app);
        return;
    }
    if ((key->ev == BSP_BTN_CLICK || key->ev == BSP_BTN_LONG) && key->btn == BSP_BTN_OK) {
        render_cg_groups(app);
    }
}
static void handle_chapters(nine_app_t *app, const nine_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        render_gallery(app);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        move_selection(&app->chapter_sel, (int)app->pack.chapter_count,
                       key->btn == BSP_BTN_UP ? -1 : 1);
        render_chapters(app);
        return;
    }
    if (key->ev != BSP_BTN_CLICK || key->btn != BSP_BTN_OK) return;
    nine_chapter_t c;
    if (!nine_pack_chapter(&app->pack, (uint32_t)app->chapter_sel, &c)) {
        show_notice(app, "章节数据损坏");
        return;
    }
    if (!nine_vm_start(&app->vm, c.node, true)) {
        show_notice(app, "章节起点无效");
        return;
    }
    app->text_page = 0;
    app->restore_text_page = -1;
    app->history.count = 0;
    app->history.head = 0;
    app->last_frame = NINE_NONE;
    app->have_visual = false;
    app->view_error = NINE_VIEW_OK;
    app->fast_forward = false;
    app->auto_mode = false;
    app->has_session = true;
    app->dirty_auto = true;
    nine_media_stop();
    goto_game(app);
}
static void handle_music(nine_app_t *app, const nine_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        nine_media_stop();
        render_gallery(app);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        move_selection(&app->music_sel, (int)nine_media_track_count(),
                       key->btn == BSP_BTN_UP ? -1 : 1);
        render_music(app);
        return;
    }
    if (key->ev != BSP_BTN_CLICK || key->btn != BSP_BTN_OK) return;
    if (nine_media_current_track() == (uint32_t)app->music_sel && nine_media_is_playing())
        nine_media_stop();
    else if (!nine_media_play((uint32_t)app->music_sel))
        show_notice(app, "音乐播放失败");
    render_music(app);
}
static void handle_about(nine_app_t *app, const nine_key_t *key)
{
    if ((key->ev == BSP_BTN_CLICK || key->ev == BSP_BTN_LONG) &&
        (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        gal_ui_scroll(&app->ui, GAL_PAGE_ABOUT, key->btn == BSP_BTN_UP ? -20 : 20);
        return;
    }
    if ((key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) ||
        (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_OK)) {
        render_title(app);
    }
}
static void handle_ending(nine_app_t *app, const nine_key_t *key)
{
    if ((key->ev == BSP_BTN_CLICK || key->ev == BSP_BTN_LONG) &&
        (key->btn == BSP_BTN_OK || key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        app->title_sel = 0;
        render_title(app);
    }
}
static void handle_start(nine_app_t *app, const nine_key_t *key)
{
    if (key->ev != BSP_BTN_CLICK && key->ev != BSP_BTN_LONG) return;
    if (!app->pack_ok) {
        show_notice(app, "资源包校验失败;请重新刷写固件");
        return;
    }
    app->title_sel = 0;
    render_title(app);
}
// ---------------------------------------------------------------- 按键入口
bool nine_app_key(nine_app_t *app, const nine_key_t *key)
{
    if (!app || !key) return true;
    app->idle_ms = 0;
    nine_key_t as_click;
    if (key->ev == BSP_BTN_DOUBLE) {
        as_click = *key;
        as_click.ev = BSP_BTN_CLICK;
        key = &as_click;
    }
    // Long press of a key must not additionally fire a click on release.
    const int btn = key->btn < 3 ? key->btn : -1;
    if (btn >= 0 && key->ev == BSP_BTN_PRESS) {
        // A new press starts a new gesture; any stale long-press latch is void.
        app->suppress_click[btn] = false;
        return true;
    }
    if (btn >= 0 && key->ev == BSP_BTN_RELEASE) {
        // 快进只在按住上键时继续;松开立即停止。释放本身不是短按。
        if (btn == BSP_BTN_UP && app->fast_forward) {
            app->fast_forward = false;
        }
        return true;
    }
    if (btn >= 0 && key->ev == BSP_BTN_CLICK && app->suppress_click[btn]) {
        app->suppress_click[btn] = false;
        return true;
    }
    if (!bsp_lvgl_lock(200)) {
        // Retry the same already-dequeued key in the input task; never drop it.
        return false;
    }
    if (app->modal != NINE_MODAL_NONE) {
        handle_modal(app, key);
        bsp_lvgl_unlock();
        return true;
    }
    switch (app->page) {
    case GAL_PAGE_START: handle_start(app, key); break;
    case GAL_PAGE_TITLE: handle_title(app, key); break;
    case GAL_PAGE_GAME: handle_game(app, key); break;
    case GAL_PAGE_CHOICE: handle_choice(app, key); break;
    case GAL_PAGE_MENU: handle_menu(app, key); break;
    case GAL_PAGE_SAVE: handle_slots(app, key, false); break;
    case GAL_PAGE_LOAD: handle_slots(app, key, true); break;
    case GAL_PAGE_SETTINGS: handle_settings(app, key); break;
    case GAL_PAGE_HISTORY: handle_history(app, key); break;
    case GAL_PAGE_GALLERY: handle_gallery(app, key); break;
    case GAL_PAGE_RECALL: handle_cg_groups(app, key); break;
    case GAL_PAGE_MUSIC: handle_music(app, key); break;
    case GAL_PAGE_CHAPTER: handle_chapters(app, key); break;
    case GAL_PAGE_TRANSITION:
        if (app->gallery_op) handle_gallery_op(app, key);
        else handle_game(app, key);
        break;
    case GAL_PAGE_ENDING: handle_ending(app, key); break;
    case GAL_PAGE_ABOUT: handle_about(app, key); break;
    case GAL_PAGE_CG: handle_cg_view(app, key); break;
    default: break;
    }
    bsp_lvgl_unlock();
    return true;
}

void nine_app_input_recover(nine_app_t *app)
{
    if (!app) return;
    // A dropped queue event must not leave a held FF or a stale click latch.
    app->fast_forward = false;
    for (int i = 0; i < 3; ++i) app->suppress_click[i] = false;
}
// ---------------------------------------------------------------- tick
void nine_app_tick(nine_app_t *app, uint32_t elapsed_ms)
{
    if (!app) return;
    app->idle_ms += elapsed_ms;
    if (app->notice_ms > 0) {
        app->notice_ms = app->notice_ms > elapsed_ms ? app->notice_ms - elapsed_ms : 0;
        if (app->notice_ms == 0 && bsp_lvgl_lock(50)) {
            gal_ui_set_notice(&app->ui, NULL);
            bsp_lvgl_unlock();
        }
    }
    if (app->page == GAL_PAGE_TRANSITION && app->gallery_op) {
        // The transition/error render mutates LVGL. Keep it under the same
        // acquisition as media_tick; a failed acquire leaves state for next tick.
        if (bsp_lvgl_lock(100)) {
            nine_media_tick();
            const bool failed = nine_media_failed();
            const bool playing = nine_media_is_playing();
            if (failed) gallery_op_fail(app, "OP 播放失败");
            else if (!playing) gallery_op_finish(app, "natural");
            bsp_lvgl_unlock();
        }
        app->idle_ms = 0;
    } else if (app->page == GAL_PAGE_TRANSITION && app->opening) {
        if (bsp_lvgl_lock(100)) {
            nine_media_tick();
            const bool playing = nine_media_is_playing();
            bsp_lvgl_unlock();
            if (!playing) {
                finish_op(app, true);
                if (bsp_lvgl_lock(200)) { render_game(app); bsp_lvgl_unlock(); }
            }
        }
        app->idle_ms = 0;
    }
    // 快进:每个输入控制 tick 至多一整句;不累积额外的句间等待或补帧循环。
    if (app->fast_forward && app->page == GAL_PAGE_GAME && !app->opening) {
        if (bsp_lvgl_lock(100)) {
            advance_reading(app, true);
            bsp_lvgl_unlock();
        }
        app->idle_ms = 0;
    }
    if (app->auto_mode && app->page == GAL_PAGE_GAME && !app->opening) {
        if (bsp_lvgl_lock(100)) {
            const bool typing = gal_ui_typing(&app->ui);
            bsp_lvgl_unlock();
            if (typing) {
                app->auto_wait_ms = 0;
            } else {
                app->auto_wait_ms += elapsed_ms;
                if (app->auto_wait_ms >= NINE_AUTO_WAIT_MS) {
                    app->auto_wait_ms = 0;
                    if (bsp_lvgl_lock(200)) {
                        advance_reading(app, false);
                        bsp_lvgl_unlock();
                    }
                }
            }
        }
        app->idle_ms = 0;
    } else {
        app->auto_wait_ms = 0;
    }
    if (nine_media_is_playing()) app->idle_ms = 0;
    if (app->dirty_auto) {
        app->progress_wait_ms += elapsed_ms;
        if (app->progress_wait_ms >= NINE_PROGRESS_MS) {
            app->progress_wait_ms = 0;
            auto_write(app);
        }
    }
    app->battery_wait_ms += elapsed_ms;
    if (app->battery_wait_ms < NINE_BATTERY_MS) return;
    app->battery_wait_ms = 0;
    const int soc = bsp_battery_soc();
    if (soc != app->battery_percent) {
        app->battery_percent = soc;
        if (bsp_lvgl_lock(50)) {
            gal_ui_set_battery(&app->ui, soc);
            bsp_lvgl_unlock();
        }
    }
}
uint32_t nine_app_idle_ms(const nine_app_t *app)
{
    return app ? app->idle_ms : 0;
}
void nine_app_flush(nine_app_t *app)
{
    if (!app) return;
    perm_sync(app);
    if (app->dirty_auto) auto_write(app);
}
void nine_app_show_sleeping(nine_app_t *app)
{
    if (!app) return;
    if (!bsp_lvgl_lock(200)) return;
    gal_ui_set_notice(&app->ui, "已休眠");
#ifndef NINE_APP_HOST
    lv_refr_now(NULL);
#endif
    bsp_lvgl_unlock();
}
// ---------------------------------------------------------------- 初始化
bool nine_app_init(nine_app_t *app)
{
    if (!app) return false;
    memset(app, 0, sizeof(*app));
    app->battery_percent = -1;
    app->restore_text_page = -1;
    app->bg_sel = 0;
    app->choice_sel = 0;
    gal_settings_default(&app->settings);
    if (!gal_settings_init()) ESP_LOGW(TAG, "设置存储不可用,使用默认值");
    else (void)gal_settings_load(&app->settings);
    app->nvs_ok = nvs_open(NINE_NS, NVS_READWRITE, &app->nvs) == ESP_OK;
    if (!app->nvs_ok) ESP_LOGW(TAG, "标题 NVS 不可用;本次运行不保存进度");
    size_t script_size = 0, image_size = 0;
    const uint8_t *script = script_data(&script_size);
    const uint8_t *images = images_data(&image_size);
    app->pack_ok = nine_pack_open(&app->pack, script, script_size);
    if (!app->pack_ok) ESP_LOGE(TAG, "剧情包校验失败 (%u 字节)", (unsigned)script_size);
    else ESP_LOGI(TAG, "%s:%u 节点 / %u 帧 / %u 变量 / %u 图集 / %u 组 CG",
                  nine_pack_title(&app->pack), (unsigned)app->pack.node_count,
                  (unsigned)app->pack.frame_count, (unsigned)app->pack.key_count,
                  (unsigned)app->pack.visual_count, (unsigned)app->pack.gallery_groups);
    nine_vm_init(&app->vm, &app->pack);
    if (app->pack_ok) {
        uint8_t buf[NINE_PERM_BYTES];
        size_t len = sizeof(buf);
        if (nvs_read(app, NINE_KEY_PERM, buf, &len)) {
            if (!nine_vm_progress_decode(&app->vm, buf, len)) {
                ESP_LOGW(TAG, "永久进度记录校验失败;按空记录继续");
            } else {
                memcpy(app->perm_buffer, buf, len);
                app->perm_len = len;
            }
        }
        len = sizeof(buf);
        if (nvs_read(app, NINE_KEY_AUTO, buf, &len)) {
            uint16_t node = 0, page = 0;
            uint32_t index = 0;
            bool preview = false;
            app->has_session = nine_vm_save_peek(&app->pack, buf, len, &node, &index, &page,
                                                 &preview);
            if (!app->has_session) ESP_LOGW(TAG, "自动进度校验失败;忽略该记录");
        }
        slot_scan(app);
    }
    app->images_ok = app->pack_ok &&
                     nine_image_bind_bundle(script, script_size, images, image_size);
    if (!app->images_ok) ESP_LOGE(TAG, "图像包校验失败;正文可读但画面不可用");
    app->media_ok = app->images_ok &&
                    nine_media_init(script, script_size, images, image_size);
    if (app->pack_ok && !app->media_ok) ESP_LOGW(TAG, "音频包不可用;静音运行");
    if (app->nvs_ok) {
        uint8_t bg = 0;
        size_t len = sizeof(bg);
        if (nvs_get_blob(app->nvs, NINE_KEY_BG, &bg, &len) == ESP_OK && len == 1) {
            app->bg_sel = bg;
        }
    }
    if (!bsp_lvgl_lock(-1)) {
        ESP_LOGE(TAG, "LVGL 未就绪");
        return false;
    }
#ifdef NINE_APP_HOST
#if defined(NINE_REALGL)
    if (!nine_image_init(lv_screen_active()) && app->images_ok) {
        app->images_ok = false;
    }
#else
    if (!nine_image_init(NULL) && app->images_ok) {
        app->images_ok = false;
    }
#endif
#else
    if (!nine_image_init(lv_screen_active()) && app->images_ok) {
        app->images_ok = false;
        ESP_LOGE(TAG, "图像条带分配失败;正文可读,画面关闭");
    }
#endif
    const bool ui_ok = gal_ui_create(&app->ui, &nine_cjk_16, &nine_cjk_20);
    if (ui_ok) {
        // The reader owns the whole screen with decoded bands; the skeleton's
        // painted gradient would otherwise cover them.
        lv_obj_add_flag(app->ui.bg, LV_OBJ_FLAG_HIDDEN);
        gal_ui_set_brand(&app->ui, "", "");
        gal_ui_set_battery(&app->ui, app->battery_percent);
        gal_ui_set_font_size(&app->ui, app->settings.font_large != 0);
        gal_ui_set_panel_transparency(&app->ui, app->settings.panel_transparency);
        app->auto_mode = false;
        app->page = GAL_PAGE_START;
        static const char *const START_ROW[] = { "开始游戏" };
        gal_ui_set_rows(&app->ui, GAL_LIST_START, START_ROW, NULL, 1, 0);
        nine_image_clear(0x0000);
        gal_ui_show_page(&app->ui, GAL_PAGE_START);
        if (!app->pack_ok) gal_ui_set_notice(&app->ui, "资源包校验失败");
        else if (!app->images_ok) gal_ui_set_notice(&app->ui, "图像资源不可用");
    }
    bsp_lvgl_unlock();
    return ui_ok;
}
