// tests/test_steins_app.c —— 真实 app + 真实 UI(薄桩)的行为回归。
//
// 驱动的是生产代码 steins_app.c / steins_ui.c:只把 LVGL 换成记录对象与行文本的
// 薄桩、把 NVS 换成内存表、把电量固定。覆盖:
//   * 启动进作品标题页(不是硬件自检 demo),标题行可见且是作品菜单
//   * 真实按键序列:短按 PRESS→CLICK→RELEASE 只走一步;长按 PRESS→LONG→RELEASE
//     不会多走一步、也不吞掉下一次独立短按
//   * 逐句推进到第一个选择点(经过 4 张章节标题卡),待选必须显式确认
//   * 菜单往返保留同一待选与非零光标;页面来源用栈而不是猜
//   * 存档:覆盖需确认;commit 失败不宣称成功、旧档不丢;坏档被拒且位置不变
//   * 全局解锁跨重启保留;读档合并只增不减
//   * 未到达的章节不能从章节选择进入;历史与设置可用
#include "steins_app.h"
#include "steins_pack.h"
#include "steins_ui.h"

#include "bsp_battery.h"
#include "lvgl.h"
#include "nvs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(cond, ...)                            \
    do {                                            \
        if (!(cond)) {                              \
            fprintf(stderr, "FAIL %s:%d ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);           \
            fprintf(stderr, "\n");                  \
            ++g_failures;                           \
        }                                           \
    } while (0)

static steins_app_t s_app;

// 字体在宿主机只作为"非空指针"使用:界面层不关心字形,只关心对象与行文本。
const lv_font_t steins_story_16;
const lv_font_t steins_ui_16;
const lv_font_t steins_ui_18;

// ---- 按键:真实短按 = PRESS → CLICK → RELEASE;长按 = PRESS → LONG → RELEASE
enum { BTN_UP = 0, BTN_DOWN = 1, BTN_OK = 2 };
enum { EV_PRESS = 0, EV_CLICK = 1, EV_DOUBLE = 2, EV_LONG = 3, EV_RELEASE = 4 };

static void press(int btn, int ev)
{
    const steins_key_t key = { (uint8_t)btn, (uint8_t)ev };
    steins_app_key(&s_app, &key);
}

static void click(int btn)
{
    press(btn, EV_PRESS);
    press(btn, EV_CLICK);
    press(btn, EV_RELEASE);
}

static void long_press(int btn)
{
    press(btn, EV_PRESS);
    press(btn, EV_LONG);
    press(btn, EV_RELEASE);
}

static void tick_ms(uint32_t ms)
{
    steins_app_tick(&s_app, ms);
}

static void settle_typing(void)
{
    for (int i = 0; i < 6000 && s_app.shown < s_app.page_data.chars; ++i) tick_ms(20);
}

static void settle_page(void)
{
    // 章节标题卡会自动收起,这里模拟等待
    for (int i = 0; i < 40 && s_app.chapter_card_active; ++i) tick_ms(200);
}

static void boot(void)
{
    lv_stub_reset_render_counters();
    memset(&s_app, 0, sizeof(s_app));
    CHECK(steins_app_init(&s_app), "app init failed");
}

// 当前页面上"被选中那一行"的可见文本(标题页/列表页通用)。
static const char *selected_row_text(void)
{
    if (s_app.page == STEINS_PAGE_TITLE) {
        const int row = s_app.sel - s_app.ui.title_first;
        if (row < 0 || row >= STEINS_UI_TITLE_VISIBLE) return "";
        return s_app.ui.title_rows[row]->text;
    }
    const int first = s_app.sel >= STEINS_UI_LIST_ROWS ? s_app.sel - STEINS_UI_LIST_ROWS + 1 : 0;
    const int row = s_app.sel - first;
    if (row < 0 || row >= STEINS_UI_LIST_ROWS) return "";
    return s_app.ui.list_rows[row]->text;
}

// 按可见文本导航:一直按 DOWN 直到选中行包含 needle,再按 OK。
// 这样测试不依赖菜单项下标,也在验证行文本真的显示出来了。
static bool activate_row(const char *needle, int count)
{
    for (int i = 0; i < count + 2; ++i) {
        if (strstr(selected_row_text(), needle) != NULL) {
            click(BTN_OK);
            return true;
        }
        click(BTN_DOWN);
    }
    fprintf(stderr, "  activate_row('%s') failed, selected='%s' page=%d\n", needle,
            selected_row_text(), (int)s_app.page);
    return false;
}

static void test_title_and_entry(void)
{
    boot();
    CHECK(s_app.page == STEINS_PAGE_TITLE, "expected title page, got %d", (int)s_app.page);
    CHECK(steins_ui_page_visible(&s_app.ui, STEINS_PAGE_TITLE), "title page not visible");
    CHECK(!steins_ui_page_visible(&s_app.ui, STEINS_PAGE_GAME), "game page visible on title");

    CHECK(s_app.ui.title_name->text[0] != '\0', "title name label empty");
    int nonempty = 0;
    for (int i = 0; i < STEINS_UI_TITLE_VISIBLE; ++i) {
        if (s_app.ui.title_rows[i]->text[0] != 0) ++nonempty;
    }
    CHECK(nonempty >= 4, "title menu window only has %d rows", nonempty);
    CHECK(strstr(s_app.ui.title_worldline->text, "1.") != NULL,
          "title worldline label '%s'", s_app.ui.title_worldline->text);
    printf("  title: '%s' / '%s' / row0 '%s'\n", s_app.ui.title_name->text,
           s_app.ui.title_worldline->text, s_app.ui.title_rows[0]->text);

    // 标题画面:菜单落在下半屏(>=120)、7 项全部不越 240;标题图真的写进了画布。
    CHECK(s_app.ui.title_rows[0]->y >= 120, "title menu row0 y=%d (expected >=120)",
          s_app.ui.title_rows[0]->y);
    const lv_obj_t *last_row = s_app.ui.title_rows[STEINS_UI_TITLE_VISIBLE - 1];
    CHECK(last_row->y + last_row->h <= 238, "title menu clipped (last row bottom %d)",
          last_row->y + last_row->h);
    int lit = 0;
    for (int i = 0; i < STEINS_IMAGE_PIXELS; ++i) {
        if (s_app.canvas[i] != 0) ++lit;
    }
    CHECK(lit > STEINS_IMAGE_PIXELS / 20, "title art not drawn (lit=%d of %d)",
          lit, STEINS_IMAGE_PIXELS);

    // 光标移动与夹取
    click(BTN_DOWN);
    CHECK(s_app.sel == 1, "title cursor %d after DOWN", s_app.sel);
    click(BTN_UP);
    CHECK(s_app.sel == 0, "title cursor %d after UP", s_app.sel);
    click(BTN_UP);
    CHECK(s_app.sel == 0, "title cursor escaped the top");

    // 单列窗口跟随：选中项始终落在 [first, first+3]，末行盒底不越面板底(240)。
    {
        // 上下键走“只挪高亮”快路径：不得再读 NVS 自动存档（原卡顿根源）。
        const int reads_before = nvs_stub_read_count;
        click(BTN_DOWN);
        CHECK(nvs_stub_read_count == reads_before,
              "title UP/DOWN must not touch NVS (%d reads)", nvs_stub_read_count - reads_before);
        click(BTN_UP);
    }
    for (int step = 0; step < 6; ++step) {
        click(BTN_DOWN);
        const int first = s_app.ui.title_first;
        CHECK(s_app.sel >= first && s_app.sel < first + STEINS_UI_TITLE_VISIBLE,
              "window: sel=%d first=%d", s_app.sel, first);
        const lv_obj_t *bottom = s_app.ui.title_rows[STEINS_UI_TITLE_VISIBLE - 1];
        CHECK(bottom->y + bottom->h <= 240, "window row bottom %d",
              bottom->y + bottom->h);
    }
    for (int step = 0; step < 6; ++step) {
        click(BTN_UP);
    }
    CHECK(s_app.sel == 0 && s_app.ui.title_first == 0,
          "window must reset at top (sel=%d first=%d)", s_app.sel, s_app.ui.title_first);

    // ABOUT 页（从标题页第 7 项进入）：专用多行 label（WRAP），正文盒底 <= 240
    for (int i = 0; i < 6; ++i) click(BTN_DOWN);
    click(BTN_OK);
    CHECK(s_app.page == STEINS_PAGE_ABOUT, "about page (%d)", (int)s_app.page);
    CHECK(s_app.ui.about_body != NULL &&
              s_app.ui.about_body->long_mode == LV_LABEL_LONG_MODE_WRAP,
          "about body must wrap");
    CHECK(s_app.ui.about_body->y + s_app.ui.about_body->h <= 240,
          "about body bottom %d", s_app.ui.about_body->y + s_app.ui.about_body->h);
    CHECK(!lv_obj_has_flag(s_app.ui.about_body, LV_OBJ_FLAG_HIDDEN),
          "about body visible");
    CHECK(lv_obj_has_flag(s_app.ui.list_rows[0], LV_OBJ_FLAG_HIDDEN),
          "about page must hide the list rows");
    // 关于页必须能读全：可滚动 + 滚动条隐藏 + 正文没被 128 字节缓冲截断，
    // 且滚到底时末行仍在可视区内。
    CHECK(lv_obj_has_flag(s_app.ui.about_body, LV_OBJ_FLAG_SCROLLABLE),
          "about body must be scrollable");
    CHECK(s_app.ui.about_body->scrollbar_mode == LV_SCROLLBAR_MODE_OFF,
          "about scrollbar hidden");
    const char *body = s_app.ui.about_body->text;
    const int body_len = (int)strlen(body);
    CHECK(body_len > 300, "about body not truncated (%d bytes)", body_len);
    {
        // 预估渲染行数：16px 字、304px 宽 ≈ 19 字/行；显式换行另起一行。
        const int cpl = (STEINS_UI_W - 16) / 16;
        int lines = 0, cur = 0;
        for (int i = 0; body[i]; ++i) {
            const unsigned char c = (unsigned char)body[i];
            if (c == 0x0A) { ++lines; cur = 0; continue; }
            if ((c & 0xC0) != 0x80) {                 // 只数 UTF-8 首字节
                if (++cur > cpl) { ++lines; cur = 1; }
            }
        }
        ++lines;
        const int content_h = lines * STEINS_UI_LINE_H;
        CHECK(content_h <= STEINS_UI_ABOUT_H ||
                  content_h - STEINS_UI_ABOUT_H <= 64 * STEINS_UI_LINE_H,
              "about content height %d", content_h);
        // 逐行往下滚到底（最多 64 次），末行必须落在可视区内。
        int scroll = 0;
        for (int i = 0; i < 64 && scroll < content_h - STEINS_UI_ABOUT_H; ++i) {
            click(BTN_DOWN);
            scroll = lv_obj_get_scroll_y(s_app.ui.about_body);
        }
        const int last_line_bottom = (lines - 1) * STEINS_UI_LINE_H + STEINS_UI_LINE_H -
                                     lv_obj_get_scroll_y(s_app.ui.about_body);
        CHECK(last_line_bottom <= STEINS_UI_ABOUT_H,
              "scrolled to bottom shows last line (%d <= %d)", last_line_bottom,
              STEINS_UI_ABOUT_H);
        // 每次按键只滚一行 18px，不重绘整页。
        const int y0 = lv_obj_get_scroll_y(s_app.ui.about_body);
        click(BTN_UP);
        CHECK(lv_obj_get_scroll_y(s_app.ui.about_body) == y0 - STEINS_UI_LINE_H,
              "UP scrolls one line up");
    }
    long_press(BTN_OK);       // 返回标题
    CHECK(s_app.page == STEINS_PAGE_TITLE, "back to title (%d)", (int)s_app.page);


    // 单列窗口跟随：选中项始终落在 [first, first+3]，且末行盒底不越面板底(240)。
    for (int step = 0; step < 6; ++step) {
        click(BTN_DOWN);
        const int first = s_app.ui.title_first;
        CHECK(s_app.sel >= first && s_app.sel < first + STEINS_UI_TITLE_VISIBLE,
              "window: sel=%d first=%d", s_app.sel, first);
        const lv_obj_t *bottom = s_app.ui.title_rows[STEINS_UI_TITLE_VISIBLE - 1];
        CHECK(bottom->y + bottom->h <= 240, "window row bottom %d",
              bottom->y + bottom->h);
    }
    for (int step = 0; step < 6; ++step) {
        click(BTN_UP);
    }
    CHECK(s_app.sel == 0 && s_app.ui.title_first == 0, "window must reset at top (sel=%d first=%d)",
          s_app.sel, s_app.ui.title_first);

    // 开始游戏
    click(BTN_OK);
    settle_page();
    if (s_app.page == STEINS_PAGE_CG) {
        click(BTN_OK);
        settle_page();
    }
    CHECK(s_app.page == STEINS_PAGE_GAME, "not on game page after start (%d)", (int)s_app.page);
    CHECK(steins_ui_page_visible(&s_app.ui, STEINS_PAGE_GAME), "game page hidden");
    settle_typing();
    CHECK(s_app.ui.game_text->text[0] != '\0', "first line is empty");
    CHECK(s_app.ui.game_date->text[0] != '\0', "date label empty");
    CHECK(strcmp(s_app.ui.game_battery->text, "87%") == 0, "battery label '%s'",
          s_app.ui.game_battery->text);
    // 阅读页四硬指标：名字在框外（<150）、页码独立且不压名字/正文、末行盒底不进 240。
    CHECK(s_app.ui.game_name->y + s_app.ui.game_name->h <= 150,
          "speaker must sit above the band (y=%d h=%d)", s_app.ui.game_name->y,
          s_app.ui.game_name->h);
    CHECK(s_app.ui.game_page->x >= s_app.ui.game_status->x + s_app.ui.game_status->w,
          "page label overlaps status (x=%d vs %d+%d)", s_app.ui.game_page->x,
          s_app.ui.game_status->x, s_app.ui.game_status->w);
    CHECK(s_app.ui.game_page->y >= 150 && s_app.ui.game_page->x >= 262,
          "page label must stay in the panel top-right (x=%d y=%d)",
          s_app.ui.game_page->x, s_app.ui.game_page->y);
    CHECK(s_app.ui.game_text->y + s_app.ui.game_text->h <= 238,
          "last line bottom %d", s_app.ui.game_text->y + s_app.ui.game_text->h);
    // 4 行正文 164..236（去掉按键提示行）；提示只在图片出错时占用同一区块。
    CHECK(s_app.ui.game_text->y >= 156 && s_app.ui.game_text->y +
              s_app.ui.game_text->h <= 238,
          "body 4 lines must sit between 156 and 238 (y=%d h=%d)", s_app.ui.game_text->y,
          s_app.ui.game_text->h);
    CHECK(STEINS_TEXT_LINES == 4, "paginator lines must match the 4-line body");
    CHECK(!lv_obj_has_flag(s_app.ui.game_text, LV_OBJ_FLAG_HIDDEN),
          "body must be visible on a healthy scene");
    CHECK(lv_obj_has_flag(s_app.ui.game_hint, LV_OBJ_FLAG_HIDDEN),
          "key hint must be gone on the reading page");
    printf("  first line: '%s'\n", s_app.ui.game_text->text);
}

static void test_key_sequences(void)
{
    boot();
    click(BTN_OK);
    settle_page();
    settle_typing();
    if (s_app.page != STEINS_PAGE_GAME) {
        CHECK(false, "not on game page (%d)", (int)s_app.page);
        return;
    }
    const uint32_t start = s_app.pos.cursor;

    // 打字机:一帧之内只揭示字符,不改变游标
    s_app.shown = 0;
    tick_ms(20);
    CHECK(s_app.pos.cursor == start, "typewriter advanced the cursor");

    // 短按一次只前进一格
    s_app.shown = s_app.page_data.chars;
    click(BTN_OK);
    CHECK(s_app.pos.cursor == start + 1u, "one click moved cursor %u -> %u", (unsigned)start,
          (unsigned)s_app.pos.cursor);

    // 单独的 PRESS 不能推进(否则长按会双触发)
    const uint32_t before = s_app.pos.cursor;
    press(BTN_OK, EV_PRESS);
    CHECK(s_app.pos.cursor == before, "PRESS alone advanced the story");

    // 长按 OK 开菜单;LONG 之后的 RELEASE 不再推进
    long_press(BTN_OK);
    CHECK(s_app.page == STEINS_PAGE_MENU, "long OK did not open menu (%d)", (int)s_app.page);
    CHECK(s_app.pos.cursor == before, "long press advanced the story");
    long_press(BTN_OK);
    CHECK(s_app.page == STEINS_PAGE_GAME, "long OK did not return to game (%d)",
          (int)s_app.page);

    // 长按之后的下一次独立短按必须有效
    settle_typing();
    const uint32_t after_long = s_app.pos.cursor;
    click(BTN_OK);
    CHECK(s_app.pos.cursor == after_long + 1u, "click after long press was swallowed");

    // 长按 UP 快进,松手必须停
    press(BTN_UP, EV_PRESS);
    press(BTN_UP, EV_LONG);
    CHECK(s_app.fast_forward, "long UP did not start fast forward");
    press(BTN_UP, EV_RELEASE);
    CHECK(!s_app.fast_forward, "fast forward still on after release");
    printf("  key sequences ok\n");
}

// 逐句推进到选择点(中途会经过章节标题卡)。
static bool walk_to_choice(void)
{
    boot();
    click(BTN_OK);
    settle_page();
    for (int step = 0; step < 12000; ++step) {
        if (s_app.page == STEINS_PAGE_CHOICE) return true;
        if (s_app.page == STEINS_PAGE_ENDING) return false;
        if (s_app.chapter_card_active) {
            tick_ms(500);
            continue;
        }
        if (s_app.page == STEINS_PAGE_CG) {
            click(BTN_OK);
            continue;
        }
        if (s_app.page != STEINS_PAGE_GAME) {
            fprintf(stderr, "  walk stopped on page %d at step %d\n", (int)s_app.page, step);
            return false;
        }
        s_app.shown = s_app.page_data.chars;
        click(BTN_OK);
        tick_ms(400);
    }
    return false;
}

static void test_choice_semantics(void)
{
    if (!walk_to_choice()) {
        CHECK(false, "could not walk to the first choice");
        return;
    }
    CHECK(steins_ui_page_visible(&s_app.ui, STEINS_PAGE_CHOICE), "choice page hidden");
    CHECK(s_app.choice_count >= 2, "choice has %u items", s_app.choice_count);
    CHECK(s_app.ui.choice_rows[0]->text[0] != '\0', "first choice row empty");
    CHECK(strstr(s_app.ui.choice_hint->text, "choice.") == NULL,
          "choice hint shows a raw key: '%s'", s_app.ui.choice_hint->text);
    CHECK(strstr(s_app.ui.choice_hint->text, "确定") != NULL,
          "choice hint should say 确定: '%s'", s_app.ui.choice_hint->text);
    printf("  choice: tag '%s' rows '%s' / '%s'\n", s_app.ui.choice_tag->text,
           s_app.ui.choice_rows[0]->text, s_app.ui.choice_rows[1]->text);

    const uint16_t node_before = s_app.pos.node;
    click(BTN_DOWN);
    CHECK(s_app.choice_sel == 1, "choice cursor %d", s_app.choice_sel);
    CHECK(strcmp(s_app.ui.choice_rows[1]->text, s_app.ui.choice_rows[0]->text) != 0,
          "choice rows did not update with the cursor");

    // 菜单往返:必须回到待选页,并保留同一待选与非零光标
    long_press(BTN_OK);
    CHECK(s_app.page == STEINS_PAGE_MENU, "long OK on choice did not open menu");
    long_press(BTN_OK);
    CHECK(s_app.page == STEINS_PAGE_CHOICE, "menu did not return to choice (%d)",
          (int)s_app.page);
    CHECK(s_app.choice_sel == 1, "choice cursor lost after menu round trip (%d)",
          s_app.choice_sel);
    CHECK(s_app.pos.node == node_before, "menu round trip changed the node");

    // 显式确认才离开
    click(BTN_OK);
    CHECK(s_app.pos.node != node_before || s_app.page != STEINS_PAGE_CHOICE,
          "confirm did not leave the choice");
    CHECK(s_app.pos.cursor == 0, "choice did not restart at cursor 0");
    printf("  choice confirmed -> node %u page %d\n", (unsigned)s_app.pos.node,
           (int)s_app.page);
}

// 走到正文页并推进两句,便于做存档相关测试。
static bool start_and_step(void)
{
    boot();
    click(BTN_OK);
    settle_page();
    settle_typing();
    if (s_app.page != STEINS_PAGE_GAME) return false;
    click(BTN_OK);
    settle_typing();
    return s_app.page == STEINS_PAGE_GAME;
}

// 读 NVS 里某槽的原始字节(测试用;不是生产路径)。
static size_t read_slot(int slot, uint8_t *out, size_t cap)
{
    char key[12];
    snprintf(key, sizeof(key), "sv%02d", slot);
    nvs_handle_t handle;
    if (nvs_open("steins", NVS_READONLY, &handle) != ESP_OK) return 0;
    size_t len = cap;
    const bool ok = nvs_get_blob(handle, key, out, &len) == ESP_OK;
    nvs_close(handle);
    return ok ? len : 0;
}

// 覆盖确认:显式移到"确定"再按。
static void confirm_yes(void)
{
    click(BTN_UP);
    click(BTN_OK);
}

static void test_save_and_load(void)
{
    nvs_stub_reset();
    if (!start_and_step()) {
        CHECK(false, "could not reach the game page");
        return;
    }
    const uint16_t node = s_app.pos.node;
    const uint32_t cursor = s_app.pos.cursor;

    long_press(BTN_OK);
    CHECK(s_app.page == STEINS_PAGE_MENU, "menu did not open");
    CHECK(activate_row("保存", 9), "no save entry in menu");
    CHECK(s_app.page == STEINS_PAGE_SAVES, "not on save list (%d)", (int)s_app.page);
    CHECK(s_app.ui.list_title->text[0] != '\0', "save list title empty");
    CHECK(s_app.ui.list_subs[0]->text[0] != '\0', "empty slot has no label");

    click(BTN_OK);   // 第一格
    CHECK(s_app.page == STEINS_PAGE_CONFIRM, "no overwrite confirmation (%d)",
          (int)s_app.page);
    CHECK(s_app.ui.confirm_text->text[0] != '\0', "confirm text empty");
    confirm_yes();
    CHECK(s_app.notice_ms > 0, "no save notice");
    CHECK(s_app.page == STEINS_PAGE_SAVES, "did not return to the save list");
    CHECK(strstr(s_app.ui.list_hint->text, s_app.notice) != NULL,
          "save notice was overwritten by the list refresh ('%s')",
          s_app.ui.list_hint->text);

    // 回标题再读档
    long_press(BTN_OK);
    CHECK(s_app.page == STEINS_PAGE_MENU, "not back in menu");
    CHECK(activate_row("返回标题", 9), "no title entry in menu");
    CHECK(s_app.page == STEINS_PAGE_TITLE, "did not reach title (%d)", (int)s_app.page);

    CHECK(activate_row("读取", 7), "no load entry on title");
    CHECK(s_app.page == STEINS_PAGE_SAVES, "load list not opened (%d)", (int)s_app.page);
    click(BTN_OK);
    CHECK(s_app.page == STEINS_PAGE_GAME, "load did not enter the game (%d)", (int)s_app.page);
    CHECK(s_app.pos.node == node && s_app.pos.cursor == cursor,
          "loaded position %u/%u != saved %u/%u", (unsigned)s_app.pos.node,
          (unsigned)s_app.pos.cursor, (unsigned)node, (unsigned)cursor);
    printf("  save/load roundtrip at node %u cursor %u\n", (unsigned)node, (unsigned)cursor);
}

// 覆盖确认默认"取消":一次 OK 不覆盖任何东西;主动移到确定才真的覆盖;
// 非零槽光标在取消后背回列表时保持。
static void test_overwrite_confirm_default(void)
{
    nvs_stub_reset();
    if (!start_and_step()) {
        CHECK(false, "could not reach the game page");
        return;
    }
    // 先在第 3 格存一次(显式确认)。
    long_press(BTN_OK);
    CHECK(activate_row("保存", 9), "no save entry");
    click(BTN_DOWN);
    click(BTN_DOWN);
    CHECK(s_app.sel == 2, "save cursor %d", s_app.sel);
    click(BTN_OK);
    CHECK(s_app.page == STEINS_PAGE_CONFIRM, "no confirm page");
    CHECK(s_app.confirm_sel == 1, "confirm default is not cancel (sel=%d)", s_app.confirm_sel);
    confirm_yes();
    CHECK(s_app.notice_ms > 0, "first save produced no notice");
    uint8_t before[STEINS_SAVE_BYTES];
    const size_t before_len = read_slot(2, before, sizeof(before));
    CHECK(before_len == STEINS_SAVE_BYTES, "slot 3 was not written (%u)", (unsigned)before_len);

    // 换一句(位置变了),再打开同一格的确认,直接按 OK = 取消。
    long_press(BTN_OK);
    CHECK(s_app.page == STEINS_PAGE_MENU, "not back in menu");
    CHECK(activate_row("保存", 9), "no save entry (2)");
    click(BTN_DOWN);
    click(BTN_DOWN);
    CHECK(s_app.sel == 2, "save cursor lost (%d)", s_app.sel);
    click(BTN_OK);
    CHECK(s_app.page == STEINS_PAGE_CONFIRM, "no confirm page (2)");
    click(BTN_OK);   // 默认取消
    CHECK(s_app.page == STEINS_PAGE_SAVES, "cancel did not return to the list (%d)",
          (int)s_app.page);
    CHECK(s_app.sel == 2, "slot cursor changed after cancel (%d)", s_app.sel);
    uint8_t after[STEINS_SAVE_BYTES];
    const size_t after_len = read_slot(2, after, sizeof(after));
    CHECK(after_len == before_len && memcmp(before, after, before_len) == 0,
          "default confirm overwrote the slot bytes");

    // 塞一条不同位置的旧档,确认"真的覆盖"能把它换掉。
    {
        uint8_t stale[STEINS_SAVE_BYTES];
        steins_pos_t other = { 5, 7, STEINS_MODE_STORY, 0 };
        steins_save_encode(&other, stale, sizeof(stale));
        nvs_handle_t handle;
        nvs_open("steins", NVS_READWRITE, &handle);
        nvs_set_blob(handle, "sv02", stale, sizeof(stale));
        nvs_commit(handle);
        nvs_close(handle);
    }

    // 真的覆盖:这次主动移到"确定"。
    click(BTN_OK);
    CHECK(s_app.page == STEINS_PAGE_CONFIRM, "no confirm page (3)");
    CHECK(s_app.confirm_sel == 1, "confirm default drifted (sel=%d)", s_app.confirm_sel);
    confirm_yes();
    uint8_t changed[STEINS_SAVE_BYTES];
    const size_t changed_len = read_slot(2, changed, sizeof(changed));
    CHECK(changed_len == STEINS_SAVE_BYTES, "overwrite wrote nothing");
    CHECK(!(changed[4] == 5 && changed[5] == 0 && changed[6] == 7),
          "explicit confirm did not overwrite the stale record");
    steins_pos_t stored;
    CHECK(steins_save_decode(&stored, changed, changed_len), "slot 3 does not decode");
    CHECK(stored.node == s_app.pos.node && stored.cursor == s_app.pos.cursor,
          "slot 3 stores %u/%u, position is %u/%u", (unsigned)stored.node,
          (unsigned)stored.cursor, (unsigned)s_app.pos.node, (unsigned)s_app.pos.cursor);

    // 回到标题读第 3 格,位置应为覆盖后的那一句,且不是最初那句。
    long_press(BTN_OK);
    CHECK(activate_row("返回标题", 9), "no title entry");
    CHECK(activate_row("读取", 7), "no load entry");
    click(BTN_DOWN);
    click(BTN_DOWN);
    click(BTN_OK);
    CHECK(s_app.page == STEINS_PAGE_GAME, "load of slot 3 failed (%d)", (int)s_app.page);
    CHECK(s_app.pos.node != 5 || s_app.pos.cursor != 7,
          "loaded slot 3 still holds the stale record");
    printf("  overwrite confirm: default cancel kept bytes, explicit confirm replaced them\n");
}

static void test_save_failures(void)
{
    nvs_stub_reset();
    if (!start_and_step()) {
        CHECK(false, "could not reach the game page");
        return;
    }
    long_press(BTN_OK);
    CHECK(activate_row("保存", 9), "no save entry");
    click(BTN_OK);
    confirm_yes();
    CHECK(s_app.notice_ms > 0, "first save produced no notice");
    char first_notice[sizeof(s_app.notice)];
    snprintf(first_notice, sizeof(first_notice), "%s", s_app.notice);

    // commit 失败:必须报失败,旧档不丢
    nvs_stub_set_fail_commit(1);
    long_press(BTN_OK);
    CHECK(s_app.page == STEINS_PAGE_MENU, "not back in menu");
    CHECK(activate_row("保存", 9), "no save entry (2)");
    click(BTN_OK);
    confirm_yes();
    CHECK(s_app.notice_ms > 0, "commit failure produced no notice");
    CHECK(strcmp(s_app.notice, first_notice) != 0, "failure reused the success notice '%s'",
          s_app.notice);
    nvs_stub_set_fail_commit(0);

    // 旧档仍可读取
    long_press(BTN_OK);
    CHECK(s_app.page == STEINS_PAGE_MENU, "not back in menu after failure");
    CHECK(activate_row("读取", 9), "no load entry in menu");
    click(BTN_OK);
    CHECK(s_app.page == STEINS_PAGE_GAME, "old save was lost after a failed commit (%d)",
          (int)s_app.page);
    printf("  failed commit kept the previous save\n");
}

static void test_corrupt_save(void)
{
    nvs_stub_reset();
    if (!start_and_step()) {
        CHECK(false, "could not reach the game page");
        return;
    }
    const uint16_t node = s_app.pos.node;
    const uint32_t cursor = s_app.pos.cursor;

    // 直接注入一条 CRC 错的存档到第 3 格
    uint8_t bad[STEINS_SAVE_BYTES];
    steins_pos_t pos = { 3, 33, STEINS_MODE_STORY, 0 };
    steins_save_encode(&pos, bad, sizeof(bad));
    bad[7] ^= 0x55u;
    nvs_handle_t handle;
    nvs_open("steins", NVS_READWRITE, &handle);
    nvs_set_blob(handle, "sv02", bad, sizeof(bad));
    nvs_commit(handle);
    nvs_close(handle);

    long_press(BTN_OK);
    CHECK(activate_row("读取", 9), "no load entry");
    CHECK(s_app.page == STEINS_PAGE_SAVES, "load list not opened");
    click(BTN_DOWN);
    click(BTN_DOWN);
    click(BTN_OK);
    CHECK(s_app.page == STEINS_PAGE_SAVES, "corrupt save was accepted (page %d)",
          (int)s_app.page);
    CHECK(s_app.notice_ms > 0, "corrupt save produced no notice");
    CHECK(s_app.pos.node == node && s_app.pos.cursor == cursor,
          "corrupt save changed the reading position");
    printf("  corrupt save rejected, position unchanged\n");
}

// 邮件正文超过一屏时必须先给可翻页的阅读页;翻完再回到同一个待选,光标不变。
static void test_mail_page(void)
{
    if (!walk_to_choice()) {
        CHECK(false, "could not walk to the first choice");
        return;
    }
    const uint16_t choice_node = s_app.pos.node;
    click(BTN_OK);   // 第一项:打开邮件 → 进入长正文的选择点
    CHECK(s_app.pos.node != choice_node || s_app.page != STEINS_PAGE_CHOICE,
          "choice did not advance to the mail decision");
    CHECK(s_app.page == STEINS_PAGE_MAIL, "long heading did not open the mail page (%d)",
          (int)s_app.page);
    CHECK(s_app.mail_pages > 1, "mail page count %d", s_app.mail_pages);
    CHECK(s_app.ui.mail_text->text[0] != '\0', "mail page body is empty");
    CHECK(strchr(s_app.ui.mail_page->text, '/') != NULL, "mail page indicator '%s'",
          s_app.ui.mail_page->text);

    // 完整正文(来自包里同一个 heading 串)
    steins_choice_t choice;
    CHECK(steins_pack_choice(&s_app.pack, s_app.choice_record, &choice), "choice unreadable");
    static char full[STEINS_CHOICE_TEXT];
    const int full_len = steins_pack_str_utf8(&s_app.pack, choice.heading, full, sizeof(full));
    CHECK(full_len > 0, "heading does not transcode (%d)", full_len);
    CHECK(full_len <= (int)sizeof(full) - 1, "heading longer than the app buffer");

    // 逐页读完,拼接结果必须等于完整 heading。
    static char joined[STEINS_TEXT_BYTES];
    size_t used = 0;
    for (int page = 0; page < s_app.mail_pages; ++page) {
        for (int k = 0; k < s_app.mail_data.bytes; ++k) {
            if (s_app.mail_data.text[k] == '\n') continue;
            if (used + 1u < sizeof(joined)) joined[used++] = s_app.mail_data.text[k];
        }
        if (page + 1 < s_app.mail_pages) {
            click(BTN_DOWN);
            CHECK(s_app.page == STEINS_PAGE_MAIL, "left the mail page while paging");
        }
    }
    joined[used] = '\0';
    // 换行是排版分隔(源正文里也可能有硬换行),比较时两边都去掉。
    static char full_flat[STEINS_CHOICE_TEXT];
    size_t flat = 0;
    for (int k = 0; k < full_len && flat + 1u < sizeof(full_flat); ++k) {
        if (full[k] == '\n') continue;
        full_flat[flat++] = full[k];
    }
    full_flat[flat] = '\0';
    CHECK(strcmp(joined, full_flat) == 0,
          "mail pages lost text: rejoined %u B, heading %u B", (unsigned)used,
          (unsigned)flat);

    // 往上翻回第一页,再确定回到选项。
    for (int i = 0; i + 1 < s_app.mail_pages; ++i) click(BTN_UP);
    CHECK(strncmp(s_app.ui.mail_page->text, "1/", 2) == 0, "did not page back to 1 (%s)",
          s_app.ui.mail_page->text);
    click(BTN_OK);
    CHECK(s_app.page == STEINS_PAGE_CHOICE, "OK on the mail page did not return to the choice (%d)",
          (int)s_app.page);
    CHECK(s_app.choice_sel == 0, "mail page changed the choice cursor (%d)", s_app.choice_sel);
    CHECK(s_app.pos.node == choice_node + 1 || s_app.pos.node != choice_node,
          "mail page confirmed the choice by itself");
    CHECK(s_app.ui.choice_rows[0]->text[0] != '\0', "choice rows empty after the mail page");
    printf("  mail page: %d pages, %d B reassembled, back to choice cursor %d\n",
           s_app.mail_pages, full_len, s_app.choice_sel);
}

// 一路按"第一项"推进到结局(会经过章节卡与邮件页)。
static bool walk_to_ending(int max_steps)
{
    for (int step = 0; step < max_steps; ++step) {
        if (s_app.page == STEINS_PAGE_ENDING) return true;
        if (s_app.chapter_card_active) {
            tick_ms(500);
            continue;
        }
        if (s_app.page == STEINS_PAGE_CG || s_app.page == STEINS_PAGE_MAIL ||
            s_app.page == STEINS_PAGE_CHOICE) {
            click(BTN_OK);
            continue;
        }
        if (s_app.page != STEINS_PAGE_GAME) return false;
        s_app.shown = s_app.page_data.chars;
        click(BTN_OK);
        tick_ms(200);
    }
    return false;
}

static int count_unlocked_endings(void)
{
    int n = 0;
    for (int i = 0; i < STEINS_MAX_ENDINGS; ++i) {
        if (steins_unlock_has_ending(&s_app.unlock, (uint8_t)i)) ++n;
    }
    return n;
}

// story 模式读到结局要点亮全局结局位;章节回顾读到结局只结束阅读,不写这些位。
static void test_ending_mode_guard(void)
{
    nvs_stub_reset();
    boot();
    click(BTN_OK);
    settle_page();
    CHECK(walk_to_ending(40000), "story walk did not reach an ending (page %d)",
          (int)s_app.page);
    CHECK(s_app.page == STEINS_PAGE_ENDING, "not on the ending page (%d)", (int)s_app.page);
    CHECK(count_unlocked_endings() == 1, "story ending unlocked %d bits",
          count_unlocked_endings());
    const bool story_true_end = steins_unlock_true_end(&s_app.unlock);
    printf("  story ending: '%s', true-end bit %d\n", s_app.ui.ending_body->text,
           story_true_end ? 1 : 0);
    CHECK(strstr(s_app.ui.ending_hint->text, "end.") == NULL,
          "ending hint shows a raw key: '%s'", s_app.ui.ending_hint->text);
    CHECK(strstr(s_app.ui.ending_hint->text, "OK") != NULL,
          "ending hint should mention OK: '%s'", s_app.ui.ending_hint->text);
    long_press(BTN_OK);
    CHECK(s_app.page == STEINS_PAGE_TITLE, "long OK on the ending did not return (%d)",
          (int)s_app.page);

    // 章节模式:同一批挑战(从第 01 章开始走到结局),不得点亮结局/真结局位。
    nvs_stub_reset();
    boot();
    CHECK(steins_unlock_set_chapter(&s_app.unlock, 1), "preset chapter bit");
    CHECK(steins_unlock_set_gallery(&s_app.unlock, 3), "preset gallery bit");
    CHECK(activate_row("章节", 7), "no chapter entry on title");
    CHECK(s_app.page == STEINS_PAGE_CHAPTERS, "chapter list not opened (%d)",
          (int)s_app.page);
    click(BTN_DOWN);
    click(BTN_OK);
    settle_page();   // 章节入口先播标题卡
    if (s_app.page == STEINS_PAGE_CG) {
        click(BTN_OK);
        settle_page();
    }
    CHECK(s_app.page == STEINS_PAGE_GAME, "chapter entry did not start reading (%d)",
          (int)s_app.page);
    CHECK(s_app.pos.mode == STEINS_MODE_CHAPTER, "chapter entry mode %u", s_app.pos.mode);
    CHECK(walk_to_ending(40000), "chapter walk did not reach an ending (page %d)",
          (int)s_app.page);
    CHECK(s_app.page == STEINS_PAGE_ENDING, "chapter walk not on the ending page");
    CHECK(count_unlocked_endings() == 0, "chapter recall unlocked %d ending bits",
          count_unlocked_endings());
    CHECK(!steins_unlock_true_end(&s_app.unlock), "chapter recall set the true-end bit");
    CHECK(steins_unlock_has_chapter(&s_app.unlock, 1), "chapter recall cleared a chapter bit");
    CHECK(steins_unlock_has_gallery(&s_app.unlock, 3), "chapter recall cleared the gallery bit");
    printf("  chapter ending: 0 ending bits, chapter/gallery bits intact\n");
}

static void test_unlock_persistence(void)
{
    nvs_stub_reset();
    boot();
    CHECK(steins_unlock_set_chapter(&s_app.unlock, 1), "set chapter bit");
    s_app.unlock_dirty = true;
    steins_app_flush(&s_app);
    CHECK(!s_app.unlock_dirty, "flush did not persist the unlock bits");

    boot();   // 模拟重启
    CHECK(steins_unlock_has_chapter(&s_app.unlock, 1), "unlock lost across restart");
    CHECK(s_app.page == STEINS_PAGE_TITLE, "restart did not land on the title page");

    steins_unlock_t extra = s_app.unlock;
    steins_unlock_set_gallery(&extra, 9);
    steins_unlock_t merged = s_app.unlock;
    CHECK(steins_unlock_merge(&merged, &extra), "merge reported no change");
    CHECK(steins_unlock_has_chapter(&merged, 1), "merge dropped a chapter bit");
    CHECK(steins_unlock_has_gallery(&merged, 9), "merge dropped the new gallery bit");
    printf("  unlock persisted and merged\n");
}

static void test_locked_chapter(void)
{
    nvs_stub_reset();
    boot();
    CHECK(activate_row("章节", 7), "no chapter entry on title");
    CHECK(s_app.page == STEINS_PAGE_CHAPTERS, "chapter list not opened (%d)",
          (int)s_app.page);
    CHECK(s_app.ui.list_title->text[0] != '\0', "chapter list title empty");
    CHECK(s_app.ui.list_subs[0]->text[0] != '\0', "locked chapter has no state label");
    printf("  chapter row: '%s' / '%s'\n", s_app.ui.list_rows[0]->text,
           s_app.ui.list_subs[0]->text);
    // 第 1 章(序章之后)未到达:确认也不能进入
    click(BTN_DOWN);
    click(BTN_OK);
    CHECK(s_app.page == STEINS_PAGE_CHAPTERS, "locked chapter was entered (%d)",
          (int)s_app.page);
    printf("  locked chapter stayed locked\n");
}

static void test_history_and_settings(void)
{
    nvs_stub_reset();
    if (!start_and_step()) {
        CHECK(false, "could not reach the game page");
        return;
    }
    click(BTN_DOWN);
    CHECK(s_app.page == STEINS_PAGE_HISTORY, "history not opened (%d)", (int)s_app.page);
    CHECK(s_app.history_count > 0, "history empty");
    CHECK(s_app.ui.list_rows[0]->text[0] != '\0', "history row empty");
    long_press(BTN_OK);
    CHECK(s_app.page == STEINS_PAGE_GAME, "history did not return to the game (%d)",
          (int)s_app.page);

    long_press(BTN_OK);
    CHECK(activate_row("设置", 9), "no settings entry in menu");
    CHECK(s_app.page == STEINS_PAGE_SETTINGS, "settings not opened (%d)", (int)s_app.page);
    const uint8_t before = s_app.text_speed;
    click(BTN_DOWN);
    CHECK(s_app.page == STEINS_PAGE_SETTINGS, "settings page left on DOWN");
    click(BTN_OK);
    CHECK(s_app.text_speed != before, "text speed did not change");
    printf("  history/settings ok\n");
}


// ---- 导航回归(2026-11 审计):返回提示 / 短 OK 返回 / 空列表反馈 ----
// 修复前:历史与关于页只有长 OK 能退,提示行为空。
static void test_back_hint_and_short_ok(void)
{
    nvs_stub_reset();
    boot();
    click(BTN_OK);
    settle_page();
    CHECK(strstr(s_app.ui.game_status->text, "长按OK") == NULL,
          "reading page must not show the key hint: '%s'", s_app.ui.game_status->text);
    click(BTN_DOWN);
    CHECK(s_app.page == STEINS_PAGE_HISTORY, "history page (%d)", (int)s_app.page);
    CHECK(strstr(s_app.ui.list_hint->text, "长按") != NULL, "history hint missing: '%s'",
          s_app.ui.list_hint->text);
    click(BTN_OK);
    CHECK(s_app.page == STEINS_PAGE_GAME, "short OK did not leave history (%d)",
          (int)s_app.page);

    boot();
    CHECK(activate_row("关于", 8), "no about row on title");
    CHECK(s_app.page == STEINS_PAGE_ABOUT, "about page (%d)", (int)s_app.page);
    CHECK(strstr(s_app.ui.list_hint->text, "返回") != NULL, "about hint missing: '%s'",
          s_app.ui.list_hint->text);
    click(BTN_OK);
    CHECK(s_app.page == STEINS_PAGE_TITLE, "short OK did not leave about (%d)",
          (int)s_app.page);
}

// 修复前:菜单第 4 行显示 pack 缺失的键名 "menu.phone"。
static void test_menu_labels_and_hint(void)
{
    boot();
    click(BTN_OK);
    settle_page();
    long_press(BTN_OK);
    CHECK(s_app.page == STEINS_PAGE_MENU, "menu page (%d)", (int)s_app.page);
    CHECK(strstr(s_app.ui.list_hint->text, "长按") != NULL, "menu hint missing: '%s'",
          s_app.ui.list_hint->text);
    bool has_phone = false;
    bool raw_key = false;
    for (int i = 0; i < STEINS_UI_LIST_ROWS; ++i) {
        const char *text = s_app.ui.list_rows[i]->text;
        if (strstr(text, "menu.") != NULL) raw_key = true;
        if (strcmp(text, "手机") == 0) has_phone = true;
    }
    CHECK(!raw_key, "menu shows a raw ui key");
    CHECK(has_phone, "menu has no 手机 row");
}

// 修复前:空收信箱/联系人按 OK 没有任何可见反馈。
static void test_empty_phone_feedback(void)
{
    boot();
    click(BTN_OK);
    settle_page();
    long_press(BTN_OK);
    s_app.sel = 3;   // 手机
    click(BTN_OK);
    CHECK(s_app.page == STEINS_PAGE_PHONE_HOME, "phone home (%d)", (int)s_app.page);
    CHECK(strstr(s_app.ui.list_hint->text, "长按") != NULL, "phone home hint missing: '%s'",
          s_app.ui.list_hint->text);
    click(BTN_OK);   // 收信箱
    CHECK(s_app.page == STEINS_PAGE_PHONE_INBOX, "inbox (%d)", (int)s_app.page);
    click(BTN_OK);   // 空表
    CHECK(s_app.page == STEINS_PAGE_PHONE_INBOX, "empty inbox left the page");
    CHECK(strstr(s_app.ui.list_hint->text, "没有") != NULL, "empty inbox silent: '%s'",
          s_app.ui.list_hint->text);
    long_press(BTN_OK);
    click(BTN_DOWN);
    click(BTN_DOWN);
    click(BTN_OK);   // 联系人
    CHECK(s_app.page == STEINS_PAGE_PHONE_CONTACTS, "contacts (%d)", (int)s_app.page);
    click(BTN_OK);
    CHECK(s_app.notice_ms > 0, "contacts OK has no feedback at all");
    CHECK(strstr(s_app.ui.list_hint->text, "联系人") != NULL, "contacts silent: '%s'",
          s_app.ui.list_hint->text);
}

// 修复前:设置页第 1 行"字号"按 OK 无变化也无提示。
static void test_settings_font_row_feedback(void)
{
    boot();
    click(BTN_OK);
    settle_page();
    long_press(BTN_OK);
    s_app.sel = 8;   // 设置
    click(BTN_OK);
    CHECK(s_app.page == STEINS_PAGE_SETTINGS, "settings (%d)", (int)s_app.page);
    CHECK(strstr(s_app.ui.list_hint->text, "长按") != NULL, "settings hint missing: '%s'",
          s_app.ui.list_hint->text);
    click(BTN_OK);   // 字号
    CHECK(strstr(s_app.ui.list_hint->text, "16") != NULL, "font row silent: '%s'",
          s_app.ui.list_hint->text);
}

static uint8_t *load_file(const char *path, uint32_t *size_out)
{
    FILE *fh = fopen(path, "rb");
    if (!fh) return NULL;
    fseek(fh, 0, SEEK_END);
    const long size = ftell(fh);
    fseek(fh, 0, SEEK_SET);
    uint8_t *data = (uint8_t *)malloc((size_t)size);
    if (!data || fread(data, 1, (size_t)size, fh) != (size_t)size) {
        free(data);
        fclose(fh);
        return NULL;
    }
    fclose(fh);
    *size_out = (uint32_t)size;
    return data;
}

int main(int argc, char **argv)
{
    setvbuf(stderr, NULL, _IONBF, 0);
    const char *path = argc > 1 ? argv[1] : "assets/steins/steins_pack.bin";
    uint32_t size = 0;
    uint8_t *blob = load_file(path, &size);
    if (!blob) {
        fprintf(stderr, "cannot read pack: %s\n", path);
        return 2;
    }
    steins_pack_host_set(blob, size);
    bsp_stub_set_soc(87);
    nvs_stub_reset();

    test_title_and_entry();
    test_key_sequences();
    test_choice_semantics();
    test_save_and_load();
    test_overwrite_confirm_default();
    test_save_failures();
    test_corrupt_save();
    test_unlock_persistence();
    test_locked_chapter();
    test_mail_page();
    test_ending_mode_guard();
    test_history_and_settings();
    test_back_hint_and_short_ok();
    test_menu_labels_and_hint();
    test_empty_phone_feedback();
    test_settings_font_row_feedback();

    free(blob);
    if (g_failures) {
        printf("FAILED (%d)\n", g_failures);
        return 1;
    }
    printf("OK\n");
    return 0;
}
