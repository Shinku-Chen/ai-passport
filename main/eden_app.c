// main/eden_app.c —— eden* 应用状态机实现。
//
// 分层:脚本执行(eden_script,纯逻辑) / 画面解码(eden_image) / 持久化(eden_store)
//       / 界面(eden_ui) / 本文件只做"什么时候调谁"。
#include "eden_app.h"

#include "bsp_battery.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "esp_heap_caps.h"
#include "eden_image.h"
#include "eden_pack.h"
#include "eden_script.h"
#include "eden_store.h"
#include "eden_ui.h"
#include "gal_settings.h"
#include "gal_text.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "eden_app";

// 链接进固件的资源包(main/CMakeLists.txt 的 EMBED_FILES)。
// 宿主机测试用 EDEN_APP_HOST_TEST 关掉这两个外部符号,改由 eden_app_init_with_pack()
// 传入内存里的合成包 —— 测的是同一份应用代码,不是复制一份逻辑。
#ifndef EDEN_APP_HOST_TEST
extern const uint8_t eden_pack_start[] asm("_binary_eden_pack_bin_start");
extern const uint8_t eden_pack_end[] asm("_binary_eden_pack_bin_end");
#endif
// 标题徽标(LVGL RGB565A8,由 tools/eden_pack.py 生成;它是一个纯数据文件,直接纳入本
// 编译单元可以省一个目标文件,并保证 W/H 与数据尺寸始终成对)。
#include "eden_data/eden_logo.c"

// 中文字体子集(assets/fonts 下由 tools/gal_font.py 生成)。
LV_FONT_DECLARE(gal_cjk_16)
LV_FONT_DECLARE(gal_cjk_20)

#define EDEN_HISTORY_MAX 8
// 标题背景帧：第 0 帧（264x168，零内存增量，复用对话页画布）。
#define EDEN_TITLE_FRAME 0u
#define EDEN_FAST_STEP_MS 110u
#define EDEN_AUTO_STEP_MS 2400u
#define EDEN_AUTO_AFTER_TYPING_MS 900u
#define EDEN_BATTERY_PERIOD_MS 30000u

typedef struct {
    uint16_t speaker;
    uint16_t text;
} eden_history_t;
typedef struct {
    bool ready;
    eden_pack_t pack;
    eden_state_t script;
    eden_ui_t ui;
    eden_image_t image;

    eden_slot_t slots[EDEN_SAVE_SLOTS];
    eden_progress_t progress;
    eden_unlock_t unlock;
    gal_settings_t settings;

    // 当前这一句
    eden_line_t line;
    uint16_t drawn_frame;          // 画布上当前是哪一帧(同帧翻页不重复解码)
    bool drawn_valid;
    bool line_valid;
    uint16_t page;                 // 这一句里的第几页
    uint16_t pages;                // 这一句共几页
    int16_t chapter;               // 当前章节下标(-1 = 未知)
    char speaker_buf[32];

    // 历史(环形,最近 8 句)
    eden_history_t history[EDEN_HISTORY_MAX];
    int history_count;
    int history_head;

    // 选择下标
    int sel[EDEN_LIST_COUNT];
    int cg_page;
    int cg_sel;

    // 时间推进
    bool fast_forward;
    uint32_t last_step_ms;
    uint32_t last_input_ms;
    uint32_t last_progress_ms;
    uint32_t last_battery_ms;
    uint32_t now_ms;

    // P0:页面来源栈。所有"长按 OK 返回"统一 pop,避免各入口硬编码目标页
    // (标题→设置、标题→读档、正文→菜单→设置 的返回目标各不相同)。
    // 容量 6:鉴赏→CG 列表→大图 + 章节往返都能放下，减少满栈风险。
    eden_page_t stack[6];
    int stack_depth;

    // P1e:解锁位落盘状态。写失败置脏并退避重试,flush 时强制再试一次。
    bool unlock_dirty;
    uint32_t unlock_retry_at_ms;
    uint32_t unlock_backoff_ms;

    // 存档页:覆盖确认(第一次 OK 只提示,第二次才写)
    int confirm_slot;
    uint32_t idle_ms;
} eden_app_t;

static eden_app_t s_app;
// 长按 OK 进菜单后驱动常再补一个 CLICK：吞掉它，避免多推一句。
static bool s_suppress_click;
// ---------------------------------------------------------------- 文本辅助
static uint32_t units_per_line(void)
{
    // 20px 字体一行少放一些字:与 gal_text 的单位宽度约定保持一致。
    return s_app.settings.font_large ? EDEN_UNITS_LARGE : EDEN_UNITS_SMALL;
}

static const char *speaker_text(uint16_t speaker)
{
    const char *name = eden_pack_text(&s_app.pack, speaker);
    return name != NULL ? name : "";
}

static uint32_t text_speed_ms(const gal_settings_t *settings)
{
    switch (settings->text_speed) {
    case 0: return 55;
    case 1: return 35;
    case 2: return 20;
    default: return 0;
    }
}

// ---------------------------------------------------------------- 解锁/进度
static void mark_unlock_dirty(void)
{
    // 已经在待重试状态时不要反复加倍退避（一次翻页可能连续解锁多个 CG）。
    if (s_app.unlock_dirty) return;
    s_app.unlock_dirty = true;
    if (s_app.unlock_backoff_ms == 0) s_app.unlock_backoff_ms = 2000;
    s_app.unlock_retry_at_ms = s_app.now_ms + s_app.unlock_backoff_ms;
}

// 落盘解锁位：失败按 2s/4s/8s...（上限 60s）退避重试；force=true 忽略退避。
static bool flush_unlock(bool force)
{
    if (!s_app.unlock_dirty) return true;
    if (!force && s_app.now_ms < s_app.unlock_retry_at_ms) return false;
    if (eden_store_save_unlock(&s_app.unlock)) {
        s_app.unlock_dirty = false;
        s_app.unlock_backoff_ms = 0;
        s_app.unlock_retry_at_ms = 0;
        return true;
    }
    s_app.unlock_backoff_ms = s_app.unlock_backoff_ms ? s_app.unlock_backoff_ms * 2 : 4000;
    if (s_app.unlock_backoff_ms > 60000) s_app.unlock_backoff_ms = 60000;
    s_app.unlock_retry_at_ms = s_app.now_ms + s_app.unlock_backoff_ms;
    return false;
}

static void mark_chapter_unlocked(uint16_t index)
{
    if (eden_unlock_has_chapter(&s_app.unlock, index)) return;
    eden_unlock_grant_chapter(&s_app.unlock, index);
    mark_unlock_dirty();
}

// 这一帧属于哪个 CG 组就解锁它（对齐鉴赏室的 group 语义）。
static void grant_cg_for_frame(uint16_t frame)
{
    const uint16_t count = eden_pack_gallery_count(&s_app.pack);
    for (uint16_t i = 0; i < count; i++) {
        eden_gallery_t entry;
        if (!eden_pack_gallery(&s_app.pack, i, &entry)) continue;
        if (entry.frame != frame) continue;
        // 同一帧可能被多条鉴赏记录共用（合集/差分），只解锁**第一条**命中的组，
        // 否则一次渲染会把同帧的所有条目一起点亮。
        if (!eden_unlock_has_cg(&s_app.unlock, entry.group)) {
            eden_unlock_grant_cg(&s_app.unlock, entry.group);
            mark_unlock_dirty();
        }
        break;
    }
}

// 进度：force=true 立即写（睡前/离开正文），否则每 2s 节流一次。
static void store_progress(bool force)
{
    if (!s_app.line_valid) return;
    if (!force && s_app.now_ms - s_app.last_progress_ms < 2000u) return;
    s_app.last_progress_ms = s_app.now_ms;
    eden_progress_t p;
    memset(&p, 0, sizeof(p));
    p.valid = 1;
    p.scene = s_app.script.scene;
    p.op = s_app.script.op;
    p.page = s_app.page;
    memcpy(p.vars, s_app.script.vars, sizeof(p.vars));
    s_app.progress = p;
    (void)eden_store_save_progress(&p);
}

// ---------------------------------------------------------------- 文本/界面
// 按 UTF-8 码点截断到 max_chars 个字符（不切半个汉字）。
static void copy_utf8_prefix(char *dst, size_t cap, const char *src, int max_chars)
{
    if (dst == NULL || cap == 0) return;
    dst[0] = 0;
    if (src == NULL) return;
    size_t out = 0;
    int chars = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && chars < max_chars;) {
        int step = 1;
        if ((*p & 0xE0u) == 0xC0u) step = 2;
        else if ((*p & 0xF0u) == 0xE0u) step = 3;
        else if ((*p & 0xF8u) == 0xF0u) step = 4;
        if (out + (size_t)step >= cap) break;
        for (int i = 0; i < step; i++) dst[out++] = (char)p[i];
        p += step;
        chars++;
    }
    dst[out] = 0;
}

// 历史行：最近 8 句，越新越靠前。
static void build_history_rows(const char *labels[])
{
    static char storage[EDEN_HISTORY_MAX][48];
    for (int i = 0; i < s_app.history_count; i++) {
        const int slot = (s_app.history_head - 1 - i + EDEN_HISTORY_MAX * 2) % EDEN_HISTORY_MAX;
        const eden_history_t *h = &s_app.history[slot];
        const char *speaker = eden_pack_text(&s_app.pack, h->speaker);
        const char *text = eden_pack_text(&s_app.pack, h->text);
        snprintf(storage[i], sizeof(storage[i]), "%s%s%s",
                 (speaker != NULL && speaker[0]) ? speaker : "",
                 (speaker != NULL && speaker[0]) ? "：" : "",
                 text != NULL ? text : "");
        labels[i] = storage[i];
    }
}

static void history_push(uint16_t speaker, uint16_t text)
{
    s_app.history[s_app.history_head].speaker = speaker;
    s_app.history[s_app.history_head].text = text;
    s_app.history_head = (s_app.history_head + 1) % EDEN_HISTORY_MAX;
    if (s_app.history_count < EDEN_HISTORY_MAX) s_app.history_count++;
}

// 顶部 HUD：章节名 + 电量 + 自动标记。
static void refresh_status(void)
{
    eden_chapter_t info;
    if (s_app.chapter >= 0 &&
        eden_pack_chapter(&s_app.pack, (uint16_t)s_app.chapter, &info)) {
        eden_ui_set_chapter(&s_app.ui, eden_pack_text(&s_app.pack, info.name));
    }
    eden_ui_set_battery(&s_app.ui, bsp_battery_soc());
    eden_ui_set_mode(&s_app.ui, s_app.settings.auto_play != 0);
}

// 设置项生效（字号/面板透明度）。
static void apply_display_settings(void)
{
    eden_ui_set_font_large(&s_app.ui, s_app.settings.font_large != 0);
    eden_ui_set_panel_opacity(&s_app.ui, gal_panel_opa(s_app.settings.panel_transparency));
}

static void render_page(void)
{
    const char *full = eden_pack_text(&s_app.pack, s_app.line.text);
    if (full == NULL) {
        full = "";
    }
    uint32_t offsets[GAL_TEXT_MAX_PAGES + 1];
    const int pages = gal_text_pages(full, units_per_line(), EDEN_TEXT_LINES, offsets,
                                     GAL_TEXT_MAX_PAGES + 1);
    s_app.pages = (uint16_t)pages;
    if (s_app.page >= s_app.pages) {
        s_app.page = (uint16_t)(s_app.pages - 1);
    }
    const uint32_t start = offsets[s_app.page];
    const uint32_t end = offsets[s_app.page + 1];

    static char page_buffer[EDEN_TEXT_CAPACITY];
    uint32_t length = end - start;
    if (length >= sizeof(page_buffer)) {
        length = sizeof(page_buffer) - 1;
    }
    memcpy(page_buffer, full + start, length);
    page_buffer[length] = '\0';

    // 人名:第 0 页显示,续页留空,避免每翻一页都重复名牌。
    const char *speaker = s_app.page == 0 ? speaker_text(s_app.line.speaker) : "";
    // 快进时整页瞬显（ms_per_char=0 直接不走打字机）；自动阅读仍逐字。
    eden_ui_set_dialogue(&s_app.ui, speaker, page_buffer,
                         s_app.fast_forward ? 0 : text_speed_ms(&s_app.settings));
    eden_ui_set_page(&s_app.ui, s_app.page, s_app.pages);
    // 同一帧只解码一次:一句话翻到第二页时画面没变,不必再走一遍 TJpgDec。
    // 只有解码**完整成功**才把这一帧记进缓存:失败后必须保持"未画出",
    // 否则下一次同帧渲染会命中缓存、误判成功,把坏图当已解锁写进鉴赏室。
    bool drawn;
    if (s_app.drawn_valid && s_app.drawn_frame == s_app.line.frame) {
        drawn = true;
    } else {
        drawn = eden_image_draw(&s_app.image, s_app.line.frame);
        s_app.drawn_valid = drawn;          // 失败 -> 缓存保持无效
        if (drawn) {
            s_app.drawn_frame = s_app.line.frame;
        } else {
            ESP_LOGE(TAG, "画面 %u 解码失败(坏包?),不解锁 CG", (unsigned)s_app.line.frame);
        }
    }
    // 只有画面完整合成时才记解锁:解码失败却解锁会让鉴赏室出现打不开的条目。
    if (drawn) {
        grant_cg_for_frame(s_app.line.frame);
        // 分带只在“画面真的变了”时重画：同一帧连着好几句话时，四条带重复刷新
        // 就是用户看到的“上方色块跳动”。指纹 = 帧号 + 三个立绘 + cutin。
        // 画面由**单个** lv_image 缩放铺满（见 eden_ui.c place_canvas）：
        // 零新增大块、无分带、无逐带刷新。
    }
    refresh_status();
    store_progress(false);
}

// 取下一句并渲染;scenes 变化时先走章节过场。
static void step_next(bool allow_transition);

static void show_line(void)
{
    s_app.line_valid = true;
    s_app.page = 0;
    history_push(s_app.line.speaker, s_app.line.text);
    render_page();
}

// 原样切页(不碰栈):只有栈操作内部用。
static void show_page_raw(eden_page_t page)
{
    // 快进只在正文页有效:离开正文页一律取消,避免回到正文后继续自动推进。
    if (page != EDEN_PAGE_GAME) {
        s_app.fast_forward = false;
    }
    eden_ui_show_page(&s_app.ui, page);
}

// 进入"子页面":记下来源,长按 OK 统一按栈返回。
static void enter_sub_page(eden_page_t page)
{
    // 同页重复进入不再压栈:列表里移动光标时重画内容,不能把来源栈填满。
    // (真正的返回仍必须调 page_back(),不能靠这里的防御掩盖错误的返回目标。)
    if (eden_ui_page(&s_app.ui) == page) {
        show_page_raw(page);
        return;
    }
    const int cap = (int)(sizeof(s_app.stack) / sizeof(s_app.stack[0]));
    if (s_app.stack_depth >= cap) {
        // 栈满时丢最旧一层并告警：宁可少一层，也不能让 page_back() 弹到错误页面。
        // 日志用 ASCII：新增汉字会进入字体子集校验，而本轮不允许改字体。
        ESP_LOGW(TAG, "return stack full (%d), dropping oldest entry", cap);
        memmove(&s_app.stack[0], &s_app.stack[1],
                (size_t)(cap - 1) * sizeof(s_app.stack[0]));
        s_app.stack_depth = cap - 1;
    }
    s_app.stack[s_app.stack_depth++] = eden_ui_page(&s_app.ui);
    show_page_raw(page);
}

// 进入"根页面"(启动/标题/正文):清空来源栈。
static void enter_root_page(eden_page_t page)
{
    s_app.stack_depth = 0;
    show_page_raw(page);
}

// 统一返回:回到真实的来源页;栈空时退回正文(没有正文则回标题)。
static void page_back(void)
{
    if (s_app.stack_depth > 0) {
        const eden_page_t previous = s_app.stack[--s_app.stack_depth];
        show_page_raw(previous);
        return;
    }
    if (s_app.line_valid) {
        show_page_raw(EDEN_PAGE_GAME);
    } else {
        show_page_raw(EDEN_PAGE_TITLE);
    }
}

// 章节过场:换章时先显示"第 N 章"卡片,OK 继续。
static bool maybe_show_transition(void)
{
    const int chapter = eden_script_chapter_of(&s_app.pack, s_app.line.scene);
    if (chapter < 0 || chapter == s_app.chapter) {
        return false;
    }
    s_app.chapter = chapter;
    mark_chapter_unlocked((uint16_t)chapter);
    eden_chapter_t info;
    if (!eden_pack_chapter(&s_app.pack, (uint16_t)chapter, &info)) {
        return false;
    }
    char eyebrow[32];
    snprintf(eyebrow, sizeof(eyebrow), "CHAPTER %02d", chapter + 1);
    eden_ui_set_transition(&s_app.ui, eyebrow, eden_pack_text(&s_app.pack, info.name),
                           "按 OK 继续");
    enter_sub_page(EDEN_PAGE_TRANSITION);
    return true;
}

static void step_next(bool allow_transition)
{
    s_app.line_valid = false;
    for (int guard = 0; guard < 4; guard++) {
        const eden_step_t kind = eden_script_step(&s_app.pack, &s_app.script, &s_app.line);
        if (kind == EDEN_STEP_END) {
            eden_ui_set_ending(&s_app.ui, "TRUE END", "eden*", "—— 感谢游玩 ——");
            enter_root_page(EDEN_PAGE_ENDING);
            eden_unlock_grant_ending(&s_app.unlock, 0);
            mark_unlock_dirty();
            (void)flush_unlock(false);
            return;
        }
        if (kind != EDEN_STEP_TEXT) {
            ESP_LOGE(TAG, "剧本执行失败 scene=%u op=%u", (unsigned)s_app.script.scene,
                     (unsigned)s_app.script.op);
            return;
        }
        if (allow_transition && s_app.chapter < 0) {
            // 第一次进入剧本:先记章节,不弹过场。
            s_app.chapter = eden_script_chapter_of(&s_app.pack, s_app.line.scene);
            mark_chapter_unlocked(s_app.chapter < 0 ? 0 : (uint16_t)s_app.chapter);
        } else if (allow_transition && maybe_show_transition()) {
            s_app.line_valid = true;   // 过场结束后要渲染这一句
            s_app.page = 0;
            history_push(s_app.line.speaker, s_app.line.text);
            return;
        }
        show_line();
        return;
    }
}

// 推进:同一句还有下一页就先翻页,否则走下一句。
static void advance(void)
{
    if (eden_ui_typing(&s_app.ui)) {
        eden_ui_show_full_text(&s_app.ui);
        return;
    }
    if (s_app.page + 1 < s_app.pages) {
        s_app.page++;
        render_page();
        return;
    }
    eden_script_advance(&s_app.script, &s_app.pack);
    step_next(true);
}

// ---------------------------------------------------------------- 列表内容
#ifdef EDEN_APP_HOST_TEST
// 仅供 host 回归：最近一次画进标题画布的帧号（0xFFFF = 还没画过）。
uint16_t eden_app_test_title_frame = 0xFFFFu;

// 仅供 host 回归：阅读页四硬指标（名字框外 / 页码在面板内且不压正文 / 末行出框）。
bool eden_app_test_typing(void)
{
    return eden_ui_typing(&s_app.ui);
}

// host 断言：设置列表行单行 + 宽度够；姓名不被切、底边不压面板。
bool eden_app_test_settings_rows(void)
{
    const eden_list_ui_t *list = &s_app.ui.lists[EDEN_LIST_SETTINGS];
    if (list->rows[0] == NULL) return false;
    lv_obj_t *label = lv_obj_get_child(list->rows[0], 0);
    if (label == NULL) return false;
    if (lv_label_get_long_mode(label) == LV_LABEL_LONG_WRAP) return false;
    if (lv_obj_get_width(label) < 200) return false;
    return true;
}

bool eden_app_test_name_label(void)
{
    const lv_obj_t *name = s_app.ui.name;
    if (name == NULL) return false;
    // 16px 字体行高 19：高度必须 >= 19 才不会被切底；底边不压面板顶。
    return lv_obj_get_height(name) >= 19 && (name->y + lv_obj_get_height(name)) <= EDEN_BAND_Y;
}

bool eden_app_test_reader_geometry(void)
{
    const eden_ui_t *ui = &s_app.ui;
    if (ui->name == NULL || ui->page_label == NULL || ui->text == NULL) return false;
    const int name_bottom = ui->name->y + 19;          // 16px 字体行高 19
    const int page_bottom = ui->page_label->y + 15;
    // 名字底边不能压面板；页码独立在面板外右上，不与正文相交；
    // 正文贴面板顶（面板内 y=4），3 行 x18 不越面板底。
    const bool name_out = name_bottom <= EDEN_BAND_Y;
    const bool page_out = page_bottom <= EDEN_BAND_Y;
    const bool text_top = ui->text->y == 4;
    const bool lines_fit = ui->text->y + EDEN_TEXT_LINES * 18 <= EDEN_BAND_H;
    return name_out && page_out && text_top && lines_fit;
}
#endif

static void show_title(void)
{
    static const char *labels[5] = { "开始游戏", "继续游戏", "读取存档", "设置", "鉴赏" };
    const char *values[5] = { NULL, NULL, NULL, NULL, NULL };
    eden_ui_set_rows(&s_app.ui, EDEN_LIST_TITLE, labels, values, 5, s_app.sel[EDEN_LIST_TITLE]);
    eden_ui_set_row_enabled(&s_app.ui, EDEN_LIST_TITLE, 1, s_app.progress.valid != 0);
    bool any_save = false;
    for (int index = 0; index < EDEN_SAVE_SLOTS; index++) {
        any_save = any_save || s_app.slots[index].valid;
    }
    eden_ui_set_row_enabled(&s_app.ui, EDEN_LIST_TITLE, 2, any_save);
    enter_root_page(EDEN_PAGE_TITLE);
    // 标题背景：画布已由 place_canvas 搬进标题槽，这里只画第 0 帧。
    (void)eden_image_draw(&s_app.image, EDEN_TITLE_FRAME);
#ifdef EDEN_APP_HOST_TEST
    eden_app_test_title_frame = EDEN_TITLE_FRAME;
#endif
}

static void show_menu(void)
{
    static const char *labels[6] = { "返回剧情", "存档", "读档", "历史", "设置", "回到标题" };
    eden_ui_set_rows(&s_app.ui, EDEN_LIST_MENU, labels, NULL, 6, s_app.sel[EDEN_LIST_MENU]);
    enter_sub_page(EDEN_PAGE_MENU);
}

// 只刷新设置页的行内容(改值后重画用,不入栈)。
static void fill_settings(void)
{
    static char speed[8];
    static char size[8];
    static char auto_text[8];
    static char transparency[8];
    static const char *labels[4] = { "文字速度", "字号", "自动阅读", "对话框透明度" };
    const char *values[4] = { speed, size, auto_text, transparency };
    snprintf(speed, sizeof(speed), "%s",
             s_app.settings.text_speed == 0 ? "慢" : s_app.settings.text_speed == 1 ? "中"
             : s_app.settings.text_speed == 2 ? "快" : "瞬间");
    snprintf(size, sizeof(size), "%s", s_app.settings.font_large ? "20px" : "16px");
    snprintf(auto_text, sizeof(auto_text), "%s", s_app.settings.auto_play ? "开" : "关");
    snprintf(transparency, sizeof(transparency), "%u%%",
             (unsigned)s_app.settings.panel_transparency);
    eden_ui_set_rows(&s_app.ui, EDEN_LIST_SETTINGS, labels, values, 4,
                     s_app.sel[EDEN_LIST_SETTINGS]);
    eden_ui_set_page_title(&s_app.ui, EDEN_PAGE_SETTINGS, "设置");
    eden_ui_set_footer(&s_app.ui, EDEN_PAGE_SETTINGS, "OK 切换 · 长按 OK 返回");
}

static void show_settings(void)
{
    fill_settings();
    enter_sub_page(EDEN_PAGE_SETTINGS);
}

// 只刷新槽位页内容(保存成功后重画用,不入栈)。
static void fill_slots(eden_page_t page, eden_list_t list)
{
    static char label_storage[EDEN_SAVE_SLOTS][48];
    static char value_storage[EDEN_SAVE_SLOTS][24];
    const char *labels[EDEN_SAVE_SLOTS];
    const char *values[EDEN_SAVE_SLOTS];
    for (int index = 0; index < EDEN_SAVE_SLOTS; index++) {
        if (!s_app.slots[index].valid) {
            snprintf(label_storage[index], sizeof(label_storage[index]), "存档 %d · 空", index + 1);
            value_storage[index][0] = '\0';
        } else {
            snprintf(label_storage[index], sizeof(label_storage[index]), "存档 %d · %s", index + 1,
                     s_app.slots[index].chapter_text);
            copy_utf8_prefix(value_storage[index], sizeof(value_storage[index]),
                             s_app.slots[index].preview, 8);
        }
        labels[index] = label_storage[index];
        values[index] = value_storage[index];
    }
    eden_ui_set_page_title(&s_app.ui, page, page == EDEN_PAGE_SAVE ? "存档" : "读档");
    eden_ui_set_footer(&s_app.ui, page, "OK 确认 · 长按 OK 返回");
    eden_ui_set_rows(&s_app.ui, list, labels, values, EDEN_SAVE_SLOTS, s_app.sel[list]);
    s_app.confirm_slot = -1;
}

static void show_slots(eden_page_t page, eden_list_t list)
{
    fill_slots(page, list);
    enter_sub_page(page);
}

static void show_history(void)
{
    const char *labels[EDEN_HISTORY_MAX];
    build_history_rows(labels);
    eden_ui_set_page_title(&s_app.ui, EDEN_PAGE_HISTORY, "历史");
    eden_ui_set_footer(&s_app.ui, EDEN_PAGE_HISTORY, "长按 OK 返回");
    eden_ui_set_rows(&s_app.ui, EDEN_LIST_HISTORY, labels, NULL, s_app.history_count,
                     s_app.sel[EDEN_LIST_HISTORY]);
    enter_sub_page(EDEN_PAGE_HISTORY);
}

static void show_gallery(void)
{
    static const char *labels[2] = { "CG 鉴赏", "章节鉴赏" };
    eden_ui_set_rows(&s_app.ui, EDEN_LIST_GALLERY, labels, NULL, 2,
                     s_app.sel[EDEN_LIST_GALLERY]);
    enter_sub_page(EDEN_PAGE_GALLERY);
}

static void show_chapters(void)
{
    const uint16_t count = eden_pack_chapter_count(&s_app.pack);
    static char storage[EDEN_ROWS_MAX][48];
    const char *labels[EDEN_ROWS_MAX];
    if (count > EDEN_ROWS_MAX) {
        return;
    }
    for (uint16_t index = 0; index < count; index++) {
        eden_chapter_t info;
        const bool unlocked = eden_unlock_has_chapter(&s_app.unlock, index);
        if (eden_pack_chapter(&s_app.pack, index, &info)) {
            snprintf(storage[index], sizeof(storage[index]), "%02u %s · %s",
                     (unsigned)index + 1, unlocked ? "已读" : "未读",
                     eden_pack_text(&s_app.pack, info.name));
        } else {
            snprintf(storage[index], sizeof(storage[index]), "%02u ?", (unsigned)index + 1);
        }
        labels[index] = storage[index];
    }
    eden_ui_set_page_title(&s_app.ui, EDEN_PAGE_CHAPTER, "章节鉴赏");
    eden_ui_set_footer(&s_app.ui, EDEN_PAGE_CHAPTER, "OK 从该章开始 · 长按 OK 返回");
    eden_ui_set_rows(&s_app.ui, EDEN_LIST_CHAPTER, labels, NULL, count,
                     s_app.sel[EDEN_LIST_CHAPTER]);
    for (uint16_t index = 0; index < count; index++) {
        eden_ui_set_row_enabled(&s_app.ui, EDEN_LIST_CHAPTER, index,
                                eden_unlock_has_chapter(&s_app.unlock, index) != 0);
    }
    enter_sub_page(EDEN_PAGE_CHAPTER);
}

// CG 缩略图:一页 6 格;缩略图直接复用画面解码结果(同一帧的 264x165 → 92x58 由 LVGL 缩放)。
// 只重画 CG 列表(移动光标/翻页/从大图返回时用,不压栈)。
static void fill_cg_list(void)
{
    const int total = eden_pack_gallery_count(&s_app.pack);
    const int pages = (total + EDEN_CG_PER_PAGE - 1) / EDEN_CG_PER_PAGE;
    if (s_app.cg_page >= pages) {
        s_app.cg_page = pages > 0 ? pages - 1 : 0;
    }
    if (s_app.cg_page < 0) {
        s_app.cg_page = 0;
    }
    // 分页展示:一页 6 条,不沿用框架的 8 格/32 位上限。
    static char storage[EDEN_CG_PER_PAGE][40];
    const char *labels[EDEN_CG_PER_PAGE];
    int count = 0;
    for (int index = 0; index < EDEN_CG_PER_PAGE; index++) {
        const int item = s_app.cg_page * EDEN_CG_PER_PAGE + index;
        if (item >= total) {
            break;
        }
        eden_gallery_t entry;
        const bool open = eden_pack_gallery(&s_app.pack, (uint16_t)item, &entry) &&
                          eden_unlock_has_cg(&s_app.unlock, entry.group);
        const char *label = open && eden_pack_gallery(&s_app.pack, (uint16_t)item, &entry)
                                ? eden_pack_text(&s_app.pack, entry.label)
                                : "？？？";
        snprintf(storage[index], sizeof(storage[index]), "%02d  %s", item + 1,
                 label != NULL ? label : "");
        labels[index] = storage[index];
        count++;
    }
    char info[48];
    snprintf(info, sizeof(info), "CG 鉴赏  %d/%d 页", s_app.cg_page + 1, pages > 0 ? pages : 1);
    eden_ui_set_page_title(&s_app.ui, EDEN_PAGE_CG_LIST, info);
    eden_ui_set_footer(&s_app.ui, EDEN_PAGE_CG_LIST, "↑↓ 选择 · OK 全屏 · 长按 OK 返回");
    eden_ui_set_rows(&s_app.ui, EDEN_LIST_CG, labels, NULL, count, s_app.cg_sel);
}

static void show_cg_list(void)
{
    fill_cg_list();
    enter_sub_page(EDEN_PAGE_CG_LIST);
}

static void show_cg(void)
{
    const int item = s_app.cg_page * EDEN_CG_PER_PAGE + s_app.cg_sel;
    if (item < 0 || item >= eden_pack_gallery_count(&s_app.pack)) {
        eden_ui_set_notice(&s_app.ui, "这一页没有更多 CG");
        return;
    }
    eden_gallery_t entry;
    if (!eden_pack_gallery(&s_app.pack, (uint16_t)item, &entry)) {
        return;
    }
    if (!eden_unlock_has_cg(&s_app.unlock, entry.group)) {
        eden_ui_set_notice(&s_app.ui, "这一张还没解锁");
        return;
    }
    if (!eden_image_draw(&s_app.image, entry.frame)) {
        s_app.drawn_valid = false;
        eden_ui_set_notice(&s_app.ui, "这张 CG 解码失败");
        return;
    }
    s_app.drawn_valid = true;
    s_app.drawn_frame = entry.frame;
    // 解码结果在 264x168 画布缓冲里；由单个缩放图对象铺满（无分带、无新增大块）。
    static char name[32];
    snprintf(name, sizeof(name), "CG %02d", item + 1);
    eden_ui_set_cg(&s_app.ui, name, eden_pack_text(&s_app.pack, entry.label));
    eden_ui_set_footer(&s_app.ui, EDEN_PAGE_CG, "OK 返回缩略图 · 长按 OK 返回");
    enter_sub_page(EDEN_PAGE_CG);
}

// ---------------------------------------------------------------- 存档动作
// 存档采用"先写副本、成功才提交 RAM"的顺序:
//   写失败(包括 nvs_commit 失败)时,NVS 里的旧档还在,RAM 里的旧档也必须原样保留,
//   否则本次运行会显示空槽、也读不回来,只能重启。
static bool write_slot(int index)
{
    eden_slot_t candidate[EDEN_SAVE_SLOTS];
    memcpy(candidate, s_app.slots, sizeof(candidate));

    eden_slot_t *slot = &candidate[index];
    eden_slot_clear(slot);
    slot->valid = 1;
    slot->scene = s_app.script.scene;
    slot->op = s_app.script.op;
    slot->page = s_app.page;
    slot->chapter = s_app.chapter < 0 ? 0 : (uint16_t)s_app.chapter;
    memcpy(slot->vars, s_app.script.vars, sizeof(slot->vars));
    eden_chapter_t info;
    if (s_app.chapter >= 0 &&
        eden_pack_chapter(&s_app.pack, (uint16_t)s_app.chapter, &info)) {
        snprintf(slot->chapter_text, sizeof(slot->chapter_text), "%s",
                 eden_pack_text(&s_app.pack, info.name));
    }
    const char *text = eden_pack_text(&s_app.pack, s_app.line.text);
    snprintf(slot->preview, sizeof(slot->preview), "%s", text != NULL ? text : "");

    if (!eden_store_save_slots(candidate)) {
        eden_ui_set_notice(&s_app.ui, "保存失败:存储不可写");
        return false;   // RAM 槽位保持原样,旧档仍可立即读取
    }
    memcpy(s_app.slots, candidate, sizeof(candidate));
    eden_ui_set_notice(&s_app.ui, "已保存");
    return true;
}

// 从位置(scene/op/page/vars)恢复剧情。读档与"继续游戏"共用。
static bool restore_at(uint16_t scene, uint32_t op, uint16_t page, const int32_t *vars)
{
    // 读档失败不得改掉当前正常状态:先在副本上试,成功才提交。
    eden_line_t candidate;
    eden_state_t trial = s_app.script;

    if (!eden_state_seek(&trial, &s_app.pack, scene, op)) {
        return false;
    }
    if (vars != NULL) {
        memcpy(trial.vars, vars, sizeof(trial.vars));
    }
    const eden_step_t kind = eden_script_step(&s_app.pack, &trial, &candidate);
    if (kind != EDEN_STEP_TEXT) {
        return false;
    }
    s_app.script = trial;
    s_app.line = candidate;
    s_app.chapter = eden_script_chapter_of(&s_app.pack, scene);
    s_app.line_valid = true;
    s_app.page = page;
    history_push(s_app.line.speaker, s_app.line.text);
    render_page();
    enter_root_page(EDEN_PAGE_GAME);
    return true;
}

static void load_slot(int index)
{
    const eden_slot_t *slot = &s_app.slots[index];
    if (!slot->valid) {
        eden_ui_set_notice(&s_app.ui, "这个槽位是空的");
        return;
    }
    if (!restore_at(slot->scene, slot->op, slot->page, slot->vars)) {
        eden_ui_set_notice(&s_app.ui, "存档位置无效");
        return;
    }
    eden_ui_set_notice(&s_app.ui, "已读取");
}

// ---------------------------------------------------------------- 按键
// 返回 true 表示"长按返回"已被调用方处理。
static bool move_selection(int *selected, int count, int delta)
{
    if (count <= 0) {
        return false;
    }
    const int next = *selected + delta;
    if (next < 0 || next >= count) {
        return false;
    }
    *selected = next;
    return true;
}

static void handle_start(uint8_t btn, uint8_t ev)
{
    if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
        show_title();
    }
}

static void handle_title(uint8_t btn, uint8_t ev)
{
    const int count = 5;
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        return;
    }
    if (btn == BSP_BTN_UP && (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG)) {
        if (move_selection(&s_app.sel[EDEN_LIST_TITLE], count, -1)) {
            eden_ui_select_row(&s_app.ui, EDEN_LIST_TITLE, s_app.sel[EDEN_LIST_TITLE]);
        }
        return;
    }
    if (btn == BSP_BTN_DOWN && (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG)) {
        if (move_selection(&s_app.sel[EDEN_LIST_TITLE], count, 1)) {
            eden_ui_select_row(&s_app.ui, EDEN_LIST_TITLE, s_app.sel[EDEN_LIST_TITLE]);
        }
        return;
    }
    if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
        const int choice = s_app.sel[EDEN_LIST_TITLE];
        if (choice == 0) {
            eden_state_reset(&s_app.script, 1);
            uint32_t first = 0;
            if (eden_pack_scene_range(&s_app.pack, 1, &first, NULL)) {
                s_app.script.op = first;
            }
            memset(s_app.script.vars, 0, sizeof(s_app.script.vars));
            s_app.chapter = -1;
            s_app.history_count = 0;
            s_app.history_head = 0;
            s_app.page = 0;
            enter_root_page(EDEN_PAGE_GAME);
            step_next(true);
        } else if (choice == 1) {
            if (!s_app.progress.valid) {
                eden_ui_set_notice(&s_app.ui, "还没有阅读进度");
                return;
            }
            if (!restore_at(s_app.progress.scene, s_app.progress.op, s_app.progress.page,
                            s_app.progress.vars)) {
                eden_ui_set_notice(&s_app.ui, "进度无法恢复");
            }
        } else if (choice == 2) {
            s_app.sel[EDEN_LIST_LOAD] = 0;
            show_slots(EDEN_PAGE_LOAD, EDEN_LIST_LOAD);
        } else if (choice == 3) {
            show_settings();
        } else {
            show_gallery();
        }
    }
}

static void handle_game(uint8_t btn, uint8_t ev)
{
    if (btn == BSP_BTN_UP) {
        if (ev == BSP_BTN_CLICK) {
            s_app.fast_forward = false;
            advance();
        } else if (ev == BSP_BTN_LONG) {
            s_app.fast_forward = true;
            s_app.last_step_ms = s_app.now_ms;
            eden_ui_show_full_text(&s_app.ui);
        } else if (ev == BSP_BTN_RELEASE) {
            s_app.fast_forward = false;   // 与全局取消重复,保留显式语义
        }
        return;
    }
    if (btn == BSP_BTN_DOWN) {
        if (ev == BSP_BTN_LONG) {
            // 长按 DOWN：开/关自动阅读（长按 UP 仍是快进）。
            s_app.settings.auto_play = s_app.settings.auto_play ? 0 : 1;
            (void)gal_settings_store(&s_app.settings);
            eden_ui_set_mode(&s_app.ui, s_app.settings.auto_play != 0);
            eden_ui_set_notice(&s_app.ui, s_app.settings.auto_play ? "自动阅读：开"
                                                                   : "自动阅读：关");
            s_app.last_step_ms = s_app.now_ms;
            return;
        }
        if (ev == BSP_BTN_CLICK) {
            s_app.sel[EDEN_LIST_HISTORY] = 0;
            show_history();
            return;
        }
        return;
    }
    if (btn == BSP_BTN_OK) {
        if (ev == BSP_BTN_LONG) {
            s_app.fast_forward = false;
            s_app.sel[EDEN_LIST_MENU] = 0;
            show_menu();
            s_suppress_click = true;   // 吞掉长按后驱动补发的 CLICK
            return;
        }
        if (ev == BSP_BTN_CLICK) {
            if (s_suppress_click) {
                s_suppress_click = false;
                return;
            }
            s_app.fast_forward = false;
            advance();                 // 短按 OK = 下一句（不再进菜单）
            return;
        }
    }
}

static void handle_menu(uint8_t btn, uint8_t ev)
{
    const int count = 6;
    if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
        page_back();
        return;
    }
    if (btn == BSP_BTN_UP && (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG)) {
        if (move_selection(&s_app.sel[EDEN_LIST_MENU], count, -1)) {
            eden_ui_select_row(&s_app.ui, EDEN_LIST_MENU, s_app.sel[EDEN_LIST_MENU]);
        }
        return;
    }
    if (btn == BSP_BTN_DOWN && (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG)) {
        if (move_selection(&s_app.sel[EDEN_LIST_MENU], count, 1)) {
            eden_ui_select_row(&s_app.ui, EDEN_LIST_MENU, s_app.sel[EDEN_LIST_MENU]);
        }
        return;
    }
    if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
        switch (s_app.sel[EDEN_LIST_MENU]) {
        case 0: page_back(); break;
        case 1: s_app.sel[EDEN_LIST_SAVE] = 0; show_slots(EDEN_PAGE_SAVE, EDEN_LIST_SAVE); break;
        case 2: s_app.sel[EDEN_LIST_LOAD] = 0; show_slots(EDEN_PAGE_LOAD, EDEN_LIST_LOAD); break;
        case 3: s_app.sel[EDEN_LIST_HISTORY] = 0; show_history(); break;
        case 4: show_settings(); break;
        default:
            store_progress(true);
            show_title();
            break;
        }
    }
}

static void handle_slots(eden_page_t page, eden_list_t list, uint8_t btn, uint8_t ev)
{
    if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
        page_back();
        return;
    }
    if (btn == BSP_BTN_UP && (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG)) {
        if (move_selection(&s_app.sel[list], EDEN_SAVE_SLOTS, -1)) {
            eden_ui_select_row(&s_app.ui, list, s_app.sel[list]);
        }
        return;
    }
    if (btn == BSP_BTN_DOWN && (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG)) {
        if (move_selection(&s_app.sel[list], EDEN_SAVE_SLOTS, 1)) {
            eden_ui_select_row(&s_app.ui, list, s_app.sel[list]);
        }
        return;
    }
    if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
        const int index = s_app.sel[list];
        if (page == EDEN_PAGE_SAVE) {
            if (s_app.slots[index].valid && s_app.confirm_slot != index) {
                s_app.confirm_slot = index;
                eden_ui_set_notice(&s_app.ui, "再按一次覆盖该存档");
                return;
            }
            if (!s_app.line_valid) {
                eden_ui_set_notice(&s_app.ui, "当前没有可保存的进度");
                return;
            }
            const bool saved = write_slot(index);
            fill_slots(EDEN_PAGE_SAVE, EDEN_LIST_SAVE);
            if (!saved) {
                eden_ui_set_notice(&s_app.ui, "保存失败:存储不可写");
            }
        } else {
            load_slot(index);
        }
    }
}

static void handle_settings(uint8_t btn, uint8_t ev)
{
    const int count = 4;
    if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
        page_back();
        return;
    }
    if (btn == BSP_BTN_UP && (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG)) {
        if (move_selection(&s_app.sel[EDEN_LIST_SETTINGS], count, -1)) {
            eden_ui_select_row(&s_app.ui, EDEN_LIST_SETTINGS, s_app.sel[EDEN_LIST_SETTINGS]);
        }
        return;
    }
    if (btn == BSP_BTN_DOWN && (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG)) {
        if (move_selection(&s_app.sel[EDEN_LIST_SETTINGS], count, 1)) {
            eden_ui_select_row(&s_app.ui, EDEN_LIST_SETTINGS, s_app.sel[EDEN_LIST_SETTINGS]);
        }
        return;
    }
    if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
        switch (s_app.sel[EDEN_LIST_SETTINGS]) {
        case 0: s_app.settings.text_speed = (uint8_t)((s_app.settings.text_speed + 1) % 4); break;
        case 1: s_app.settings.font_large = !s_app.settings.font_large; break;
        case 2: s_app.settings.auto_play = !s_app.settings.auto_play; break;
        default:
            s_app.settings.panel_transparency =
                gal_transparency_next(s_app.settings.panel_transparency);
            break;
        }
        (void)gal_settings_store(&s_app.settings);
        apply_display_settings();
        fill_settings();          // 留在设置页,只刷新取值
        if (s_app.line_valid) {
            render_page();        // 字号变化后重排正文(页面不切换)
        }
    }
}

static void handle_history(uint8_t btn, uint8_t ev)
{
    if (btn == BSP_BTN_OK && (ev == BSP_BTN_LONG || ev == BSP_BTN_CLICK)) {
        page_back();
        return;
    }
    if (btn == BSP_BTN_UP && (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG)) {
        if (move_selection(&s_app.sel[EDEN_LIST_HISTORY], s_app.history_count, -1)) {
            eden_ui_select_row(&s_app.ui, EDEN_LIST_HISTORY, s_app.sel[EDEN_LIST_HISTORY]);
        }
        return;
    }
    if (btn == BSP_BTN_DOWN && (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG)) {
        if (move_selection(&s_app.sel[EDEN_LIST_HISTORY], s_app.history_count, 1)) {
            eden_ui_select_row(&s_app.ui, EDEN_LIST_HISTORY, s_app.sel[EDEN_LIST_HISTORY]);
        }
    }
}

static void handle_gallery(uint8_t btn, uint8_t ev)
{
    const int count = 2;
    if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
        page_back();
        return;
    }
    if (btn == BSP_BTN_UP && (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG)) {
        if (move_selection(&s_app.sel[EDEN_LIST_GALLERY], count, -1)) {
            eden_ui_select_row(&s_app.ui, EDEN_LIST_GALLERY, s_app.sel[EDEN_LIST_GALLERY]);
        }
        return;
    }
    if (btn == BSP_BTN_DOWN && (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG)) {
        if (move_selection(&s_app.sel[EDEN_LIST_GALLERY], count, 1)) {
            eden_ui_select_row(&s_app.ui, EDEN_LIST_GALLERY, s_app.sel[EDEN_LIST_GALLERY]);
        }
        return;
    }
    if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
        if (s_app.sel[EDEN_LIST_GALLERY] == 0) {
            s_app.cg_page = 0;
            s_app.cg_sel = 0;
            show_cg_list();
        } else {
            s_app.sel[EDEN_LIST_CHAPTER] = 0;
            show_chapters();
        }
    }
}

static void handle_cg_list(uint8_t btn, uint8_t ev)
{
    const int total = eden_pack_gallery_count(&s_app.pack);
    const int pages = (total + EDEN_CG_PER_PAGE - 1) / EDEN_CG_PER_PAGE;
    if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
        page_back();
        return;
    }
    if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
        show_cg();
        return;
    }
    if (btn == BSP_BTN_UP && (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG)) {
        if (s_app.cg_sel > 0) {
            s_app.cg_sel--;
        } else if (s_app.cg_page > 0) {
            s_app.cg_page--;
            s_app.cg_sel = EDEN_CG_PER_PAGE - 1;
        }
        fill_cg_list();
        return;
    }
    if (btn == BSP_BTN_DOWN && (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG)) {
        const int item = s_app.cg_page * EDEN_CG_PER_PAGE + s_app.cg_sel;
        if (s_app.cg_sel + 1 < EDEN_CG_PER_PAGE && item + 1 < total) {
            s_app.cg_sel++;
        } else if (s_app.cg_page + 1 < pages) {
            s_app.cg_page++;
            s_app.cg_sel = 0;
        }
        fill_cg_list();
        return;
    }
}

static void handle_chapters(uint8_t btn, uint8_t ev)
{
    const int count = eden_pack_chapter_count(&s_app.pack);
    if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
        // 章节鉴赏的“返回”必须出栈回鉴赏入口；之前调 show_gallery() 会再压一层，
        // 造成 鉴赏→章节→鉴赏→章节 的无限互跳（长按永远回不到标题）。
        page_back();
        return;
    }
    if (btn == BSP_BTN_UP && (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG)) {
        if (move_selection(&s_app.sel[EDEN_LIST_CHAPTER], count, -1)) {
            eden_ui_select_row(&s_app.ui, EDEN_LIST_CHAPTER, s_app.sel[EDEN_LIST_CHAPTER]);
        }
        return;
    }
    if (btn == BSP_BTN_DOWN && (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG)) {
        if (move_selection(&s_app.sel[EDEN_LIST_CHAPTER], count, 1)) {
            eden_ui_select_row(&s_app.ui, EDEN_LIST_CHAPTER, s_app.sel[EDEN_LIST_CHAPTER]);
        }
        return;
    }
    if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
        const int index = s_app.sel[EDEN_LIST_CHAPTER];
        if (!eden_unlock_has_chapter(&s_app.unlock, (uint16_t)index)) {
            eden_ui_set_notice(&s_app.ui, "这一章还没到过");
            return;
        }
        eden_chapter_t info;
        if (!eden_pack_chapter(&s_app.pack, (uint16_t)index, &info)) {
            return;
        }
        eden_state_reset(&s_app.script, info.scene);
        s_app.script.op = info.op;
        memset(s_app.script.vars, 0, sizeof(s_app.script.vars));
        s_app.chapter = index;
        s_app.page = 0;
        enter_root_page(EDEN_PAGE_GAME);
        step_next(false);
    }
}

static void handle_transition(uint8_t btn, uint8_t ev)
{
    if (btn == BSP_BTN_OK && (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG)) {
        // 过场前已经取到这一句并推过历史,这里只渲染。
        s_app.page = 0;
        render_page();
        enter_root_page(EDEN_PAGE_GAME);
    }
}

static void handle_ending(uint8_t btn, uint8_t ev)
{
    if (btn == BSP_BTN_OK && (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG)) {
        show_title();
    }
}

void eden_app_key(const eden_key_t *key)
{
    if (!s_app.ready || key == NULL) {
        return;
    }
    s_app.idle_ms = 0;
    s_app.last_input_ms = s_app.now_ms;

    // P1c:快进只在"按住 UP"期间成立。松手、按其它键、换页一律取消 ——
    // 否则 长UP -> DOWN 进历史 -> 松手(历史页没人处理 RELEASE) -> 回正文继续快进。
    if (!(key->btn == BSP_BTN_UP && (key->ev == BSP_BTN_LONG || key->ev == BSP_BTN_PRESS))) {
        s_app.fast_forward = false;
    }

    if (!bsp_lvgl_lock(200)) {
        ESP_LOGW(TAG, "拿不到 LVGL 锁,丢弃按键");
        return;
    }
    // 所有 UI 变更都在锁内:set_notice 会调 lv_label_set_text / lv_obj 标志。
    eden_ui_set_notice(&s_app.ui, "");
    const eden_page_t page = eden_ui_page(&s_app.ui);
    switch (page) {
    case EDEN_PAGE_START: handle_start(key->btn, key->ev); break;
    case EDEN_PAGE_TITLE: handle_title(key->btn, key->ev); break;
    case EDEN_PAGE_GAME: handle_game(key->btn, key->ev); break;
    case EDEN_PAGE_MENU: handle_menu(key->btn, key->ev); break;
    case EDEN_PAGE_SAVE: handle_slots(EDEN_PAGE_SAVE, EDEN_LIST_SAVE, key->btn, key->ev); break;
    case EDEN_PAGE_LOAD: handle_slots(EDEN_PAGE_LOAD, EDEN_LIST_LOAD, key->btn, key->ev); break;
    case EDEN_PAGE_SETTINGS: handle_settings(key->btn, key->ev); break;
    case EDEN_PAGE_HISTORY: handle_history(key->btn, key->ev); break;
    case EDEN_PAGE_GALLERY: handle_gallery(key->btn, key->ev); break;
    case EDEN_PAGE_CG_LIST: handle_cg_list(key->btn, key->ev); break;
    case EDEN_PAGE_CG:
        if (key->btn == BSP_BTN_OK &&
            (key->ev == BSP_BTN_CLICK || key->ev == BSP_BTN_LONG)) {
            // 关闭大图 = 返回到进入时的 CG 列表(出栈),不是再压一层列表;
            // 返回后重画列表以保留原来的页码与选中项。
            page_back();
            fill_cg_list();
        }
        break;
    case EDEN_PAGE_CHAPTER: handle_chapters(key->btn, key->ev); break;
    case EDEN_PAGE_TRANSITION: handle_transition(key->btn, key->ev); break;
    case EDEN_PAGE_ENDING: handle_ending(key->btn, key->ev); break;
    default: break;
    }
    bsp_lvgl_unlock();
}

void eden_app_tick(uint32_t now_ms)
{
    s_app.now_ms = now_ms;
    if (s_app.settings.auto_play && s_app.page == EDEN_PAGE_GAME && s_app.line_valid) {
        s_app.idle_ms = 0;
    } else {
        s_app.idle_ms += 50;
    }
    if (!s_app.ready) {
        return;
    }
    // 解锁位落盘与当前页面无关:标题/鉴赏/CG 列表/结局页上同样要重试,
    // 结局页还没有 line_valid,所以这条必须在页面过滤之前。
    (void)flush_unlock(false);

    if (eden_ui_page(&s_app.ui) != EDEN_PAGE_GAME || !s_app.line_valid) {
        return;
    }
    if (now_ms - s_app.last_battery_ms >= EDEN_BATTERY_PERIOD_MS) {
        s_app.last_battery_ms = now_ms;
        if (bsp_lvgl_lock(50)) {
            eden_ui_set_battery(&s_app.ui, bsp_battery_soc());
            bsp_lvgl_unlock();
        }
    }

    const bool typing = eden_ui_typing(&s_app.ui);
    if (s_app.fast_forward) {
        if (now_ms - s_app.last_step_ms < EDEN_FAST_STEP_MS) {
            return;
        }
        s_app.last_step_ms = now_ms;
        if (bsp_lvgl_lock(200)) {
            advance();
            bsp_lvgl_unlock();
        }
        return;
    }
    if (s_app.settings.auto_play && !typing) {
        if (now_ms - s_app.last_step_ms >= EDEN_AUTO_STEP_MS) {
            s_app.last_step_ms = now_ms;
            if (bsp_lvgl_lock(200)) {
                advance();
                bsp_lvgl_unlock();
            }
        }
    } else {
        s_app.last_step_ms = now_ms;
    }
}


// 睡前/切页前落盘：进度立即写，解锁位强制再试一次。
void eden_app_flush(void)
{
    if (!s_app.ready) return;
    if (bsp_lvgl_lock(1000)) {
        store_progress(true);
        bsp_lvgl_unlock();
    }
    (void)flush_unlock(true);
}

uint32_t eden_app_idle_ms(void)
{
    // 自动阅读开启且正在正文时不休眠。
    if (s_app.settings.auto_play && s_app.page == EDEN_PAGE_GAME && s_app.line_valid) return 0;
    return s_app.idle_ms;
}

// ---------------------------------------------------------------- 初始化
bool eden_app_init_with_pack(const uint8_t *blob, uint32_t size)
{
    memset(&s_app, 0, sizeof(s_app));
    s_app.chapter = -1;
    s_app.sel[EDEN_LIST_TITLE] = 0;

    if (blob == NULL || !eden_pack_open(&s_app.pack, blob, size)) {
        ESP_LOGE(TAG, "资源包校验失败 (%u 字节)", (unsigned)size);
        return false;
    }
    ESP_LOGI(TAG, "资源包就绪: %u 字节, 场景 %u, 章节 %u, CG %u, 画面 %u",
             (unsigned)size, (unsigned)eden_pack_scene_count(&s_app.pack),
             (unsigned)eden_pack_chapter_count(&s_app.pack),
             (unsigned)eden_pack_gallery_count(&s_app.pack),
             (unsigned)eden_pack_frame_count(&s_app.pack));

    (void)eden_store_init();
    if (!gal_settings_init()) {
        ESP_LOGW(TAG, "设置读取失败,使用默认值");
    }
    gal_settings_default(&s_app.settings);
    (void)gal_settings_load(&s_app.settings);
    (void)eden_store_load_slots(s_app.slots);
    (void)eden_store_load_progress(&s_app.progress);
    (void)eden_store_load_unlock(&s_app.unlock);

    // 画面缓冲:静态分配,生命周期与 LVGL 画布一致。
    static uint16_t s_canvas_pixels[EDEN_CANVAS_W * EDEN_CANVAS_H];
    static uint16_t s_patch_pixels[EDEN_PATCH_W * EDEN_PATCH_H];

    if (!bsp_lvgl_lock(-1)) {
        ESP_LOGE(TAG, "拿不到 LVGL 锁");
        return false;
    }

    // 徽标描述符必须是静态的:LVGL 只保存指针,栈上对象的生命周期不够长。
    static lv_image_dsc_t s_logo;
    memset(&s_logo, 0, sizeof(s_logo));
    s_logo.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_logo.header.cf = LV_COLOR_FORMAT_RGB565A8;
    s_logo.header.w = EDEN_LOGO_W;
    s_logo.header.h = EDEN_LOGO_H;
    s_logo.header.stride = EDEN_LOGO_W * 2;
    s_logo.data_size = sizeof(eden_logo_data);
    s_logo.data = eden_logo_data;

    bool ok = eden_ui_create(&s_app.ui, &gal_cjk_16, &gal_cjk_20);
    if (ok) {
        ok = eden_image_init(&s_app.image, s_app.ui.canvas_slot, s_canvas_pixels,
                             s_patch_pixels);
    }
    if (ok) {
        eden_image_set_pack(&s_app.image, &s_app.pack);
        eden_ui_attach_canvas(&s_app.ui, s_app.image.canvas);
        apply_display_settings();
        eden_ui_set_brand_image(&s_app.ui, &s_logo);
        eden_ui_set_brand(&s_app.ui, "eden*", "—— they were only two, on the planet. ——");
        eden_ui_show_page(&s_app.ui, EDEN_PAGE_START);
    }
    bsp_lvgl_unlock();

    s_app.ready = ok;
    return ok;
}

#ifndef EDEN_APP_HOST_TEST
bool eden_app_init(void)
{
    return eden_app_init_with_pack(eden_pack_start,
                                   (uint32_t)(eden_pack_end - eden_pack_start));
}
#endif
