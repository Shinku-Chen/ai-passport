// tests/test_steins0_ui_visibility.c —— 生产 UI 的可见性/行文本/坐标薄桩回归。
#include "steins0_ui.h"

#include "lvgl.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); ++failures; } } while (0)

static uint16_t s_pixels[SG0_SCREEN_W * SG0_SCREEN_H];
static uint16_t s_stamp[96 * 96];
static steins0_ui_t s_ui;
static const lv_font_t s_font = {0};

static void boot(void)
{
    host_lvgl_reset();
    host_lvgl_set_locked(1);
    CHECK(steins0_ui_create(&s_ui, lv_screen_active(), s_pixels, s_stamp, &s_font), "ui create");
}

static void only_one_page(void)
{
    int visible = 0;
    const lv_obj_t *pages[] = {s_ui.page_title, s_ui.page_game, s_ui.page_list, s_ui.page_cg,
                               s_ui.page_history, s_ui.page_about, s_ui.page_ending,
                               s_ui.page_confirm, s_ui.page_phone, s_ui.page_phone_thread,
                               s_ui.page_card};
    for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); ++i) {
        if (pages[i] && !pages[i]->hidden) ++visible;
    }
    CHECK(visible == 1, "visible pages %d", visible);
}

int main(void)
{
    boot();
    CHECK(!s_ui.page_title->hidden, "title visible at create");
    only_one_page();
    CHECK(s_ui.page_title->w == 320 && s_ui.page_title->h == 240, "title size");
    const char *titles[6] = {"开始游戏", "继续游戏", "读取存档", "章节选择", "鉴赏室", "设置"};
    steins0_ui_title(&s_ui, titles, 6, 2);
    for (int i = 0; i < 6; ++i) {
        CHECK(strcmp(s_ui.title_rows[i]->text, titles[i]) == 0, "title row %d", i);
        CHECK(s_ui.title_rows[i]->x >= 0 && s_ui.title_rows[i]->x + s_ui.title_rows[i]->w <= 320,
              "title row %d x", i);
        CHECK(s_ui.title_rows[i]->y >= 180 && s_ui.title_rows[i]->y + s_ui.title_rows[i]->h <= 240,
              "title row %d y %d", i, s_ui.title_rows[i]->y);
    }
    host_lvgl_fail_next(1);
    CHECK(!steins0_ui_show(&s_ui, SG0_PAGE_GAME), "allocation failure reported");
    CHECK(s_ui.page_title != NULL && !s_ui.page_title->hidden, "title retained on failure");
    CHECK(s_ui.page_game == NULL, "failed target page not retained");
    CHECK(!s_ui.notice_box->hidden, "allocation failure notice visible");
    CHECK(strcmp(s_ui.notice_text->text, "内存不足") == 0,
          "allocation failure notice text %s", s_ui.notice_text->text);
    steins0_ui_notice_hide(&s_ui);

    host_lvgl_fail_after(1);
    CHECK(!steins0_ui_show(&s_ui, SG0_PAGE_GAME), "child allocation failure reported");
    CHECK(s_ui.page_title != NULL && !s_ui.page_title->hidden, "title retained on child failure");
    CHECK(s_ui.page_game == NULL, "partial target page cleaned after child failure");
    CHECK(!s_ui.notice_box->hidden, "child allocation failure notice visible");
    steins0_ui_notice_hide(&s_ui);

    steins0_ui_show(&s_ui, SG0_PAGE_GAME);
    only_one_page();
    CHECK(s_ui.page_game != NULL && !s_ui.page_game->hidden, "game visible");
    CHECK(s_ui.page_list == NULL, "list lazy before first visit");
    const char *lines[3] = {"第一行正文", "第二行正文", ""};
    steins0_ui_game_text(&s_ui, "红莉栖", lines, 2);
    CHECK(strcmp(s_ui.speaker_label->text, "红莉栖") == 0, "speaker text");
    CHECK(s_ui.speaker_label->w == LV_SIZE_CONTENT, "speaker width is content-sized");
    for (int i = 0; i < 2; ++i) {
        CHECK(strcmp(s_ui.text_lines[i]->text, lines[i]) == 0, "line %d text", i);
        CHECK(s_ui.text_lines[i]->y == 172 + i * 19 && s_ui.text_lines[i]->w == 304,
              "line %d geometry", i);
        CHECK(s_ui.text_lines[i]->y + s_ui.text_lines[i]->h <= 240, "line %d inside", i);
    }
    steins0_ui_game_page(&s_ui, 1, 3);
    CHECK(strcmp(s_ui.page_label->text, "2/3") == 0, "page label");
    steins0_ui_choice(&s_ui, "来电", (const char *const[]){"接听", "不接"}, 2, 1);
    CHECK(!s_ui.choice_box->hidden, "choice visible");
    CHECK(strcmp(s_ui.choice_rows[1]->text, "不接") == 0, "choice row text");
    CHECK(s_ui.choice_box->y + s_ui.choice_box->h <= 240, "choice box inside");
    steins0_ui_choice_hide(&s_ui);
    CHECK(s_ui.choice_box->hidden, "choice hidden");

    host_lvgl_fail_next(1);
    CHECK(!steins0_ui_show(&s_ui, SG0_PAGE_LIST), "secondary allocation failure reported");
    CHECK(s_ui.page_game != NULL && !s_ui.page_game->hidden, "current game retained on failure");
    CHECK(s_ui.page_list == NULL, "failed list not retained");
    CHECK(!s_ui.notice_box->hidden, "secondary failure notice visible");
    steins0_ui_notice_hide(&s_ui);

    steins0_ui_show(&s_ui, SG0_PAGE_LIST);
    only_one_page();
    CHECK(s_ui.page_game == NULL, "game released after leaving");
    CHECK(s_ui.page_list != NULL, "list created on demand");
    const char *rows[8] = {"r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7"};
    const char *values[8] = {"v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7"};
    steins0_ui_list(&s_ui, "设置", rows, values, 8, 7, "长按 OK 返回");
    for (int i = 0; i < 8; ++i) {
        CHECK(strcmp(s_ui.list_rows[i]->text, rows[i]) == 0, "list row %d", i);
        CHECK(strcmp(s_ui.list_values[i]->text, values[i]) == 0, "list value %d", i);
        CHECK(s_ui.list_rows[i]->y >= 28 && s_ui.list_rows[i]->y + s_ui.list_rows[i]->h <= 240,
              "list row %d inside", i);
    }
    CHECK(strcmp(s_ui.list_title->text, "设置") == 0, "list title");
    CHECK(strcmp(s_ui.list_footer->text, "长按 OK 返回") == 0, "list footer");

    steins0_ui_notice(&s_ui, "");
    CHECK(s_ui.notice_box->hidden, "empty notice hidden");
    CHECK(s_ui.notice_text->text[0] == 0, "empty notice text");

    steins0_ui_show(&s_ui, SG0_PAGE_GAME);
    CHECK(s_ui.page_game != NULL && s_ui.page_list == NULL, "game recreated, list released");
    steins0_ui_game_text(&s_ui, "回到正文", (const char *const[]){"重开的行", "", ""}, 1);
    CHECK(strcmp(s_ui.text_lines[0]->text, "重开的行") == 0, "recreated game text");
    steins0_ui_show(&s_ui, SG0_PAGE_LIST);
    CHECK(s_ui.page_list != NULL && s_ui.page_game == NULL, "list recreated, game released");

    steins0_ui_show(&s_ui, SG0_PAGE_CG);
    only_one_page();
    steins0_ui_cg(&s_ui, "EV_C01", "2/3");
    CHECK(strcmp(s_ui.cg_caption->text, "EV_C01") == 0, "cg caption");
    CHECK(strcmp(s_ui.cg_variant->text, "2/3") == 0, "cg variant");

    steins0_ui_show(&s_ui, SG0_PAGE_HISTORY);
    only_one_page();
    steins0_ui_history(&s_ui, "历史正文");
    CHECK(strcmp(s_ui.history_label->text, "历史正文") == 0, "history text");
    steins0_ui_scroll(&s_ui, SG0_PAGE_HISTORY, 19);
    CHECK(s_ui.history_view->scroll_y != 0, "history scrolled %d", s_ui.history_view->scroll_y);

    steins0_ui_show(&s_ui, SG0_PAGE_ABOUT);
    only_one_page();
    steins0_ui_about(&s_ui, "关于正文");
    CHECK(strcmp(s_ui.about_label->text, "关于正文") == 0, "about text");
    steins0_ui_scroll(&s_ui, SG0_PAGE_ABOUT, -19);
    CHECK(s_ui.about_view->scroll_y != 0, "about scrolled");

    steins0_ui_show(&s_ui, SG0_PAGE_ENDING);
    only_one_page();
    steins0_ui_ending(&s_ui, "ENDING", "结局名", "短按 OK 返回标题");
    CHECK(strcmp(s_ui.ending_label->text, "结局名") == 0, "ending text");
    CHECK(strcmp(s_ui.ending_hint->text, "短按 OK 返回标题") == 0, "ending hint");

    steins0_ui_show(&s_ui, SG0_PAGE_CONFIRM);
    only_one_page();
    steins0_ui_confirm(&s_ui, "覆盖存档?", "覆盖", "取消", 0);
    CHECK(!s_ui.page_confirm->hidden, "confirm visible");
    CHECK(strcmp(s_ui.confirm_prompt->text, "覆盖存档?") == 0, "confirm prompt");
    CHECK(strcmp(s_ui.confirm_rows[0]->text, "覆盖") == 0, "confirm yes");

    steins0_ui_choice_hide(&s_ui);
    steins0_ui_scroll(&s_ui, SG0_PAGE_HISTORY, 7);
    for (int cycle = 0; cycle < 3; ++cycle) {
        steins0_ui_show(&s_ui, SG0_PAGE_GAME);
        only_one_page();
        CHECK(s_ui.page_game != NULL, "cycle %d game", cycle);
        steins0_ui_game_text(&s_ui, "循环", (const char *const[]){"页", "", ""}, 1);
        steins0_ui_show(&s_ui, SG0_PAGE_PHONE);
        only_one_page();
        CHECK(s_ui.page_phone != NULL && s_ui.page_game == NULL, "cycle %d phone", cycle);
        steins0_ui_phone(&s_ui, "手机", "", "", (const char *const[]){"收件箱"}, NULL, 1, 0, "");
        steins0_ui_show(&s_ui, SG0_PAGE_PHONE_THREAD);
        only_one_page();
        CHECK(s_ui.page_phone_thread != NULL && s_ui.page_phone == NULL,
              "cycle %d thread", cycle);
        steins0_ui_phone_thread(&s_ui, "会话", "对方", (const char *const[]){"消息", "", ""}, 1,
                                false, "");
        steins0_ui_show(&s_ui, SG0_PAGE_CARD);
        only_one_page();
        CHECK(s_ui.page_card != NULL && s_ui.page_phone_thread == NULL, "cycle %d card", cycle);
        steins0_ui_card(&s_ui, "章节");
    }
    steins0_ui_show(&s_ui, SG0_PAGE_LIST);
    CHECK(s_ui.page_list != NULL, "list available after cycles");

    steins0_ui_notice(&s_ui, "保存成功");
    CHECK(!s_ui.notice_box->hidden, "notice visible");
    CHECK(strcmp(s_ui.notice_text->text, "保存成功") == 0, "notice text");
    steins0_ui_notice_hide(&s_ui);
    CHECK(s_ui.notice_box->hidden, "notice hidden");

    CHECK(host_lvgl_violations() == 0, "lock violations %d", host_lvgl_violations());
    if (failures) {
        fprintf(stderr, "FAIL %d checks\n", failures);
        return 1;
    }
    printf("PASS ui pages/rows/geometry/visibility\n");
    return 0;
}
