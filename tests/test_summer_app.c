// tests/test_summer_app.c —— production app/UI regression with thin LVGL/NVS stubs.
#include "summer_app.h"
#include "summer_ui.h"
#include "bsp_button.h"
#include "bsp_display.h"

#include "lvgl.h"
#include "nvs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const lv_font_t gal_cjk_16 = { 0 };

#ifndef SUMMER_PACK_PATH
#define SUMMER_PACK_PATH "main/summer_data/summer_pack.bin"
#endif

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        failures++; \
    } \
} while (0)

static uint8_t *read_all(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) {
        fclose(f);
        return NULL;
    }
    uint8_t *p = malloc((size_t)n);
    if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) {
        free(p);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return p;
}

static void key(summer_app_t *app, int btn, int ev)
{
    summer_key_t k = { (uint8_t)btn, (uint8_t)ev };
    summer_app_key(app, &k);
}

static void short_press(summer_app_t *app, int btn)
{
    key(app, btn, BSP_BTN_PRESS);
    key(app, btn, BSP_BTN_RELEASE);
    key(app, btn, BSP_BTN_CLICK);
}

static void long_press(summer_app_t *app, int btn)
{
    key(app, btn, BSP_BTN_PRESS);
    key(app, btn, BSP_BTN_LONG);
    key(app, btn, BSP_BTN_RELEASE);
    key(app, btn, BSP_BTN_CLICK);   // 驱动常补发一次 CLICK
}

static summer_app_t *g_app;

int main(void)
{
    size_t size = 0;
    uint8_t *blob = read_all(SUMMER_PACK_PATH, &size);
    if (!blob) {
        fprintf(stderr, "SKIP: cannot read %s\n", SUMMER_PACK_PATH);
        return 77;
    }
    stub_nvs_reset();
    static summer_app_t app;
    g_app = &app;
    CHECK(summer_app_init(&app, blob, (uint32_t)size), "init failed");
    CHECK(app.pack_ok, "pack failed");
    CHECK(app.page == APP_PAGE_TITLE, "title page expected, got %d", app.page);
    CHECK(app.title_count >= 1, "title rows %d", app.title_count);
    CHECK(app.title_action_ids[0] == SUMMER_ACT_START, "first title action %u",
          app.title_action_ids[0]);
    CHECK(stub_visible(app.ui.rows[0]), "title row 0 not visible");
    CHECK(strstr(stub_text(app.ui.row_text[0]), "开始阅读") != NULL, "title row text: %s",
          stub_text(app.ui.row_text[0]));

    // Start the story with an explicit short OK sequence.
    short_press(&app, BSP_BTN_OK);
    CHECK(app.page == APP_PAGE_GAME, "game page expected, got %d", app.page);
    CHECK(stub_visible(app.ui.text_label), "text label hidden");
    // 阅读页四硬指标：面板压到 176..240（64 高）、名字贴面板上沿且在框外、
    // 页码独占面板上方一行（不占正文高度）、正文 3 行 19px 末行不出 240。
    CHECK(app.ui.panel->y == SUMMER_PANEL_Y && app.ui.panel->h == SUMMER_PANEL_READ_H,
          "panel must be 176..240 (y=%d h=%d)", app.ui.panel->y, app.ui.panel->h);
    CHECK(app.ui.speaker_box->parent == app.ui.root &&
              app.ui.speaker_box->y + app.ui.speaker_box->h <= app.ui.panel->y,
          "speaker box must sit above the panel (y=%d h=%d)", app.ui.speaker_box->y,
          app.ui.speaker_box->h);
    CHECK(app.ui.speaker_box->w == LV_SIZE_CONTENT,
          "speaker box must hug its text (w=%d)", app.ui.speaker_box->w);
    CHECK(app.ui.page_label->parent == app.ui.root &&
              app.ui.page_label->y + app.ui.page_label->h <= app.ui.panel->y,
          "page label must sit just above the panel (y=%d h=%d)", app.ui.page_label->y,
          app.ui.page_label->h);
    CHECK(app.ui.text_label->y + app.ui.text_label->h <= SUMMER_PANEL_READ_H,
          "text bottom %d exceeds panel", app.ui.text_label->y + app.ui.text_label->h);
    CHECK(3 * 19 <= app.ui.text_label->h,
          "3 lines at 19px must fit the text box (h=%d)", app.ui.text_label->h);
    // 电量只显示数值，不再带“电量”两字。（LVGL 调用必须在锁内）
    bsp_lvgl_lock(-1);
    summer_ui_set_battery(&app.ui, 87);
    CHECK(strcmp(stub_text(app.ui.battery_label), "87%") == 0, "battery '%s'",
          stub_text(app.ui.battery_label));
    summer_ui_set_battery(&app.ui, -1);
    CHECK(strcmp(stub_text(app.ui.battery_label), "--") == 0, "battery unknown '%s'",
          stub_text(app.ui.battery_label));
    // CG 页：面板缩成 194..240 的字幕带，CG 可见区 0..194（81%），不再被 90 高的框压掉。
    summer_ui_show_page(&app.ui, SUMMER_PAGE_CG);
    CHECK(app.ui.panel->y == SUMMER_PANEL_CG_Y && app.ui.panel->h == SUMMER_PANEL_CG_H,
          "cg panel must be 194..240 (y=%d h=%d)", app.ui.panel->y, app.ui.panel->h);
    CHECK(app.ui.panel->y >= 194, "cg visible area must be >= 194px (y=%d)", app.ui.panel->y);
    summer_ui_show_page(&app.ui, SUMMER_PAGE_GAME);
    CHECK(app.ui.panel->y == SUMMER_PANEL_Y, "game panel restored (y=%d)", app.ui.panel->y);
    bsp_lvgl_unlock();
    summer_app_tick(&app, 500);
    CHECK(strlen(stub_text(app.ui.text_label)) > 0, "empty first text");
    CHECK(stub_text(app.ui.chapter_label)[0] != 0, "empty chapter label");

    const char *first = stub_text(app.ui.text_label);
    char first_copy[256];
    snprintf(first_copy, sizeof(first_copy), "%s", first);

    // Short UP: PRESS advances, the tail CLICK must be swallowed.
    key(&app, BSP_BTN_UP, BSP_BTN_PRESS);
    key(&app, BSP_BTN_UP, BSP_BTN_RELEASE);
    key(&app, BSP_BTN_UP, BSP_BTN_CLICK);
    CHECK(app.page == APP_PAGE_GAME, "page after UP %d", app.page);
    CHECK(strcmp(stub_text(app.ui.text_label), first_copy) != 0, "UP did not advance");

    // Long UP must not eat the next independent short UP.
    key(&app, BSP_BTN_UP, BSP_BTN_PRESS);
    key(&app, BSP_BTN_UP, BSP_BTN_LONG);
    key(&app, BSP_BTN_UP, BSP_BTN_RELEASE);
    CHECK(!app.ff, "ff still on after release");
    const char *before = stub_text(app.ui.text_label);
    char before_copy[256];
    snprintf(before_copy, sizeof(before_copy), "%s", before);
    key(&app, BSP_BTN_UP, BSP_BTN_PRESS);
    key(&app, BSP_BTN_UP, BSP_BTN_RELEASE);
    key(&app, BSP_BTN_UP, BSP_BTN_CLICK);
    CHECK(strcmp(stub_text(app.ui.text_label), before_copy) != 0,
          "next short UP after long press did not advance");

    // Menu round trip: OK opens the menu, long OK goes back to the same text.
    summer_app_tick(&app, 5000);
    CHECK(!app.typing, "typing did not finish");
    const char *game_text = stub_text(app.ui.text_label);
    char game_copy[256];
    snprintf(game_copy, sizeof(game_copy), "%s", game_text);
    // 短 OK = 下一句；长 OK = 进菜单。
    {
        char before2[256];
        snprintf(before2, sizeof(before2), "%s", stub_text(app.ui.text_label));
        short_press(&app, BSP_BTN_OK);
        CHECK(app.page == APP_PAGE_GAME, "short OK must stay in game, page %d", app.page);
        CHECK(strcmp(stub_text(app.ui.text_label), before2) != 0 || app.typing,
              "short OK must advance text");
        summer_app_tick(&app, 9000);
    }
    snprintf(game_copy, sizeof(game_copy), "%s", stub_text(app.ui.text_label));
    long_press(&app, BSP_BTN_OK);
    CHECK(app.page == APP_PAGE_MENU, "menu expected, got %d", app.page);
    CHECK(app.menu_sel == 0, "trailing click must not activate a row (sel=%d)", app.menu_sel);
    key(&app, BSP_BTN_OK, BSP_BTN_LONG);
    CHECK(app.page == APP_PAGE_GAME, "back to game expected, got %d", app.page);
    CHECK(strcmp(stub_text(app.ui.text_label), game_copy) == 0, "text changed in menu round trip");

    // Save to slot 1 (index 0) and verify the NVS blob exists.
    long_press(&app, BSP_BTN_OK);
    CHECK(app.page == APP_PAGE_MENU, "menu expected");
    // Move to "保存进度" (row 2).
    short_press(&app, BSP_BTN_DOWN);
    short_press(&app, BSP_BTN_DOWN);
    CHECK(app.menu_sel == 2, "menu sel %d", app.menu_sel);
    short_press(&app, BSP_BTN_OK);
    CHECK(app.page == APP_PAGE_SLOTS, "slots expected, got %d", app.page);
    CHECK(app.slot_sel == 0, "slot sel %d", app.slot_sel);
    short_press(&app, BSP_BTN_OK);
    CHECK(app.slot_meta[0].occupied, "slot 0 not occupied after save");
    CHECK(stub_nvs_key_count() >= 1, "nvs keys %d", stub_nvs_key_count());

    // Return to game; the notice must survive the redraw.
    if (app.page == APP_PAGE_MENU) {
        key(&app, BSP_BTN_OK, BSP_BTN_LONG);
    } else if (app.page == APP_PAGE_SLOTS) {
        key(&app, BSP_BTN_OK, BSP_BTN_LONG);
    }
    CHECK(app.page == APP_PAGE_GAME || app.page == APP_PAGE_CHOICE ||
          app.page == APP_PAGE_MENU, "unexpected page %d", app.page);

    // Open history with DOWN and back with long OK.
    short_press(&app, BSP_BTN_DOWN);
    if (app.page == APP_PAGE_HISTORY) {
        CHECK(app.history_count > 0, "history empty");
        key(&app, BSP_BTN_OK, BSP_BTN_LONG);
    }

    // Choice entry: advance until the first choice, verify rows and the
    // menu round trip preserves the pending choice and selection.
    for (int i = 0; i < 500 && app.page == APP_PAGE_GAME; ++i) short_press(&app, BSP_BTN_UP);
    CHECK(app.page == APP_PAGE_CHOICE, "choice page expected, got %d", app.page);
    CHECK(app.choice_count > 0, "choice count");
    CHECK(stub_visible(app.ui.rows[0]), "choice row hidden");
    CHECK(strlen(stub_text(app.ui.row_text[0])) > 0, "choice text empty");
    const int choice_before = app.choice_sel;
    short_press(&app, BSP_BTN_DOWN);
    const int choice_moved = app.choice_sel;
    CHECK(app.choice_count == 1 || choice_moved != choice_before, "choice selection did not move");
    key(&app, BSP_BTN_OK, BSP_BTN_LONG);
    CHECK(app.page == APP_PAGE_MENU, "choice menu expected");
    key(&app, BSP_BTN_OK, BSP_BTN_LONG);
    CHECK(app.page == APP_PAGE_CHOICE, "choice back expected");
    CHECK(app.choice_sel == choice_moved, "choice selection not preserved");
    short_press(&app, BSP_BTN_OK);
    CHECK(app.page != APP_PAGE_ERROR, "choice confirm error");

    // Save read failure: an occupied slot whose blob cannot be read must say so
    // (not "empty slot") and must keep the live VM unchanged.
    key(&app, BSP_BTN_OK, BSP_BTN_LONG);   // open the menu from game/choice/map
    if (app.page == APP_PAGE_MENU) {
        app.menu_sel = 3;                  // load
        short_press(&app, BSP_BTN_OK);
    }
    CHECK(app.page == APP_PAGE_SLOTS, "load slots expected, got %d", app.page);
    CHECK(app.slot_meta[0].occupied, "slot 0 should be occupied");
    const uint16_t before_scene = app.vm.scene_id;
    const uint32_t before_op = app.vm.op_index;
    stub_nvs_set_fail_get(1);
    short_press(&app, BSP_BTN_OK);
    CHECK(app.page == APP_PAGE_SLOTS, "read failure left the slots page (%d)", app.page);
    CHECK(strstr(app.notice, "存档读取失败") != NULL, "read failure notice: %s", app.notice);
    CHECK(strstr(app.notice, "还没有存档") == NULL, "read failure uses empty-slot text");
    CHECK(app.vm.scene_id == before_scene && app.vm.op_index == before_op,
          "read failure changed the VM");
    stub_nvs_set_fail_get(0);
    short_press(&app, BSP_BTN_OK);         // retry the occupied slot
    CHECK(app.page == APP_PAGE_GAME || app.page == APP_PAGE_CHOICE ||
          app.page == APP_PAGE_MAP || app.page == APP_PAGE_ENDING,
          "retry load page %d", app.page);
    CHECK(strstr(app.notice, "已读取存档") != NULL, "retry load notice: %s", app.notice);

    // Reload with get failure: the occupied slot stays visible as unreadable.
    key(&app, BSP_BTN_OK, BSP_BTN_LONG);
    if (app.page == APP_PAGE_MENU) {
        app.menu_sel = 3;
        stub_nvs_set_fail_get(1);
        short_press(&app, BSP_BTN_OK);
    }
    CHECK(app.page == APP_PAGE_SLOTS, "reload failure page %d", app.page);
    CHECK(app.slot_meta[0].occupied, "read-failed slot became empty");
    CHECK(!app.slot_meta[0].valid, "read-failed slot marked valid");
    CHECK(strstr(stub_text(app.ui.row_meta[0]), "存档无法读取") != NULL,
          "read-failed row text: %s", stub_text(app.ui.row_meta[0]));
    short_press(&app, BSP_BTN_OK);
    CHECK(app.page == APP_PAGE_SLOTS, "unreadable slot load page %d", app.page);
    CHECK(strstr(app.notice, "存档无法读取") != NULL, "unreadable notice: %s", app.notice);
    CHECK(strstr(app.notice, "还没有存档") == NULL, "unreadable slot uses empty-slot text");
    stub_nvs_set_fail_get(0);
    key(&app, BSP_BTN_OK, BSP_BTN_LONG);    // back to the source page

    // Load the saved slot back: open slots, select, load.
    size_t len = 0;
    uint8_t slot_buf[4096];
    CHECK(summer_store_read_slot(&app.store, 0, slot_buf, sizeof(slot_buf), &len),
          "slot read");
    CHECK(stub_lock_violations() == 0, "LVGL lock violations: %d", stub_lock_violations());

    free(blob);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_summer_app: PASS\n");
    return 0;
}
