// tests/test_gal_choice_ff.c —— 快进遇到选项 / 长按释放 / 分支标记的 host 回归。
//
// 使用 tests/gal_app_stubs/gal_content_choice.c 的最小分支内容驱动生产的
// gal_app.c：本作本身没有选项，但框架必须保证“快进到需要确认的界面就停住，
// 不替玩家选”，并且松手后不补发一次短按。
#include "gal_app.h"
#include "gal_content.h"
#include "bsp_button.h"
#include "lvgl.h"

#include <stdio.h>

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { failures++; printf("FAIL: %s\n", msg); } } while (0)

static void key(gal_app_t *app, int btn, int ev)
{
    gal_key_t k = { (uint8_t)btn, (uint8_t)ev };
    gal_app_key(app, &k);
}
static void press(gal_app_t *app, int btn) { key(app, btn, BSP_BTN_CLICK); }
static void hold(gal_app_t *app, int btn) { key(app, btn, BSP_BTN_LONG); }
static void release(gal_app_t *app, int btn) { key(app, btn, BSP_BTN_RELEASE); }

int main(void)
{
    gal_app_t app;
    CHECK(gal_app_init(&app), "app init");
    press(&app, BSP_BTN_OK);
    app.title_sel = GAL_TITLE_MENU_NEW;
    press(&app, BSP_BTN_OK);
    CHECK(app.page == GAL_PAGE_GAME, "new game enters the reader");

    // 长按上：立即推进一句，然后按住按节拍推进，直到选项页停下。
    const int base = app.event_index;
    hold(&app, BSP_BTN_UP);
    CHECK(app.fast_forward, "long UP starts fast-forward");
    CHECK(app.event_index == base + 1, "long UP advances immediately");
    release(&app, BSP_BTN_UP);
    CHECK(!app.fast_forward, "release stops fast-forward");
    const int after_release = app.event_index;
    gal_app_tick(&app, 5000);
    CHECK(app.event_index == after_release, "release does not leave a queued advance");

    // 再按一次进入选项。
    press(&app, BSP_BTN_OK);
    CHECK(app.page == GAL_PAGE_CHOICE, "reader reaches the choice page");
    CHECK(app.event_index == 2, "choice event index");

    // 选项页长按上不应启动快进，也不应替玩家选择。
    const int choice_event = app.event_index;
    hold(&app, BSP_BTN_UP);
    gal_app_tick(&app, 500);
    CHECK(app.page == GAL_PAGE_CHOICE && app.event_index == choice_event,
          "fast-forward does not run on the choice page");
    release(&app, BSP_BTN_UP);

    // 上下移动、确定选择 A（写入标记 1），随后推进到 A 专属台词。
    press(&app, BSP_BTN_DOWN);
    CHECK(app.choice_sel == 1, "DOWN moves the choice cursor");
    press(&app, BSP_BTN_UP);
    CHECK(app.choice_sel == 0, "UP moves the choice cursor back");
    press(&app, BSP_BTN_OK);
    CHECK(app.page == GAL_PAGE_GAME, "choosing returns to the reader");
    CHECK((app.flags & 2u) != 0 && (app.flags & 1u) == 0, "option A writes run flag 1 (bit 1)");
    CHECK(app.event_index == 3, "option A reaches its exclusive line");

    // 完整读到结局：过场 + 结局，run_mask 只有一章，够满（本测试内容一章）。
    hold(&app, BSP_BTN_UP);
    for (int i = 0; i < 20 && app.page != GAL_PAGE_ENDING; ++i) gal_app_tick(&app, 90);
    release(&app, BSP_BTN_UP);
    CHECK(app.page == GAL_PAGE_ENDING, "fast-forward stops at the ending card");
    CHECK(!app.fast_forward, "ending clears fast-forward");
    CHECK(gal_unlock_has_any_ending(&app.unlock), "single-chapter run grants its ending");

    if (failures) {
        printf("FAIL: %d checks\n", failures);
        return 1;
    }
    printf("PASS: gal choice/fast-forward\n");
    return 0;
}
