// main/atri_app.c —— 应用状态机实现(《千恋＊万花》阅读器,数据层接到 senren 模型/存档)。
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
// 长按上开始快进时,每句话之间的间隔。
#define ATRI_FASTFORWARD_MS 110u
// 自动阅读:打字机打完之后再等这么久翻到下一句(与旧应用一致)。
#define ATRI_AUTO_MS 900u
// 一次推进里最多连跳多少个"不是章节卡"的 CHAPTER 标记(CHAPTERshow/hide),防坏数据死循环。
#define ATRI_CHAPTER_MARK_MAX 64
// 标题页固定 4 行:开始阅读 / 继续阅读 / 读取进度 / 系统设置。
#define ATRI_TITLE_ROWS 4
// 存档页:自动存档 + 5 个手动存档 + 返回。
#define ATRI_SLOT_ROWS (SENREN_SAVE_SLOTS + 2)

// 打字机速度(毫秒/字),对应设置页的 慢/中/快。
static const uint32_t s_speed_ms[3] = { 60, 38, 18 };
static const char *const s_speed_names[3] = { "慢", "中", "快" };
#define ATRI_SPEED_DEFAULT 1   // 中速

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
    const uint8_t level = app->settings.text_speed > 2 ? ATRI_SPEED_DEFAULT
                                                      : app->settings.text_speed;
    return s_speed_ms[level];
}

// 页面切换(定义在后面,推进流程里会用到)
static void show_title(atri_app_t *app);
static void show_ending(atri_app_t *app);

// 存档/列表里的"第几章":0 表示还没进第一章(存档在序章开头),按第 1 章显示。
static uint16_t chapter_display(uint16_t chapter)
{
    return chapter > 0 ? chapter : 1;
}

// 章节卡的章号。源数据里 CHAPTER 节点的标题是 "CHAPTER<章>-<节>";另有
// CHAPTERshow / CHAPTERhide 两个显示开关标记,它们不是章节卡(不要画在屏幕上)。
static bool chapter_card_number(const char *title, uint16_t *number)
{
    if (!title || strncmp(title, "CHAPTER", 7) != 0) return false;
    const char *cursor = title + 7;
    if (*cursor < '0' || *cursor > '9') return false;
    uint32_t value = 0;
    while (*cursor >= '0' && *cursor <= '9') {
        value = value * 10u + (uint32_t)(*cursor - '0');
        if (value > 0xFFFFu) return false;
        ++cursor;
    }
    if (number) *number = (uint16_t)value;
    return true;
}

// ---------------------------------------------------------------- 画面区
// 按数据层当前的三个名字合成画面区。名字没变就不重复解码(JPEG/PNG 都是毫秒级)。
static void render_art(atri_app_t *app)
{
    if (app->rendered_valid && strcmp(app->rendered_bg, app->player.bg) == 0 &&
        strcmp(app->rendered_sprite, app->player.sprite) == 0 &&
        strcmp(app->rendered_ev, app->player.ev) == 0) {
        return;
    }
    atri_ui_compose(&app->ui, &app->pack, app->player.bg, app->player.sprite, app->player.ev);
    snprintf(app->rendered_bg, sizeof(app->rendered_bg), "%s", app->player.bg);
    snprintf(app->rendered_sprite, sizeof(app->rendered_sprite), "%s", app->player.sprite);
    snprintf(app->rendered_ev, sizeof(app->rendered_ev), "%s", app->player.ev);
    app->rendered_valid = true;
}

// 标题页与章节卡的黑底画面(图片包里没有标题画,标题文字由 LVGL 画)。
static void render_title_art(atri_app_t *app)
{
    // 标题页用包里的整屏标题图(官方主视觉),菜单叠在它下面那条正文带上。
    atri_ui_compose(&app->ui, &app->pack, SENREN_TITLE_ART_NAME, "", "");
    app->rendered_valid = true;
    snprintf(app->rendered_bg, sizeof(app->rendered_bg), "%s", SENREN_TITLE_ART_NAME);
    app->rendered_sprite[0] = '\0';
    app->rendered_ev[0] = '\0';
}

// 把当前这一步正文/选项画出来(画面区 + 正文带 + 浮层)。
static void render_scene(atri_app_t *app)
{
    render_art(app);

    atri_ui_set_progress(&app->ui, (int)app->player.chapter, (int)app->player.page_index + 1,
                         (int)app->player.page_count);

    if (app->at_choice) {
        const char *texts[SENREN_CHOICE_MAX];
        int count = app->player.choice_count;
        if (count > SENREN_CHOICE_MAX) count = SENREN_CHOICE_MAX;
        for (int index = 0; index < count; ++index) texts[index] = app->player.choice[index];
        atri_ui_set_text(&app->ui, "", "", 0);
        atri_ui_show_choices(&app->ui, texts, count);
        return;
    }

    atri_ui_hide_choices(&app->ui);
    char speaker[SENREN_NAME_MAX] = { 0 };
    char text[SENREN_TEXT_BUFFER] = { 0 };
    senren_player_speaker(&app->player, speaker, sizeof(speaker));
    senren_player_page_text(&app->player, text, sizeof(text));
    // 快进时整句直接铺满,不要一个字一个字往外吐。
    atri_ui_set_text(&app->ui, speaker, text, app->fast_forward ? 0 : speed_ms(app));
}

// 章节卡:显示 `第 N 章`,等任意键或过场计时结束再往下读。
static void show_chapter_card(atri_app_t *app, uint16_t number)
{
    char text[32];
    snprintf(text, sizeof(text), "第 %u 章", (unsigned)number);
    atri_ui_hide_choices(&app->ui);
    atri_ui_set_progress(&app->ui, (int)number, 0, 0);
    render_art(app);
    atri_ui_set_text(&app->ui, "", text, 0);
    set_page(app, ATRI_PAGE_GAME);
    app->transition_pending = true;
    app->transition_ms = ATRI_TRANSITION_MS;
}

static void auto_save(atri_app_t *app)
{
    senren_save_t save;
    senren_save_from_player(&app->player, &save);
    if (senren_auto_store(&save)) {
        app->have_auto = true;
    }
}

// ---------------------------------------------------------------- 阅读推进
// 往前读一步:已经在打字先显示全文,同一句还有下一页先翻页,否则走状态机。
// skip_typing 为真时(快进/自动阅读/切页后恢复)直接推进,不在打字和翻页上停留。
static void advance_reading(atri_app_t *app, bool skip_typing)
{
    if (!skip_typing && atri_ui_typing(&app->ui)) {
        atri_ui_finish_typing(&app->ui);
        return;
    }
    if (!skip_typing && senren_player_next_page(&app->player)) {
        render_scene(app);
        return;
    }

    for (int guard = 0; guard < ATRI_CHAPTER_MARK_MAX; ++guard) {
        switch (senren_player_advance(&app->player, &app->scn, &app->layout_hint)) {
        case SENREN_STEP_TEXT:
            app->at_choice = false;
            render_scene(app);
            return;
        case SENREN_STEP_CHOICE:
            app->at_choice = true;
            auto_save(app);
            app->fast_forward = false;   // 选项页停下,交回玩家
            app->auto_play = false;
            atri_ui_set_auto(&app->ui, false);
            render_scene(app);
            return;
        case SENREN_STEP_CHAPTER: {
            uint16_t number = 0;
            if (!chapter_card_number(app->player.chapter_title, &number)) {
                continue;   // CHAPTERshow / CHAPTERhide:不是章节卡,接着推进
            }
            // 章号取自源数据的 CHAPTERx-y,存档里的"第几章"与画面上的一致。
            app->player.chapter = number;
            app->at_choice = false;
            auto_save(app);
            // 换章过场不关自动模式:过场计时结束后自动接着读下一章。
            show_chapter_card(app, number);
            return;
        }
        case SENREN_STEP_ENDING:
            app->at_choice = false;
            app->fast_forward = false;
            app->auto_play = false;
            atri_ui_set_auto(&app->ui, false);
            show_ending(app);
            return;
        case SENREN_STEP_STUCK:
            ESP_LOGE(TAG, "剧情数据异常: %s", senren_get_error());
        default:
            app->at_choice = false;
            app->fast_forward = false;
            app->auto_play = false;
            atri_ui_set_auto(&app->ui, false);
            ESP_LOGE(TAG, "剧情推进失败:块 %u / 节点 %u,回到标题页",
                     (unsigned)app->player.chunk, (unsigned)app->player.node);
            notify(app, "剧情数据异常");
            show_title(app);
            return;
        }
    }
    ESP_LOGE(TAG, "连续 %d 个 CHAPTER 标记都没有正文: %s", ATRI_CHAPTER_MARK_MAX, senren_get_error());
    show_title(app);
}

// ---------------------------------------------------------------- 列表页
static void open_menu(atri_app_t *app)
{
    static const char *const labels[] = { "继续阅读", "保存进度", "读取进度", "跳过章节",
                                          "返回标题" };
    app->menu_sel = 0;
    atri_ui_set_list(&app->ui, ATRI_LIST_MENU, "菜单", "上/下选择 · 确定确认 · 长按确定关闭",
                     labels, NULL, (int)(sizeof(labels) / sizeof(labels[0])), app->menu_sel);
    set_page(app, ATRI_PAGE_MENU);
}

static void open_settings(atri_app_t *app, atri_page_t from)
{
    static const char *const labels[] = { "文字速度", "关于本作", "关机", "返回" };
    const uint8_t level = app->settings.text_speed > 2 ? ATRI_SPEED_DEFAULT
                                                      : app->settings.text_speed;
    const char *values[] = { s_speed_names[level], NULL, NULL, NULL };
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
        "《千恋＊万花》阅读器\n\n"
        "原作 SAGA PLANETS\n\n"
        "仅供学习交流，请支持正版\n\n"
        "按确定返回";
    atri_ui_set_about(&app->ui, body);
    app->about_scroll = 0;
    atri_ui_set_about_scroll(&app->ui, 0);
    set_page(app, ATRI_PAGE_ABOUT);
}

// 存档页:自动存档 + 5 个手动存档 + 返回(顺序固定,两种模式一样)。
static void slots_refresh(atri_app_t *app)
{
    static char labels[ATRI_UI_MAX_ROWS][16];
    static char values[ATRI_UI_MAX_ROWS][24];
    const char *label_ptrs[ATRI_UI_MAX_ROWS];
    const char *value_ptrs[ATRI_UI_MAX_ROWS];
    int n = 0;
    senren_save_t save;

    snprintf(labels[n], sizeof(labels[n]), "自动存档");
    if (senren_auto_load(&save)) {
        snprintf(values[n], sizeof(values[n]), "第 %u 章", (unsigned)chapter_display(save.chapter));
        value_ptrs[n] = values[n];
    } else {
        value_ptrs[n] = "空";
    }
    label_ptrs[n] = labels[n];
    ++n;

    for (int slot = 0; slot < SENREN_SAVE_SLOTS; ++slot) {
        snprintf(labels[n], sizeof(labels[n]), "存档 %d", slot + 1);
        if (senren_slot_load((uint8_t)slot, &save)) {
            snprintf(values[n], sizeof(values[n]), "第 %u 章",
                     (unsigned)chapter_display(save.chapter));
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
    atri_ui_set_list(&app->ui, ATRI_LIST_SLOTS, app->slots_saving ? "保存进度" : "读取进度",
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

    // "继续阅读"指向自动存档(每次换章/选选项/睡前写入)。没有就显示"空"。
    senren_save_t save;
    if (app->have_auto && senren_auto_load(&save)) {
        snprintf(auto_value, sizeof(auto_value), "第 %u 章",
                 (unsigned)chapter_display(save.chapter));
    } else {
        app->have_auto = false;
        snprintf(auto_value, sizeof(auto_value), "空");
    }
    labels[n] = "继续阅读";
    value_ptrs[n] = auto_value;
    ++n;

    labels[n] = "读取进度";
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
    atri_ui_set_auto(&app->ui, false);
    app->at_choice = false;
    app->ended = false;
    app->fast_forward = false;
    app->auto_play = false;
    app->transition_pending = false;
    render_title_art(app);
    title_refresh(app);
    set_page(app, ATRI_PAGE_TITLE);
}

static bool start_reading(atri_app_t *app)
{
    if (!senren_player_start(&app->player, &app->scn, 0, 0, 0, &app->layout_hint)) {
        notify(app, "剧本数据异常");
        return false;
    }
    app->started = true;
    app->at_choice = false;
    app->ended = false;
    app->transition_pending = false;
    app->rendered_valid = false;   // 新的一局:强制按名字重画
    set_page(app, ATRI_PAGE_GAME);
    advance_reading(app, false);
    return true;
}

// 从存档接上:位置与画面状态都来自存档,再走一次 advance 把这一步重现出来。
static bool load_save(atri_app_t *app, const senren_save_t *save)
{
    if (!senren_player_load(&app->player, &app->scn, save, &app->layout_hint)) {
        return false;
    }
    app->started = true;
    app->at_choice = false;
    app->ended = false;
    app->transition_pending = false;
    app->rendered_valid = false;   // 存档里的名字可能和现在画的不同
    set_page(app, ATRI_PAGE_GAME);
    advance_reading(app, true);
    return true;
}

static void show_ending(atri_app_t *app)
{
    ESP_LOGI(TAG, "走到结局 %u", (unsigned)app->player.ending_index);
    app->ended = true;
    atri_ui_hide_choices(&app->ui);
    atri_ui_set_ending(&app->ui, "全剧终", "感谢阅读", "按确定返回标题");
    set_page(app, ATRI_PAGE_ENDING);

    // 通关后不再保留自动存档:标题页的"继续阅读"应指向未结束的进度。
    (void)senren_auto_clear();
    app->have_auto = false;
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
        app->settings.seen_tips = 1;
        (void)senren_settings_store(&app->settings);
        show_title(app);
    }
}

static void title_select(atri_app_t *app)
{
    switch (app->title_sel) {
    case 0:   // 开始阅读:从剧本开头读起
        if (start_reading(app)) {
            // 新游戏从头开始,清掉旧进度的自动存档。
            (void)senren_auto_clear();
            app->have_auto = false;
        }
        return;
    case 1: {   // 继续阅读:接自动存档
        senren_save_t save;
        if (app->have_auto && senren_auto_load(&save) && load_save(app, &save)) {
            return;
        }
        notify(app, "还没有可继续的进度");
        app->have_auto = false;
        title_refresh(app);
        return;
    }
    case 2:   // 读取进度
        open_slots(app, false, ATRI_PAGE_TITLE);
        return;
    default:   // 系统设置
        open_settings(app, ATRI_PAGE_TITLE);
        return;
    }
}

static void key_title(atri_app_t *app, const atri_key_t *key)
{
    if (key->btn == BSP_BTN_UP && key->ev == BSP_BTN_CLICK) {
        key_list_move(app, &app->title_sel, ATRI_TITLE_ROWS, -1);
    } else if (key->btn == BSP_BTN_DOWN && key->ev == BSP_BTN_CLICK) {
        key_list_move(app, &app->title_sel, ATRI_TITLE_ROWS, 1);
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
        case 0:   // 继续阅读:回到正文页(画面区与文字按当前状态重画)
            set_page(app, ATRI_PAGE_GAME);
            render_scene(app);
            break;
        case 1:   // 保存进度
            open_slots(app, true, ATRI_PAGE_MENU);
            break;
        case 2:   // 读取进度
            open_slots(app, false, ATRI_PAGE_MENU);
            break;
        case 3:   // 跳过章节:直接跳到下一章的章节卡
            if (senren_player_skip_chapter(&app->player, &app->scn, &app->layout_hint)) {
                set_page(app, ATRI_PAGE_GAME);
                advance_reading(app, true);
                auto_save(app);   // 把新位置写进自动存档(章节卡里已经写过一次)
            } else {
                notify(app, "已经是最后一章");
            }
            break;
        default:  // 返回标题
            show_title(app);
            break;
        }
    } else if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_LONG) {
        set_page(app, ATRI_PAGE_GAME);
        render_scene(app);
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
        case 0:   // 文字速度:循环 慢/中/快
            app->settings.text_speed = (uint8_t)((app->settings.text_speed + 1) % 3);
            atri_ui_set_list_value(&app->ui, ATRI_LIST_SETTINGS, 0,
                                   s_speed_names[app->settings.text_speed]);
            (void)senren_settings_store(&app->settings);
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

// 存档页里第几行是自动存档(行 1..SENREN_SAVE_SLOTS 是手动存档,最后一行是返回)。
static bool slot_row_is_auto(int row)
{
    return row == 0;
}

static void key_slots(atri_app_t *app, const atri_key_t *key)
{
    const int count = ATRI_SLOT_ROWS;
    const int back_index = count - 1;
    if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_UP) {
        key_list_move(app, &app->slots_sel, count, -1);
    } else if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_DOWN) {
        key_list_move(app, &app->slots_sel, count, 1);
    } else if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_CLICK) {
        if (app->slots_sel == back_index) {
            set_page(app, app->slots_return);
            if (app->slots_return == ATRI_PAGE_TITLE) title_refresh(app);
            return;
        }
        const bool is_auto = slot_row_is_auto(app->slots_sel);
        const uint8_t slot = (uint8_t)(app->slots_sel - 1);
        if (app->slots_saving) {
            senren_save_t save;
            senren_save_from_player(&app->player, &save);
            const bool ok = is_auto ? senren_auto_store(&save) : senren_slot_store(slot, &save);
            if (ok && is_auto) app->have_auto = true;
            notify(app, ok ? "保存成功" : "保存失败");
            slots_refresh(app);
        } else {
            senren_save_t save;
            const bool ok = is_auto ? senren_auto_load(&save) : senren_slot_load(slot, &save);
            if (ok && load_save(app, &save)) {
                return;
            }
            notify(app, "该存档为空");
        }
    } else if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_LONG) {
        // 存档模式:长按删除当前档;其他情况返回上一层。
        if (app->slots_saving && app->slots_sel < back_index) {
            const bool is_auto = slot_row_is_auto(app->slots_sel);
            const uint8_t slot = (uint8_t)(app->slots_sel - 1);
            const bool ok = is_auto ? senren_auto_clear() : senren_slot_clear(slot);
            if (ok && is_auto) app->have_auto = false;
            notify(app, ok ? "已删除" : "删除失败");
            slots_refresh(app);
            return;
        }
        set_page(app, app->slots_return);
        if (app->slots_return == ATRI_PAGE_TITLE) title_refresh(app);
    }
}

static void key_game(atri_app_t *app, const atri_key_t *key)
{
    // 章节卡:任意键跳过(过场计时结束也会自动跳过)。
    if (app->transition_pending) {
        app->transition_pending = false;
        advance_reading(app, true);
        return;
    }
    // 长按下 = 自动阅读模式开关(按固定节奏自动翻页)。
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_DOWN) {
        app->auto_play = !app->auto_play;
        app->auto_ms = 0;
        app->fast_forward = false;
        atri_ui_set_auto(&app->ui, app->auto_play);
        notify(app, app->auto_play ? "自动阅读:开" : "自动阅读:关");
        return;
    }
    // 长按上 = 快进(按住就一直推,松手停)。
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_UP) {
        app->fast_forward = true;
        app->fast_forward_ms = ATRI_FASTFORWARD_MS;
        if (!app->at_choice) advance_reading(app, true);
        return;
    }
    if (key->ev == BSP_BTN_RELEASE) {
        app->fast_forward = false;
        return;
    }
    if (app->at_choice) {
        if (key->btn == BSP_BTN_UP && key->ev == BSP_BTN_CLICK) {
            atri_ui_set_choice_selected(&app->ui, atri_ui_choice_selected(&app->ui) - 1);
        } else if (key->btn == BSP_BTN_DOWN && key->ev == BSP_BTN_CLICK) {
            atri_ui_set_choice_selected(&app->ui, atri_ui_choice_selected(&app->ui) + 1);
        } else if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_CLICK) {
            if (senren_player_choose(&app->player, &app->scn,
                                     (uint8_t)atri_ui_choice_selected(&app->ui),
                                     &app->layout_hint)) {
                app->at_choice = false;
                advance_reading(app, true);
                auto_save(app);   // 存下选项之后的新位置
            }
        }
        return;
    }
    // 正文页:上/下推进对白(短按下一句 / 翻页),确定键(短按或长按)打开菜单。
    if (key->btn == BSP_BTN_OK && (key->ev == BSP_BTN_CLICK || key->ev == BSP_BTN_LONG)) {
        open_menu(app);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        advance_reading(app, false);
    }
}

// ---------------------------------------------------------------- 对外接口
bool atri_app_init(atri_app_t *app, const uint8_t *pack_data, uint32_t pack_size,
                   const uint8_t *scn_data, uint32_t scn_size, uint16_t *art_pixels,
                   const lv_font_t *font_cjk)
{
    if (!app || !pack_data || !scn_data || !art_pixels || !font_cjk) return false;
    memset(app, 0, sizeof(*app));
    app->battery_percent = -1;

    if (!senren_pack_open(&app->pack, pack_data, pack_size)) {
        ESP_LOGE(TAG, "图片包解析失败(%u 字节)", (unsigned)pack_size);
        return false;
    }
    if (!senren_scn_open(&app->scn, scn_data, scn_size)) {
        ESP_LOGE(TAG, "剧本包解析失败(%u 字节)", (unsigned)scn_size);
        return false;
    }
    if (!senren_save_init()) {
        ESP_LOGW(TAG, "NVS 不可用,本次运行不保存进度");
    } else {
        senren_settings_default(&app->settings);
        (void)senren_settings_load(&app->settings);
    }
    if (app->settings.text_speed > 2) app->settings.text_speed = ATRI_SPEED_DEFAULT;

    // 分页参数与界面上的正文带一致:每行 26 个单位(13 个全角字)、每页 5 行。
    app->layout_hint.units_per_line = ATRI_UNITS_PER_LINE;
    app->layout_hint.lines_per_page = ATRI_TEXT_LINES;

    if (!atri_ui_create(&app->ui, art_pixels, font_cjk)) {
        ESP_LOGE(TAG, "界面创建失败");
        return false;
    }

    senren_save_t save;
    app->have_auto = senren_auto_load(&save);

    if (!app->settings.seen_tips) {
        static const char tips[] =
            "同人移植阅读器\n\n"
            "本机运行的是《千恋＊万花》阅读器,剧本与素材"
            "来自《千恋＊万花》的同人移植工程,"
            "仅供个人学习与交流,请支持正版。\n\n"
            "上/下 翻页\n"
            "确定 继续\n"
            "长按确定 菜单\n"
            "长按上 快进\n"
            "长按下 自动播放";
        atri_ui_set_warning(&app->ui, tips, "知道了");
        set_page(app, ATRI_PAGE_WARNING);
    } else {
        show_title(app);
    }

    ESP_LOGI(TAG, "就绪[v3 小块]:块 %u / 小块 %u / 字符 %u / 说话人 %u / 立绘 %u / 事件图 %u / 背景 %u / 结局 %u",
             (unsigned)app->scn.chunk_count, (unsigned)app->scn.block_count,
             (unsigned)app->scn.char_count, (unsigned)app->scn.speaker_count,
             (unsigned)app->scn.sprite_count, (unsigned)app->scn.event_count,
             (unsigned)app->scn.bg_count, (unsigned)app->scn.ending_count);
    ESP_LOGI(TAG, "图片包:条目 %u(背景 %u 池 / 立绘 %u 池 / 事件 %u 池)",
             (unsigned)app->pack.entry_count, (unsigned)app->scn.bg_count,
             (unsigned)app->scn.sprite_count, (unsigned)app->scn.event_count);
    return true;
}

void atri_app_key(atri_app_t *app, const atri_key_t *key)
{
    if (!app || !key) return;
    app->idle_ms = 0;
    app->notice_ms = 0;
    atri_ui_notice(&app->ui, "");

    // 自动阅读模式:真实的“按键”(单击/长按)才停下来,把控制权交回玩家。
    // 注意不能把 PRESS/RELEASE 也算进去 —— 长按下的抬起事件会在刚开完开关后
    // 立刻把自动模式关掉,表现就是“开了不自动”。
    const bool is_auto_toggle =
        (key->btn == BSP_BTN_DOWN && key->ev == BSP_BTN_LONG && app->page == ATRI_PAGE_GAME);
    const bool is_real_press = (key->ev == BSP_BTN_CLICK || key->ev == BSP_BTN_LONG);
    if (app->auto_play && is_real_press && !is_auto_toggle) {
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
            open_settings(app, app->settings_return);
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
    // 自动阅读/快进是应用自己在推进画面,玩家不需要碰机器,这段时间不能算空闲 ——
    // 否则会读到一半自己变暗、熄屏甚至休眠。停在选项或结局上时两者都已停下,
    // 照旧按时限熄灭。
    if (atri_app_auto_reading(app) || app->fast_forward) {
        app->idle_ms = 0;
    } else {
        app->idle_ms += elapsed_ms;
    }

    if (app->notice_ms > 0) {
        app->notice_ms = app->notice_ms > elapsed_ms ? app->notice_ms - elapsed_ms : 0;
        if (app->notice_ms == 0) atri_ui_notice(&app->ui, "");
    }
    if (app->transition_pending) {
        if (app->transition_ms > elapsed_ms) {
            app->transition_ms -= elapsed_ms;
        } else {
            app->transition_pending = false;
            advance_reading(app, true);
        }
    }
    // 自动阅读:打字机打完后固定等 ATRI_AUTO_MS 再翻到下一句。
    if (app->auto_play && app->page == ATRI_PAGE_GAME && !app->transition_pending) {
        if (atri_ui_typing(&app->ui) || app->at_choice || app->ended) {
            app->auto_ms = ATRI_AUTO_MS;   // 还在打字 / 等玩家选择:倒计时保持在满格
        } else if (app->auto_ms > elapsed_ms) {
            app->auto_ms -= elapsed_ms;
        } else {
            app->auto_ms = ATRI_AUTO_MS;
            advance_reading(app, false);
        }
    }

    // 快进:长按上时每 ATRI_FASTFORWARD_MS 推进一步。
    if (app->fast_forward && app->page == ATRI_PAGE_GAME && !app->transition_pending) {
        app->fast_forward_ms += elapsed_ms;
        while (app->fast_forward_ms >= ATRI_FASTFORWARD_MS) {
            app->fast_forward_ms -= ATRI_FASTFORWARD_MS;
            advance_reading(app, true);
            if (!app->fast_forward || app->at_choice || app->transition_pending) break;
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

bool atri_app_auto_reading(const atri_app_t *app)
{
    if (!app) return false;
    return app->auto_play && app->page == ATRI_PAGE_GAME && !app->at_choice && !app->ended;
}

uint32_t atri_app_idle_ms(const atri_app_t *app)
{
    return app ? app->idle_ms : 0;
}

void atri_app_before_sleep(atri_app_t *app)
{
    if (!app) return;
    if (app->page == ATRI_PAGE_GAME) auto_save(app);
    (void)senren_settings_store(&app->settings);
}

void atri_app_show_sleeping(atri_app_t *app)
{
    if (!app) return;
    atri_ui_set_ending(&app->ui, "", "休眠中", "按任意键唤醒");
    set_page(app, ATRI_PAGE_ENDING);
}

// ---------------------------------------------------------------- 调试定位
// 顺流推进到第 chapter 章(1 起;0 = 剧本开头)之后的第 step 句正文,让
// player 正好停在那一句上。返回 false 表示整本读完都没找到。
static bool debug_seek(atri_app_t *app, uint16_t chapter, uint16_t step)
{
    if (!senren_player_start(&app->player, &app->scn, 0, 0, 0, &app->layout_hint)) return false;
    app->at_choice = false;
    app->ended = false;

    bool found = chapter == 0;
    uint16_t texts = 0;
    for (uint32_t guard = 0; guard < 400000u; ++guard) {
        switch (senren_player_advance(&app->player, &app->scn, &app->layout_hint)) {
        case SENREN_STEP_CHAPTER: {
            uint16_t number = 0;
            if (!chapter_card_number(app->player.chapter_title, &number)) break;
            app->player.chapter = number;
            if (!found && number == chapter) {
                found = true;
                texts = 0;
            }
            break;
        }
        case SENREN_STEP_TEXT:
            if (!found) break;
            if (texts == step) return true;
            ++texts;
            break;
        case SENREN_STEP_CHOICE:
            if (found && texts == step) {
                app->at_choice = true;
                return true;
            }
            return false;
        case SENREN_STEP_ENDING:
        case SENREN_STEP_STUCK:
            ESP_LOGE(TAG, "剧情数据异常: %s", senren_get_error());
        default:
            return false;
        }
    }
    return false;
}

bool atri_app_debug_render(atri_app_t *app, uint16_t chapter, uint16_t step)
{
    if (!app) return false;
    if (!debug_seek(app, chapter, step)) return false;
    app->rendered_valid = false;   // 强制按定位到的名字重画
    render_art(app);
    ESP_LOGW(TAG, "调试:画到第 %u 章第 %u 句(块 %u / 节点 %u)", (unsigned)chapter,
             (unsigned)step, (unsigned)app->player.chunk, (unsigned)app->player.node);
    return true;
}

bool atri_app_debug_title(atri_app_t *app)
{
    if (!app) return false;
    // 和真实的标题页走同一条路:先把标题画渲染进画面区,再切页——否则截图会是黑底。
    show_title(app);
    return true;
}

bool atri_app_debug_start(atri_app_t *app, uint16_t chapter, uint16_t step)
{
    if (!app) return false;
    if (!debug_seek(app, chapter, step)) return false;
    app->started = true;
    app->ended = false;
    app->transition_pending = false;
    app->rendered_valid = false;   // 强制按定位到的名字重画
    set_page(app, ATRI_PAGE_GAME);
    render_scene(app);
    ESP_LOGW(TAG, "调试:从第 %u 章第 %u 句开始阅读(块 %u / 节点 %u)", (unsigned)chapter,
             (unsigned)step, (unsigned)app->player.chunk, (unsigned)app->player.node);
    return true;
}

bool atri_app_take_sleep_request(atri_app_t *app)
{
    if (!app || !app->sleep_requested) return false;
    app->sleep_requested = false;
    return true;
}
