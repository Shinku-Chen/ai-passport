// tests/test_gal_app_flow.c —— 本作内容表与启动链的 host 回归。
//
// 内容由 tools/maitetsu_assets.py 生成；这里独立检查表的形状与启动流程：
// 1,537 段对白、四章、一个结局、35 张 CG、6 段回想、1 首标题曲，以及
// 章节过场位置与事件下标的一致性。更细的按键/快进/试读行为在
// tests/test_gal_keys.c 与 tests/test_gal_choice_ff.c。
#include "gal_app.h"
#include "gal_content.h"
#include "bsp_button.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { failures++; printf("FAIL: %s\n", msg); } } while (0)

static void key(gal_app_t *app, int btn, int ev)
{
    gal_key_t k = { (uint8_t)btn, (uint8_t)ev };
    gal_app_key(app, &k);
}

static void test_tables(void)
{
    CHECK(gal_content_line_count == 1537, "1537 paragraphs");
    CHECK(gal_content_event_count == 1542, "4 transitions + 1537 lines + 1 ending");
    CHECK(gal_content_transition_count == 4, "four chapters");
    CHECK(gal_content_ending_count == 1, "one ending");
    CHECK(gal_content_cg_count == 35, "35 gallery CGs");
    CHECK(gal_content_chapter_count == 4, "four chapter entries");
    CHECK(gal_content_recall_count == 6, "six recall entries");
    CHECK(gal_content_track_count == 1, "one title track");
    CHECK(gal_content_choice_count == 0, "linear story has no choices");

    const int chapter_events[] = { 0, 355, 701, 1070 };
    int transitions = 0;
    int lines = 0;
    for (int i = 0; i < gal_content_event_count; ++i) {
        const gal_event_t *ev = &gal_content_events[i];
        if (ev->kind == GAL_EV_LINE) {
            CHECK(ev->index < gal_content_line_count, "line index in range");
            lines++;
        } else if (ev->kind == GAL_EV_TRANSITION) {
            CHECK(ev->index < gal_content_transition_count, "transition index in range");
            if (transitions < 4) {
                CHECK(i == chapter_events[transitions], "chapter transition position");
            }
            transitions++;
        } else if (ev->kind == GAL_EV_ENDING) {
            CHECK(i == gal_content_event_count - 1, "ending is the last event");
        } else {
            CHECK(0, "no choice events in this title");
        }
    }
    CHECK(lines == gal_content_line_count, "every line is in the event stream");
    CHECK(transitions == 4, "four chapter transitions");

    for (int i = 0; i < gal_content_chapter_count; ++i) {
        CHECK(gal_content_chapters[i].unlock_event == chapter_events[i],
              "chapter entry event matches its transition");
        CHECK(gal_content_chapters[i].name[0] != '\0', "chapter has a name");
    }
    for (int i = 0; i < gal_content_line_count; ++i) {
        const gal_line_t *line = &gal_content_lines[i];
        CHECK(line->text && line->text[0] != '\0', "line has text");
        CHECK(line->scene == GAL_SCENE_NONE || line->scene < 675, "line scene in range");
        CHECK(line->require_flag == GAL_UNLOCK_NONE, "linear story has no conditional lines");
    }
    CHECK(strcmp(gal_content_lines[0].speaker, "赤井") == 0, "first speaker");
    CHECK(strstr(gal_content_lines[0].text, "哪位") != NULL, "first paragraph");
    CHECK(strcmp(gal_content_lines[1537 - 1].speaker, "二六") == 0 ||
              gal_content_lines[1537 - 1].speaker[0] == '\0',
          "last paragraph speaker");
    for (int i = 0; i < gal_content_cg_count; ++i) {
        CHECK(gal_content_cgs[i].unlock_event == GAL_EVENT_NONE,
              "gallery is open from the start");
        CHECK(gal_content_cgs[i].caption[0] != '\0', "CG has a caption");
    }
}

static void test_boot_flow(void)
{
    gal_app_t app;
    CHECK(gal_app_init(&app), "app init");
    CHECK(app.page == GAL_PAGE_START, "starts at the start page");
    key(&app, BSP_BTN_OK, BSP_BTN_CLICK);
    CHECK(app.page == GAL_PAGE_TITLE, "start -> title");
    CHECK(!app.progress.valid, "no automatic progress on a fresh device");

    app.title_sel = GAL_TITLE_MENU_NEW;
    key(&app, BSP_BTN_OK, BSP_BTN_CLICK);
    CHECK(app.page == GAL_PAGE_TRANSITION, "new game -> first chapter card");
    key(&app, BSP_BTN_OK, BSP_BTN_CLICK);
    CHECK(app.page == GAL_PAGE_GAME, "chapter card -> reader");
    CHECK(app.event_index == 1, "reader starts at the first dialogue");
    CHECK(app.shown_line == 0, "first line rendered");

    // 第四章之前不应解锁后面章节。
    CHECK(gal_content_chapter_count == 4, "four chapters");
    for (int i = 1; i < 4; ++i) {
        CHECK(!gal_unlock_has_chapter(&app.unlock, (uint8_t)i), "later chapter locked");
    }
    CHECK(gal_unlock_has_chapter(&app.unlock, 0), "first chapter unlocked on entry");
}

int main(void)
{
    test_tables();
    test_boot_flow();
    if (failures) {
        printf("FAIL: %d checks\n", failures);
        return 1;
    }
    printf("PASS: gal content/app flow\n");
    return 0;
}
