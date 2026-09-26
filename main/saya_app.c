// main/saya_app.c —— 应用状态机实现。
#include "saya_app.h"

#include "bsp_battery.h"
#include "bsp_display.h"
#include "esp_log.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "saya";

LV_FONT_DECLARE(saya_cjk_16);
LV_FONT_DECLARE(saya_cjk_20);

// 打字机速度:慢 / 中 / 快(毫秒/字)
static const uint32_t SPEED_MS[3] = { 60, 40, 18 };
static const char *const SPEED_LABEL[3] = { "慢", "中", "快" };

static const char *const ABOUT_TEXT =
    "《沙耶之歌》视觉小说移植\n"
    "原移植:liuyuze61(小米手环 10 版)\n"
    "本移植:AI Passport 横屏版\n"
    "\n"
    "操作:\n"
    "  确定     推进对白 / 打字中立即显示\n"
    "  长按确定 打开菜单(存档、跳场景)\n"
    "  上 / 下  翻页,选项里选择\n"
    "\n"
    "本作含有大量血腥描写,请谨慎阅读。\n"
    "有能力请支持正版。";

static const char *warning_text(void)
{
    return "含有大量血腥内容!心理承受能力差者慎入!\n\n"
           "《沙耶之歌》(沙耶の唄)是 Nitroplus 的商业作品。\n"
           "本固件是个人移植的学习产物,素材来自公开的"
           "小米手环移植项目,仅供个人设备使用。\n\n"
           "下载、安装或使用本软件,即表示你已知晓并接受上述说明;"
           "若不同意请立即停止使用并删除本固件。\n\n"
           "有能力请支持正版。";
}

// ---------------------------------------------------------------- 小工具
static uint32_t speed_ms(const saya_app_t *app)
{
    const uint8_t idx = app->settings.text_speed > 2 ? 1 : app->settings.text_speed;
    return SPEED_MS[idx];
}

static const char *speed_label(const saya_app_t *app)
{
    const uint8_t idx = app->settings.text_speed > 2 ? 1 : app->settings.text_speed;
    return SPEED_LABEL[idx];
}

static void layout_for(const saya_app_t *app, saya_layout_t *layout)
{
    if (app->settings.font_large) {
        layout->units_per_line = 30;   // 20px:一行 15 个全角字
        layout->lines_per_page = 3;
    } else {
        layout->units_per_line = 38;   // 16px:一行 19 个全角字
        layout->lines_per_page = 4;
    }
}

static void chapter_label(const saya_app_t *app, char *out, size_t capacity)
{
    saya_chapter_t ch;
    saya_pack_chapter(&app->pack, app->player.chapter, &ch);
    if (app->player.at_choice || ch.id == 0) {
        snprintf(out, capacity, "第 %u 章", (unsigned)ch.id);
    } else {
        snprintf(out, capacity, "第 %u 章  %u/%u", (unsigned)ch.id,
                 (unsigned)(app->player.scene + 1), (unsigned)ch.scene_count);
    }
}

// 顶部提示条:2 秒后自动恢复成章节进度。
static void show_notice(saya_app_t *app, const char *text)
{
    snprintf(app->notice, sizeof(app->notice), "%s", text ? text : "");
    app->notice_ms = 2000;
    saya_ui_set_chapter(&app->ui, app->notice);
}

static void autosave(saya_app_t *app)
{
    if (!app->player.ended && !app->player.at_choice) {
        saya_save_t save;
        saya_save_from_player(&app->player, &save);
        if (!saya_auto_store(&save)) ESP_LOGW(TAG, "自动存档写入失败");
    }
}

// ---------------------------------------------------------------- 渲染
// 画面区只在(背景, 立绘)真的变化时重解码:同一场景的连续对白不该反复解码 JPEG。
static void render_scene(saya_app_t *app)
{
    saya_layout_t layout;
    layout_for(app, &layout);

    saya_chapter_t ch;
    saya_pack_chapter(&app->pack, app->player.chapter, &ch);
    saya_scene_t sc;
    saya_pack_scene(&app->pack, (uint16_t)(ch.first_scene + app->player.scene), &sc);

    const uint16_t bg = sc.bg;
    const uint16_t fg = app->player.fg;
    if (bg != app->rendered_bg || fg != app->rendered_fg) {
        if (saya_ui_set_scene(&app->ui, &app->pack, bg, fg)) {
            app->rendered_bg = bg;
            app->rendered_fg = fg;
        } else {
            // 解码失败不记账,下次推进时会再试一遍。
            ESP_LOGW(TAG, "画面刷新失败(背景 %u 立绘 %u)", (unsigned)bg, (unsigned)fg);
        }
    }

    char label[32];
    chapter_label(app, label, sizeof(label));
    saya_ui_set_chapter(&app->ui, label);

    if (app->player.at_choice) {
        char first[128];
        char second[128];
        saya_pack_name(&app->pack, sc.choice_name[0], first, sizeof(first));
        saya_pack_name(&app->pack, sc.choice_name[1], second, sizeof(second));
        saya_ui_show_page(&app->ui, SAYA_PAGE_GAME);
        saya_ui_show_choices(&app->ui, first, second);
        saya_ui_set_choice_selected(&app->ui, 0);
        return;
    }

    saya_ui_hide_choices(&app->ui);
    saya_ui_show_page(&app->ui, SAYA_PAGE_GAME);

    char speaker[64];
    char text[SAYA_TEXT_BUFFER];
    saya_player_speaker(&app->player, &app->pack, speaker, sizeof(speaker));
    saya_player_page_text(&app->player, &app->pack, &layout, text, sizeof(text));
    saya_ui_set_text(&app->ui, speaker, text, speed_ms(app));
}

static void render_scene(saya_app_t *app);
static void render_slots(saya_app_t *app);
static void goto_page(saya_app_t *app, saya_page_t page);

static void render_current_page(saya_app_t *app)
{
    switch (app->page) {
    case SAYA_PAGE_TITLE: {
        // 标题菜单最多 4 行(画面上方是标题图,菜单只有 104px 可用);"关于"在设置页里。
        static const char *rows_continue[4] = { "继续", "从第 1 章开始", "读取存档", "设置" };
        static const char *rows_default[3] = { "从第 1 章开始", "读取存档", "设置" };
        const char *const *rows = app->have_auto ? rows_continue : rows_default;
        const int count = app->have_auto ? 4 : 3;
        saya_ui_set_title_art(&app->ui, &app->pack);
        saya_ui_show_page(&app->ui, SAYA_PAGE_TITLE);
        saya_ui_set_title_rows(&app->ui, rows, count, app->title_sel);
        return;
    }
    case SAYA_PAGE_MENU: {
        static const char *rows[5] = { "保存", "读取", "跳过场景", "返回标题", "关闭菜单" };
        saya_ui_show_page(&app->ui, SAYA_PAGE_MENU);
        saya_ui_set_menu_rows(&app->ui, rows, 5, app->menu_sel);
        return;
    }
    case SAYA_PAGE_SETTINGS: {
        static const char *labels[4] = { "文字速度", "字号", "关于", "返回" };
        const char *values[4] = { speed_label(app), app->settings.font_large ? "大" : "小",
                                  "", "" };
        saya_ui_show_page(&app->ui, SAYA_PAGE_SETTINGS);
        saya_ui_set_settings_rows(&app->ui, labels, values, 4, app->settings_sel);
        return;
    }
    case SAYA_PAGE_ABOUT: {
        static const char *rows[1] = { "返回" };
        saya_ui_show_page(&app->ui, SAYA_PAGE_ABOUT);
        saya_ui_set_about_text(&app->ui, ABOUT_TEXT);
        saya_ui_set_about_rows(&app->ui, rows, 1, 0);
        return;
    }
    case SAYA_PAGE_SLOTS: {
        render_slots(app);
        return;
    }
    case SAYA_PAGE_ENDING: {
        char name[64];
        saya_pack_name(&app->pack, app->player.end_name, name, sizeof(name));
        if (name[0] == '\0') snprintf(name, sizeof(name), "%s", "End");
        saya_ui_show_page(&app->ui, SAYA_PAGE_ENDING);
        saya_ui_set_ending(&app->ui, name, "确定返回标题");
        return;
    }
    case SAYA_PAGE_WARNING: {
        saya_ui_show_page(&app->ui, SAYA_PAGE_WARNING);
        saya_ui_set_warning(&app->ui, warning_text(), "按确定继续阅读");
        return;
    }
    case SAYA_PAGE_GAME:
    default:
        render_scene(app);
        return;
    }
}

static void render_slots(saya_app_t *app)
{
    // 这两个数组是【指针数组】,分别指向下面的字符缓冲 —— 不能把二维字符数组
    // 强转成指针数组传给列表:那样被调方会把字符串内容当地址读,直接野指针崩溃。
    static const char *labels[SAYA_UI_MAX_ROWS];
    static const char *values[SAYA_UI_MAX_ROWS];
    static char label_buf[SAYA_UI_MAX_ROWS][16];
    static char value_buf[SAYA_UI_MAX_ROWS][40];

    const int slot_rows = SAYA_SAVE_SLOTS;
    for (int i = 0; i < slot_rows; ++i) {
        snprintf(label_buf[i], sizeof(label_buf[i]), "存档 %d", i + 1);
        labels[i] = label_buf[i];
        saya_save_t save;
        if (saya_slot_load((uint8_t)i, &save) && save.chapter < app->pack.chapter_count) {
            saya_chapter_t ch;
            saya_pack_chapter(&app->pack, save.chapter, &ch);
            snprintf(value_buf[i], sizeof(value_buf[i]), "第 %u 章 · %u/%u", (unsigned)ch.id,
                     (unsigned)(save.scene + 1), (unsigned)ch.scene_count);
        } else {
            snprintf(value_buf[i], sizeof(value_buf[i]), "空");
        }
        values[i] = value_buf[i];
    }
    labels[slot_rows] = "返回";
    values[slot_rows] = "";

    saya_ui_show_page(&app->ui, SAYA_PAGE_SLOTS);
    saya_ui_slots_setup(&app->ui, app->slots_saving ? "保存进度" : "读取进度",
                        app->slots_saving ? "确定保存 · 长按删除该槽 · 选“返回”离开"
                                          : "确定读取 · 选“返回”离开",
                        labels, values, slot_rows + 1);
    saya_ui_set_slots_selected(&app->ui, app->slots_sel);
}

static void goto_page(saya_app_t *app, saya_page_t page)
{
    app->page = page;
    app->idle_ms = 0;
    // 换页可能让画面区画布被别的画面占用(标题图),下次进正文时按需重画。
    app->rendered_bg = SAYA_NONE;
    app->rendered_fg = SAYA_NONE;
    render_current_page(app);
}

// 从存档恢复阅读位置;失败时从第一章开始。
static void begin_reading(saya_app_t *app, const saya_save_t *save)
{
    saya_layout_t layout;
    layout_for(app, &layout);
    saya_player_reset(&app->player);

    bool loaded = false;
    if (save) {
        loaded = saya_player_load(&app->player, &app->pack, save, &layout);
        if (!loaded) ESP_LOGW(TAG, "存档已失效,从头开始");
    }
    if (!loaded && !saya_player_start(&app->player, &app->pack, 0, &layout)) {
        ESP_LOGE(TAG, "第一章装载失败,资源包可能已损坏");
        return;
    }

    app->started = true;
    goto_page(app, SAYA_PAGE_GAME);
    autosave(app);
}

// ---------------------------------------------------------------- 事件
static void handle_title(saya_app_t *app, const saya_key_t *key)
{
    static const char *rows_continue[4] = { "继续", "从第 1 章开始", "读取存档", "设置" };
    static const char *rows_default[3] = { "从第 1 章开始", "读取存档", "设置" };
    const char *const *rows = app->have_auto ? rows_continue : rows_default;
    const int count = app->have_auto ? 4 : 3;

    if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_UP) {
        app->title_sel = app->title_sel > 0 ? app->title_sel - 1 : count - 1;
        saya_ui_set_title_rows(&app->ui, rows, count, app->title_sel);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_DOWN) {
        app->title_sel = app->title_sel + 1 < count ? app->title_sel + 1 : 0;
        saya_ui_set_title_rows(&app->ui, rows, count, app->title_sel);
        return;
    }
    if (key->ev != BSP_BTN_CLICK || key->btn != BSP_BTN_OK) return;

    const char *choice = rows[app->title_sel];
    if (strcmp(choice, "继续") == 0) {
        saya_save_t save;
        if (saya_auto_load(&save)) begin_reading(app, &save);
        return;
    }
    if (strcmp(choice, "从第 1 章开始") == 0) {
        begin_reading(app, NULL);
        return;
    }
    if (strcmp(choice, "读取存档") == 0) {
        app->slots_saving = false;
        app->slots_sel = 0;
        app->slots_return = SAYA_PAGE_TITLE;
        goto_page(app, SAYA_PAGE_SLOTS);
        return;
    }
    if (strcmp(choice, "设置") == 0) {
        app->settings_return = SAYA_PAGE_TITLE;
        app->settings_sel = 0;
        goto_page(app, SAYA_PAGE_SETTINGS);
    }
}

static void handle_game(saya_app_t *app, const saya_key_t *key)
{
    saya_layout_t layout;
    layout_for(app, &layout);

    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        app->menu_sel = 0;
        goto_page(app, SAYA_PAGE_MENU);
        return;
    }

    if (app->player.at_choice) {
        const int count = 2;
        if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
            const int delta = key->btn == BSP_BTN_UP ? -1 : 1;
            int sel = saya_ui_choice_selected(&app->ui) + delta;
            if (sel < 0) sel = count - 1;
            if (sel >= count) sel = 0;
            saya_ui_set_choice_selected(&app->ui, sel);
            return;
        }
        if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_OK) {
            const int sel = saya_ui_choice_selected(&app->ui);
            if (saya_player_choose(&app->player, &app->pack, (uint8_t)sel, &layout)) {
                render_scene(app);
                autosave(app);
            }
        }
        return;
    }

    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        // 上/下翻页:只在本句被切成多页时有意义。
        if (key->btn == BSP_BTN_UP && app->player.page > 0) {
            app->player.page--;
            render_scene(app);
        } else if (key->btn == BSP_BTN_DOWN && app->player.page + 1 < app->player.page_count) {
            app->player.page++;
            render_scene(app);
        }
        return;
    }

    if (key->ev != BSP_BTN_CLICK || key->btn != BSP_BTN_OK) return;

    if (saya_ui_typing(&app->ui)) {
        saya_ui_show_full_text(&app->ui);
        return;
    }

    const saya_step_t step = saya_player_advance(&app->player, &app->pack, &layout);
    switch (step) {
    case SAYA_STEP_TEXT:
        render_scene(app);
        return;
    case SAYA_STEP_SCENE:
    case SAYA_STEP_CHAPTER:
        render_scene(app);
        autosave(app);
        return;
    case SAYA_STEP_CHOICE:
        render_scene(app);
        return;
    case SAYA_STEP_ENDING:
        autosave(app);
        goto_page(app, SAYA_PAGE_ENDING);
        return;
    default:
        ESP_LOGW(TAG, "推进失败(数据异常)");
        goto_page(app, SAYA_PAGE_ENDING);
        return;
    }
}

static void handle_menu(saya_app_t *app, const saya_key_t *key)
{
    const int count = 5;
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        goto_page(app, SAYA_PAGE_GAME);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        const int delta = key->btn == BSP_BTN_UP ? -1 : 1;
        app->menu_sel = (app->menu_sel + delta + count) % count;
        static const char *rows[5] = { "保存", "读取", "跳过场景", "返回标题", "关闭菜单" };
        saya_ui_set_menu_rows(&app->ui, rows, count, app->menu_sel);
        return;
    }
    if (key->ev != BSP_BTN_CLICK || key->btn != BSP_BTN_OK) return;

    static const char *rows[5] = { "保存", "读取", "跳过场景", "返回标题", "关闭菜单" };
    const char *choice = rows[app->menu_sel];
    if (strcmp(choice, "保存") == 0) {
        app->slots_saving = true;
        app->slots_sel = 0;
        app->slots_return = SAYA_PAGE_MENU;
        goto_page(app, SAYA_PAGE_SLOTS);
    } else if (strcmp(choice, "读取") == 0) {
        app->slots_saving = false;
        app->slots_sel = 0;
        app->slots_return = SAYA_PAGE_MENU;
        goto_page(app, SAYA_PAGE_SLOTS);
    } else if (strcmp(choice, "跳过场景") == 0) {
        saya_layout_t layout;
        layout_for(app, &layout);
        if (saya_player_skip_scene(&app->player, &app->pack, &layout)) {
            autosave(app);
            goto_page(app, SAYA_PAGE_GAME);
        } else {
            ESP_LOGI(TAG, "本场景不可跳过");
            goto_page(app, SAYA_PAGE_GAME);
            show_notice(app, "本场景不可跳过");
        }
    } else if (strcmp(choice, "返回标题") == 0) {
        goto_page(app, SAYA_PAGE_TITLE);
    } else {
        goto_page(app, SAYA_PAGE_GAME);
    }
}

static void handle_settings(saya_app_t *app, const saya_key_t *key)
{
    const int count = 4;
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        goto_page(app, app->settings_return);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        const int delta = key->btn == BSP_BTN_UP ? -1 : 1;
        app->settings_sel = (app->settings_sel + delta + count) % count;
        saya_ui_set_settings_selected(&app->ui, app->settings_sel);
        return;
    }
    if (key->ev != BSP_BTN_CLICK || key->btn != BSP_BTN_OK) return;

    switch (app->settings_sel) {
    case 0:
        app->settings.text_speed = (uint8_t)((app->settings.text_speed + 1) % 3);
        saya_ui_set_setting_value(&app->ui, 0, speed_label(app));
        break;
    case 1:
        app->settings.font_large = app->settings.font_large ? 0 : 1;
        saya_ui_set_setting_value(&app->ui, 1, app->settings.font_large ? "大" : "小");
        saya_ui_set_font_size(&app->ui, app->settings.font_large != 0);
        break;
    case 2:
        app->about_return = SAYA_PAGE_SETTINGS;
        goto_page(app, SAYA_PAGE_ABOUT);
        return;
    default:
        goto_page(app, app->settings_return);
        return;
    }
    saya_settings_store(&app->settings);
}

static void handle_slots(saya_app_t *app, const saya_key_t *key)
{
    const int count = SAYA_SAVE_SLOTS + 1;
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        // 槽位上长按 = 删除(仅保存模式);其余位置长按 = 离开本页。
        if (app->slots_saving && app->slots_sel < SAYA_SAVE_SLOTS) {
            if (saya_slot_clear((uint8_t)app->slots_sel)) {
                ESP_LOGI(TAG, "已清空存档 %u", (unsigned)(app->slots_sel + 1));
            }
            render_slots(app);
            return;
        }
        goto_page(app, app->slots_return);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        const int delta = key->btn == BSP_BTN_UP ? -1 : 1;
        app->slots_sel = (app->slots_sel + delta + count) % count;
        saya_ui_set_slots_selected(&app->ui, app->slots_sel);
        return;
    }
    if (key->ev != BSP_BTN_CLICK || key->btn != BSP_BTN_OK) return;

    if (app->slots_sel >= SAYA_SAVE_SLOTS) {
        goto_page(app, app->slots_return);
        return;
    }

    const uint8_t slot = (uint8_t)app->slots_sel;
    if (app->slots_saving) {
        saya_save_t save;
        saya_save_from_player(&app->player, &save);
        if (saya_slot_store(slot, &save)) {
            ESP_LOGI(TAG, "已保存到存档 %u", (unsigned)(slot + 1));
        }
        render_slots(app);
        return;
    }

    saya_save_t save;
    if (saya_slot_load(slot, &save)) {
        begin_reading(app, &save);
    }
}

static void handle_ending(saya_app_t *app, const saya_key_t *key)
{
    if ((key->ev == BSP_BTN_CLICK || key->ev == BSP_BTN_LONG) && key->btn == BSP_BTN_OK) {
        app->title_sel = 0;
        goto_page(app, SAYA_PAGE_TITLE);
    }
}

static void handle_about(saya_app_t *app, const saya_key_t *key)
{
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        goto_page(app, SAYA_PAGE_TITLE);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_OK) {
        goto_page(app, app->about_return);
    }
}

void saya_app_key(saya_app_t *app, const saya_key_t *key)
{
    if (!app || !key) return;
    app->idle_ms = 0;
    if (!bsp_lvgl_lock(200)) {
        ESP_LOGW(TAG, "拿不到 LVGL 锁,忽略本次按键");
        return;
    }
    switch (app->page) {
    case SAYA_PAGE_WARNING:
        if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_OK) {
            app->settings.seen_warning = 1;
            saya_settings_store(&app->settings);
            app->title_sel = 0;
            goto_page(app, SAYA_PAGE_TITLE);
        }
        break;
    case SAYA_PAGE_TITLE: handle_title(app, key); break;
    case SAYA_PAGE_GAME: handle_game(app, key); break;
    case SAYA_PAGE_MENU: handle_menu(app, key); break;
    case SAYA_PAGE_SETTINGS: handle_settings(app, key); break;
    case SAYA_PAGE_SLOTS: handle_slots(app, key); break;
    case SAYA_PAGE_ENDING: handle_ending(app, key); break;
    case SAYA_PAGE_ABOUT: handle_about(app, key); break;
    default: break;
    }
    bsp_lvgl_unlock();
}

void saya_app_tick(saya_app_t *app, uint32_t elapsed_ms)
{
    if (!app) return;
    app->idle_ms += elapsed_ms;

    if (app->notice_ms > 0) {
        app->notice_ms = app->notice_ms > elapsed_ms ? app->notice_ms - elapsed_ms : 0;
        if (app->notice_ms == 0 && app->page == SAYA_PAGE_GAME && !app->player.at_choice) {
            char label[32];
            chapter_label(app, label, sizeof(label));
            if (bsp_lvgl_lock(50)) {
                saya_ui_set_chapter(&app->ui, label);
                bsp_lvgl_unlock();
            }
        }
    }
    app->battery_accum_ms += elapsed_ms;
    if (app->battery_accum_ms < 10000) return;
    app->battery_accum_ms = 0;
    const int soc = bsp_battery_soc();
    if (soc == app->battery_percent) return;
    app->battery_percent = soc;
    if (!bsp_lvgl_lock(50)) return;
    saya_ui_set_battery(&app->ui, soc);
    bsp_lvgl_unlock();
}

uint32_t saya_app_idle_ms(const saya_app_t *app)
{
    return app ? app->idle_ms : 0;
}

void saya_app_before_sleep(saya_app_t *app)
{
    if (!app) return;
    if (app->page == SAYA_PAGE_GAME) autosave(app);
}

void saya_app_show_sleeping(saya_app_t *app)
{
    if (!app) return;
    saya_ui_show_page(&app->ui, SAYA_PAGE_WARNING);
    saya_ui_set_warning(&app->ui, "已休眠\n\n按任意按键继续阅读", "");
}

bool saya_app_init(saya_app_t *app, const uint8_t *pack_data, uint32_t pack_size,
                   const saya_app_buffers_t *buffers)
{
    if (!app || !pack_data || !buffers) return false;
    memset(app, 0, sizeof(*app));
    app->battery_percent = -1;

    if (!saya_pack_open(&app->pack, pack_data, pack_size)) {
        ESP_LOGE(TAG, "资源包校验失败(%u 字节)", (unsigned)pack_size);
        return false;
    }
    saya_player_reset(&app->player);

    saya_settings_default(&app->settings);
    if (!saya_save_init()) {
        ESP_LOGW(TAG, "存档不可用,本次阅读不会保存进度");
    } else {
        (void)saya_settings_load(&app->settings);
        saya_save_t save;
        app->have_auto = saya_auto_load(&save);
    }

    if (!bsp_lvgl_lock(-1)) {
        ESP_LOGE(TAG, "LVGL 未就绪");
        return false;
    }
    const bool ui_ok = saya_ui_create(&app->ui, buffers->art_pixels, buffers->sprite_scratch,
                                      buffers->sprite_scratch_size, &saya_cjk_16, &saya_cjk_20);
    if (ui_ok) {
        saya_ui_set_battery(&app->ui, app->battery_percent);
        saya_ui_set_font_size(&app->ui, app->settings.font_large != 0);
        app->page = app->settings.seen_warning ? SAYA_PAGE_TITLE : SAYA_PAGE_WARNING;
        render_current_page(app);
    }
    bsp_lvgl_unlock();
    return ui_ok;
}
