// main/limelight_app.c —— 应用状态机实现(数据层接 limelight 剧本/素材/存档)。
#include "limelight_app.h"

#include "bsp_battery.h"
#include "bsp_button.h"

#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "lime_app";

#define LIME_BATTERY_INTERVAL_MS 30000u
#define LIME_NOTICE_MS 2000u
#define LIME_TRANSITION_MS 1200u
// 长按上快进时,每句话之间的间隔。
#define LIME_FASTFORWARD_MS 110u
// 自动阅读:打字机打完之后再等这么久翻下一句(设置项 auto_delay 三档)。
static const uint32_t s_auto_ms[3] = { 600, 900, 1400 };

// 打字机速度(毫秒/字),对应设置页的 慢/中/快。
static const uint32_t s_speed_ms[3] = { 60, 38, 18 };
static const char *const s_speed_names[3] = { "慢", "中", "快" };
static const char *const s_auto_names[3] = { "短", "中", "长" };
static const char *const s_on_off[2] = { "关", "开" };

// ---------------------------------------------------------------- 小工具
static void set_page(lime_app_t *app, lime_page_t page)
{
    app->page = page;
    lime_ui_show_page(&app->ui, page);
}

static void notify(lime_app_t *app, const char *text)
{
    snprintf(app->notice, sizeof(app->notice), "%s", text ? text : "");
    app->notice_ms = LIME_NOTICE_MS;
    lime_ui_notice(&app->ui, app->notice);
}

static uint32_t speed_ms(const lime_app_t *app)
{
    return s_speed_ms[app->settings.text_speed > 2 ? 1 : app->settings.text_speed];
}

static uint32_t auto_delay_ms(const lime_app_t *app)
{
    return s_auto_ms[app->settings.auto_delay > 2 ? 1 : app->settings.auto_delay];
}

// 名字下标 -> 素材条目下标(<0 表示找不到)。
static int asset_of(lime_app_t *app, uint16_t name_index)
{
    if (name_index == LIME_NAME_NONE) return -1;
    const char *name = NULL;
    uint16_t length = 0;
    if (!lime_script_name(&app->script, name_index, &name, &length)) return -1;
    // lime_assets_find 需要 NUL 结尾,这里用一个小栈缓冲拷一份(名字不长)。
    char buffer[192];
    if (length >= sizeof(buffer)) return -1;
    memcpy(buffer, name, length);
    buffer[length] = '\0';
    return lime_assets_find(&app->assets, buffer);
}

// ---------------------------------------------------------------- 画面
static void render_art(lime_app_t *app, int bg, int sprite)
{
    if (app->rendered_bg == bg && app->rendered_sprite == sprite) return;   // 同一屏不重复解码
    if (lime_ui_set_art(&app->ui, &app->assets, bg, sprite)) {
        app->rendered_bg = bg;
        app->rendered_sprite = sprite;
    } else {
        app->rendered_bg = LIME_ASSET_NONE;
        app->rendered_sprite = LIME_ASSET_NONE;
    }
}

// 当前这一句该显示哪张背景/立绘:CG 优先于背景(源移植版就是 CG 盖在背景上)。
static void resolve_art(lime_app_t *app, int *bg_out, int *sprite_out)
{
    int bg = asset_of(app, app->player.bg);
    if (app->player.cg != LIME_NAME_NONE) {
        const int cg = asset_of(app, app->player.cg);
        if (cg >= 0) bg = cg;
    }
    int sprite = -1;
    if (app->settings.show_sprite && lime_player_show_sprite(&app->player)) {
        sprite = asset_of(app, app->player.sprite);
    }
    *bg_out = bg;
    *sprite_out = sprite;
}

static void render_scene(lime_app_t *app)
{
    int bg = LIME_ASSET_NONE;
    int sprite = LIME_ASSET_NONE;
    resolve_art(app, &bg, &sprite);
    render_art(app, bg, sprite);

    const uint32_t chapter_index = app->player.chapter;
    const bool chapter_known = chapter_index < (uint32_t)app->chapter_count;
    // 章节显示成 X-X(如 10-3),取自 [CHAPTERx-y] 标记。
    lime_ui_set_progress(&app->ui,
                         chapter_known ? app->chapter_major[chapter_index] : -1,
                         chapter_known ? app->chapter_minor[chapter_index] : 0,
                         app->player.page + 1, app->player.page_count);

    if (app->player.at_choice) {
        // 选项:texts[] 指向名字池,块缓存一旦换块就失效,所以先拷进栈缓冲。
        lime_choice_option_t options[LIME_UI_MAX_CHOICES];
        uint8_t count = 0;
        const char *texts[LIME_UI_MAX_CHOICES] = { NULL };
        static char text_storage[LIME_UI_MAX_CHOICES][96];
        if (lime_script_choice_for(&app->script, app->player.id, &count, options,
                                   LIME_UI_MAX_CHOICES)) {
            for (uint8_t i = 0; i < count; i++) {
                const char *name = NULL;
                uint16_t length = 0;
                if (lime_script_name(&app->script, options[i].name, &name, &length) &&
                    length < sizeof(text_storage[0])) {
                    memcpy(text_storage[i], name, length);
                    text_storage[i][length] = '\0';
                    texts[i] = text_storage[i];
                } else {
                    texts[i] = "?";
                }
            }
            lime_ui_set_text(&app->ui, "", "", 0);
            lime_ui_show_choices(&app->ui, texts, (int)count);
        }
        return;
    }

    lime_ui_hide_choices(&app->ui);
    char speaker[64] = { 0 };
    char text[LIME_TEXT_BUFFER] = { 0 };
    lime_player_speaker(&app->player, &app->script, speaker, sizeof(speaker));
    // 章节标记 "[CHAPTER12-1]" 不是说话人:这类条目只是章节卡,
    // 否则名牌上会顶着一个 [CHAPTER12-1](正文页/存档恢复都会碰到)。
    const char *speaker_name = strncmp(speaker, "[CHAPTER", 8) == 0 ? "" : speaker;
    lime_player_page_text(&app->player, &app->script, &app->layout, text, sizeof(text));
    // 快进时整句直接铺满,不要一个字一个字往外吐。
    lime_ui_set_text(&app->ui, speaker_name, text, app->fast_forward ? 0 : speed_ms(app));
}

// 换章过场:先摆标题画,再进正文(与 ATRI 阅读器一致);从菜单发起时也能切回正文页。
static void start_transition(lime_app_t *app)
{
    lime_ui_hide_choices(&app->ui);
    lime_ui_set_text(&app->ui, "", "", 0);
    set_page(app, LIME_PAGE_GAME);
    const int title = lime_assets_find(&app->assets, "title_bg0");
    render_art(app, title, LIME_ASSET_NONE);
    app->transition_pending = true;
    app->transition_ms = LIME_TRANSITION_MS;
}

static void auto_save(lime_app_t *app)
{
    lime_save_t save;
    lime_save_from_player(&app->player, &save);
    if (lime_auto_store(&save)) app->have_auto = true;
}

// ---------------------------------------------------------------- 列表页
static void open_menu(lime_app_t *app)
{
    static const char *const labels[] = { "继续阅读", "保存进度", "读取存档",
                                          "章节跳转", "CG 鉴赏", "返回标题" };
    app->menu_sel = 0;
    app->ui.lists[LIME_LIST_MENU].row_h = 30;
    app->ui.lists[LIME_LIST_MENU].top_y = 48;
    lime_ui_set_list(&app->ui, LIME_LIST_MENU, "菜单", "上下选择 · 确定 · 长按关闭",
                     labels, NULL, (int)(sizeof(labels) / sizeof(labels[0])), app->menu_sel);
    set_page(app, LIME_PAGE_MENU);
}

static void settings_refresh(lime_app_t *app)
{
    static const char *const labels[] = { "文字速度", "自动阅读", "显示立绘", "关于本作",
                                          "关机", "返回" };
    // 同样必须静态:列表控件切页时会重新读这个数组。
    static const char *values[6];
    values[0] = s_speed_names[app->settings.text_speed > 2 ? 1 : app->settings.text_speed];
    values[1] = s_auto_names[app->settings.auto_delay > 2 ? 1 : app->settings.auto_delay];
    values[2] = s_on_off[app->settings.show_sprite ? 1 : 0];
    values[3] = NULL; values[4] = NULL; values[5] = NULL;
    app->ui.lists[LIME_LIST_SETTINGS].row_h = 30;
    app->ui.lists[LIME_LIST_SETTINGS].top_y = 48;
    lime_ui_set_list(&app->ui, LIME_LIST_SETTINGS, "系统设置", "上下选择 · 确定",
                     labels, values, (int)(sizeof(labels) / sizeof(labels[0])),
                     app->settings_sel);
}

static void open_settings(lime_app_t *app, lime_page_t from)
{
    app->settings_return = from;
    app->settings_sel = 0;
    settings_refresh(app);
    set_page(app, LIME_PAGE_SETTINGS);
}

static void open_about(lime_app_t *app)
{
    static const char body[] =
        "limelight lemonade jam 阅读器\n\n"
        "阅读引擎用 C + LVGL 重写,\n"
        "剧本与素材来自 skdkzzx/limelight-\n"
        "lemonade-jam-xiaomi-band10。\n\n"
        "原作:SAGA PLANETS\n"
        "素材:skdkzzx / hezdaaa\n"
        "本机版:Shinku-Chen/ai-passport\n\n"
        "字体:Noto Sans SC(OFL-1.1)子集,\n"
        "4bpp 位图,由 tools/limelight_lvgl_font.py\n"
        "生成。\n"
        "画面:ESP32-C3 ROM JPEG 解码(esp_jpeg),\n"
        "资源包直接映射自 Flash。\n\n"
        "操作:\n"
        "  上/下短按:下一句(打字中=显示全文)\n"
        "  长按上:快进(松手即停)\n"
        "  长按下:自动阅读开关(任意键停)\n"
        "  确定:菜单;长按确定:返回上一层";
    lime_ui_set_about(&app->ui, body);
    set_page(app, LIME_PAGE_ABOUT);
}

static void slots_refresh(lime_app_t *app)
{
    static char labels[LIME_UI_MAX_ROWS][16];
    static char values[LIME_UI_MAX_ROWS][24];
    static const char *label_ptrs[LIME_UI_MAX_ROWS];
    static const char *value_ptrs[LIME_UI_MAX_ROWS];   // 同上:必须静态
    int n = 0;

    for (int slot = 0; slot < LIME_SAVE_SLOTS; ++slot) {
        lime_save_t save;
        snprintf(labels[n], sizeof(labels[n]), "手动 %d", slot + 1);
        if (lime_slot_load((uint8_t)slot, &save)) {
            snprintf(values[n], sizeof(values[n]), "第 %u 句", (unsigned)save.id);
            value_ptrs[n] = values[n];
        } else {
            value_ptrs[n] = "空";
        }
        label_ptrs[n] = labels[n];
        ++n;
    }
    if (!app->slots_saving) {
        lime_save_t save;
        snprintf(labels[n], sizeof(labels[n]), "自动存档");
        if (lime_auto_load(&save)) {
            snprintf(values[n], sizeof(values[n]), "第 %u 句", (unsigned)save.id);
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
    app->ui.lists[LIME_LIST_SLOTS].row_h = 30;
    app->ui.lists[LIME_LIST_SLOTS].top_y = 44;
    lime_ui_set_list(&app->ui, LIME_LIST_SLOTS, app->slots_saving ? "保存进度" : "读取存档",
                     app->slots_saving ? "确定覆盖 · 长按删除" : "确定读取",
                     label_ptrs, value_ptrs, n, app->slots_sel);
}

static void open_slots(lime_app_t *app, bool saving, lime_page_t from)
{
    app->slots_saving = saving;
    app->slots_return = from;
    app->slots_sel = 0;
    slots_refresh(app);
    set_page(app, LIME_PAGE_SLOTS);
}

static void open_chapters(lime_app_t *app)
{
    app->chapter_sel = (int)app->player.chapter;
    app->ui.lists[LIME_LIST_CHAPTERS].row_h = 30;
    app->ui.lists[LIME_LIST_CHAPTERS].top_y = 48;
    lime_ui_set_list(&app->ui, LIME_LIST_CHAPTERS, "章节跳转",
                     "选择章节 · 确定 · 长按返回", app->chapter_label_ptrs, NULL,
                     app->chapter_count, app->chapter_sel);
    set_page(app, LIME_PAGE_CHAPTERS);
}

static void gallery_show(lime_app_t *app)
{
    if (app->gallery_count <= 0) {
        notify(app, "没有可看的 CG");
        return;
    }
    if (app->gallery_index < 0) app->gallery_index = 0;
    if (app->gallery_index >= app->gallery_count) app->gallery_index = app->gallery_count - 1;
    const int entry = app->gallery_entries[app->gallery_index];
    if (lime_ui_set_backdrop(&app->ui, &app->assets, entry)) {
        app->rendered_bg = entry;
        app->rendered_sprite = LIME_ASSET_NONE;
    }
    lime_ui_set_gallery_info(&app->ui, app->gallery_index, app->gallery_count);
}

static void open_gallery(lime_app_t *app, lime_page_t from)
{
    app->gallery_return = from;
    gallery_show(app);
    set_page(app, LIME_PAGE_GALLERY);
}

static void title_refresh(lime_app_t *app)
{
    static const char *const labels[] = { "开始阅读", "继续阅读", "读取存档", "CG 鉴赏",
                                          "系统设置" };
    static char auto_value[24];
    // ⚠ 这两个数组必须是静态的:共享列表控件会在**切页时**才去读它们(见 list_paint),
    // 用栈上数组会变成悬垂指针(真机上就是这么崩的:lv_label_set_text(垃圾指针))。
    static const char *values[5];
    static const char *ptrs[5];
    int n = 0;
    values[0] = NULL; values[1] = NULL; values[2] = NULL; values[3] = NULL; values[4] = NULL;

    ptrs[n] = labels[0];
    n++;
    if (app->have_auto) {
        lime_save_t save;
        if (lime_auto_load(&save)) {
            snprintf(auto_value, sizeof(auto_value), "第 %u 句", (unsigned)save.id);
        } else {
            auto_value[0] = '\0';
        }
        ptrs[n] = labels[1];
        values[n] = auto_value;
        n++;
    }
    for (int i = 2; i < 5; i++) {
        ptrs[n] = labels[i];
        n++;
    }
    if (app->title_sel >= n) app->title_sel = 0;
    app->ui.lists[LIME_LIST_TITLE].row_h = 20;
    app->ui.lists[LIME_LIST_TITLE].top_y = 6;
    lime_ui_set_list(&app->ui, LIME_LIST_TITLE, NULL, NULL, ptrs, values, n, app->title_sel);
}

static void show_title(lime_app_t *app)
{
    lime_ui_hide_choices(&app->ui);
    const int title = lime_assets_find(&app->assets, "title_bg0");
    render_art(app, title, LIME_ASSET_NONE);
    title_refresh(app);
    set_page(app, LIME_PAGE_TITLE);
}

// ---------------------------------------------------------------- 阅读推进
static bool start_reading(lime_app_t *app, uint32_t id)
{
    if (!lime_player_start(&app->player, &app->script, id, &app->layout)) {
        notify(app, "剧本数据异常");
        return false;
    }
    app->started = true;
    app->transition_pending = false;
    app->fast_forward = false;
    app->auto_play = false;
    lime_ui_set_auto(&app->ui, false);
    set_page(app, LIME_PAGE_GAME);
    render_scene(app);
    return true;
}

static void show_ending(lime_app_t *app)
{
    lime_ui_set_ending(&app->ui, "读完了", "感谢阅读", "按确定返回标题");
    set_page(app, LIME_PAGE_ENDING);
    // 通关后不再保留自动存档:标题页的"继续阅读"不该指向已走完的进度。
    (void)lime_auto_clear();
    app->have_auto = false;
}

static void advance_reading(lime_app_t *app, bool skip_typing)
{
    if (!skip_typing && lime_ui_typing(&app->ui)) {
        lime_ui_finish_typing(&app->ui);
        return;
    }
    switch (lime_player_advance(&app->player, &app->script, &app->layout)) {
    case LIME_STEP_TEXT:
        render_scene(app);
        break;
    case LIME_STEP_CHAPTER:
        auto_save(app);
        // 换章过场不关自动模式:过场计时结束后自动接着读。
        start_transition(app);
        break;
    case LIME_STEP_CHOICE:
        auto_save(app);
        app->fast_forward = false;      // 选项页停下,交回玩家
        app->auto_play = false;
        lime_ui_set_auto(&app->ui, false);
        render_scene(app);
        break;
    case LIME_STEP_ENDING:
        app->fast_forward = false;
        app->auto_play = false;
        lime_ui_set_auto(&app->ui, false);
        show_ending(app);
        break;
    case LIME_STEP_STUCK:
    default:
        app->fast_forward = false;
        notify(app, "剧本数据异常");
        break;
    }
}

// ---------------------------------------------------------------- 按键
static void key_list_move(lime_app_t *app, int *selected, int count, int delta,
                          lime_list_id_t id)
{
    if (count <= 0) return;
    *selected += delta;
    if (*selected < 0) *selected = count - 1;
    if (*selected >= count) *selected = 0;
    lime_ui_set_list_selected(&app->ui, id, *selected);
}

static void key_warning(lime_app_t *app, const lime_key_t *key)
{
    if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_CLICK) {
        app->settings.seen_tips = 1;
        (void)lime_settings_store(&app->settings);
        show_title(app);
    }
}

static void title_select(lime_app_t *app)
{
    int index = 0;
    if (app->title_sel == index++) {                 // 开始阅读
        if (start_reading(app, 1)) {
            (void)lime_auto_clear();                 // 新游戏:清掉旧进度
            app->have_auto = false;
        }
        return;
    }
    if (app->have_auto) {
        if (app->title_sel == index++) {             // 继续阅读
            lime_save_t save;
            if (lime_auto_load(&save) && lime_player_jump(&app->player, &app->script, save.id,
                                                         &app->layout)) {
                app->started = true;
                app->transition_pending = false;
                set_page(app, LIME_PAGE_GAME);
                render_scene(app);
            } else {
                notify(app, "自动存档不可用");
                app->have_auto = false;
                title_refresh(app);
            }
            return;
        }
    }
    if (app->title_sel == index++) {
        open_slots(app, false, LIME_PAGE_TITLE);
        return;
    }
    if (app->title_sel == index++) {
        open_gallery(app, LIME_PAGE_TITLE);
        return;
    }
    if (app->title_sel == index++) {
        open_settings(app, LIME_PAGE_TITLE);
        return;
    }
}

static int title_count(const lime_app_t *app)
{
    return app->have_auto ? 5 : 4;
}

static void key_title(lime_app_t *app, const lime_key_t *key)
{
    if (key->btn == BSP_BTN_UP && key->ev == BSP_BTN_CLICK) {
        key_list_move(app, &app->title_sel, title_count(app), -1, LIME_LIST_TITLE);
    } else if (key->btn == BSP_BTN_DOWN && key->ev == BSP_BTN_CLICK) {
        key_list_move(app, &app->title_sel, title_count(app), 1, LIME_LIST_TITLE);
    } else if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_CLICK) {
        title_select(app);
    } else if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_LONG) {
        app->sleep_requested = true;                 // 标题页长按确定 = 关机
    }
}

static void key_game(lime_app_t *app, const lime_key_t *key)
{
    // 抬起事件:BSP 的 PRESS_UP。"长按上快进、松手即停"就靠它。
    if (key->ev == BSP_BTN_RELEASE) {
        app->fast_forward = false;
        return;
    }
    if (app->player.at_choice) {
        if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_UP) {
            lime_ui_set_choice_selected(&app->ui,
                                        lime_ui_choice_selected(&app->ui) - 1);
        } else if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_DOWN) {
            lime_ui_set_choice_selected(&app->ui,
                                        lime_ui_choice_selected(&app->ui) + 1);
        } else if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_CLICK) {
            const uint8_t pick = (uint8_t)lime_ui_choice_selected(&app->ui);
            if (lime_player_choose(&app->player, &app->script, pick, &app->layout)) {
                auto_save(app);
                lime_ui_hide_choices(&app->ui);
                render_scene(app);
            }
        }
        return;
    }
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        app->auto_play = false;                      // 手动翻页退出自动模式
        lime_ui_set_auto(&app->ui, false);
        advance_reading(app, false);
    } else if (key->btn == BSP_BTN_UP && key->ev == BSP_BTN_LONG) {
        app->fast_forward = true;                    // 长按上快进,松手即停
        app->fast_forward_ms = LIME_FASTFORWARD_MS;  // 立刻推一句,不用等第一个间隔
    } else if (key->btn == BSP_BTN_DOWN && key->ev == BSP_BTN_LONG) {
        app->auto_play = !app->auto_play;            // 长按下切换自动阅读
        app->auto_ms = 0;
        lime_ui_set_auto(&app->ui, app->auto_play);
    } else if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_CLICK) {
        open_menu(app);
    }
}

static void key_menu(lime_app_t *app, const lime_key_t *key)
{
    const int count = 6;
    if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_UP) {
        key_list_move(app, &app->menu_sel, count, -1, LIME_LIST_MENU);
    } else if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_DOWN) {
        key_list_move(app, &app->menu_sel, count, 1, LIME_LIST_MENU);
    } else if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_CLICK) {
        switch (app->menu_sel) {
        case 0:   // 继续阅读
            set_page(app, LIME_PAGE_GAME);
            render_scene(app);
            break;
        case 1:
            open_slots(app, true, LIME_PAGE_MENU);
            break;
        case 2:
            open_slots(app, false, LIME_PAGE_MENU);
            break;
        case 3:   // 章节跳转
            open_chapters(app);
            break;
        case 4:   // CG 鉴赏
            open_gallery(app, LIME_PAGE_MENU);
            break;
        default:  // 返回标题
            show_title(app);
            break;
        }
    } else if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_LONG) {
        set_page(app, LIME_PAGE_GAME);
    }
}

static void key_settings(lime_app_t *app, const lime_key_t *key)
{
    const int count = 6;
    if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_UP) {
        key_list_move(app, &app->settings_sel, count, -1, LIME_LIST_SETTINGS);
    } else if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_DOWN) {
        key_list_move(app, &app->settings_sel, count, 1, LIME_LIST_SETTINGS);
    } else if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_CLICK) {
        switch (app->settings_sel) {
        case 0:   // 文字速度:循环 慢/中/快
            app->settings.text_speed = (uint8_t)((app->settings.text_speed + 1) % 3);
            break;
        case 1:   // 自动阅读间隔:循环 短/中/长
            app->settings.auto_delay = (uint8_t)((app->settings.auto_delay + 1) % 3);
            break;
        case 2:   // 显示立绘
            app->settings.show_sprite = (uint8_t)(app->settings.show_sprite ? 0 : 1);
            break;
        case 3:   // 关于本作
            open_about(app);
            return;
        case 4:   // 关机
            app->sleep_requested = true;
            return;
        default:  // 返回
            set_page(app, app->settings_return);
            return;
        }
        (void)lime_settings_store(&app->settings);
        settings_refresh(app);
    } else if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_LONG) {
        set_page(app, app->settings_return);
    }
}

static void key_slots(lime_app_t *app, const lime_key_t *key)
{
    const int count = app->slots_saving ? LIME_SAVE_SLOTS + 1 : LIME_SAVE_SLOTS + 2;
    if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_UP) {
        key_list_move(app, &app->slots_sel, count, -1, LIME_LIST_SLOTS);
    } else if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_DOWN) {
        key_list_move(app, &app->slots_sel, count, 1, LIME_LIST_SLOTS);
    } else if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_CLICK) {
        if (app->slots_sel < LIME_SAVE_SLOTS) {
            if (app->slots_saving) {
                lime_save_t save;
                lime_save_from_player(&app->player, &save);
                notify(app, lime_slot_store((uint8_t)app->slots_sel, &save) ? "已保存"
                                                                            : "保存失败");
                slots_refresh(app);
            } else {
                lime_save_t save;
                if (lime_slot_load((uint8_t)app->slots_sel, &save) &&
                    lime_player_jump(&app->player, &app->script, save.id, &app->layout)) {
                    app->started = true;
                    set_page(app, LIME_PAGE_GAME);
                    render_scene(app);
                } else {
                    notify(app, "这个存档位是空的");
                }
            }
        } else if (!app->slots_saving && app->slots_sel == LIME_SAVE_SLOTS) {
            lime_save_t save;                        // 自动存档
            if (lime_auto_load(&save) &&
                lime_player_jump(&app->player, &app->script, save.id, &app->layout)) {
                app->started = true;
                set_page(app, LIME_PAGE_GAME);
                render_scene(app);
            } else {
                notify(app, "没有自动存档");
            }
        } else {
            set_page(app, app->slots_return);
        }
    } else if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_LONG) {
        if (app->slots_saving && app->slots_sel < LIME_SAVE_SLOTS) {
            (void)lime_slot_clear((uint8_t)app->slots_sel);
            notify(app, "已删除");
            slots_refresh(app);
        } else {
            set_page(app, app->slots_return);
        }
    }
}

static void key_chapters(lime_app_t *app, const lime_key_t *key)
{
    if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_UP) {
        key_list_move(app, &app->chapter_sel, app->chapter_count, -1, LIME_LIST_CHAPTERS);
    } else if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_DOWN) {
        key_list_move(app, &app->chapter_sel, app->chapter_count, 1, LIME_LIST_CHAPTERS);
    } else if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_CLICK) {
        if (app->chapter_sel >= 0 && app->chapter_sel < app->chapter_count) {
            if (lime_player_jump(&app->player, &app->script,
                                 app->chapter_first_ids[app->chapter_sel], &app->layout)) {
                auto_save(app);
                start_transition(app);               // 跳章走同一个过场
            } else {
                notify(app, "章节数据异常");
            }
        }
    } else if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_LONG) {
        set_page(app, LIME_PAGE_MENU);
    }
}

static void key_gallery(lime_app_t *app, const lime_key_t *key)
{
    if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_UP) {
        app->gallery_index--;
        gallery_show(app);
    } else if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_DOWN) {
        app->gallery_index++;
        gallery_show(app);
    } else if (key->btn == BSP_BTN_OK &&
               (key->ev == BSP_BTN_CLICK || key->ev == BSP_BTN_LONG)) {
        set_page(app, app->gallery_return);
    }
}

static void key_about(lime_app_t *app, const lime_key_t *key)
{
    if (key->btn == BSP_BTN_OK && (key->ev == BSP_BTN_CLICK || key->ev == BSP_BTN_LONG)) {
        set_page(app, LIME_PAGE_SETTINGS);
    }
}

static void key_ending(lime_app_t *app, const lime_key_t *key)
{
    if (key->btn == BSP_BTN_OK && (key->ev == BSP_BTN_CLICK || key->ev == BSP_BTN_LONG)) {
        show_title(app);
    }
}

void lime_app_key(lime_app_t *app, const lime_key_t *key)
{
    if (!app || !key) return;
    app->idle_ms = 0;
    switch (app->page) {
    case LIME_PAGE_WARNING: key_warning(app, key); break;
    case LIME_PAGE_TITLE: key_title(app, key); break;
    case LIME_PAGE_GAME: key_game(app, key); break;
    case LIME_PAGE_MENU: key_menu(app, key); break;
    case LIME_PAGE_SETTINGS: key_settings(app, key); break;
    case LIME_PAGE_SLOTS: key_slots(app, key); break;
    case LIME_PAGE_CHAPTERS: key_chapters(app, key); break;
    case LIME_PAGE_GALLERY: key_gallery(app, key); break;
    case LIME_PAGE_ABOUT: key_about(app, key); break;
    case LIME_PAGE_ENDING: key_ending(app, key); break;
    default: break;
    }
}

// ---------------------------------------------------------------- 计时
void lime_app_tick(lime_app_t *app, uint32_t elapsed_ms)
{
    if (!app) return;
    app->idle_ms += elapsed_ms;

    // 自动阅读和快进时屏幕一直在动,按"有活动"算:不计空闲。
    // 否则自动看故事时没有按键事件,60 秒后屏幕会自己变暗、180 秒后黑屏。
    if (app->page == LIME_PAGE_GAME && (app->auto_play || app->fast_forward)) {
        app->idle_ms = 0;
    }

    if (app->notice_ms > 0) {
        app->notice_ms = app->notice_ms > elapsed_ms ? app->notice_ms - elapsed_ms : 0;
        if (app->notice_ms == 0) lime_ui_notice(&app->ui, "");
    }

    app->battery_accum_ms += elapsed_ms;
    if (app->battery_accum_ms >= LIME_BATTERY_INTERVAL_MS) {
        app->battery_accum_ms = 0;
        app->battery_percent = bsp_battery_soc();
        lime_ui_set_battery(&app->ui, app->battery_percent);
    }

    if (app->transition_pending) {
        app->transition_ms = app->transition_ms > elapsed_ms ? app->transition_ms - elapsed_ms : 0;
        if (app->transition_ms == 0) {
            app->transition_pending = false;
            render_scene(app);
        }
        return;
    }

    if (app->page == LIME_PAGE_GAME && !app->player.at_choice) {
        if (app->fast_forward) {
            app->fast_forward_ms += elapsed_ms;
            if (app->fast_forward_ms >= LIME_FASTFORWARD_MS) {
                app->fast_forward_ms = 0;
                advance_reading(app, true);
            }
        } else if (app->auto_play) {
            if (lime_ui_typing(&app->ui)) {
                app->auto_ms = 0;
            } else {
                app->auto_ms += elapsed_ms;
                if (app->auto_ms >= auto_delay_ms(app)) {
                    app->auto_ms = 0;
                    advance_reading(app, false);
                }
            }
        }
    }
}

bool lime_app_auto_reading(const lime_app_t *app)
{
    return app && app->auto_play && !app->player.at_choice && !app->player.ended;
}

uint32_t lime_app_idle_ms(const lime_app_t *app)
{
    return app ? app->idle_ms : 0;
}

void lime_app_before_sleep(lime_app_t *app)
{
    if (!app || !app->started) return;
    auto_save(app);
}

bool lime_app_take_sleep_request(lime_app_t *app)
{
    if (!app) return false;
    const bool requested = app->sleep_requested;
    app->sleep_requested = false;
    return requested;
}

// ---------------------------------------------------------------- 初始化
static void build_chapters(lime_app_t *app)
{
    const uint32_t total = lime_script_chapters(&app->script);
    app->chapter_count = total > LIME_CHAPTER_MAX ? LIME_CHAPTER_MAX : (int)total;
    for (int i = 0; i < app->chapter_count; i++) {
        uint32_t first_id = 0;
        uint16_t name_index = 0;
        if (!lime_script_chapter(&app->script, (uint32_t)i, &first_id, &name_index)) break;
        const char *text = NULL;
        uint16_t length = 0;
        if (!lime_script_name(&app->script, name_index, &text, &length)) continue;
        if (length >= LIME_CHAPTER_LABEL_MAX) length = LIME_CHAPTER_LABEL_MAX - 1;
        // 标签形如 "[CHAPTER7-1]":去掉方括号与 CHAPTER 前缀,列表里直接显示 "7-1"。
        const char *begin = text;
        uint16_t span = length;
        if (span >= 2 && begin[0] == '[') {
            begin++;
            span -= 2;                       // 去掉 "[" 与 "]"
        }
        if (span >= 7 && memcmp(begin, "CHAPTER", 7) == 0) {
            begin += 7;
            span -= 7;
        }
        memcpy(app->chapter_labels[i], begin, span);
        app->chapter_labels[i][span] = '\0';
        app->chapter_label_ptrs[i] = app->chapter_labels[i];

        int major = 0, minor = 0;
        if (!lime_chapter_pair(begin, span, &major, &minor)) {
            major = 0;
            minor = 0;
        }
        app->chapter_major[i] = (uint8_t)(major > 255 ? 255 : major);
        app->chapter_minor[i] = (uint8_t)(minor > 255 ? 255 : minor);
        // 章节起点 id 也留一份:跳转时用。
        app->chapter_first_ids[i] = first_id;
    }
}

static void build_gallery(lime_app_t *app)
{
    app->gallery_count = 0;
    const uint16_t total = lime_assets_count(&app->assets);
    for (uint16_t i = 0; i < total && app->gallery_count < LIME_GALLERY_MAX; i++) {
        uint16_t length = 0;
        const char *family = lime_assets_family(&app->assets, i, &length);
        if (!family || length != 2 || memcmp(family, "cg", 2) != 0) continue;
        app->gallery_entries[app->gallery_count++] = (int16_t)i;
    }
}

bool lime_app_init(lime_app_t *app, const uint8_t *script_data, uint32_t script_size,
                   const uint8_t *asset_data, uint32_t asset_size, uint16_t *art_pixels,
                   uint8_t *sprite_buffer, uint32_t sprite_capacity, uint8_t *mask_buffer,
                   uint32_t mask_capacity, uint8_t *script_cache, uint32_t script_cache_size,
                   const lv_font_t *font_cjk)
{
    if (!app || !script_data || !asset_data || !art_pixels) return false;
    memset(app, 0, sizeof(*app));
    app->rendered_bg = LIME_ASSET_NONE;
    app->rendered_sprite = LIME_ASSET_NONE;
    app->layout.units_per_line = LIME_UI_UNITS_PER_LINE;
    app->layout.lines_per_page = LIME_UI_LINES;

    if (!lime_script_open(&app->script, script_data, script_size)) {
        ESP_LOGE(TAG, "剧本包打开失败");
        return false;
    }
    if (!lime_assets_open(&app->assets, asset_data, asset_size)) {
        ESP_LOGE(TAG, "素材包打开失败");
        return false;
    }
    if (script_cache_size < app->script.max_chunk_raw) {
        ESP_LOGE(TAG, "剧本块缓存太小: 需要 %u,给了 %u",
                 (unsigned)app->script.max_chunk_raw, (unsigned)script_cache_size);
        return false;
    }
    lime_script_set_cache(&app->script, script_cache, script_cache_size);
    lime_settings_default(&app->settings);
    (void)lime_save_init();
    (void)lime_settings_load(&app->settings);

    if (!lime_ui_create(&app->ui, art_pixels, sprite_buffer, sprite_capacity, mask_buffer,
                        mask_capacity, font_cjk)) {
        ESP_LOGE(TAG, "界面创建失败");
        return false;
    }

    build_chapters(app);
    build_gallery(app);
    ESP_LOGI(TAG, "剧本 %u 条 / 章节 %d / CG %d", (unsigned)lime_script_entries(&app->script),
             app->chapter_count, app->gallery_count);

    lime_player_reset(&app->player);
    lime_ui_set_battery(&app->ui, -1);
    lime_ui_set_auto(&app->ui, false);

    lime_save_t auto_save_state;
    app->have_auto = lime_auto_load(&auto_save_state);

    if (app->settings.seen_tips) {
        show_title(app);
    } else {
        lime_ui_set_warning(&app->ui,
                            "limelight lemonade jam\n"
                            "竖屏阅读器\n\n"
                            "上 / 下：下一句\n"
                            "长按上：快进\n"
                            "长按下：自动阅读\n"
                            "确定：菜单\n"
                            "长按确定：返回上一层",
                            "按确定继续");
        set_page(app, LIME_PAGE_WARNING);
    }
    return true;
}

bool lime_app_debug_start(lime_app_t *app, uint32_t id)
{
    if (!app) return false;
    if (!lime_player_start(&app->player, &app->script, id, &app->layout)) return false;
    app->started = true;
    app->transition_pending = false;
    set_page(app, LIME_PAGE_GAME);
    render_scene(app);
    return true;
}

bool lime_app_debug_title(lime_app_t *app)
{
    if (!app) return false;
    show_title(app);
    return true;
}

// 调试用:直接开关自动阅读(串口验收用,和长按下等价)。
void lime_app_debug_set_auto(lime_app_t *app, bool on)
{
    if (!app) return;
    app->auto_play = on;
    app->auto_ms = 0;
    app->fast_forward = false;
    app->idle_ms = 0;
    lime_ui_set_auto(&app->ui, on);
}

// 调试用:把画面区重新合成一遍。串口抓帧会把拼好的整帧写回同一块画布
// (memset 先清空),所以抓帧前必须重画背景/立绘,否则回传的画面区是黑的。
void lime_app_debug_redraw_art(lime_app_t *app)
{
    if (!app) return;
    const int bg = app->rendered_bg;
    const int sprite = app->rendered_sprite;
    // render_art 会把"同屏不重复解码"当缓存命中直接返回,先清掉缓存逼它真画。
    app->rendered_bg = LIME_ASSET_NONE;
    app->rendered_sprite = LIME_ASSET_NONE;
    render_art(app, bg, sprite);
}

// 调试用:按名字直接切到某个列表页(串口抓图/验收时用,不进正常操作路径)。
bool lime_app_debug_page(lime_app_t *app, const char *name)
{
    if (!app || !name) return false;
    if (strcmp(name, "chapters") == 0) {
        open_chapters(app);
    } else if (strcmp(name, "gallery") == 0) {
        open_gallery(app, LIME_PAGE_MENU);
    } else if (strcmp(name, "menu") == 0) {
        open_menu(app);
    } else if (strcmp(name, "settings") == 0) {
        open_settings(app, LIME_PAGE_MENU);
    } else if (strcmp(name, "about") == 0) {
        open_about(app);
    } else if (strcmp(name, "title") == 0) {
        show_title(app);
    } else {
        return false;
    }
    return true;
}
