// main/tsxx_app.c —— 应用状态机实现。
#include "tsxx_app.h"

#include "bsp_battery.h"
#include "bsp_button.h"

#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "tsxx_app";

// 文字速度(毫秒/字)。字号只有 16px 一种(应用分区吃紧,不做字号设置)。
static const uint32_t s_speed_ms[3] = { 50, 28, 12 };
static const char *const s_speed_names[3] = { "慢", "中", "快" };
#define TSXX_SPEED_DEFAULT 1

// 结局覆盖层上的标题就是包里的结局名(例如 "Noa end" / "END"),最长几十字节。
#define TSXX_ENDING_NAME 40

// 自动阅读:打字机打完之后再等这么久翻到下一句(也是两次翻页的最小间隔)。
#define TSXX_AUTO_MS 700u
// 长按上快进时每句话之间的间隔。
#define TSXX_FASTFORWARD_MS 110u
// 自动存档节流:阅读时每 4 秒最多落一次盘(选项点与休眠前强制落盘)。
#define TSXX_AUTOSAVE_MS 4000u
#define TSXX_NOTICE_MS 2000u
#define TSXX_BATTERY_INTERVAL_MS 30000u
// 警告页/关于页每次按键滚动的像素数。关于页是"还想看就往下翻"的附加内容,步长给大一点。
#define TSXX_SCROLL_STEP 28
#define TSXX_SCROLL_STEP_ABOUT 160

// ---------------------------------------------------------------- 小工具
static bool is_click(const tsxx_key_t *key)
{
    // 按键驱动把快速连按合并成 DOUBLE:对阅读器来说它和一次单击完全等价。
    return key->ev == BSP_BTN_CLICK || key->ev == BSP_BTN_DOUBLE;
}

static bool is_press(const tsxx_key_t *key)
{
    return is_click(key) || key->ev == BSP_BTN_LONG;
}

static uint8_t speed_level(const tsxx_app_t *app)
{
    return app->settings.text_speed > 2 ? TSXX_SPEED_DEFAULT : app->settings.text_speed;
}

static uint32_t speed_ms(const tsxx_app_t *app)
{
    return s_speed_ms[speed_level(app)];
}

static void set_page(tsxx_app_t *app, tsxx_page_id_t page)
{
    app->page = page;
    tsxx_ui_show_page(&app->ui, page);
    if (page == TSXX_PAGE_GAME) {
        // 从别的页面回到正文时,按设置里的开关恢复自动阅读状态。
        // 这样"设置里开自动阅读 → 回正文 → 放下机器"是通的;中途按键仍然会停下它。
        app->auto_play = app->settings.auto_play != 0 && !app->player.at_choice;
        app->auto_ms = TSXX_AUTO_MS;
        tsxx_ui_set_mode(&app->ui, app->auto_play, app->fast_forward);
    }
}

// 前置声明:后文的按键处理与标题页选择互相调用。
static void title_select(tsxx_app_t *app);

static void notify(tsxx_app_t *app, const char *text)
{
    snprintf(app->notice, sizeof(app->notice), "%s", text ? text : "");
    app->notice_ms = TSXX_NOTICE_MS;
    tsxx_ui_notice(&app->ui, app->notice);
}

// 列表光标:两端夹紧,不绕圈(绕圈会被读成"上下键反了")。
static int clamp_cursor(int selected, int count)
{
    if (count <= 0) return 0;
    if (selected < 0) return 0;
    if (selected >= count) return count - 1;
    return selected;
}

// ASCII 数字 -> 全角数字:字体子集来自剧本正文,里面从没出现过半角 '5' 与 '9',
// 直接用 %u 画出来会变成两个缺字方框;全角 ０-９ 都在子集里。
static void to_full_width_digits(const char *src, char *out, size_t capacity)
{
    if (!out || capacity == 0) return;
    size_t n = 0;
    for (const char *p = src; p != NULL && *p != '\0'; ++p) {
        if (*p >= '0' && *p <= '9') {
            if (n + 3 >= capacity) break;
            out[n++] = (char)0xEF;
            out[n++] = (char)0xBC;
            out[n++] = (char)(0x90 + (*p - '0'));   // U+FF10 起
            continue;
        }
        if (n + 1 >= capacity) break;
        out[n++] = *p;
    }
    out[n] = '\0';
}

// "[CHAPTER 3-3]" -> "CHAPTER ３－３":对玩家有意义的只有编号。方括号、半角连字符
// 与半角数字都换掉(字体子集里没有 '-' / '5' / '9' 这几个码位),否则会画成方框。
// 源标签形如 [CHAPTER5-3] / [CHAPTER 3-3];只保留编号部分 "5-3"。
static void format_chapter(const char *raw, char *out, size_t capacity)
{
    if (!raw || !out || capacity == 0) {
        if (out && capacity > 0) out[0] = '\0';
        return;
    }
    const char *p = strstr(raw, "CHAPTER");
    p = p ? (p + 7) : raw;   // 跳过 "CHAPTER" 前缀
    size_t n = 0;
    for (; *p != '\0' && *p != ']'; ++p) {
        if (*p == ' ' || *p == '[') continue;
        if (n + 1 >= capacity) break;
        out[n++] = *p;
    }
    out[n] = '\0';
}

// 该页所属的章节标签;在第一章之前(序章)返回空串。
static void chapter_label_for(const tsxx_app_t *app, uint32_t page, char *out, size_t capacity)
{
    int last = -1;
    for (int i = 0; i < app->chapter_count; ++i) {
        if (app->chapters[i].page <= page) last = i;
    }
    if (last < 0) {
        out[0] = '\0';
        return;
    }
    char raw[48];
    if (tsxx_pack_name(&app->pack, TSXX_TABLE_SPEAKER, app->chapters[last].name_id, raw,
                       sizeof(raw)) == 0) {
        out[0] = '\0';
        return;
    }
    format_chapter(raw, out, capacity);
}

// 标题页背景:优先黄昏的天空,其次白昼,都没有就用第一张背景。
static uint8_t title_bg_id(const tsxx_pack_t *pack)
{
    // 标题背景是打包时追加到背景表末尾的那一张(--title-image),
    // 下标记在 META 的 title_bg=;源素材里没有可用的标题画。
    return tsxx_pack_title_bg(pack);
}

// ---------------------------------------------------------------- 画面渲染
// 这些缓冲是文件级静态的:渲染路径最深处的分页函数自己还要 1 KB 左右栈,
// 输入任务只有 4 KB 栈,不能在这里再摆几个大数组。
// 它们受 LVGL 锁保护:按键与调试命令都只在持有 bsp_lvgl_lock() 时才进渲染。
static char s_speaker[64];
static char s_text[TSXX_TEXT_BUFFER];
static char s_choice[TSXX_CHOICE_MAX_LOCAL][160];

// 把某一页画出来(画面区 + 正文/选项 + 章节标签)。player 可以是临时副本,
// 所以调试截图与正常阅读共用同一条渲染路径。
static void render_player(tsxx_app_t *app, const tsxx_player_t *player)
{
    tsxx_page_t view;   // 资源包里的页记录(背景/立绘/说话人),不是界面页面 id
    uint8_t bg = TSXX_NONE8;
    uint8_t sprite = TSXX_NONE8;
    if (!tsxx_pack_page(&app->pack, player->page, &view)) {
        ESP_LOGE(TAG, "页号越界: %u", (unsigned)player->page);
        return;
    }
    bg = view.bg;
    sprite = view.sprite;

    tsxx_cg_t cg;
    const bool has_cg = tsxx_player_cg(player, &app->pack, &cg);
    const bool changed =
        !app->art_valid || app->art_bg != bg || app->art_sprite != sprite ||
        app->art_has_cg != has_cg ||
        (has_cg && (app->art_cg.kind != cg.kind || app->art_cg.img != cg.img));
    if (changed) {
        if (tsxx_ui_set_art(&app->ui, &app->pack, bg, has_cg ? &cg : NULL, sprite)) {
            app->art_valid = true;
            app->art_bg = bg;
            app->art_sprite = sprite;
            app->art_has_cg = has_cg;
            app->art_cg = cg;
        } else {
            app->art_valid = false;   // 画失败:下一次强制重画,不要让缓存撒谎
        }
    }

    char label[TSXX_CHAPTER_LABEL];
    chapter_label_for(app, player->page, label, sizeof(label));
    tsxx_ui_set_chapter(&app->ui, label);

    if (player->at_choice) {
        const uint8_t count = player->choice_count;
        uint8_t used = 0;
        for (uint8_t i = 0; i < count && i < TSXX_CHOICE_MAX_LOCAL; ++i) {
            if (tsxx_pack_choice_option(&app->pack, player->page, i, s_choice[i],
                                        sizeof(s_choice[i]), NULL)) {
                ++used;
            }
        }
        const char *labels[TSXX_CHOICE_MAX_LOCAL];
        for (uint8_t i = 0; i < used; ++i) labels[i] = s_choice[i];
        tsxx_ui_set_text(&app->ui, "", "", 0);
        tsxx_ui_show_choices(&app->ui, labels, used);
        return;
    }

    tsxx_ui_hide_choices(&app->ui);
    s_speaker[0] = '\0';
    s_text[0] = '\0';
    tsxx_player_speaker(player, &app->pack, s_speaker, sizeof(s_speaker));
    tsxx_player_screen_text(player, &app->pack, &app->layout, s_text, sizeof(s_text));
    // 快进时整句直接铺满,不要一个字一个字往外吐。
    tsxx_ui_set_text(&app->ui, s_speaker, s_text, app->fast_forward ? 0 : speed_ms(app));
}

// 当前场景是否需要重新渲染(画面缓存 + 正文)。用于"同一页重复渲染"的短路。
static void render_scene(tsxx_app_t *app)
{
    render_player(app, &app->player);
}

// ---------------------------------------------------------------- 存档
static void auto_save(tsxx_app_t *app, bool force)
{
    if (!force && app->auto_save_ms < TSXX_AUTOSAVE_MS) return;
    app->auto_save_ms = 0;
    tsxx_save_t save;
    tsxx_save_from_player(&app->player, &save);
    if (tsxx_slot_store(TSXX_AUTO_SLOT, &save)) {
        app->have_auto = true;
    }
}

// 存档槽的取值文字:"CHAPTER ３－３ · ４７％"(数字必须走全角,见 to_full_width_digits)。
static void slot_value(const tsxx_app_t *app, const tsxx_save_t *save, char *out, size_t capacity)
{
    const uint32_t pages = tsxx_pack_pages(&app->pack);
    const unsigned percent = pages > 0 ? (unsigned)(save->page * 100u / pages) : 0u;
    char label[TSXX_CHAPTER_LABEL];
    char plain[TSXX_CHAPTER_LABEL + 16];
    chapter_label_for(app, save->page, label, sizeof(label));
    if (label[0]) {
        snprintf(plain, sizeof(plain), "%s · %u%%", label, percent);
    } else {
        snprintf(plain, sizeof(plain), "%u%%", percent);
    }
    to_full_width_digits(plain, out, capacity);
}

// ---------------------------------------------------------------- 页面构建
static void title_refresh(tsxx_app_t *app)
{
    static char auto_value[TSXX_CHAPTER_LABEL + 8];
    const char *labels[TSXX_UI_MAX_ROWS];
    const char *values[TSXX_UI_MAX_ROWS];
    int n = 0;

    labels[n] = "开始阅读";
    values[n] = NULL;
    ++n;
    if (app->have_auto) {
        tsxx_save_t save;
        if (tsxx_slot_load(TSXX_AUTO_SLOT, &save)) {
            char label[TSXX_CHAPTER_LABEL];
            chapter_label_for(app, save.page, label, sizeof(label));
            snprintf(auto_value, sizeof(auto_value), "%s", label[0] ? label : "继续");
        } else {
            auto_value[0] = '\0';
        }
        labels[n] = "继续阅读";
        values[n] = auto_value;
        ++n;
    }
    labels[n] = "读取存档";
    values[n] = NULL;
    ++n;
    labels[n] = "章节跳转";
    values[n] = NULL;
    ++n;
    labels[n] = "系统设置";
    values[n] = NULL;
    ++n;
    labels[n] = "关于本作";
    values[n] = NULL;
    ++n;

    app->title_sel = clamp_cursor(app->title_sel, n);
    tsxx_ui_set_list(&app->ui, TSXX_LIST_TITLE, NULL, NULL, NULL, labels, values, n,
                     app->title_sel);
}

static void show_title(tsxx_app_t *app)
{
    tsxx_ui_hide_choices(&app->ui);
    tsxx_ui_ending_overlay(&app->ui, NULL, NULL, false);
    app->ended = false;
    app->auto_play = false;
    app->fast_forward = false;
    tsxx_ui_set_mode(&app->ui, false, false);
    // 标题画复用同一块画布:没有事件图、没有立绘,正文带由 show_page 收起。
    const uint8_t bg = title_bg_id(&app->pack);
    if (tsxx_ui_set_art(&app->ui, &app->pack, bg, NULL, TSXX_NONE8)) {
        app->art_valid = true;
        app->art_bg = bg;
        app->art_sprite = TSXX_NONE8;
        app->art_has_cg = false;
    } else {
        app->art_valid = false;
    }
    tsxx_ui_set_title(&app->ui, "天使☆骚骚", "RE－BOOT! 阅读器",
                      "上/下选择 · 确定进入 · 长按确定关机");
    title_refresh(app);
    set_page(app, TSXX_PAGE_TITLE);
}

static void open_menu(tsxx_app_t *app)
{
    static const char *const labels[] = { "继续阅读", "保存进度", "读取存档",
                                          "跳过章节", "章节跳转", "返回标题" };
    app->menu_sel = 0;
    tsxx_ui_set_list(&app->ui, TSXX_LIST_MENU, "菜单", "上/下选择 · 确定确认 · 长按确定关闭",
                     NULL, labels, NULL, (int)(sizeof(labels) / sizeof(labels[0])),
                     app->menu_sel);
    set_page(app, TSXX_PAGE_MENU);
}

static void open_settings(tsxx_app_t *app, tsxx_page_id_t from)
{
    static const char *const labels[] = { "文字速度", "自动阅读", "关于本作", "返回" };
    const char *values[4];
    app->settings_return = from;
    app->settings_sel = 0;
    values[0] = s_speed_names[speed_level(app)];
    values[1] = app->settings.auto_play ? "开" : "关";
    values[2] = NULL;
    values[3] = NULL;
    tsxx_ui_set_list(&app->ui, TSXX_LIST_SETTINGS, "系统设置", "确定切换 · 长按确定返回", NULL,
                     labels, values, 4, app->settings_sel);
    set_page(app, TSXX_PAGE_SETTINGS);
}

static void open_about(tsxx_app_t *app)
{
    static const char body[] =
        "《天使☆骚骚 RE－BOOT!》阅读器\n\n"
        "剧本 61,436 页,13 个选择支、\n"
        "5 道分支闸门与 15 个结局点。\n"
        "剧本与素材来自小米手环的同人移植。\n\n"
        "本机用 C + LVGL 重写了竖屏阅读引擎。\n"
        "资源包直接映射自闪存,没有解压,\n"
        "没有文本解析,也没有逐页分配。\n\n"
        "美术层 240x320 与屏幕 1:1,立绘原尺寸合成,\n"
        "背景与事件图 180x240 按最近邻放大 4/3,\n"
        "文字层画在原生分辨率上。\n\n"
        "中文字体：思源黑体 SC 子集(开源字型授权),\n"
        "4 位色深位图,由 tools/tsxx_font.py 生成。\n"
        "画面：ESP32C3 上的软件 esp_jpeg 解码。\n\n"
        "操作：\n"
        "  上/下/确定 短按 下一句\n"
        "  长按上 快进(松手即停)\n"
        "  长按下 自动阅读开关\n"
        "  长按确定 菜单(存读档、章节、设置)\n"
        "  列表页 上/下选择,确定进入,\n"
        "         长按确定返回\n\n"
        "原作 《天使☆骚骚 RE－BOOT!》\n"
        "制作 柚子社(Yuzusoft)\n"
        "手环移植 hezdaaa\n"
        "本机移植 Shinku－Chen/ai－passport\n\n"
        "本作仅供个人学习与交流,请支持正版。";
    tsxx_ui_set_about(&app->ui, body, "上/下滚动 · 确定返回");
    set_page(app, TSXX_PAGE_ABOUT);
}

static void slots_refresh(tsxx_app_t *app)
{
    char labels[TSXX_UI_MAX_ROWS][16];
    char values[TSXX_UI_MAX_ROWS][40];
    const char *label_ptrs[TSXX_UI_MAX_ROWS];
    const char *value_ptrs[TSXX_UI_MAX_ROWS];
    int n = 0;

    for (int slot = 0; slot < TSXX_SAVE_SLOTS; ++slot) {
        tsxx_save_t save;
        snprintf(labels[n], sizeof(labels[n]), "手动 %d", slot + 1);
        if (tsxx_slot_load((uint8_t)slot, &save)) {
            slot_value(app, &save, values[n], sizeof(values[n]));
        } else {
            snprintf(values[n], sizeof(values[n]), "空");
        }
        label_ptrs[n] = labels[n];
        value_ptrs[n] = values[n];
        ++n;
    }
    if (!app->slots_saving) {
        tsxx_save_t save;
        snprintf(labels[n], sizeof(labels[n]), "自动存档");
        if (tsxx_slot_load(TSXX_AUTO_SLOT, &save)) {
            slot_value(app, &save, values[n], sizeof(values[n]));
        } else {
            snprintf(values[n], sizeof(values[n]), "空");
        }
        label_ptrs[n] = labels[n];
        value_ptrs[n] = values[n];
        ++n;
    }
    snprintf(labels[n], sizeof(labels[n]), "返回");
    label_ptrs[n] = labels[n];
    value_ptrs[n] = NULL;
    ++n;

    app->slots_sel = clamp_cursor(app->slots_sel, n);
    tsxx_ui_set_list(&app->ui, app->slots_saving ? TSXX_LIST_SAVE : TSXX_LIST_LOAD,
                     app->slots_saving ? "保存进度" : "读取存档",
                     app->slots_saving ? "确定保存 · 长按确定删除" : "确定读取 · 长按确定返回", NULL,
                     label_ptrs, value_ptrs, n, app->slots_sel);
}

static void open_slots(tsxx_app_t *app, bool saving, tsxx_page_id_t from)
{
    app->slots_saving = saving;
    app->slots_return = from;
    app->slots_sel = 0;
    slots_refresh(app);
    set_page(app, saving ? TSXX_PAGE_SAVE : TSXX_PAGE_LOAD);
}

static void chapters_refresh(tsxx_app_t *app)
{
    char labels[TSXX_UI_MAX_ROWS][TSXX_CHAPTER_LABEL];
    const char *label_ptrs[TSXX_UI_MAX_ROWS];
    char counter[16];
    const int period = TSXX_UI_MAX_ROWS;

    if (app->chapter_count <= period) {
        app->chapters_top = 0;
    } else {
        int top = app->chapters_sel - period / 2;
        if (top < 0) top = 0;
        if (top > app->chapter_count - period) top = app->chapter_count - period;
        app->chapters_top = top;
    }

    int count = 0;
    for (int i = 0; i < period && app->chapters_top + i < app->chapter_count; ++i) {
        const int index = app->chapters_top + i;
        char raw[48];
        if (tsxx_pack_name(&app->pack, TSXX_TABLE_SPEAKER, app->chapters[index].name_id, raw,
                           sizeof(raw)) == 0) {
            labels[count][0] = '\0';
        } else {
            format_chapter(raw, labels[count], sizeof(labels[count]));
        }
        label_ptrs[count] = labels[count];
        ++count;
    }
    snprintf(counter, sizeof(counter), "%d/%d", app->chapters_sel + 1, app->chapter_count);
    tsxx_ui_set_list(&app->ui, TSXX_LIST_CHAPTERS, "章节跳转", "确定跳转 · 长按确定返回", counter,
                     label_ptrs, NULL, count, app->chapters_sel - app->chapters_top);
}

static void open_chapters(tsxx_app_t *app)
{
    app->chapters_sel = 0;
    chapters_refresh(app);
    set_page(app, TSXX_PAGE_CHAPTERS);
    if (app->chapter_count == 0) notify(app, "资源包里没有章节点");
}

// ---------------------------------------------------------------- 阅读推进
// 源工程的 "回到主页" 是一个特殊结局名:它表示剧本结束、直接回标题页,
// 所以不弹覆盖层。
static void show_ending(tsxx_app_t *app)
{
    char name[TSXX_ENDING_NAME];
    const bool named = tsxx_pack_end(&app->pack, app->player.page, name, sizeof(name));

    app->ended = true;
    app->auto_play = false;
    app->fast_forward = false;
    tsxx_ui_set_mode(&app->ui, false, false);
    tsxx_ui_hide_choices(&app->ui);
    render_scene(app);
    // 读完了就不再保留自动存档:标题页的"继续阅读"应该指向没读完的进度。
    (void)tsxx_slot_clear(TSXX_AUTO_SLOT);
    app->have_auto = false;

    if (named && strcmp(name, "回到主页") == 0) {
        ESP_LOGI(TAG, "结局「回到主页」(第 %u 页),直接回标题页",
                 (unsigned)app->player.page);
        show_title(app);
        return;
    }
    tsxx_ui_ending_overlay(&app->ui, named ? name : "—— 完 ——", "按确定返回标题", true);
    ESP_LOGI(TAG, "结局 %s(第 %u 页),按确定返回标题",
             named ? name : "页表末尾", (unsigned)app->player.page);
}

static bool start_reading(tsxx_app_t *app, uint32_t page)
{
    if (!tsxx_player_start(&app->player, &app->pack, page, &app->layout)) {
        // 走到这里只可能是 pack->page_count == 0 —— 启动时明明打印过页数,
        // 所以要么结构被写坏,要么打开就失败了。把关键字段都打出来定位。
        ESP_LOGE(TAG, "开读失败: 请求页=%u blob=%u bytes=%u pages=%u bg=%u fg=%u evb=%u"
                 " meta=%u",
                 (unsigned)page, (unsigned)app->pack.blob_size,
                 (unsigned)app->pack.blob_size, (unsigned)app->pack.page_count,
                 (unsigned)app->pack.bg_count, (unsigned)app->pack.fg_count,
                 (unsigned)app->pack.evb_count, (unsigned)app->pack.meta_size);
        notify(app, "剧本数据异常");
        return false;
    }
    app->started = true;
    app->ended = false;
    tsxx_ui_ending_overlay(&app->ui, NULL, NULL, false);
    set_page(app, TSXX_PAGE_GAME);
    render_scene(app);
    return true;
}

static void advance_reading(tsxx_app_t *app, bool skip_typing)
{
    // 打字中按任意键 = 立即显示全文(先看完整句,再按一次才翻页)。
    if (!skip_typing && tsxx_ui_typing(&app->ui)) {
        tsxx_ui_finish_typing(&app->ui);
        return;
    }
    switch (tsxx_player_advance(&app->player, &app->pack, &app->layout)) {
    case TSXX_STEP_SCREEN:
        render_scene(app);
        break;
    case TSXX_STEP_PAGE:
        auto_save(app, false);
        render_scene(app);
        break;
    case TSXX_STEP_CHOICE:
        // 选项一定要落盘:玩家可能就在这里放下设备。
        auto_save(app, true);
        app->fast_forward = false;
        app->auto_play = false;
        tsxx_ui_set_mode(&app->ui, false, false);
        render_scene(app);
        break;
    case TSXX_STEP_END:
        show_ending(app);
        break;
    case TSXX_STEP_STUCK:
    default:
        break;
    }
}

// ---------------------------------------------------------------- 按键
static void key_list(tsxx_app_t *app, int *selected, int count, int delta, tsxx_list_id_t id)
{
    if (count <= 0) return;
    *selected = clamp_cursor(*selected + delta, count);
    tsxx_ui_set_list_selected(&app->ui, id, *selected);
}

static void confirm_choice(tsxx_app_t *app)
{
    const int selected = tsxx_ui_choice_selected(&app->ui);
    if (!tsxx_player_choose(&app->player, &app->pack, (uint8_t)selected, &app->layout)) {
        return;
    }
    auto_save(app, true);
    ESP_LOGI(TAG, "选择支 #%d -> 第 %u 页", selected, (unsigned)app->player.page);
    render_scene(app);
}

static void key_warning(tsxx_app_t *app, const tsxx_key_t *key)
{
    if (key->ev == BSP_BTN_CLICK && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        // 提示正文必须真的能被翻完:先滚动,滚到底后同一颗键才开始放行。
        tsxx_ui_scroll_by(&app->ui, TSXX_PAGE_WARNING,
                          key->btn == BSP_BTN_DOWN ? TSXX_SCROLL_STEP : -TSXX_SCROLL_STEP);
        const bool bottom = tsxx_ui_scrolled_to_bottom(&app->ui, TSXX_PAGE_WARNING);
        tsxx_ui_set_warning(&app->ui, NULL, bottom ? "确定 开始阅读" : "上/下 滚动阅读到底部");
        return;
    }
    if (key->btn != BSP_BTN_OK || key->ev != BSP_BTN_CLICK) return;
    if (!tsxx_ui_scrolled_to_bottom(&app->ui, TSXX_PAGE_WARNING)) {
        const bool bottom = tsxx_ui_scroll_by(&app->ui, TSXX_PAGE_WARNING, 160);
        tsxx_ui_set_warning(&app->ui, NULL, bottom ? "确定 开始阅读" : "上/下 滚动阅读到底部");
        return;
    }
    // 同人提示已移除:这里只把警告页当作普通页处理(不再从标题页进入)。
    show_title(app);
}

static void title_select(tsxx_app_t *app)
{
    // 标题菜单是动态的(有自动存档时才多一行"继续阅读"),按当前显示的行取动作,
    // 顺序与 title_refresh() 一致。
    int index = 0;
    if (app->title_sel == index++) {
        (void)tsxx_slot_clear(TSXX_AUTO_SLOT);   // 新游戏:旧进度的自动存档不再有意义
        app->have_auto = false;
        (void)start_reading(app, 0);
        return;
    }
    if (app->have_auto) {
        if (app->title_sel == index++) {
            tsxx_save_t save;
            if (tsxx_slot_load(TSXX_AUTO_SLOT, &save) &&
                tsxx_player_load(&app->player, &app->pack, &save, &app->layout)) {
                app->started = true;
                app->ended = false;
                tsxx_ui_ending_overlay(&app->ui, NULL, NULL, false);
                set_page(app, TSXX_PAGE_GAME);
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
        open_slots(app, false, TSXX_PAGE_TITLE);
        return;
    }
    if (app->title_sel == index++) {
        open_chapters(app);
        return;
    }
    if (app->title_sel == index++) {
        open_settings(app, TSXX_PAGE_TITLE);
        return;
    }
    if (app->title_sel == index++) {
        open_about(app);
        return;
    }
}

static int title_row_count(const tsxx_app_t *app)
{
    return 5 + (app->have_auto ? 1 : 0);
}

static void key_title(tsxx_app_t *app, const tsxx_key_t *key)
{
    if (is_click(key) && key->btn == BSP_BTN_UP) {
        key_list(app, &app->title_sel, title_row_count(app), -1, TSXX_LIST_TITLE);
    } else if (is_click(key) && key->btn == BSP_BTN_DOWN) {
        key_list(app, &app->title_sel, title_row_count(app), 1, TSXX_LIST_TITLE);
    } else if (key->ev == BSP_BTN_CLICK && key->btn == BSP_BTN_OK) {
        title_select(app);
    } else if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        app->sleep_requested = true;   // 标题页长按确定 = 关机
    }
}

static void key_menu(tsxx_app_t *app, const tsxx_key_t *key)
{
    const int count = 6;
    if (is_click(key) && key->btn == BSP_BTN_UP) {
        key_list(app, &app->menu_sel, count, -1, TSXX_LIST_MENU);
    } else if (is_click(key) && key->btn == BSP_BTN_DOWN) {
        key_list(app, &app->menu_sel, count, 1, TSXX_LIST_MENU);
    } else if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_CLICK) {
        switch (app->menu_sel) {
        case 0:   // 继续阅读
            set_page(app, TSXX_PAGE_GAME);
            render_scene(app);
            break;
        case 1:   // 保存进度
            open_slots(app, true, TSXX_PAGE_MENU);
            break;
        case 2:   // 读取存档
            open_slots(app, false, TSXX_PAGE_MENU);
            break;
        case 3: { // 跳过章节:一直推到下一个章节点,遇到选项/末尾就停下
            const tsxx_step_t step = tsxx_player_skip_chapter(&app->player, &app->pack,
                                                             &app->layout);
            set_page(app, TSXX_PAGE_GAME);
            switch (step) {
            case TSXX_STEP_CHOICE:
                auto_save(app, true);
                render_scene(app);
                notify(app, "遇到选项,已停下");
                break;
            case TSXX_STEP_END:
                show_ending(app);
                break;
            case TSXX_STEP_STUCK:
                render_scene(app);
                notify(app, "已经是本章末尾");
                break;
            default:
                auto_save(app, true);
                render_scene(app);
                break;
            }
            break;
        }
        case 4:   // 章节跳转
            open_chapters(app);
            break;
        default:  // 返回标题
            show_title(app);
            break;
        }
    } else if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_LONG) {
        set_page(app, TSXX_PAGE_GAME);   // 长按确定 = 回到正文
    }
}

static void key_settings(tsxx_app_t *app, const tsxx_key_t *key)
{
    const int count = 4;
    if (is_click(key) && key->btn == BSP_BTN_UP) {
        key_list(app, &app->settings_sel, count, -1, TSXX_LIST_SETTINGS);
    } else if (is_click(key) && key->btn == BSP_BTN_DOWN) {
        key_list(app, &app->settings_sel, count, 1, TSXX_LIST_SETTINGS);
    } else if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_CLICK) {
        switch (app->settings_sel) {
        case 0:   // 文字速度:循环 慢 / 中 / 快
            app->settings.text_speed =
                (uint8_t)((speed_level(app) + 1) % (uint8_t)(sizeof(s_speed_ms) / sizeof(s_speed_ms[0])));
            tsxx_ui_set_list_value(&app->ui, TSXX_LIST_SETTINGS, 0,
                                   s_speed_names[speed_level(app)]);
            (void)tsxx_settings_store(&app->settings);
            break;
        case 1:   // 自动阅读:开关
            app->settings.auto_play = app->settings.auto_play ? 0 : 1;
            app->auto_play = app->settings.auto_play != 0;
            app->auto_ms = TSXX_AUTO_MS;
            tsxx_ui_set_list_value(&app->ui, TSXX_LIST_SETTINGS, 1,
                                   app->settings.auto_play ? "开" : "关");
            tsxx_ui_set_mode(&app->ui, app->auto_play, app->fast_forward);
            (void)tsxx_settings_store(&app->settings);
            break;
        case 2:   // 关于本作
            open_about(app);
            break;
        default:  // 返回
            set_page(app, app->settings_return);
            if (app->settings_return == TSXX_PAGE_TITLE) title_refresh(app);
            break;
        }
    } else if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_LONG) {
        set_page(app, app->settings_return);
        if (app->settings_return == TSXX_PAGE_TITLE) title_refresh(app);
    }
}

static void key_slots(tsxx_app_t *app, const tsxx_key_t *key)
{
    // 存档页只有 5 个手动槽 + 返回;读档页多一个"自动存档"。
    const int count = TSXX_SAVE_SLOTS + (app->slots_saving ? 1 : 2);
    const tsxx_list_id_t id = app->slots_saving ? TSXX_LIST_SAVE : TSXX_LIST_LOAD;
    if (is_click(key) && key->btn == BSP_BTN_UP) {
        key_list(app, &app->slots_sel, count, -1, id);
    } else if (is_click(key) && key->btn == BSP_BTN_DOWN) {
        key_list(app, &app->slots_sel, count, 1, id);
    } else if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_CLICK) {
        if (app->slots_sel >= count - 1) {   // 返回
            set_page(app, app->slots_return);
            if (app->slots_return == TSXX_PAGE_TITLE) title_refresh(app);
            return;
        }
        if (app->slots_saving) {
            tsxx_save_t save;
            tsxx_save_from_player(&app->player, &save);
            notify(app, tsxx_slot_store((uint8_t)app->slots_sel, &save) ? "已保存" : "保存失败");
            slots_refresh(app);
        } else {
            const bool is_auto = app->slots_sel >= TSXX_SAVE_SLOTS;
            const uint8_t slot = is_auto ? TSXX_AUTO_SLOT : (uint8_t)app->slots_sel;
            tsxx_save_t save;
            if (tsxx_slot_load(slot, &save) &&
                tsxx_player_load(&app->player, &app->pack, &save, &app->layout)) {
                app->started = true;
                app->ended = false;
                tsxx_ui_ending_overlay(&app->ui, NULL, NULL, false);
                set_page(app, TSXX_PAGE_GAME);
                render_scene(app);
            } else {
                notify(app, "该存档为空");
            }
        }
    } else if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_LONG) {
        // 存档模式:长按删除当前槽;其他情况长按返回上一层。
        if (app->slots_saving && app->slots_sel < TSXX_SAVE_SLOTS) {
            notify(app, tsxx_slot_clear((uint8_t)app->slots_sel) ? "已删除" : "删除失败");
            slots_refresh(app);
            return;
        }
        set_page(app, app->slots_return);
        if (app->slots_return == TSXX_PAGE_TITLE) title_refresh(app);
    }
}

static void key_chapters(tsxx_app_t *app, const tsxx_key_t *key)
{
    const int count = app->chapter_count;
    if (is_click(key) && key->btn == BSP_BTN_UP) {
        if (count > 0) {
            app->chapters_sel = clamp_cursor(app->chapters_sel - 1, count);
            chapters_refresh(app);
        }
    } else if (is_click(key) && key->btn == BSP_BTN_DOWN) {
        if (count > 0) {
            app->chapters_sel = clamp_cursor(app->chapters_sel + 1, count);
            chapters_refresh(app);
        }
    } else if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_CLICK) {
        if (count > 0) {
            const uint32_t page = app->chapters[app->chapters_sel].page;
            char label[TSXX_CHAPTER_LABEL];
            if (start_reading(app, page)) {
                chapter_label_for(app, page, label, sizeof(label));
                ESP_LOGI(TAG, "跳到章节 %s(第 %u 页)", label, (unsigned)page);
                auto_save(app, true);
            }
        }
    } else if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_LONG) {
        show_title(app);
    }
}

static void key_game(tsxx_app_t *app, const tsxx_key_t *key)
{
    if (app->ended) {   // 结局覆盖层:任意键回标题
        if (is_press(key)) show_title(app);
        return;
    }
    // 长按下 = 自动阅读开关(任意按键停下,见 tsxx_app_key)。
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_DOWN) {
        app->auto_play = !app->auto_play;
        app->auto_ms = TSXX_AUTO_MS;
        app->fast_forward = false;
        app->settings.auto_play = app->auto_play ? 1 : 0;
        (void)tsxx_settings_store(&app->settings);
        tsxx_ui_set_mode(&app->ui, app->auto_play, false);
        notify(app, app->auto_play ? "自动阅读：开" : "自动阅读：关");
        return;
    }
    // 长按上 = 快进:按住就一直推,松手停。选项上不快进 —— 玩家必须自己做选择。
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_UP) {
        if (app->player.at_choice) return;
        app->fast_forward = true;
        app->fast_forward_ms = 0;
        app->auto_play = false;
        tsxx_ui_set_mode(&app->ui, false, true);
        advance_reading(app, true);
        return;
    }
    if (key->ev == BSP_BTN_RELEASE) {
        if (app->fast_forward) {
            app->fast_forward = false;
            tsxx_ui_set_mode(&app->ui, app->auto_play, false);
        }
        return;
    }
    if (key->ev == BSP_BTN_LONG && key->btn == BSP_BTN_OK) {
        open_menu(app);
        return;
    }
    if (!is_click(key)) return;

    if (app->player.at_choice) {
        const int selected = tsxx_ui_choice_selected(&app->ui);
        if (key->btn == BSP_BTN_UP) {
            tsxx_ui_set_choice_selected(&app->ui, selected - 1);
        } else if (key->btn == BSP_BTN_DOWN) {
            tsxx_ui_set_choice_selected(&app->ui, selected + 1);
        } else {
            confirm_choice(app);
        }
        return;
    }
    // 短按 OK = 打开菜单;上/下 推进正文。
    if (key->btn == BSP_BTN_OK) {
        open_menu(app);
        return;
    }
    advance_reading(app, false);
}

static void key_about(tsxx_app_t *app, const tsxx_key_t *key)
{
    if (key->btn == BSP_BTN_OK && key->ev == BSP_BTN_CLICK) {
        open_settings(app, app->settings_return);
        return;
    }
    if (is_click(key) && (key->btn == BSP_BTN_UP || key->btn == BSP_BTN_DOWN)) {
        tsxx_ui_scroll_by(&app->ui, TSXX_PAGE_ABOUT,
                          key->btn == BSP_BTN_DOWN ? TSXX_SCROLL_STEP_ABOUT
                                                   : -TSXX_SCROLL_STEP_ABOUT);
    }
}

// ---------------------------------------------------------------- 对外接口

// 打开资源包之前把应用状态清干净(画面缓存、电量、美术层下标)。
static void app_reset(tsxx_app_t *app)
{
    memset(app, 0, sizeof(*app));
    app->art_bg = TSXX_NONE8;
    app->art_sprite = TSXX_NONE8;
    app->battery_percent = -1;
}

// 资源包已经挂载之后的初始化:存档/设置/界面/章节/首屏。
static bool app_after_pack_open(tsxx_app_t *app, uint16_t *art_pixels, const lv_font_t *font_cjk)
{
    if (!tsxx_save_init()) {
        ESP_LOGW(TAG, "NVS 不可用,本次运行不保存进度");
    }
    // 先给默认值再读 NVS:没有存档时也要是"中速",而不是 0(慢)。
    tsxx_settings_default(&app->settings);
    (void)tsxx_settings_load(&app->settings);
    if (app->settings.text_speed > 2) app->settings.text_speed = TSXX_SPEED_DEFAULT;
    app->auto_play = app->settings.auto_play != 0;

    // 16px 字体、208px 正文区:一行 13 个全角字,一屏 5 行。
    app->layout.units_per_line = TSXX_UNITS_PER_LINE;
    app->layout.lines_per_page = TSXX_TEXT_LINES;

    if (!tsxx_ui_create(&app->ui, art_pixels, font_cjk)) {
        ESP_LOGE(TAG, "界面创建失败");
        return false;
    }
    {
        // 无 PSRAM 的板子上池子很紧,把建完界面后的余量留在日志里。
        lv_mem_monitor_t mon;
        lv_mem_monitor(&mon);
        ESP_LOGI(TAG, "LVGL 池 %u B,建完界面空闲 %u B (最大连续 %u B,碎片 %u%%)",
                 (unsigned)mon.total_size, (unsigned)mon.free_size,
                 (unsigned)mon.free_biggest_size, (unsigned)mon.frag_pct);
    }

    app->chapter_count = tsxx_chapters_scan(&app->pack, app->chapters, TSXX_MAX_CHAPTERS);

    tsxx_save_t save;
    app->have_auto = tsxx_slot_load(TSXX_AUTO_SLOT, &save);

    // 开机直接进标题页;同人移植提示推迟到第一次真的要进正文之前
    // (见 title_select 与 key_warning)。
    show_title(app);

    ESP_LOGI(TAG, "就绪:页 %u / 背景 %u / 立绘 %u / 事件图 %u / 选择支 %u / 章节点 %d",
             (unsigned)tsxx_pack_pages(&app->pack), (unsigned)tsxx_pack_bg_count(&app->pack),
             (unsigned)tsxx_pack_sprite_count(&app->pack), (unsigned)tsxx_pack_cg_count(&app->pack),
             (unsigned)app->pack.choice_count, app->chapter_count);
    ESP_LOGI(TAG, "分支:结局点 %u 个 / 闸门 %u 道",
             (unsigned)tsxx_pack_end_count(&app->pack),
             (unsigned)tsxx_pack_gate_count(&app->pack));
    return true;
}

bool tsxx_app_init(tsxx_app_t *app, const uint8_t *pack_data, uint32_t pack_size,
                   uint16_t *art_pixels, const lv_font_t *font_cjk)
{
    if (!app || !pack_data || !art_pixels || !font_cjk) return false;
    app_reset(app);
    if (!tsxx_pack_open(&app->pack, pack_data, pack_size)) {
        ESP_LOGE(TAG, "资源包解析失败(%u 字节)", (unsigned)pack_size);
        return false;
    }
    return app_after_pack_open(app, art_pixels, font_cjk);
}

bool tsxx_app_init_partition(tsxx_app_t *app, const char *pack_partition, uint16_t *art_pixels,
                             const lv_font_t *font_cjk)
{
    if (!app || !pack_partition || !art_pixels || !font_cjk) return false;
    app_reset(app);
    if (!tsxx_pack_open_partition(&app->pack, pack_partition)) {
        ESP_LOGE(TAG, "资源分区 %s 解析失败", pack_partition);
        return false;
    }
    return app_after_pack_open(app, art_pixels, font_cjk);
}

void tsxx_app_key(tsxx_app_t *app, const tsxx_key_t *key)
{
    if (!app || !key) return;
    app->idle_ms = 0;
    if (app->notice_ms) {
        app->notice_ms = 0;
        tsxx_ui_notice(&app->ui, "");
    }

    // 自动阅读:只有真实的"按键"(单击/双击/长按)才停下来。不能把 PRESS/RELEASE
    // 也算进去 —— 长按下的抬起事件会在刚开完开关后立刻把自动模式关掉。
    const bool is_auto_toggle =
        (key->btn == BSP_BTN_DOWN && key->ev == BSP_BTN_LONG && app->page == TSXX_PAGE_GAME);
    if (app->auto_play && is_press(key) && !is_auto_toggle) {
        app->auto_play = false;
        app->fast_forward = false;
        tsxx_ui_set_mode(&app->ui, false, false);
    }

    switch (app->page) {
    case TSXX_PAGE_WARNING: key_warning(app, key); break;
    case TSXX_PAGE_TITLE: key_title(app, key); break;
    case TSXX_PAGE_GAME: key_game(app, key); break;
    case TSXX_PAGE_MENU: key_menu(app, key); break;
    case TSXX_PAGE_SAVE:
    case TSXX_PAGE_LOAD: key_slots(app, key); break;
    case TSXX_PAGE_CHAPTERS: key_chapters(app, key); break;
    case TSXX_PAGE_SETTINGS: key_settings(app, key); break;
    case TSXX_PAGE_ABOUT: key_about(app, key); break;
    default: break;
    }
}

void tsxx_app_tick(tsxx_app_t *app, uint32_t elapsed_ms)
{
    if (!app) return;
    // 自动阅读时玩家不需要碰机器,这段时间不能算空闲 —— 否则会读到一半自己变暗、
    // 熄屏甚至休眠。停在选项或结局上时自动翻页已经停下,照旧按时限熄灭。
    if (tsxx_app_auto_reading(app)) {
        app->idle_ms = 0;
    } else {
        app->idle_ms += elapsed_ms;
    }

    if (app->notice_ms > 0) {
        app->notice_ms = app->notice_ms > elapsed_ms ? app->notice_ms - elapsed_ms : 0;
        if (app->notice_ms == 0) tsxx_ui_notice(&app->ui, "");
    }
    if (app->auto_save_ms < TSXX_AUTOSAVE_MS) {
        app->auto_save_ms += elapsed_ms;
    }

    // 打字机:先推进,再让自动阅读按"打完的时间"开始计时。
    if (app->page == TSXX_PAGE_GAME && tsxx_ui_typing(&app->ui)) {
        tsxx_ui_typing_tick(&app->ui, elapsed_ms, speed_ms(app));
    }

    if (app->auto_play && app->page == TSXX_PAGE_GAME && !app->ended) {
        if (tsxx_ui_typing(&app->ui) || app->player.at_choice) {
            app->auto_ms = TSXX_AUTO_MS;   // 还在打字 / 等玩家选择:倒计时保持在满格
        } else if (app->auto_ms > elapsed_ms) {
            app->auto_ms -= elapsed_ms;
        } else {
            app->auto_ms = TSXX_AUTO_MS;
            advance_reading(app, false);
        }
    }

    if (app->fast_forward && app->page == TSXX_PAGE_GAME && !app->ended) {
        app->fast_forward_ms += elapsed_ms;
        while (app->fast_forward_ms >= TSXX_FASTFORWARD_MS) {
            app->fast_forward_ms -= TSXX_FASTFORWARD_MS;
            advance_reading(app, true);
            if (!app->fast_forward || app->player.at_choice || app->ended) break;
        }
    }

    app->battery_accum_ms += elapsed_ms;
    if (app->battery_accum_ms >= TSXX_BATTERY_INTERVAL_MS || app->battery_percent < 0) {
        app->battery_accum_ms = 0;
        const int soc = bsp_battery_soc();
        if (soc != app->battery_percent) {
            app->battery_percent = soc;
            tsxx_ui_set_battery(&app->ui, soc);
        }
    }
}

bool tsxx_app_auto_reading(const tsxx_app_t *app)
{
    if (!app) return false;
    return app->auto_play && app->page == TSXX_PAGE_GAME && !app->player.at_choice && !app->ended;
}

uint32_t tsxx_app_idle_ms(const tsxx_app_t *app)
{
    return app ? app->idle_ms : 0;
}

void tsxx_app_before_sleep(tsxx_app_t *app)
{
    if (!app) return;
    if (app->page == TSXX_PAGE_GAME && !app->ended) auto_save(app, true);
    if (app->started) {
        (void)tsxx_settings_store(&app->settings);
    }
}

void tsxx_app_show_sleeping(tsxx_app_t *app)
{
    if (!app) return;
    tsxx_ui_hide_choices(&app->ui);
    tsxx_ui_ending_overlay(&app->ui, "休眠中", "按任意键唤醒", true);
    set_page(app, TSXX_PAGE_GAME);
}

bool tsxx_app_debug_render(tsxx_app_t *app, uint32_t page)
{
    if (!app) return false;
    tsxx_player_t temp = app->player;
    if (!tsxx_player_jump(&temp, &app->pack, page, &app->layout)) return false;
    // 只换画面,不动玩家状态:截图不该改变"读到哪"。
    tsxx_ui_ending_overlay(&app->ui, NULL, NULL, false);
    set_page(app, TSXX_PAGE_GAME);
    render_player(app, &temp);
    ESP_LOGW(TAG, "调试:已把第 %u 页画到屏幕上(玩家进度未改动)", (unsigned)page);
    return true;
}

bool tsxx_app_debug_start(tsxx_app_t *app, uint32_t page)
{
    if (!app) return false;
    if (!start_reading(app, page)) return false;
    ESP_LOGW(TAG, "调试:直接从第 %u 页开始阅读", (unsigned)page);
    return true;
}

bool tsxx_app_take_sleep_request(tsxx_app_t *app)
{
    if (!app || !app->sleep_requested) return false;
    app->sleep_requested = false;
    return true;
}
