// main/gal/gal_app.c —— 框架骨架的应用状态机实现。
#include "gal_app.h"

#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "gal_content.h"
#include "gal_text.h"

#include "esp_log.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "gal";

// 快进:按住“上”期间每 180ms 推进一步。自动阅读:每句打完后再等 900ms。
#define GAL_FAST_FORWARD_MS 180u
#define GAL_AUTO_ADVANCE_MS 900u

LV_FONT_DECLARE(gal_cjk_16);
LV_FONT_DECLARE(gal_cjk_20);

static uint32_t speed_ms(const gal_app_t *app)
{
    static const uint32_t SPEED_MS[4] = { 60, 40, 18, 0 };   // 0 = 瞬间显示整页
    const uint8_t idx = app->settings.text_speed > 3 ? 1 : app->settings.text_speed;
    return SPEED_MS[idx];
}

static const char *speed_label(const gal_app_t *app)
{
    static const char *const LABEL[4] = { "慢", "中", "快", "瞬间" };
    const uint8_t idx = app->settings.text_speed > 3 ? 1 : app->settings.text_speed;
    return LABEL[idx];
}

// 竖屏排版(框架规范):16px 档 13 字/行 × 5 行,20px 档 10 字/行 × 4 行。
static void layout_for(const gal_app_t *app, int *units_per_line, int *lines_per_page)
{
    if (app->settings.font_large) {
        *units_per_line = 20;
        *lines_per_page = GAL_TEXT_LINES_LARGE;
    } else {
        *units_per_line = 26;
        *lines_per_page = GAL_TEXT_LINES_SMALL;
    }
}

static void show_notice(gal_app_t *app, const char *text)
{
    snprintf(app->notice, sizeof(app->notice), "%s", text ? text : "");
    app->notice_ms = 2000;
    gal_ui_set_notice(&app->ui, app->notice);
}

static void goto_page(gal_app_t *app, gal_page_t page);

static void render_current_page(gal_app_t *app);

// 显示当前句的当前页(自动分页,长句不会被裁掉)。
static void show_line(gal_app_t *app)
{
    if (app->line_index < 0 || app->line_index >= gal_content_line_count) return;
    const gal_line_t *line = &gal_content_lines[app->line_index];

    int units = 26;
    int lines = 5;
    layout_for(app, &units, &lines);

    uint32_t offsets[GAL_TEXT_MAX_PAGES + 1];
    int pages = gal_text_pages(line->text, units, lines, offsets, GAL_TEXT_MAX_PAGES + 1);
    if (pages < 1) pages = 1;
    if (pages > GAL_TEXT_MAX_PAGES) pages = GAL_TEXT_MAX_PAGES;
    if (app->page_index >= pages) app->page_index = pages - 1;
    app->page_count = pages;

    const uint32_t begin = offsets[app->page_index];
    uint32_t end = offsets[app->page_index + 1];
    if (end < begin) end = begin;
    size_t len = end - begin;
    if (len >= GAL_TEXT_CAPACITY) len = GAL_TEXT_CAPACITY - 1;
    char page_text[GAL_TEXT_CAPACITY];
    memcpy(page_text, line->text + begin, len);
    page_text[len] = '\0';

    gal_ui_set_text(&app->ui, line->speaker, page_text, speed_ms(app));
}

static void advance_reading(gal_app_t *app, bool fast)
{
    // 打字中:先补全整页(快进模式跳过动画直接推进)。
    if (!fast && gal_ui_typing(&app->ui)) {
        gal_ui_show_full_text(&app->ui);
        return;
    }
    if (app->page_index + 1 < app->page_count) {
        app->page_index++;
        show_line(app);
        return;
    }
    if (app->line_index + 1 < gal_content_line_count) {
        app->line_index++;
        app->page_index = 0;
        show_line(app);
        return;
    }
    // 内容读完:回到标题页。
    app->fast_forward = false;
    app->auto_mode = false;
    app->settings.auto_play = 0;
    gal_settings_store(&app->settings);
    goto_page(app, GAL_PAGE_TITLE);
    show_notice(app, "内容结束");
}

static void render_current_page(gal_app_t *app)
{
    switch (app->page) {
    case GAL_PAGE_START: {
        static const char *rows[1] = { "开始游戏" };
        gal_ui_set_rows(&app->ui, GAL_LIST_START, rows, NULL, 1, 0);
        gal_ui_show_page(&app->ui, GAL_PAGE_START);
        return;
    }
    case GAL_PAGE_TITLE: {
        static const char *rows[3] = { "开始阅读", "设置", "关于" };
        gal_ui_set_rows(&app->ui, GAL_LIST_TITLE, rows, NULL, 3, app->title_sel);
        gal_ui_show_page(&app->ui, GAL_PAGE_TITLE);
        return;
    }
    case GAL_PAGE_SETTINGS: {
        // 阅读中打开:设置项 + 操作项(返回标题);标题页打开:只有设置项。
        static const char *labels_reader[7] = { "文字速度", "文字大小", "自动阅读", "对话透明度",
                                                "返回标题", "恢复默认", "返回" };
        static const char *labels_title[6] = { "文字速度", "文字大小", "自动阅读", "对话透明度",
                                               "恢复默认", "返回" };
        static char transparency_buf[8];
        snprintf(transparency_buf, sizeof(transparency_buf), "%u%%",
                 (unsigned)app->settings.panel_transparency);
        const char *values_reader[7] = {
            speed_label(app), app->settings.font_large ? "大" : "小",
            app->settings.auto_play ? "开" : "关", transparency_buf, "", "", "",
        };
        const char *values_title[6] = {
            speed_label(app), app->settings.font_large ? "大" : "小",
            app->settings.auto_play ? "开" : "关", transparency_buf, "", "",
        };
        gal_ui_show_page(&app->ui, GAL_PAGE_SETTINGS);
        if (app->settings_from_reader) {
            gal_ui_set_rows(&app->ui, GAL_LIST_SETTINGS, labels_reader, values_reader, 7,
                            app->settings_sel);
        } else {
            gal_ui_set_rows(&app->ui, GAL_LIST_SETTINGS, labels_title, values_title, 6,
                            app->settings_sel);
        }
        return;
    }
    case GAL_PAGE_ABOUT: {
        gal_ui_set_about_text(&app->ui, GAL_CONTENT_ABOUT);
        static const char *rows[1] = { "返回" };
        gal_ui_set_rows(&app->ui, GAL_LIST_ABOUT, rows, NULL, 1, 0);
        gal_ui_show_page(&app->ui, GAL_PAGE_ABOUT);
        return;
    }
    case GAL_PAGE_GAME:
    default:
        gal_ui_set_chapter(&app->ui, GAL_CONTENT_TITLE);
        gal_ui_show_page(&app->ui, GAL_PAGE_GAME);
        show_line(app);
        return;
    }
}

static void goto_page(gal_app_t *app, gal_page_t page)
{
    app->page = page;
    app->idle_ms = 0;
    app->fast_forward = false;
    app->auto_mode = false;
    render_current_page(app);
}

// ---------------------------------------------------------------- 事件
static void move_selection(int *sel, int count, int delta)
{
    if (count <= 0) return;
    int next = *sel + delta;
    if (next < 0) next = 0;          // 列表不绕圈:到顶/到底停住
    if (next >= count) next = count - 1;
    *sel = next;
}

static void handle_start(gal_app_t *app, const gal_key_t *key)
{
    if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_OK) {
        app->title_sel = 0;
        goto_page(app, GAL_PAGE_TITLE);
    }
}

static void handle_title(gal_app_t *app, const gal_key_t *key)
{
    const int count = 3;
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        move_selection(&app->title_sel, count, key->btn == BSP_BTN_UP ? -1 : 1);
        gal_ui_select_row(&app->ui, GAL_LIST_TITLE, app->title_sel);
        return;
    }
    if (key->ev != BSP_BTN_CLICK || key->btn != BSP_BTN_OK) return;

    switch (app->title_sel) {
    case 0:
        app->line_index = 0;
        app->page_index = 0;
        goto_page(app, GAL_PAGE_GAME);
        break;
    case 1:
        app->settings_from_reader = false;
        app->settings_return = GAL_PAGE_TITLE;
        app->settings_sel = 0;
        goto_page(app, GAL_PAGE_SETTINGS);
        break;
    default:
        goto_page(app, GAL_PAGE_ABOUT);
        break;
    }
}

static void handle_game(gal_app_t *app, const gal_key_t *key)
{
    // 短按(确定/上/下任意一个):下一句(打字中先全显)。
    if (key->ev == BSP_BTN_CLICK &&
        (key->btn == BSP_BTN_OK || key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        app->fast_forward = false;
        advance_reading(app, false);
        return;
    }
    // 长按确定:设置面板。
    if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_LONG) {
        app->fast_forward = false;
        app->settings_from_reader = true;
        app->settings_return = GAL_PAGE_GAME;
        app->settings_sel = 0;
        goto_page(app, GAL_PAGE_SETTINGS);
        return;
    }
    // 长按上:快进,松开停。
    if (key->btn == BSP_BTN_UP) {
        if (key->ev == BSP_BTN_LONG) {
            app->fast_forward = true;
            app->ff_accum_ms = 0;
            gal_ui_show_full_text(&app->ui);
            show_notice(app, "快进");
            return;
        }
        if (key->ev == BSP_BTN_RELEASE) {
            app->fast_forward = false;
            show_notice(app, NULL);
            return;
        }
    }
    // 长按下:自动阅读开关(任意键停止)。
    if (key->btn == BSP_BTN_DOWN && key->ev == BSP_BTN_LONG) {
        app->settings.auto_play = app->settings.auto_play ? 0 : 1;
        gal_settings_store(&app->settings);
        app->auto_mode = app->settings.auto_play != 0;
        app->auto_accum_ms = 0;
        app->fast_forward = false;
        if (app->auto_mode) gal_ui_show_full_text(&app->ui);
        show_notice(app, app->auto_mode ? "自动阅读 开" : "自动阅读 关");
        return;
    }
}

enum {
    SACT_SPEED = 0,
    SACT_SIZE,
    SACT_AUTO,
    SACT_TRANSPARENCY,
    SACT_TITLE,
    SACT_RESET,
    SACT_BACK,
};

static int settings_action(const gal_app_t *app, int index)
{
    if (app->settings_from_reader) return index;   // 阅读中:下标即动作
    static const int title_map[6] = { SACT_SPEED, SACT_SIZE, SACT_AUTO,
                                      SACT_TRANSPARENCY, SACT_RESET, SACT_BACK };
    return (index >= 0 && index < 6) ? title_map[index] : SACT_BACK;
}

static void handle_settings(gal_app_t *app, const gal_key_t *key)
{
    const int count = app->settings_from_reader ? 7 : 6;
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        app->settings_from_reader = false;
        goto_page(app, app->settings_return);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        move_selection(&app->settings_sel, count, key->btn == BSP_BTN_UP ? -1 : 1);
        gal_ui_select_row(&app->ui, GAL_LIST_SETTINGS, app->settings_sel);
        return;
    }
    if (key->ev != BSP_BTN_CLICK || key->btn != BSP_BTN_OK) return;

    switch (settings_action(app, app->settings_sel)) {
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
    case SACT_TITLE:
        app->settings_from_reader = false;
        goto_page(app, GAL_PAGE_TITLE);
        return;
    case SACT_RESET:
        gal_settings_reset(&app->settings);
        gal_ui_set_font_size(&app->ui, app->settings.font_large != 0);
        gal_ui_set_panel_transparency(&app->ui, app->settings.panel_transparency);
        break;
    default:   // SACT_BACK
        app->settings_from_reader = false;
        goto_page(app, app->settings_return);
        return;
    }
    gal_settings_store(&app->settings);
    render_current_page(app);   // 列表右侧的值就地刷新
}

static void handle_about(gal_app_t *app, const gal_key_t *key)
{
    if ((key->ev == BSP_BTN_CLICK || key->ev == BSP_BTN_LONG) &&
        (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        const int dir = key->btn == BSP_BTN_UP ? -1 : 1;
        gal_ui_about_scroll(&app->ui, dir * (key->ev == BSP_BTN_LONG ? 80 : 20));
        return;
    }
    if ((key->ev == BSP_BTN_CLICK || key->ev == BSP_BTN_LONG) && key->btn == BSP_BTN_OK) {
        goto_page(app, GAL_PAGE_TITLE);
    }
}

void gal_app_key(gal_app_t *app, const gal_key_t *key)
{
    if (!app || !key) return;
    app->idle_ms = 0;

    // iot_button 会把窗口内的两次快速点击合并成 DOUBLE,且不补发单击:
    // 阅读时连按很常见,把双击当一次单击处理。
    gal_key_t as_click;
    if (key->ev == BSP_BTN_DOUBLE) {
        as_click = *key;
        as_click.ev = BSP_BTN_CLICK;
        key = &as_click;
    }

    if (!bsp_lvgl_lock(200)) {
        ESP_LOGW(TAG, "拿不到 LVGL 锁,忽略本次按键");
        return;
    }

    // 自动阅读中按任意键:退出自动阅读,并吃掉这次按键。
    // PRESS/RELEASE 不算“按键”:否则下键长按进入自动后,松手就会把它关掉。
    if (app->auto_mode && key->ev != BSP_BTN_PRESS && key->ev != BSP_BTN_RELEASE) {
        app->auto_mode = false;
        app->auto_accum_ms = 0;
        app->settings.auto_play = 0;
        gal_settings_store(&app->settings);
        show_notice(app, "已退出自动阅读");
        bsp_lvgl_unlock();
        return;
    }

    switch (app->page) {
    case GAL_PAGE_START: handle_start(app, key); break;
    case GAL_PAGE_TITLE: handle_title(app, key); break;
    case GAL_PAGE_GAME: handle_game(app, key); break;
    case GAL_PAGE_SETTINGS: handle_settings(app, key); break;
    case GAL_PAGE_ABOUT: handle_about(app, key); break;
    default: break;
    }
    bsp_lvgl_unlock();
}

void gal_app_tick(gal_app_t *app, uint32_t elapsed_ms)
{
    if (!app) return;
    app->idle_ms += elapsed_ms;

    if (app->fast_forward && app->page == GAL_PAGE_GAME) {
        app->ff_accum_ms += elapsed_ms;
        if (app->ff_accum_ms >= GAL_FAST_FORWARD_MS) {
            app->ff_accum_ms = 0;
            if (bsp_lvgl_lock(100)) {
                advance_reading(app, true);
                bsp_lvgl_unlock();
            }
        }
    }

    // 自动阅读:当前页打完之后才开始计时,每 900ms 推进一步。
    if (app->auto_mode && app->page == GAL_PAGE_GAME) {
        if (gal_ui_typing(&app->ui)) {
            app->auto_accum_ms = 0;
        } else {
            app->auto_accum_ms += elapsed_ms;
            if (app->auto_accum_ms >= GAL_AUTO_ADVANCE_MS) {
                app->auto_accum_ms = 0;
                if (bsp_lvgl_lock(100)) {
                    advance_reading(app, false);
                    bsp_lvgl_unlock();
                }
            }
        }
        // 自动阅读期间不算空闲:不调暗、不息屏、不进 deep sleep。
        app->idle_ms = 0;
    } else {
        app->auto_accum_ms = 0;
    }

    if (app->notice_ms > 0) {
        app->notice_ms = app->notice_ms > elapsed_ms ? app->notice_ms - elapsed_ms : 0;
        if (app->notice_ms == 0 && bsp_lvgl_lock(50)) {
            gal_ui_set_notice(&app->ui, NULL);
            bsp_lvgl_unlock();
        }
    }

    app->battery_accum_ms += elapsed_ms;
    if (app->battery_accum_ms < 10000) return;
    app->battery_accum_ms = 0;
    const int soc = bsp_battery_soc();
    if (soc == app->battery_percent) return;
    app->battery_percent = soc;
    if (!bsp_lvgl_lock(50)) return;
    gal_ui_set_battery(&app->ui, soc);
    bsp_lvgl_unlock();
}

uint32_t gal_app_idle_ms(const gal_app_t *app)
{
    return app ? app->idle_ms : 0;
}

void gal_app_show_sleeping(gal_app_t *app)
{
    if (!app) return;
    gal_ui_set_notice(&app->ui, "已休眠");
}

bool gal_app_init(gal_app_t *app)
{
    if (!app) return false;
    memset(app, 0, sizeof(*app));
    app->battery_percent = -1;

    gal_settings_default(&app->settings);
    if (!gal_settings_init()) {
        ESP_LOGW(TAG, "设置存储不可用,本次使用默认值");
    } else {
        (void)gal_settings_load(&app->settings);
    }

    if (!bsp_lvgl_lock(-1)) {
        ESP_LOGE(TAG, "LVGL 未就绪");
        return false;
    }
    const bool ui_ok = gal_ui_create(&app->ui, &gal_cjk_16, &gal_cjk_20);
    if (ui_ok) {
        gal_ui_set_brand(&app->ui, GAL_CONTENT_TITLE, GAL_CONTENT_SUBTITLE);
        gal_ui_set_battery(&app->ui, app->battery_percent);
        gal_ui_set_font_size(&app->ui, app->settings.font_large != 0);
        gal_ui_set_panel_transparency(&app->ui, app->settings.panel_transparency);
        gal_ui_set_about_text(&app->ui, GAL_CONTENT_ABOUT);
        app->auto_mode = app->settings.auto_play != 0;
        goto_page(app, GAL_PAGE_START);
    }
    bsp_lvgl_unlock();
    return ui_ok;
}
