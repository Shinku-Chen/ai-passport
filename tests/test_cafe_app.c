// tests/test_cafe_app.c —— 横屏应用状态机：短按推进 / 长按菜单 / 自动 / 快进 / 选择点。
#include "cafe_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #cond); return 1; } } while (0)

static unsigned char *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f); *size = (size_t)n; return b;
}

static void tap(cafe_app_t *app, uint8_t key, uint32_t t)
{
    cafe_app_key(app, key, true, t);
    cafe_app_key(app, key, false, t + 40);
}

static void hold(cafe_app_t *app, uint8_t key, uint32_t t)
{
    cafe_app_key(app, key, true, t);
    cafe_app_tick(app, t + 520, 520);
}

int main(int argc, char **argv)
{
    (void)argc;
    size_t n = 0;
    unsigned char *pack = read_file(argv[1], &n);
    CHECK(pack && cafe_pack_set_data(pack, n));
    cafe_ui_t ui;
    memset(&ui, 0, sizeof(ui));
    cafe_bands_t bands;
    memset(&bands, 0, sizeof(bands));
    cafe_app_t app;
    CHECK(cafe_app_init(&app, &ui, &bands));
    app.now_ms = 1000;
    app.last_activity_ms = 1000;

    // 启动页 -> 标题 -> 新游戏。
    tap(&app, CAFE_KEY_OK, 1000);
    CHECK(app.page == CAFE_PAGE_TITLE);
    // A long direction must still move one row, not be swallowed by long OK-only menus.
    hold(&app, CAFE_KEY_DOWN, 1020);
    CHECK(app.title_sel == 1);
    CHECK(ui.rows_selected == 1);
    cafe_app_key(&app, CAFE_KEY_DOWN, false, 1540);
    hold(&app, CAFE_KEY_UP, 1560);
    CHECK(app.title_sel == 0);
    CHECK(ui.rows_selected == 0);
    cafe_app_key(&app, CAFE_KEY_UP, false, 2080);
    tap(&app, CAFE_KEY_OK, 1100);
    CHECK(app.page == CAFE_PAGE_GAME);
    CHECK(app.vm.node == 2 && app.vm.index == 0);
    CHECK(ui.text[0] != '\0');

    // 短按三键：即使正在打字，也立即推进到下一句/下一分页。
    CHECK(ui.typing);
    uint16_t before = app.vm.index;
    uint16_t before_page = app.frame_page;
    tap(&app, CAFE_KEY_OK, 1200);
    CHECK(app.vm.index != before || app.frame_page != before_page);

    cafe_ui_show_full_text(&ui);
    before = app.vm.index;
    before_page = app.frame_page;
    tap(&app, CAFE_KEY_UP, 1400);
    CHECK(app.vm.index != before || app.frame_page != before_page);

    cafe_ui_show_full_text(&ui);
    before = app.vm.index;
    before_page = app.frame_page;
    tap(&app, CAFE_KEY_DOWN, 1500);
    CHECK(app.vm.index != before || app.frame_page != before_page);

    // 长按 OK 进入菜单；松手不额外当短按。
    hold(&app, CAFE_KEY_OK, 2000);
    CHECK(app.page == CAFE_PAGE_MENU);
    cafe_app_key(&app, CAFE_KEY_OK, false, 2600);
    CHECK(app.page == CAFE_PAGE_MENU);
    hold(&app, CAFE_KEY_OK, 2700);
    CHECK(app.page == CAFE_PAGE_GAME);
    cafe_app_key(&app, CAFE_KEY_OK, false, 3300);
    CHECK(app.page == CAFE_PAGE_GAME);

    // 长按 DOWN 打开自动；自动间隔到点推进。
    hold(&app, CAFE_KEY_DOWN, 4000);
    CHECK(app.auto_mode);
    cafe_app_key(&app, CAFE_KEY_DOWN, false, 4600);
    cafe_ui_show_full_text(&ui);
    const uint16_t auto_before = app.vm.index;
    const uint16_t auto_page = app.frame_page;
    cafe_app_tick(&app, 5000, 1000);
    CHECK(app.vm.index != auto_before || app.frame_page != auto_page);
    // 自动中手动短按：先退出自动，同时仍然推进。
    CHECK(app.auto_mode);
    const uint16_t manual_before = app.vm.index;
    const uint16_t manual_page = app.frame_page;
    tap(&app, CAFE_KEY_DOWN, 5100);
    CHECK(!app.auto_mode);
    CHECK(app.vm.index != manual_before || app.frame_page != manual_page);

    // 长按 UP：不等打字/自动间隔，持续快进；松手立即停。
    cafe_ui_set_text(&ui, "测试", "长按快进应跳过打字", 30);
    CHECK(ui.typing);
    const uint16_t ff_before = app.vm.index;
    cafe_app_key(&app, CAFE_KEY_UP, true, 6000);
    cafe_app_tick(&app, 6520, 520);   // threshold tick: one sentence immediately
    CHECK(app.fast_forward);
    CHECK(app.vm.index != ff_before || app.frame_page == 0);
    cafe_app_tick(&app, 6580, 60);    // one more input tick, one more sentence
    CHECK(app.vm.index != ff_before || app.frame_page != auto_page);
    cafe_app_key(&app, CAFE_KEY_UP, false, 6600);
    CHECK(!app.fast_forward);
    const uint16_t stopped = app.vm.index;
    cafe_app_tick(&app, 6700, 100);
    CHECK(app.vm.index == stopped);

    // 快进到选择点停住：从 n6 最后一帧开始 continue。
    cafe_save_t save;
    memset(&save, 0, sizeof(save));
    save.occupied = true;
    save.node = 6;
    save.index = 441;
    save.progression = 0;
    save.last_complete = 0xFF;
    snprintf(save.chapter, sizeof(save.chapter), "chapter 1-1");
    CHECK(cafe_store_save_auto(&save));
    CHECK(cafe_app_init(&app, &ui, &bands));
    app.now_ms = 8000;
    app.last_activity_ms = 8000;
    tap(&app, CAFE_KEY_OK, 8000);
    CHECK(app.page == CAFE_PAGE_TITLE);
    app.title_sel = 1;
    tap(&app, CAFE_KEY_OK, 8100);
    CHECK(app.page == CAFE_PAGE_GAME);
    cafe_ui_show_full_text(&ui);
    cafe_app_key(&app, CAFE_KEY_UP, true, 9000);
    cafe_app_tick(&app, 9520, 520);
    CHECK(app.fast_forward);
    for (int i = 0; i < 20 && app.page == CAFE_PAGE_GAME; ++i) {
        cafe_app_tick(&app, 9580 + (uint32_t)i * 60, 60);
    }
    CHECK(app.page == CAFE_PAGE_CHOICE);
    CHECK(app.choice_count == 2);
    CHECK(!app.fast_forward);
    cafe_app_key(&app, CAFE_KEY_UP, false, 11000);

    // 从选项页一次长按 OK 直接进入菜单；再从菜单长按返回完整选项页。
    const uint16_t choice_node = app.vm.node;
    const uint16_t choice_index = app.vm.index;
    const int choice_sel = app.choice_sel;
    hold(&app, CAFE_KEY_OK, 11500);
    CHECK(app.page == CAFE_PAGE_MENU);
    CHECK(app.vm.node == choice_node && app.vm.index == choice_index);
    hold(&app, CAFE_KEY_DOWN, 11520);
    CHECK(app.menu_sel == 1);
    CHECK(ui.rows_selected == 1);
    cafe_app_key(&app, CAFE_KEY_DOWN, false, 12040);
    hold(&app, CAFE_KEY_UP, 12060);
    CHECK(app.menu_sel == 0);
    CHECK(ui.rows_selected == 0);
    cafe_app_key(&app, CAFE_KEY_UP, false, 12580);
    cafe_app_key(&app, CAFE_KEY_OK, false, 12100);
    hold(&app, CAFE_KEY_OK, 12200);
    CHECK(app.page == CAFE_PAGE_CHOICE);
    CHECK(app.vm.node == choice_node && app.vm.index == choice_index);
    CHECK(app.choice_sel == choice_sel);
    cafe_app_key(&app, CAFE_KEY_OK, false, 12800);

    free(pack);
    printf("cafe app input/state PASS\n");
    return 0;
}
