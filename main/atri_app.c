// main/atri_app.c —— 应用状态机实现。
#include "atri_app.h"

#include "bsp_battery.h"
#include "bsp_button.h"

#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "atri_app";

#define ATRI_BATTERY_INTERVAL_MS 30000u
#define ATRI_NOTICE_MS 2000u
#define ATRI_TRANSITION_MS 1200u
// 长按上/下开始快进时,每句话之间的间隔。
#define ATRI_FASTFORWARD_MS 110u
// 自动阅读:一句话打完之后再等这么久翻到下一句(给阅读留时间)。
#define ATRI_AUTO_MS 1600u

// 打字机速度(毫秒/字):0 = 瞬间显示整页。对应设置页的 瞬间/慢/中/快。
static const uint32_t s_speed_ms[4] = { 0, 60, 35, 18 };
static const char *const s_speed_names[4] = { "瞬间", "慢", "中", "快" };
#define ATRI_SPEED_DEFAULT 2   // 中速

static void set_page(atri_app_t *app, atri_page_t page)
{
    app->page = page;
    atri_ui_show_page(&app->ui, page);
}

static void notify(atri_app_t *app, const char *text)
{
    snprintf(app->notice, sizeof(app->notice), "%s", text ? text : "");
    app->notice_ms = ATRI_NOTICE_MS;
    atri_ui_notice(&app->ui, app->notice);
}

static uint32_t speed_ms(const atri_app_t *app)
{
    const uint8_t level = app->settings.text_speed > 3 ? ATRI_SPEED_DEFAULT
                                                      : app->settings.text_speed;
    return s_speed_ms[level];
}

static bool true_end_unlocked(const atri_app_t *app)
{
    return app->settings.seen_happy && app->settings.seen_bad;
}

// 源脚本用英文结局名,这里换算成中文标签;未知名称原样显示。
static const char *ending_label(const char *name)
{
    if (!name) return "结局";
    if (strcmp(name, "Happy Ending") == 0) return "圆满结局";
    if (strcmp(name, "Bad Ending") == 0) return "悲剧结局";
    if (strncmp(name, "True Ending", 11) == 0) return "真正的结局";
    return name;
}

// ---------------------------------------------------------------- 画面区
// chr 传当前角色立绘下标(ATRI_CHAR_KEEP = 这一屏没有立绘)。
static void render_art(atri_app_t *app, uint16_t bg, uint16_t ovl, int16_t x, int16_t y,
                       uint16_t chr)
{
    if (app->rendered_bg == bg && app->rendered_ovl == ovl && app->rendered_chr == chr &&
        app->rendered_x == x && app->rendered_y == y && bg != ATRI_NONE) {
        return;   // 同一张画面,不重复解码
    }
    if (atri_ui_set_art(&app->ui, &app->pack, bg, ovl, x, y, chr)) {
        app->rendered_bg = bg;
        app->rendered_ovl = ovl;
        app->rendered_chr = chr;
        app->rendered_x = x;
        app->rendered_y = y;
    } else {
        app->rendered_bg = ATRI_NONE;
        app->rendered_ovl = ATRI_NONE;
        app->rendered_chr = ATRI_CHAR_KEEP;
    }
}

// 把当前场景画出来(画面区 + 正文)。
static void render_scene(atri_app_t *app)
{
    atri_scene_t scene;
    if (!atri_player_scene_view(&app->player, &app->pack, &scene)) return;
    render_art(app, scene.bg, scene.ovl, scene.ovl_x, scene.ovl_y,
               atri_player_char(&app->player));

    const int chapter_ordinal = (int)app->player.chapter + 1;
    atri_ui_set_progress(&app->ui, chapter_ordinal, app->player.page + 1,
                         app->player.page_count);

    if (app->player.at_choice) {
        char first[128] = { 0 };
        char second[128] = { 0 };
        atri_pack_name(&app->pack, scene.choice_name[0], first, sizeof(first));
        atri_pack_name(&app->pack, scene.choice_name[1], second, sizeof(second));
        atri_ui_set_text(&app->ui, "", "", 0);
        atri_ui_show_choices(&app->ui, first, scene.choice_count > 1 ? second : NULL);
        return;
    }

    atri_ui_hide_choices(&app->ui);
    char speaker[64] = { 0 };
    char text[ATRI_TEXT_BUFFER] = { 0 };
    atri_player_speaker(&app->player, &app->pack, speaker, sizeof(speaker));
    atri_player_page_text(&app->player, &app->pack, &app->layout_hint, text, sizeof(text));
    // 快进时整句直接铺满,不要一个字一个字往外吐。
    atri_ui_set_text(&app->ui, speaker, text, app->fast_forward ? 0 : speed_ms(app));
}

// 换章过场:先摆标题画,再进正文(源工程换章时也是把画面换回标题图)。
static void start_transition(atri_app_t *app)
{
    atri_ui_hide_choices(&app->ui);
    atri_ui_set_text(&app->ui, "", "", 0);
    render_art(app, ATRI_BG_TITLE, ATRI_NONE, 0, 0, ATRI_CHAR_KEEP);
    app->transition_pending = true;
    app->transition_ms = ATRI_TRANSITION_MS;
}

static void auto_save(atri_app_t *app)
{
    atri_save_t save;
    atri_save_from_player(&app->player, &save);
    if (atri_slot_store(ATRI_AUTO_SLOT, &save)) {
        app->have_auto = true;
    }
}

// ---------------------------------------------------------------- 列表页
static void open_menu(atri_app_t *app)
{
    static const char *const labels[] = { "继续阅读", "保存进度", "读取存档", "跳过章节",
                                          "返回标题" };
    app->menu_sel = 0;
    atri_ui_set_list(&app->ui, ATRI_LIST_MENU, "菜单", "上/下选择 · 确定确认 · 长按确定关闭",
                     labels, NULL, (int)(sizeof(labels) / sizeof(labels[0])), app->menu_sel);
    set_page(app, ATRI_PAGE_MENU);
}

static void open_settings(atri_app_t *app, atri_page_t from)
{
    static const char *const labels[] = { "文字速度", "关于本作", "关机", "返回" };
    const char *values[] = { s_speed_names[app->settings.text_speed > 3 ? ATRI_SPEED_DEFAULT
                                                                     : app->settings.text_speed],
                             NULL, NULL, NULL };
    app->settings_return = from;
    app->settings_sel = 0;
    atri_ui_set_list(&app->ui, ATRI_LIST_SETTINGS, "系统设置", "上/下选择 · 确定确认",
                     labels, values, (int)(sizeof(labels) / sizeof(labels[0])),
                     app->settings_sel);
    set_page(app, ATRI_PAGE_SETTINGS);
}

static void open_about(atri_app_t *app)
{
    static const char body[] =
        "《ATRI -My Dear Moments-》阅读器\n\n"
        "剧本与素材来自小米手环 9 上的同人移植\n"
        "liuyuze61/ATRI-miband,本机用 C + LVGL\n"
        "重写了竖屏阅读引擎。\n\n"
        "原作:《ATRI -My Dear Moments-》\n"
        "制作:ANIPLEX.EXE / Frontwing / 枕\n"
        "手环移植:@liuyuze61\n"
        "本机移植:Shinku-Chen/ai-passport\n\n"
        "字体:Noto Sans SC(OFL-1.1)子集,\n"
        "4bpp 位图,由 tools/atri_font.py 生成。\n"
        "画面:ESP32-C3 软件 JPEG 解码(esp_jpeg),\n"
        "资源包直接映射自 Flash。\n\n"
        "操作:\n"
        "  上 / 下短按：下一句\n"
        "  长按上：快进(松手即停)\n"
        "  长按下：自动阅读开关(任意键停)\n"
        "  确定：打开菜单(存/读档、跳过章节、回标题)\n"
        "  选项页：上/下选择,确定确认\n"
        "  标题页 / 列表：上/下选择,长按确定返回\n\n"
        "本作仅供个人学习与交流,请支持正版。";
    atri_ui_set_about(&app->ui, body);
    app->about_scroll = 0;
    atri_ui_set_about_scroll(&app->ui, 0);
    set_page(app, ATRI_PAGE_ABOUT);
}

static void slots_refresh(atri_app_t *app)
{
    char labels[ATRI_UI_MAX_ROWS][16];
    const char *label_ptrs[ATRI_UI_MAX_ROWS];
    static char values[ATRI_UI_MAX_ROWS][24];
    const char *value_ptrs[ATRI_UI_MAX_ROWS];
    int n = 0;

    for (int slot = 0; slot < ATRI_SAVE_SLOTS; ++slot) {
        atri_save_t save;
        snprintf(labels[n], sizeof(labels[n]), "手动 %d", slot + 1);
        if (atri_slot_load((uint8_t)slot, &save)) {
            snprintf(values[n], sizeof(values[n]), "第 %u 章", (unsigned)(save.chapter + 1));
            value_ptrs[n] = values[n];
        } else {
            value_ptrs[n] = "空";
        }
        label_ptrs[n] = labels[n];
        ++n;
    }
    if (!app->slots_saving) {
        atri_save_t save;
        snprintf(labels[n], sizeof(labels[n]), "自动存档");
        if (atri_slot_load(ATRI_AUTO_SLOT, &save)) {
            snprintf(values[n], sizeof(values[n]), "第 %u 章", (unsigned)(save.chapter + 1));
            value_ptrs[n] = values[n];
        } else {
            value_ptrs[n] = "空";
        }
        label_ptrs[n] = labels[n];
        ++n;
    }
    snprintf(labels[n], sizeof(labels[n]), "返回");
    value_ptrs[n] = NULL;
    label_ptrs[n] = labels[n];
    ++n;

    if (app->slots_sel >= n) app->slots_sel = n - 1;
    atri_ui_set_list(&app->ui, ATRI_LIST_SLOTS, app->slots_saving ? "保存进度" : "读取存档",
                     app->slots_saving ? "确定覆盖 · 长按确定删除" : "确定读取",
                     label_ptrs, value_ptrs, n, app->slots_sel);
}

static void open_slots(atri_app_t *app, bool saving, atri_page_t from)
{
    app->slots_saving = saving;
    app->slots_return = from;
    app->slots_sel = 0;
    slots_refresh(app);
    set_page(app, ATRI_PAGE_SLOTS);
}

static void title_refresh(atri_app_t *app)
{
    static char auto_value[24];
    static const char *value_ptrs[ATRI_UI_MAX_ROWS];
    static const char *labels[ATRI_UI_MAX_ROWS];
    int n = 0;

    labels[n] = "开始阅读";
    value_ptrs[n] = NULL;
    ++n;
    if (app->have_auto) {
        atri_save_t save;
        if (atri_slot_load(ATRI_AUTO_SLOT, &save)) {
            snprintf(auto_value, sizeof(auto_value), "第 %u 章", (unsigned)(save.chapter + 1));
        } else {
            auto_value[0] = '\0';
        }
        labels[n] = "继续阅读";
        value_ptrs[n] = auto_value;
        ++n;
    }
    if (true_end_unlocked(app)) {
        labels[n] = "真正的结局";
        value_ptrs[n] = NULL;
        ++n;
    }
    labels[n] = "读取存档";
    value_ptrs[n] = NULL;
    ++n;
    labels[n] = "系统设置";
    value_ptrs[n] = NULL;
    ++n;

    if (app->title_sel >= n) app->title_sel = 0;
    atri_ui_set_list(&app->ui, ATRI_LIST_TITLE, NULL, NULL, labels, value_ptrs, n,
                     app->title_sel);
}

static void show_title(atri_app_t *app)
{
    atri_ui_hide_choices(&app->ui);
    const uint16_t ovl = true_end_unlocked(app) ? ATRI_OVL_TRUE_END : ATRI_NONE;
    render_art(app, ATRI_BG_TITLE, ovl, 0, 0, ATRI_CHAR_KEEP);
    title_refresh(app);
    set_page(app, ATRI_PAGE_TITLE);
}

// ---------------------------------------------------------------- 阅读推进
static bool start_reading(atri_app_t *app, uint16_t chapter)
{
    if (!atri_player_start(&app->player, &app->pack, chapter, &app->layout_hint)) {
        notify(app, "章节数据异常");
        return false;
    }
    app->started = true;
    app->transition_pending = false;
    set_page(app, ATRI_PAGE_GAME);
    render_scene(app);
    return true;
}

static void show_ending(atri_app_t *app)
{
    char name[64] = { 0 };
    atri_pack_name(&app->pack, app->player.end_name, name, sizeof(name));
    const uint8_t flags = atri_player_chapter_flags(&app->player, &app->pack);
    const bool had_true = app->settings.seen_true != 0;

    if (flags & ATRI_CH_HAPPY_END) app->settings.seen_happy = 1;
    if (flags & ATRI_CH_BAD_END) app->settings.seen_bad = 1;
    if (flags & ATRI_CH_TRUE_END) app->settings.seen_true = 1;
    (void)atri_settings_store(&app->settings);

    char hint[128];
    if ((flags & ATRI_CH_TRUE_END) == 0 && !had_true && true_end_unlocked(app)) {
        snprintf(hint, sizeof(hint), "两个结局都达成了\n标题页出现「真正的结局」\n按确定返回标题");
    } else {
        snprintf(hint, sizeof(hint), "按确定返回标题");
    }
    atri_ui_set_ending(&app->ui, "达成结局", ending_label(name), hint);
    set_page(app, ATRI_PAGE_ENDING);

    // 通关后不再保留自动存档:标题页的"继续阅读"应指向未结束的进度。
    atri_slot_clear(ATRI_AUTO_SLOT);
    app->have_auto = false;
}

static void advance_reading(atri_app_t *app, bool skip_typing)
{
    if (!skip_typing && atri_ui_typing(&app->ui)) {
        atri_ui_finish_typing(&app->ui);
        return;
    }
    switch (atri_player_advance(&app->player, &app->pack, &app->layout_hint)) {
    case ATRI_STEP_TEXT:
        render_scene(app);
        break;
    case ATRI_STEP_SCENE:
        auto_save(app);
        render_scene(app);
        break;
    case ATRI_STEP_CHOICE:
        auto_save(app);
        app->fast_forward = false;   // 选项页停下,交回玩家
        app->auto_play = false;
        atri_ui_set_auto(&app->ui, false);
        render_scene(app);
        break;
    case ATRI_STEP_CHAPTER:
        auto_save(app);
        app->fast_forward = false;   // 过场停下,让玩家看到换章
        start_transition(app);
        break;
    case ATRI_STEP_ENDING:
        app->fast_forward = false;
        app->auto_play = false;
        atri_ui_set_auto(&app->ui, false);
        show_ending(app);
        break;
    case ATRI_STEP_STUCK:
    default:
        app->fast_forward = false;
        notify(app, "剧情数据异常");
        break;
    }
}

// ---------------------------------------------------------------- 按键
static void key_list_move(atri_app_t *app, int *selected, int count, int delta)
{
    if (count <= 0) return;
    *selected += delta;
    if (*selected < 0) *selected = count - 1;
    if (*selected >= count) *selected = 0;
    atri_list_id_t id = ATRI_LIST_TITLE;
    if (selected == &app->menu_sel) id = ATRI_LIST_MENU;
    else if (selected == &app->settings_sel) id = ATRI_LIST_SETTINGS;
    else if (selected == &app->slots_sel) id = ATRI_LIST_SLOTS;
    atri_ui_set_list_selected(&app->ui, id, *selected);
}

static void key_warning(atri_app_t *app, const atri_key_t *key)
{
    if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_CLICK) {
        app->settings.seen_warning = 1;
        (void)atri_settings_store(&app->settings);
        show_title(app);
    }
}

static void title_select(atri_app_t *app)
{
    // 标题菜单是动态的:按当前显示的行取动作,标签顺序与 title_refresh 一致。
    int index = 0;
    if (app->title_sel == index++) {
        if (start_reading(app, 0)) {
            // 新游戏从序章开始,清掉旧进度的自动存档。
            atri_slot_clear(ATRI_AUTO_SLOT);
            app->have_auto = false;
        }
        return;
    }
    if (app->have_auto) {
        if (app->title_sel == index++) {
            atri_save_t save;
            if (atri_slot_load(ATRI_AUTO_SLOT, &save) &&
                atri_player_load(&app->player, &app->pack, &save, &app->layout_hint)) {
                app->started = true;
                app->transition_pending = false;
                set_page(app, ATRI_PAGE_GAME);
                render_scene(app);
            } else {
                notify(app, "自动存档不可用");
                app->have_auto = false;
                title_refresh(app);
            }
            return;
        }
    }
    if (true_end_unlocked(app)) {
        if (app->title_sel == index++) {
            const int idx = atri_pack_find_chapter(&app->pack, 701u);
            if (idx < 0 || !start_reading(app, (uint16_t)idx)) {
                notify(app, "真正的结局章节缺失");
            }
            return;
        }
    }
    if (app->title_sel == index++) {
        open_slots(app, false, ATRI_PAGE_TITLE);
        return;
    }
    if (app->title_sel == index++) {
        open_settings(app, ATRI_PAGE_TITLE);
        return;
    }
}

static void key_title(atri_app_t *app, const atri_key_t *key)
{
    int count = 4 + (app->have_auto ? 1 : 0) + (true_end_unlocked(app) ? 1 : 0);
    if (key->btn == BSP_BTN_UP && key->ev == BSP_BTN_CLICK) {
        key_list_move(app, &app->title_sel, count, -1);
    } else if (key->btn == BSP_BTN_DOWN && key->ev == BSP_BTN_CLICK) {
        key_list_move(app, &app->title_sel, count, 1);
    } else if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_CLICK) {
        title_select(app);
    } else if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_LONG) {
        app->sleep_requested = true;   // 标题页长按确定 = 关机
    }
}

static void key_menu(atri_app_t *app, const atri_key_t *key)
{
    const int count = 5;
    if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_UP) {
        key_list_move(app, &app->menu_sel, count, -1);
    } else if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_DOWN) {
        key_list_move(app, &app->menu_sel, count, 1);
    } else if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_CLICK) {
        switch (app->menu_sel) {
        case 0:   // 继续阅读
            set_page(app, ATRI_PAGE_GAME);
            render_scene(app);
            break;
        case 1:   // 保存进度
            open_slots(app, true, ATRI_PAGE_MENU);
            break;
        case 2:   // 读取存档
            open_slots(app, false, ATRI_PAGE_MENU);
            break;
        case 3: { // 跳过章节:一直推到换章,遇到选项/结局则停下
            const atri_step_t step =
                atri_player_skip_chapter(&app->player, &app->pack, &app->layout_hint);
            auto_save(app);
            set_page(app, ATRI_PAGE_GAME);
            switch (step) {
            case ATRI_STEP_CHOICE:
                render_scene(app);
                notify(app, "遇到选项,已停下");
                break;
            case ATRI_STEP_ENDING:
                show_ending(app);
                break;
            case ATRI_STEP_CHAPTER:
                start_transition(app);
                break;
            case ATRI_STEP_STUCK:
            default:
                render_scene(app);
                notify(app, "已经是本章末尾");
                break;
            }
            break;
        }
        default:  // 返回标题
            show_title(app);
            break;
        }
    } else if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_LONG) {
        set_page(app, ATRI_PAGE_GAME);
    }
}

static void key_settings(atri_app_t *app, const atri_key_t *key)
{
    const int count = 4;
    if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_UP) {
        key_list_move(app, &app->settings_sel, count, -1);
    } else if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_DOWN) {
        key_list_move(app, &app->settings_sel, count, 1);
    } else if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_CLICK) {
        switch (app->settings_sel) {
        case 0:   // 文字速度:循环 瞬间/慢/中/快
            app->settings.text_speed = (uint8_t)((app->settings.text_speed + 1) % 4);
            atri_ui_set_list_value(&app->ui, ATRI_LIST_SETTINGS, 0,
                                   s_speed_names[app->settings.text_speed]);
            (void)atri_settings_store(&app->settings);
            break;
        case 1:   // 关于本作
            open_about(app);
            break;
        case 2:   // 关机
            app->sleep_requested = true;
            break;
        default:  // 返回
            set_page(app, app->settings_return);
            if (app->settings_return == ATRI_PAGE_TITLE) title_refresh(app);
            break;
        }
    } else if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_LONG) {
        set_page(app, app->settings_return);
        if (app->settings_return == ATRI_PAGE_TITLE) title_refresh(app);
    }
}

static void key_slots(atri_app_t *app, const atri_key_t *key)
{
    const int count = ATRI_SAVE_SLOTS + (app->slots_saving ? 1 : 2);   // + 自动(读档) + 返回
    if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_UP) {
        key_list_move(app, &app->slots_sel, count, -1);
    } else if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_DOWN) {
        key_list_move(app, &app->slots_sel, count, 1);
    } else if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_CLICK) {
        const int back_index = count - 1;
        if (app->slots_sel == back_index) {
            set_page(app, app->slots_return);
            if (app->slots_return == ATRI_PAGE_TITLE) title_refresh(app);
            return;
        }
        const bool is_auto = app->slots_sel >= ATRI_SAVE_SLOTS;
        if (app->slots_saving) {
            if (is_auto) {
                notify(app, "自动存档不可覆盖");
                return;
            }
            atri_save_t save;
            atri_save_from_player(&app->player, &save);
            if (atri_slot_store((uint8_t)app->slots_sel, &save)) {
                notify(app, "保存成功");
            } else {
                notify(app, "保存失败");
            }
            slots_refresh(app);
        } else {
            atri_save_t save;
            const uint8_t slot = is_auto ? ATRI_AUTO_SLOT : (uint8_t)app->slots_sel;
            if (atri_slot_load(slot, &save) &&
                atri_player_load(&app->player, &app->pack, &save, &app->layout_hint)) {
                app->started = true;
                app->transition_pending = false;
                set_page(app, ATRI_PAGE_GAME);
                render_scene(app);
            } else {
                notify(app, "该存档为空");
            }
        }
    } else if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_LONG) {
        // 存档模式:长按删除当前槽;其他情况返回上一层。
        if (app->slots_saving && app->slots_sel < ATRI_SAVE_SLOTS) {
            if (atri_slot_clear((uint8_t)app->slots_sel)) {
                notify(app, "已删除");
            } else {
                notify(app, "删除失败");
            }
            slots_refresh(app);
            return;
        }
        set_page(app, app->slots_return);
        if (app->slots_return == ATRI_PAGE_TITLE) title_refresh(app);
    }
}

static void key_game(atri_app_t *app, const atri_key_t *key)
{
    if (app->transition_pending) {
        app->transition_pending = false;
        render_scene(app);
        return;
    }
    // 长按下 = 自动阅读模式开关(按固定节奏自动翻页)。
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_DOWN) {
        app->auto_play = !app->auto_play;
        app->auto_ms = ATRI_AUTO_MS;
        app->fast_forward = false;
        atri_ui_set_auto(&app->ui, app->auto_play);
        notify(app, app->auto_play ? "自动阅读:开" : "自动阅读:关");
        return;
    }
    // 长按上 = 快进(按住就一直推,松手停)。
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_UP) {
        app->fast_forward = true;
        app->fast_forward_ms = ATRI_FASTFORWARD_MS;
        if (!app->player.at_choice) advance_reading(app, true);
        return;
    }
    if (key->ev == BSP_BTN_RELEASE) {
        app->fast_forward = false;
        return;
    }
    if (app->player.at_choice) {
        if (key->btn == BSP_BTN_UP && key->ev == BSP_BTN_CLICK) {
            atri_ui_set_choice_selected(&app->ui, atri_ui_choice_selected(&app->ui) - 1);
        } else if (key->btn == BSP_BTN_DOWN && key->ev == BSP_BTN_CLICK) {
            atri_ui_set_choice_selected(&app->ui, atri_ui_choice_selected(&app->ui) + 1);
        } else if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_CLICK) {
            if (atri_player_choose(&app->player, &app->pack,
                                   (uint8_t)atri_ui_choice_selected(&app->ui),
                                   &app->layout_hint)) {
                auto_save(app);
                render_scene(app);
            }
        }
        return;
    }
    // 正文页:上/下推进对白(短按下一句),确定键只用来开菜单。
    if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_CLICK) {
        open_menu(app);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        advance_reading(app, false);
    }
}

// ---------------------------------------------------------------- 对外接口
bool atri_app_init(atri_app_t *app, const uint8_t *pack_data, uint32_t pack_size,
                   uint16_t *art_pixels, const lv_font_t *font_cjk)
{
    if (!app || !pack_data || !art_pixels || !font_cjk) return false;
    memset(app, 0, sizeof(*app));
    app->rendered_bg = ATRI_NONE;
    app->rendered_ovl = ATRI_NONE;
    app->rendered_chr = ATRI_CHAR_KEEP;
    app->battery_percent = -1;

    if (!atri_pack_open(&app->pack, pack_data, pack_size)) {
        ESP_LOGE(TAG, "资源包解析失败(%u 字节)", (unsigned)pack_size);
        return false;
    }
    if (!atri_save_init()) {
        ESP_LOGW(TAG, "NVS 不可用,本次运行不保存进度");
    } else {
        (void)atri_settings_load(&app->settings);
    }
    if (app->settings.text_speed > 3) app->settings.text_speed = ATRI_SPEED_DEFAULT;

    app->layout_hint.units_per_line = ATRI_UNITS_PER_LINE;
    app->layout_hint.lines_per_page = ATRI_TEXT_LINES;

    if (!atri_ui_create(&app->ui, art_pixels, font_cjk)) {
        ESP_LOGE(TAG, "界面创建失败");
        return false;
    }

    atri_save_t save;
    app->have_auto = atri_slot_load(ATRI_AUTO_SLOT, &save);

    if (!app->settings.seen_warning) {
        static const char warning[] =
            "同人移植提示\n\n"
            "本机运行的是《ATRI -My Dear Moments-》阅读器,"
            "由小米手环 9 的同人移植工程(liuyuze61/ATRI-miband)的剧本与素材转换而来。\n\n"
            "剧本、图像与译文版权归原作品与移植者所有,"
            "仅供个人学习交流使用,请支持正版。\n\n"
            "按「确定」开始阅读。";
        atri_ui_set_warning(&app->ui, warning, "确定 · 开始");
        set_page(app, ATRI_PAGE_WARNING);
    } else {
        show_title(app);
    }
    ESP_LOGI(TAG, "就绪:章节 %u / 场景 %u / 对白 %u / 背景 %u / 叠加 %u",
             (unsigned)app->pack.chapter_count, (unsigned)app->pack.scene_count,
             (unsigned)app->pack.dialogue_count, (unsigned)app->pack.bg_count,
             (unsigned)app->pack.ovl_count);
    return true;
}

void atri_app_key(atri_app_t *app, const atri_key_t *key)
{
    if (!app || !key) return;
    app->idle_ms = 0;
    app->notice_ms = 0;
    atri_ui_notice(&app->ui, "");

    // 自动阅读模式:除了“长按下”这个开关本身,任何按键都停下来(先把控制权还给玩家)。
    const bool is_auto_toggle =
        (key->btn == BSP_BTN_DOWN && key->ev == BSP_BTN_LONG && app->page == ATRI_PAGE_GAME);
    if (app->auto_play && !is_auto_toggle) {
        app->auto_play = false;
        app->fast_forward = false;
        atri_ui_set_auto(&app->ui, false);
    }

    switch (app->page) {
    case ATRI_PAGE_WARNING: key_warning(app, key); break;
    case ATRI_PAGE_TITLE: key_title(app, key); break;
    case ATRI_PAGE_GAME: key_game(app, key); break;
    case ATRI_PAGE_MENU: key_menu(app, key); break;
    case ATRI_PAGE_SETTINGS: key_settings(app, key); break;
    case ATRI_PAGE_SLOTS: key_slots(app, key); break;
    case ATRI_PAGE_ENDING:
        if (key->ev == BSP_BTN_CLICK) show_title(app);
        break;
    case ATRI_PAGE_ABOUT:
        if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_CLICK) {
            open_settings(app, ATRI_PAGE_TITLE);
        } else if ((key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN) &&
                   key->ev == BSP_BTN_CLICK) {
            const int delta = (key->btn == BSP_BTN_DOWN) ? 24 : -24;
            app->about_scroll += delta;
            atri_ui_set_about_scroll(&app->ui, app->about_scroll);
        }
        break;
    default: break;
    }
}

void atri_app_tick(atri_app_t *app, uint32_t elapsed_ms)
{
    if (!app) return;
    app->idle_ms += elapsed_ms;

    if (app->notice_ms > 0) {
        app->notice_ms = app->notice_ms > elapsed_ms ? app->notice_ms - elapsed_ms : 0;
        if (app->notice_ms == 0) atri_ui_notice(&app->ui, "");
    }
    if (app->transition_pending) {
        if (app->transition_ms > elapsed_ms) {
            app->transition_ms -= elapsed_ms;
        } else {
            app->transition_pending = false;
            render_scene(app);
        }
    }
    // 自动阅读:打字机打完后再等 ATRI_AUTO_MS 才翻页;选项/过场/结局自动停。
    if (app->auto_play && app->page == ATRI_PAGE_GAME && !app->transition_pending) {
        if (atri_ui_typing(&app->ui) || app->player.at_choice || app->player.ended) {
            app->auto_ms = ATRI_AUTO_MS;   // 还在打字 / 等玩家选择:计时不累计
        } else {
            app->auto_ms = app->auto_ms > elapsed_ms ? app->auto_ms - elapsed_ms : 0;
            if (app->auto_ms == 0) {
                app->auto_ms = ATRI_AUTO_MS;
                advance_reading(app, false);
            }
        }
    }

    // 快进:长按上时每 ATRI_FASTFORWARD_MS 推进一步。
    if (app->fast_forward && app->page == ATRI_PAGE_GAME && !app->transition_pending) {
        app->fast_forward_ms += elapsed_ms;
        while (app->fast_forward_ms >= ATRI_FASTFORWARD_MS) {
            app->fast_forward_ms -= ATRI_FASTFORWARD_MS;
            advance_reading(app, true);
            if (!app->fast_forward || app->player.at_choice || app->transition_pending) break;
        }
    }
    app->battery_accum_ms += elapsed_ms;
    if (app->battery_accum_ms >= ATRI_BATTERY_INTERVAL_MS || app->battery_percent < 0) {
        app->battery_accum_ms = 0;
        const int soc = bsp_battery_soc();
        if (soc != app->battery_percent) {
            app->battery_percent = soc;
            atri_ui_set_battery(&app->ui, soc);
        }
    }
}

uint32_t atri_app_idle_ms(const atri_app_t *app)
{
    return app ? app->idle_ms : 0;
}

void atri_app_before_sleep(atri_app_t *app)
{
    if (!app) return;
    if (app->page == ATRI_PAGE_GAME) auto_save(app);
    (void)atri_settings_store(&app->settings);
}

void atri_app_show_sleeping(atri_app_t *app)
{
    if (!app) return;
    atri_ui_set_ending(&app->ui, "", "休眠中", "按任意键唤醒");
    set_page(app, ATRI_PAGE_ENDING);
}

bool atri_app_debug_render(atri_app_t *app, uint16_t chapter, uint16_t scene)
{
    if (!app || chapter >= app->pack.chapter_count) return false;
    atri_chapter_t ch;
    atri_pack_chapter(&app->pack, chapter, &ch);
    if (scene >= ch.scene_count) return false;
    atri_scene_t sc;
    atri_pack_scene(&app->pack, (uint16_t)(ch.first_scene + scene), &sc);
    app->rendered_bg = ATRI_NONE;   // 强制重画,不受"同画面不重复解码"缓存影响
    app->rendered_ovl = ATRI_NONE;
    app->rendered_chr = ATRI_CHAR_KEEP;
    // 从本章开头逐句推出这一幕开始时的立绘(粘性),这样截图不依赖玩家当前进度。
    uint16_t chr = ATRI_CHAR_KEEP;
    for (uint32_t d = ch.first_dlg; d <= sc.first_dlg && d < app->pack.dialogue_count; ++d) {
        atri_dialogue_t dlg;
        atri_pack_dialogue(&app->pack, (uint16_t)d, &dlg);
        if (dlg.chr != ATRI_CHAR_KEEP) chr = dlg.chr;
    }
    return atri_ui_set_art(&app->ui, &app->pack, sc.bg, sc.ovl, sc.ovl_x, sc.ovl_y, chr);
}

bool atri_app_debug_start(atri_app_t *app, uint16_t chapter, uint16_t scene)
{
    if (!app) return false;
    if (!atri_player_start(&app->player, &app->pack, chapter, &app->layout_hint)) return false;
    if (scene > 0) {
        atri_save_t save;
        memset(&save, 0, sizeof(save));
        save.chapter = chapter;
        save.scene = scene;
        save.chr = ATRI_CHAR_KEEP;
        if (!atri_player_load(&app->player, &app->pack, &save, &app->layout_hint)) return false;
    }
    app->started = true;
    app->transition_pending = false;
    set_page(app, ATRI_PAGE_GAME);
    render_scene(app);
    ESP_LOGW(TAG, "调试:直接从第 %u 章 第 %u 幕开始阅读", (unsigned)chapter, (unsigned)scene);
    return true;
}

bool atri_app_take_sleep_request(atri_app_t *app)
{
    if (!app || !app->sleep_requested) return false;
    app->sleep_requested = false;
    return true;
}
