// main/dracu_app.c —— 应用状态机实现(《DRACU-RIOT!》阅读器)。
#include "dracu_app.h"

#include "bsp_battery.h"
#include "bsp_button.h"

#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "dracu_app";

#define DRACU_BATTERY_INTERVAL_MS 30000u
#define DRACU_NOTICE_MS 2000u
#define DRACU_TRANSITION_MS 1400u
// 长按上开始快进时,每一屏之间的间隔。
#define DRACU_FASTFORWARD_MS 110u
// 自动阅读:打字机打完之后再等这么久翻到下一页(与源工程的 0.9s 一致)。
#define DRACU_AUTO_MS 900u
// 标题页 4 行:开始阅读 / 继续阅读 / 读取进度 / 系统设置。
#define DRACU_TITLE_ROWS 5
// 存档页:自动存档 + 5 个手动存档 + 返回。
#define DRACU_SLOT_ROWS (DRACU_SAVE_SLOTS + 2)
// 章节列表页一次显示几行(与界面里的行数一致)
#define DRACU_CHAPTER_ROWS DRACU_UI_MAX_ROWS

// 打字机速度(毫秒/字),对应设置页的 慢/中/快。
static const uint32_t s_speed_ms[3] = { 60, 38, 18 };
static const char *const s_speed_names[3] = { "慢", "中", "快" };
#define DRACU_SPEED_DEFAULT 1   // 中速

static void set_page(dracu_app_t *app, dracu_screen_t page)
{
    app->page = page;
    dracu_ui_show_page(&app->ui, page);
}

static void notify(dracu_app_t *app, const char *text)
{
    snprintf(app->notice, sizeof(app->notice), "%s", text ? text : "");
    app->notice_ms = DRACU_NOTICE_MS;
    dracu_ui_notice(&app->ui, app->notice);
}

static uint32_t speed_ms(const dracu_app_t *app)
{
    const uint8_t level =
        app->settings.text_speed > 2 ? DRACU_SPEED_DEFAULT : app->settings.text_speed;
    return s_speed_ms[level];
}

// 页面切换(定义在后面,推进流程里会用到)
static void show_title(dracu_app_t *app);
static void show_ending(dracu_app_t *app);

// ---------------------------------------------------------------- 画面区
// 当前页要画的四个图层。页记录里的 0xFF / 0xFFFF 表示"没有"。
static void current_layers(const dracu_app_t *app, dracu_layers_t *layers)
{
    // 页记录里的背景 / SD 是 u8(0xFF = 没有),画布层统一用 DRACU_NO_ENTRY
    layers->bg = app->player.record.bg == DRACU_SCN_NONE8 ? DRACU_NO_ENTRY
                                                          : app->player.record.bg;
    layers->cg = app->player.record.cg;
    layers->sd = app->player.record.sd == DRACU_SCN_NONE8 ? DRACU_NO_ENTRY
                                                          : app->player.record.sd;
    layers->sprite = app->player.record.sprite;
}

// 按当前页合成画面区(图层没变时内部只重画正文带)。
static void render_art(dracu_app_t *app)
{
    dracu_layers_t layers;
    current_layers(app, &layers);
    dracu_image_compose(&app->ui.art, &layers);
}

// 标题页:用图片包里的标题图(背景 id 0)整屏铺底。
static void render_title_art(dracu_app_t *app)
{
    dracu_layers_t layers = {
        .bg = app->title_bg,
        .cg = DRACU_NO_ENTRY,
        .sd = DRACU_NO_ENTRY,
        .sprite = DRACU_NO_ENTRY,
    };
    dracu_image_compose(&app->ui.art, &layers);
}

// 章节文案(源数据的章节名形如 "美羽 第1话 意识")。
static void chapter_label(dracu_app_t *app, uint32_t chapter, char *out, size_t capacity)
{
    out[0] = '\0';
    if (chapter == DRACU_CHAPTER_NONE) {
        return;
    }
    dracu_chapter_t entry;
    if (dracu_scn_chapter(&app->scn, chapter, &entry)) {
        dracu_scn_string(&app->scn, entry.name, out, capacity);
    }
}

// 把当前这一步的正文/选项/浮层画出来。
static void render_scene(dracu_app_t *app)
{
    render_art(app);

    char label[DRACU_NAME_MAX];
    chapter_label(app, app->player.chapter, label, sizeof(label));
    dracu_ui_set_progress(&app->ui, label, (int)app->player.page_index + 1,
                          (int)app->player.page_count);

    if (app->at_choice) {
        const char *texts[DRACU_CHOICE_MAX];
        char storage[DRACU_CHOICE_MAX][DRACU_NAME_MAX];
        int count = app->player.choice_view.count;
        if (count > DRACU_CHOICE_MAX) count = DRACU_CHOICE_MAX;
        for (int index = 0; index < count; ++index) {
            dracu_scn_text(&app->scn, app->player.choice_view.text_off[index],
                           app->player.choice_view.text_len[index], storage[index],
                           sizeof(storage[index]));
            texts[index] = storage[index];
        }
        dracu_ui_set_text(&app->ui, "", "", 0);
        dracu_ui_show_choices(&app->ui, texts, count);
        return;
    }

    dracu_ui_hide_choices(&app->ui);
    char speaker[DRACU_NAME_MAX] = { 0 };
    char text[DRACU_TEXT_BUFFER] = { 0 };
    dracu_player_speaker(&app->player, &app->scn, speaker, sizeof(speaker));
    dracu_player_page_text(&app->player, text, sizeof(text));
    // 快进时整句直接铺满,不要一个字一个字往外吐。
    dracu_ui_set_text(&app->ui, speaker, text, app->fast_forward ? 0 : speed_ms(app));
}

// 章节卡:显示章节名,等任意键或过场计时结束再往下读。
static void show_chapter_card(dracu_app_t *app, const char *label)
{
    char text[DRACU_NAME_MAX + 8];
    snprintf(text, sizeof(text), "%s", label);
    dracu_ui_hide_choices(&app->ui);
    dracu_ui_set_progress(&app->ui, label, 0, 0);
    render_art(app);
    dracu_ui_set_text(&app->ui, "", text, 0);
    set_page(app, DRACU_PAGE_GAME);
    app->transition_pending = true;
    app->transition_ms = DRACU_TRANSITION_MS;
}

static void auto_save(dracu_app_t *app)
{
    dracu_save_t save;
    dracu_save_from_player(&app->player, &save);
    if (dracu_auto_store(&save)) {
        app->have_auto = true;
    }
}

// ---------------------------------------------------------------- 阅读推进
// 往前读一步:打字中先显示全文,同一页还有下一屏先翻屏,否则走状态机。
// skip_typing 为真时(快进/自动阅读/切页后恢复)直接推进。
static void advance_reading(dracu_app_t *app, bool skip_typing)
{
    if (!skip_typing && dracu_ui_typing(&app->ui)) {
        dracu_ui_finish_typing(&app->ui);
        return;
    }

    const uint32_t before_chapter = app->player.chapter;
    for (int guard = 0; guard < 64; ++guard) {
        switch (dracu_player_advance(&app->player, &app->scn, &app->layout_hint)) {
        case DRACU_STEP_TEXT:
            app->at_choice = false;
            render_scene(app);
            return;
        case DRACU_STEP_PAGE:
            app->at_choice = false;
            if (app->player.chapter != before_chapter &&
                app->player.chapter != DRACU_CHAPTER_NONE) {
                auto_save(app);
                char label[DRACU_NAME_MAX];
                chapter_label(app, app->player.chapter, label, sizeof(label));
                show_chapter_card(app, label);
                return;
            }
            render_scene(app);
            return;
        case DRACU_STEP_CHOICE:
            app->at_choice = true;
            auto_save(app);
            app->fast_forward = false;   // 选项页停下,交回玩家
            app->auto_play = false;
            dracu_ui_set_auto(&app->ui, false);
            render_scene(app);
            return;
        case DRACU_STEP_ENDING:
            app->at_choice = false;
            app->fast_forward = false;
            app->auto_play = false;
            dracu_ui_set_auto(&app->ui, false);
            show_ending(app);
            return;
        case DRACU_STEP_STUCK:
        default:
            app->at_choice = false;
            app->fast_forward = false;
            app->auto_play = false;
            dracu_ui_set_auto(&app->ui, false);
            ESP_LOGE(TAG, "剧情推进失败(页 %u):%s", (unsigned)app->player.page,
                     dracu_get_error());
            notify(app, "剧情数据异常");
            show_title(app);
            return;
        }
    }
    ESP_LOGE(TAG, "连续 64 步都没能推进: %s", dracu_get_error());
    show_title(app);
}

// 往回读一屏(菜单与调试用;堆死页由数据层拦住)。
static void back_reading(dracu_app_t *app)
{
    if (dracu_ui_typing(&app->ui)) {
        dracu_ui_finish_typing(&app->ui);
        return;
    }
    switch (dracu_player_back(&app->player, &app->scn, &app->layout_hint)) {
    case DRACU_STEP_TEXT:
    case DRACU_STEP_PAGE:
    case DRACU_STEP_CHOICE:
    case DRACU_STEP_ENDING:
        app->at_choice = app->player.at_choice;
        app->ended = app->player.ended;
        render_scene(app);
        return;
    default:
        notify(app, "已经是开头");
        return;
    }
}

// ---------------------------------------------------------------- 列表页
static void open_menu(dracu_app_t *app)
{
    static const char *const labels[] = { "继续阅读", "保存进度", "读取进度", "跳过章节",
                                          "返回标题" };
    app->menu_sel = 0;
    dracu_ui_set_list(&app->ui, DRACU_LIST_MENU, "菜单", "上/下选择 · 确定确认 · 长按确定关闭",
                      labels, NULL, (int)(sizeof(labels) / sizeof(labels[0])), app->menu_sel);
    set_page(app, DRACU_PAGE_MENU);
}

static void open_settings(dracu_app_t *app, dracu_screen_t from)
{
    static const char *const labels[] = { "文字速度", "关于本作", "关机", "返回" };
    const uint8_t level =
        app->settings.text_speed > 2 ? DRACU_SPEED_DEFAULT : app->settings.text_speed;
    const char *values[] = { s_speed_names[level], NULL, NULL, NULL };
    app->settings_return = from;
    app->settings_sel = 0;
    dracu_ui_set_list(&app->ui, DRACU_LIST_SETTINGS, "系统设置", "上/下选择 · 确定确认", labels,
                      values, (int)(sizeof(labels) / sizeof(labels[0])), app->settings_sel);
    set_page(app, DRACU_PAGE_SETTINGS);
}

static void open_about(dracu_app_t *app)
{
    static const char body[] =
        "《DRACU-RIOT!》阅读器\n\n"
        "原作 Yuzusoft(柚子社)\n"
        "剧本与素材来自小米手环同人移植版\n"
        "(github.com/hezdaaa/dracu-riot-miband)\n\n"
        "本移植仅供个人学习交流,请支持正版\n\n"
        "上/下 翻页\n"
        "确定 继续\n"
        "长按确定 菜单\n"
        "长按上 快进\n"
        "长按下 自动阅读\n\n"
        "按确定返回";
    dracu_ui_set_about(&app->ui, body);
    app->about_scroll = 0;
    dracu_ui_set_about_scroll(&app->ui, 0);
    set_page(app, DRACU_PAGE_ABOUT);
}

// 存档页:自动存档 + 5 个手动存档 + 返回(顺序固定,两种模式一样)。
static void slots_refresh(dracu_app_t *app)
{
    static char labels[DRACU_UI_MAX_ROWS][16];
    static char values[DRACU_UI_MAX_ROWS][DRACU_NAME_MAX + 8];
    const char *label_ptrs[DRACU_UI_MAX_ROWS];
    const char *value_ptrs[DRACU_UI_MAX_ROWS];
    int n = 0;
    dracu_save_t save;
    char chapter[DRACU_NAME_MAX];

    snprintf(labels[n], sizeof(labels[n]), "自动存档");
    if (dracu_auto_load(&save)) {
        chapter_label(app, save.chapter, chapter, sizeof(chapter));
        snprintf(values[n], sizeof(values[n]), "%s", chapter[0] ? chapter : "阅读中");
        value_ptrs[n] = values[n];
    } else {
        value_ptrs[n] = "空";
    }
    label_ptrs[n] = labels[n];
    ++n;

    for (int slot = 0; slot < DRACU_SAVE_SLOTS; ++slot) {
        snprintf(labels[n], sizeof(labels[n]), "存档 %d", slot + 1);
        if (dracu_slot_load((uint8_t)slot, &save)) {
            chapter_label(app, save.chapter, chapter, sizeof(chapter));
            snprintf(values[n], sizeof(values[n]), "%s", chapter[0] ? chapter : "阅读中");
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
    dracu_ui_set_list(&app->ui, DRACU_LIST_SLOTS, app->slots_saving ? "保存进度" : "读取进度",
                      app->slots_saving ? "确定覆盖 · 长按确定删除" : "确定读取", label_ptrs,
                      value_ptrs, n, app->slots_sel);
}

static void open_slots(dracu_app_t *app, bool saving, dracu_screen_t from)
{
    app->slots_saving = saving;
    app->slots_return = from;
    app->slots_sel = 0;
    slots_refresh(app);
    set_page(app, DRACU_PAGE_SLOTS);
}

// 章节列表:源工程的流程图页(lct.ux)就是一张"路线 -> 章节 -> 页号"的清单。
// 三键设备上一次显示 7 行,选中的行滚出窗口时整屏跟着滚。
static void chapters_refresh(dracu_app_t *app)
{
    static char labels[DRACU_CHAPTER_ROWS][DRACU_NAME_MAX + 8];
    const char *label_ptrs[DRACU_CHAPTER_ROWS];
    const uint32_t count = dracu_scn_chapter_count(&app->scn);
    if (count == 0) {
        return;
    }
    if (app->chapters_sel >= (int)count) app->chapters_sel = (int)count - 1;
    if (app->chapters_sel < 0) app->chapters_sel = 0;
    int first = app->chapters_sel - DRACU_CHAPTER_ROWS / 2;
    if (first > (int)count - DRACU_CHAPTER_ROWS) first = (int)count - DRACU_CHAPTER_ROWS;
    if (first < 0) first = 0;
    int rows = 0;
    for (int row = 0; row < DRACU_CHAPTER_ROWS && first + row < (int)count; ++row) {
        dracu_chapter_t entry;
        if (!dracu_scn_chapter(&app->scn, (uint32_t)(first + row), &entry)) {
            break;
        }
        dracu_scn_string(&app->scn, entry.name, labels[row], sizeof(labels[row]));
        label_ptrs[row] = labels[row];
        ++rows;
    }
    char title[32];
    snprintf(title, sizeof(title), "章节(%d/%u)", app->chapters_sel + 1, (unsigned)count);
    dracu_ui_set_list(&app->ui, DRACU_LIST_CHAPTERS, title, "上/下选择 · 确定跳转 · 长按确定返回",
                      label_ptrs, NULL, rows, app->chapters_sel - first);
}

static void open_chapters(dracu_app_t *app, dracu_screen_t from)
{
    app->chapters_return = from;
    app->chapters_sel = 0;
    // 从当前阅读位置附近的章节开始,免得每次都从头翻。
    if (app->started) {
        uint32_t chapter = dracu_chapter_of_page(&app->scn, app->player.page);
        if (chapter != DRACU_CHAPTER_NONE) {
            app->chapters_sel = (int)chapter;
        }
    }
    chapters_refresh(app);
    set_page(app, DRACU_PAGE_CHAPTERS);
}

static void title_refresh(dracu_app_t *app)
{
    static char auto_value[DRACU_NAME_MAX + 8];
    static const char *value_ptrs[DRACU_UI_MAX_ROWS];
    static const char *labels[DRACU_UI_MAX_ROWS];
    int n = 0;

    labels[n] = "开始阅读";
    value_ptrs[n] = NULL;
    ++n;

    // "继续阅读"指向自动存档(每次换章/选选项/睡前写入)。
    dracu_save_t save;
    if (app->have_auto && dracu_auto_load(&save)) {
        char chapter[DRACU_NAME_MAX];
        chapter_label(app, save.chapter, chapter, sizeof(chapter));
        snprintf(auto_value, sizeof(auto_value), "%s", chapter[0] ? chapter : "阅读中");
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
    // 章节跳转(源工程的流程图页):不是"开始游戏"的入口 —— galgame 的流程是
    // 从头一路读下去,章节列表只是给回看/跳线用的辅助入口。
    labels[n] = "章节跳转";
    value_ptrs[n] = NULL;
    ++n;
    labels[n] = "系统设置";
    value_ptrs[n] = NULL;
    ++n;

    if (app->title_sel >= n) app->title_sel = 0;
    dracu_ui_set_list(&app->ui, DRACU_LIST_TITLE, NULL, NULL, labels, value_ptrs, n,
                      app->title_sel);
}

static void show_title(dracu_app_t *app)
{
    dracu_ui_hide_choices(&app->ui);
    dracu_ui_set_auto(&app->ui, false);
    app->at_choice = false;
    app->ended = false;
    app->fast_forward = false;
    app->auto_play = false;
    app->transition_pending = false;
    render_title_art(app);
    title_refresh(app);
    set_page(app, DRACU_PAGE_TITLE);
}

static bool start_reading(dracu_app_t *app, uint32_t page)
{
    if (!dracu_player_start(&app->player, &app->scn, page, &app->layout_hint)) {
        notify(app, "剧本数据异常");
        return false;
    }
    app->started = true;
    app->at_choice = app->player.at_choice;
    app->ended = app->player.ended;
    app->transition_pending = false;
    set_page(app, DRACU_PAGE_GAME);
    render_scene(app);
    return true;
}

// 从存档接上:位置与选择历史都来自存档。
static bool load_save(dracu_app_t *app, const dracu_save_t *save)
{
    if (!dracu_player_resume(&app->player, &app->scn, save->page, save->choice,
                             &app->layout_hint)) {
        return false;
    }
    app->started = true;
    app->at_choice = app->player.at_choice;
    app->ended = app->player.ended;
    app->transition_pending = false;
    set_page(app, DRACU_PAGE_GAME);
    if (app->ended) {
        show_ending(app);
        return true;
    }
    render_scene(app);
    return true;
}

static void show_ending(dracu_app_t *app)
{
    app->ended = true;
    dracu_ui_hide_choices(&app->ui);
    char name[DRACU_NAME_MAX] = { 0 };
    if (app->player.end_name != DRACU_SCN_NONE16) {
        dracu_scn_string(&app->scn, app->player.end_name, name, sizeof(name));
    }
    dracu_ui_set_ending(&app->ui, "全剧终", name[0] ? name : "感谢阅读", "按确定返回标题");
    set_page(app, DRACU_PAGE_ENDING);

    // 通关后不再保留自动存档:标题页的"继续阅读"应指向未结束的进度。
    (void)dracu_auto_clear();
    app->have_auto = false;
}

// ---------------------------------------------------------------- 按键
static void key_list_move(dracu_app_t *app, int *selected, int count, int delta)
{
    if (count <= 0) return;
    *selected += delta;
    if (*selected < 0) *selected = count - 1;
    if (*selected >= count) *selected = 0;
    dracu_list_id_t id = DRACU_LIST_TITLE;
    if (selected == &app->menu_sel) id = DRACU_LIST_MENU;
    else if (selected == &app->settings_sel) id = DRACU_LIST_SETTINGS;
    else if (selected == &app->slots_sel) id = DRACU_LIST_SLOTS;
    if (selected == &app->chapters_sel) {
        chapters_refresh(app);
        return;
    }
    dracu_ui_set_list_selected(&app->ui, id, *selected);
}

static void title_select(dracu_app_t *app)
{
    switch (app->title_sel) {
    case 0:   // 开始阅读:从第 1 页(序幕)开始,一路连续读到结局
        if (start_reading(app, 1)) {
            (void)dracu_auto_clear();   // 新游戏:旧的自动续读点作废
            app->have_auto = false;
            auto_save(app);
        }
        return;
    case 1: {   // 继续阅读:接自动存档
        dracu_save_t save;
        if (app->have_auto && dracu_auto_load(&save) && load_save(app, &save)) {
            return;
        }
        notify(app, "还没有可继续的进度");
        app->have_auto = false;
        title_refresh(app);
        return;
    }
    case 2:   // 读取进度
        open_slots(app, false, DRACU_PAGE_TITLE);
        return;
    case 3:   // 章节跳转(流程图)
        open_chapters(app, DRACU_PAGE_TITLE);
        return;
    default:   // 系统设置
        open_settings(app, DRACU_PAGE_TITLE);
        return;
    }
}

static void key_title(dracu_app_t *app, const dracu_key_t *key)
{
    if (key->btn == BSP_BTN_UP && key->ev == BSP_BTN_CLICK) {
        key_list_move(app, &app->title_sel, DRACU_TITLE_ROWS, -1);
    } else if (key->btn == BSP_BTN_DOWN && key->ev == BSP_BTN_CLICK) {
        key_list_move(app, &app->title_sel, DRACU_TITLE_ROWS, 1);
    } else if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_CLICK) {
        title_select(app);
    } else if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_LONG) {
        app->sleep_requested = true;   // 标题页长按确定 = 关机
    }
}

static void key_chapters(dracu_app_t *app, const dracu_key_t *key)
{
    const uint32_t count = dracu_scn_chapter_count(&app->scn);
    if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_UP) {
        key_list_move(app, &app->chapters_sel, (int)count, -1);
    } else if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_DOWN) {
        key_list_move(app, &app->chapters_sel, (int)count, 1);
    } else if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_CLICK) {
        dracu_chapter_t entry;
        if (!dracu_scn_chapter(&app->scn, (uint32_t)app->chapters_sel, &entry)) {
            return;
        }
        if (start_reading(app, entry.page)) {
            (void)dracu_auto_clear();
            app->have_auto = false;
            auto_save(app);
        }
    } else if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_LONG) {
        set_page(app, app->chapters_return);
        if (app->chapters_return == DRACU_PAGE_TITLE) title_refresh(app);
    }
}

static bool skip_chapter_action(dracu_app_t *app)
{
    const uint32_t before = app->player.page;
    const dracu_step_t step = dracu_player_skip_chapter(&app->player, &app->scn,
                                                        &app->layout_hint);
    if (step == DRACU_STEP_STUCK) {
        ESP_LOGI(TAG, "跳过章节: 没有下一章(%s)", dracu_get_error());
        return false;
    }
    ESP_LOGI(TAG, "跳过章节: 页 %u -> 页 %u", (unsigned)before, (unsigned)app->player.page);
    app->at_choice = app->player.at_choice;
    set_page(app, DRACU_PAGE_GAME);
    auto_save(app);
    if (step == DRACU_STEP_ENDING) {
        show_ending(app);
        return true;
    }
    render_scene(app);
    return true;
}

static void key_menu(dracu_app_t *app, const dracu_key_t *key)
{
    const int count = 5;
    if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_UP) {
        key_list_move(app, &app->menu_sel, count, -1);
    } else if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_DOWN) {
        key_list_move(app, &app->menu_sel, count, 1);
    } else if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_CLICK) {
        switch (app->menu_sel) {
        case 0:   // 继续阅读:回到正文页(画面区与文字按当前状态重画)
            set_page(app, DRACU_PAGE_GAME);
            render_scene(app);
            break;
        case 1:   // 保存进度
            open_slots(app, true, DRACU_PAGE_MENU);
            break;
        case 2:   // 读取进度
            open_slots(app, false, DRACU_PAGE_MENU);
            break;
        case 3:   // 跳过章节:直接跳到下一章的起始页
            if (!skip_chapter_action(app)) {
                notify(app, "已经是最后一章");
            }
            break;
        default:  // 返回标题
            show_title(app);
            break;
        }
    } else if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_LONG) {
        set_page(app, DRACU_PAGE_GAME);
        render_scene(app);
    }
}

static void key_settings(dracu_app_t *app, const dracu_key_t *key)
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
            dracu_ui_set_list_value(&app->ui, DRACU_LIST_SETTINGS, 0,
                                    s_speed_names[app->settings.text_speed]);
            (void)dracu_settings_store(&app->settings);
            break;
        case 1:   // 关于本作
            open_about(app);
            break;
        case 2:   // 关机
            app->sleep_requested = true;
            break;
        default:  // 返回
            set_page(app, app->settings_return);
            if (app->settings_return == DRACU_PAGE_TITLE) title_refresh(app);
            break;
        }
    } else if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_LONG) {
        set_page(app, app->settings_return);
        if (app->settings_return == DRACU_PAGE_TITLE) title_refresh(app);
    }
}

// 存档页里第 0 行是自动存档(行 1..DRACU_SAVE_SLOTS 是手动存档,最后一行是返回)。
static bool slot_row_is_auto(int row)
{
    return row == 0;
}

static void key_slots(dracu_app_t *app, const dracu_key_t *key)
{
    const int count = DRACU_SLOT_ROWS;
    const int back_index = count - 1;
    if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_UP) {
        key_list_move(app, &app->slots_sel, count, -1);
    } else if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_DOWN) {
        key_list_move(app, &app->slots_sel, count, 1);
    } else if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_CLICK) {
        if (app->slots_sel == back_index) {
            set_page(app, app->slots_return);
            if (app->slots_return == DRACU_PAGE_TITLE) title_refresh(app);
            return;
        }
        const bool is_auto = slot_row_is_auto(app->slots_sel);
        const uint8_t slot = (uint8_t)(app->slots_sel - 1);
        if (app->slots_saving) {
            dracu_save_t save;
            dracu_save_from_player(&app->player, &save);
            const bool ok = is_auto ? dracu_auto_store(&save) : dracu_slot_store(slot, &save);
            if (ok && is_auto) app->have_auto = true;
            notify(app, ok ? "保存成功" : "保存失败");
            slots_refresh(app);
        } else {
            dracu_save_t save;
            const bool ok = is_auto ? dracu_auto_load(&save) : dracu_slot_load(slot, &save);
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
            const bool ok = is_auto ? dracu_auto_clear() : dracu_slot_clear(slot);
            if (ok && is_auto) app->have_auto = false;
            notify(app, ok ? "已删除" : "删除失败");
            slots_refresh(app);
            return;
        }
        set_page(app, app->slots_return);
        if (app->slots_return == DRACU_PAGE_TITLE) title_refresh(app);
    }
}

static void key_game(dracu_app_t *app, const dracu_key_t *key)
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
        dracu_ui_set_auto(&app->ui, app->auto_play);
        notify(app, app->auto_play ? "自动阅读:开" : "自动阅读:关");
        return;
    }
    // 长按上 = 快进(按住就一直推,松手停)。
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_UP) {
        app->fast_forward = true;
        app->fast_forward_ms = DRACU_FASTFORWARD_MS;
        if (!app->at_choice) advance_reading(app, true);
        return;
    }
    if (key->ev == BSP_BTN_RELEASE) {
        app->fast_forward = false;
        return;
    }
    if (app->at_choice) {
        if (key->btn == BSP_BTN_UP && key->ev == BSP_BTN_CLICK) {
            dracu_ui_set_choice_selected(&app->ui, dracu_ui_choice_selected(&app->ui) - 1);
        } else if (key->btn == BSP_BTN_DOWN && key->ev == BSP_BTN_CLICK) {
            dracu_ui_set_choice_selected(&app->ui, dracu_ui_choice_selected(&app->ui) + 1);
        } else if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_CLICK) {
            if (dracu_player_choose(&app->player, &app->scn,
                                    (uint8_t)dracu_ui_choice_selected(&app->ui),
                                    &app->layout_hint)) {
                app->at_choice = false;
                auto_save(app);   // 存下选项之后的新位置与选择历史
                render_scene(app);
            }
        }
        return;
    }
    // 正文页:上/下短按推进(反方向短按回退一屏),确定键打开菜单。
    if (key->btn == BSP_BTN_OK && (key->ev == BSP_BTN_CLICK || key->ev == BSP_BTN_LONG)) {
        open_menu(app);
        return;
    }
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        advance_reading(app, false);
    }
}

// ---------------------------------------------------------------- 对外接口
bool dracu_app_init(dracu_app_t *app, const uint8_t *pack_data, uint32_t pack_size,
                    const uint8_t *scn_data, uint32_t scn_size, uint16_t *art_pixels,
                    const lv_font_t *font_cjk)
{
    if (!app || !pack_data || !scn_data || !art_pixels || !font_cjk) return false;
    memset(app, 0, sizeof(*app));
    app->battery_percent = -1;

    if (!dracu_pack_open(&app->pack, pack_data, pack_size)) {
        ESP_LOGE(TAG, "图片包解析失败(%u 字节)", (unsigned)pack_size);
        return false;
    }
    if (!dracu_scn_open(&app->scn, scn_data, scn_size)) {
        ESP_LOGE(TAG, "剧本包解析失败(%u 字节)", (unsigned)scn_size);
        return false;
    }
    dracu_scn_attach(&app->scn, app->scn_scratch, sizeof(app->scn_scratch));
    if (!dracu_save_init()) {
        ESP_LOGW(TAG, "NVS 不可用,本次运行不保存进度");
    } else {
        dracu_settings_default(&app->settings);
        (void)dracu_settings_load(&app->settings);
    }
    if (app->settings.text_speed > 2) app->settings.text_speed = DRACU_SPEED_DEFAULT;

    // 分页参数与界面上的正文带一致:每行 26 个单位(13 个全角字)、每页 5 行。
    app->layout_hint.units_per_line = DRACU_UNITS_PER_LINE;
    app->layout_hint.lines_per_page = DRACU_TEXT_LINES;

    // 标题图:按名字查一次,换成背景池内的 id(池是按名字排序的,id 不固定)
    app->title_bg = DRACU_NO_ENTRY;
    {
        dracu_asset_t title;
        if (dracu_pack_find(&app->pack, DRACU_POOL_BG, DRACU_BG_TITLE_NAME, &title) &&
            dracu_pack_pool_id(&app->pack, title.index, DRACU_POOL_BG, &app->title_bg)) {
            ESP_LOGI(TAG, "标题图: %s -> 背景 id %u", DRACU_BG_TITLE_NAME,
                     (unsigned)app->title_bg);
        } else {
            ESP_LOGW(TAG, "图片包里没有标题图(背景名 %s),标题页将只有菜单", DRACU_BG_TITLE_NAME);
        }
    }

    dracu_player_reset(&app->player);

    if (!dracu_ui_create(&app->ui, art_pixels, font_cjk, &app->pack)) {
        ESP_LOGE(TAG, "界面创建失败");
        return false;
    }

    dracu_save_t save;
    app->have_auto = dracu_auto_load(&save);

    // 启动自检:把第 1 页读出来,页表/正文/字符表三条路都走一遍 —— 以前的移植里
    // "一进阅读就全剧终" 全是这里出问题(解块失败被当成剧情结束)。
    dracu_page_t first;
    if (!dracu_scn_page(&app->scn, 1, &first)) {
        ESP_LOGE(TAG, "自检失败: 第 1 页读不出来");
    } else {
        char sample[DRACU_TEXT_BUFFER];
        const size_t chars = dracu_scn_text(&app->scn, first.text_off, first.text_len, sample,
                                            sizeof(sample));
        ESP_LOGI(TAG, "自检: 页 1 正文 %u 字 / %u 字节: %s", (unsigned)first.text_len,
                 (unsigned)chars, sample);
    }

    // 开机直接进标题页 —— 不再有首次运行的「同人移植提示」页(2026-09-27 用户要求移除)。
    // 操作说明保留在「系统设置 → 关于本作」里,不占一次开机交互。
    show_title(app);

    ESP_LOGI(TAG, "就绪:页 %u / 块 %u(页表 %u) / 字符 %u / 字符串 %u / 选项 %u / 章节 %u",
             (unsigned)app->scn.page_count, (unsigned)app->scn.block_count,
             (unsigned)app->scn.page_block_count, (unsigned)app->scn.char_count,
             (unsigned)app->scn.string_count, (unsigned)app->scn.choice_count,
             (unsigned)app->scn.chapter_count);
    ESP_LOGI(TAG, "图片包:条目 %u(背景 %u / CG %u / SD %u / 身体 %u / 表情 %u / 立绘 %u)",
             (unsigned)app->pack.entry_count, (unsigned)app->pack.pool_count[DRACU_POOL_BG],
             (unsigned)app->pack.pool_count[DRACU_POOL_CG],
             (unsigned)app->pack.pool_count[DRACU_POOL_SD],
             (unsigned)app->pack.pool_count[DRACU_POOL_BODY],
             (unsigned)app->pack.pool_count[DRACU_POOL_FACE],
             (unsigned)app->pack.sprite_count);
    return true;
}

void dracu_app_key(dracu_app_t *app, const dracu_key_t *key)
{
    if (!app || !key) return;
    app->idle_ms = 0;
    app->notice_ms = 0;
    dracu_ui_notice(&app->ui, "");

    // 自动阅读模式:真实的"按键"(单击/长按)才停下来,把控制权交回玩家。
    // 注意不能把 PRESS/RELEASE 也算进去 —— 长按下的抬起事件会在刚开完开关后
    // 立刻把自动模式关掉,表现就是"开了不自动"。
    const bool is_auto_toggle =
        (key->btn == BSP_BTN_DOWN && key->ev == BSP_BTN_LONG && app->page == DRACU_PAGE_GAME);
    const bool is_real_press = (key->ev == BSP_BTN_CLICK || key->ev == BSP_BTN_LONG);
    if (app->auto_play && is_real_press && !is_auto_toggle) {
        app->auto_play = false;
        app->fast_forward = false;
        dracu_ui_set_auto(&app->ui, false);
    }

    switch (app->page) {
    case DRACU_PAGE_TITLE: key_title(app, key); break;
    case DRACU_PAGE_CHAPTERS: key_chapters(app, key); break;
    case DRACU_PAGE_GAME: key_game(app, key); break;
    case DRACU_PAGE_MENU: key_menu(app, key); break;
    case DRACU_PAGE_SETTINGS: key_settings(app, key); break;
    case DRACU_PAGE_SLOTS: key_slots(app, key); break;
    case DRACU_PAGE_ENDING:
        if (key->ev == BSP_BTN_CLICK) show_title(app);
        break;
    case DRACU_PAGE_ABOUT:
        if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_CLICK) {
            open_settings(app, app->settings_return);
        } else if ((key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN) &&
                   key->ev == BSP_BTN_CLICK) {
            const int delta = (key->btn == BSP_BTN_DOWN) ? 24 : -24;
            app->about_scroll += delta;
            app->about_scroll = dracu_ui_set_about_scroll(&app->ui, app->about_scroll);
        }
        break;
    default: break;
    }
}

void dracu_app_tick(dracu_app_t *app, uint32_t elapsed_ms)
{
    if (!app) return;
    // 自动阅读/快进是应用自己在推进画面,玩家不需要碰机器,这段时间不能算空闲 ——
    // 否则会读到一半自己变暗、熄屏甚至休眠。停在选项或结局上时两者都已停下,
    // 照旧按时限熄灭。
    if (dracu_app_wants_screen_on(app)) {
        app->idle_ms = 0;
    } else {
        app->idle_ms += elapsed_ms;
    }

    if (app->notice_ms > 0) {
        app->notice_ms = app->notice_ms > elapsed_ms ? app->notice_ms - elapsed_ms : 0;
        if (app->notice_ms == 0) dracu_ui_notice(&app->ui, "");
    }
    if (app->transition_pending) {
        if (app->transition_ms > elapsed_ms) {
            app->transition_ms -= elapsed_ms;
        } else {
            app->transition_pending = false;
            advance_reading(app, true);
        }
    }
    // 自动阅读:打字机打完后固定等 DRACU_AUTO_MS 再翻到下一页。
    if (app->auto_play && app->page == DRACU_PAGE_GAME && !app->transition_pending) {
        if (dracu_ui_typing(&app->ui) || app->at_choice || app->ended) {
            app->auto_ms = DRACU_AUTO_MS;   // 还在打字 / 等玩家选择:倒计时保持在满格
        } else if (app->auto_ms > elapsed_ms) {
            app->auto_ms -= elapsed_ms;
        } else {
            app->auto_ms = DRACU_AUTO_MS;
            advance_reading(app, false);
        }
    }

    // 快进:长按上时每 DRACU_FASTFORWARD_MS 推进一步。
    if (app->fast_forward && app->page == DRACU_PAGE_GAME && !app->transition_pending) {
        app->fast_forward_ms += elapsed_ms;
        while (app->fast_forward_ms >= DRACU_FASTFORWARD_MS) {
            app->fast_forward_ms -= DRACU_FASTFORWARD_MS;
            advance_reading(app, true);
            if (!app->fast_forward || app->at_choice || app->transition_pending) break;
        }
    }
    app->battery_accum_ms += elapsed_ms;
    if (app->battery_accum_ms >= DRACU_BATTERY_INTERVAL_MS || app->battery_percent < 0) {
        app->battery_accum_ms = 0;
        const int soc = bsp_battery_soc();
        if (soc != app->battery_percent) {
            app->battery_percent = soc;
            dracu_ui_set_battery(&app->ui, soc);
        }
    }
}

bool dracu_app_auto_reading(const dracu_app_t *app)
{
    if (!app) return false;
    return app->auto_play && app->page == DRACU_PAGE_GAME && !app->at_choice && !app->ended;
}

// 屏幕该不该保持亮着。玩家的明确要求:自动模式下不停屏、不暗屏 —— 所以只要自动阅读
// 开着(不管当下是正文、章节卡过场,还是碰巧停在选项/结局上)就不能计时;快进和换章
// 过场同理,那是应用在推画面,玩家并没有离开。
bool dracu_app_wants_screen_on(const dracu_app_t *app)
{
    if (!app) return false;
    if (app->auto_play || app->fast_forward || app->transition_pending) return true;
    return false;
}

uint32_t dracu_app_idle_ms(const dracu_app_t *app)
{
    return app ? app->idle_ms : 0;
}

void dracu_app_before_sleep(dracu_app_t *app)
{
    if (!app) return;
    if (app->page == DRACU_PAGE_GAME) auto_save(app);
    (void)dracu_settings_store(&app->settings);
}

void dracu_app_show_sleeping(dracu_app_t *app)
{
    if (!app) return;
    dracu_ui_set_ending(&app->ui, "", "休眠中", "按任意键唤醒");
    set_page(app, DRACU_PAGE_ENDING);
}

// ---------------------------------------------------------------- 调试定位
bool dracu_app_debug_render(dracu_app_t *app, uint32_t page)
{
    if (!app) return false;
    if (!dracu_player_goto(&app->player, &app->scn, page, &app->layout_hint)) {
        return false;
    }
    app->at_choice = app->player.at_choice;
    app->ended = app->player.ended;
    render_scene(app);
    ESP_LOGW(TAG, "调试:画到第 %u 页(章节 %u)", (unsigned)page,
             (unsigned)app->player.chapter);
    return true;
}

bool dracu_app_debug_start(dracu_app_t *app, uint32_t page)
{
    if (!app) return false;
    if (!start_reading(app, page)) return false;
    ESP_LOGW(TAG, "调试:从第 %u 页开始阅读", (unsigned)page);
    return true;
}

void dracu_app_debug_settle(dracu_app_t *app)
{
    if (!app) return;
    app->auto_play = false;
    app->fast_forward = false;
    app->transition_pending = false;
    dracu_ui_set_auto(&app->ui, false);
    dracu_ui_finish_typing(&app->ui);
}

bool dracu_app_debug_title(dracu_app_t *app)
{
    if (!app) return false;
    show_title(app);
    return true;
}

bool dracu_app_debug_back(dracu_app_t *app)
{
    if (!app) return false;
    if (app->page != DRACU_PAGE_GAME) return false;
    back_reading(app);
    return true;
}

bool dracu_app_debug_skip_chapter(dracu_app_t *app)
{
    if (!app) return false;
    return skip_chapter_action(app);
}

bool dracu_app_debug_load(dracu_app_t *app, int slot)
{
    if (!app) return false;
    dracu_save_t save;
    const bool ok = slot < 0 ? dracu_auto_load(&save) : dracu_slot_load((uint8_t)slot, &save);
    return ok && load_save(app, &save);
}

bool dracu_app_debug_save(dracu_app_t *app, int slot)
{
    if (!app) return false;
    dracu_save_t save;
    dracu_save_from_player(&app->player, &save);
    const bool ok = slot < 0 ? dracu_auto_store(&save) : dracu_slot_store((uint8_t)slot, &save);
    if (ok && slot < 0) app->have_auto = true;
    return ok;
}

int dracu_app_debug_info(dracu_app_t *app, char *out, size_t capacity)
{
    if (!app || !out || capacity == 0) return 0;
    char chapter[DRACU_NAME_MAX];
    chapter_label(app, app->player.chapter, chapter, sizeof(chapter));
    dracu_layers_t layers;
    current_layers(app, &layers);
    char text[96] = { 0 };
    char speaker[DRACU_NAME_MAX] = { 0 };
    size_t used = dracu_player_page_text(&app->player, text, sizeof(text));
    if (used > 0) {
        // 正文截到 42 字节左右(UTF-8 边界),串口一行看得下
        size_t cut = 0;
        while (cut < used && cut < 42u) {
            cut = dracu_utf8_next_boundary(text, used, cut);
        }
        text[cut] = 0;
    }
    dracu_player_speaker(&app->player, &app->scn, speaker, sizeof(speaker));
    return snprintf(out, capacity,
                    "page=%u chapter=%u '%s' bg=%u cg=%u sd=%u sprite=%u screen=%d/%d"
                    " idle=%ums auto=%d ff=%d state=%d speaker='%s' text='%s'",
                    (unsigned)app->player.page, (unsigned)app->player.chapter, chapter,
                    (unsigned)layers.bg, (unsigned)layers.cg, (unsigned)layers.sd,
                    (unsigned)layers.sprite, (int)app->player.page_index + 1,
                    (int)app->player.page_count, (unsigned)app->idle_ms, app->auto_play ? 1 : 0,
                    app->fast_forward ? 1 : 0, (int)app->page, speaker, text);
}

bool dracu_app_take_sleep_request(dracu_app_t *app)
{
    if (!app || !app->sleep_requested) return false;
    app->sleep_requested = false;
    return true;
}
