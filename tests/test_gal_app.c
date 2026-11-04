// tests/test_gal_app.c —— 真实 gal_app.c + gal_ui.c 的按键/页面回归(LVGL 薄桩)。
//
// 覆盖 Lead 指出的调用路径:选择点打开菜单、返回后选项仍在、选择点存档/读档、
// toast 在其它页面可见、历史行布局与返回来源、快进跨页取消、菜单行数。
#include "gal_app.h"

#include "gal_ui.h"
#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        failures++; \
    } \
} while (0)

static uint16_t g_art[320 * 150];
static uint16_t g_strip[160 * 45];
static uint8_t g_scratch[96 * 150 * 2];
static gal_app_t g_app;

static void key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    const gal_key_t k = { (uint8_t)btn, (uint8_t)ev };
    gal_app_key(&g_app, &k);
}

static void click(bsp_btn_t b) { key(b, BSP_BTN_CLICK); }

// 真实组件事件顺序:PRESS -> RELEASE -> CLICK。UP 的推进在 PRESS 上。
static void tap(bsp_btn_t b)
{
    key(b, BSP_BTN_PRESS);
    key(b, BSP_BTN_RELEASE);
    key(b, BSP_BTN_CLICK);
}
static void press(bsp_btn_t b) { key(b, BSP_BTN_PRESS); }
static void release(bsp_btn_t b) { key(b, BSP_BTN_RELEASE); }
static void long_press(bsp_btn_t b) { key(b, BSP_BTN_LONG); }

static bool choice_visible(void)
{
    const bool pending = g_app.choice_pending;
    const bool vis = g_app.ui.choice_box && lv_stub_visible(g_app.ui.choice_box);
    return pending && vis;
}

static void advance_to_choice(void)
{
    for (int i = 0; i < 6000 && !g_app.choice_pending; ++i) {
        tap(BSP_BTN_UP);     // 先补全打字,再下一页
        tap(BSP_BTN_UP);
    }
}

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "main/gal_data/gal_pack.bin";
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
    fseek(f, 0, SEEK_END);
    const long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *blob = malloc((size_t)len);
    if (!blob || fread(blob, 1, (size_t)len, f) != (size_t)len) return 2;
    fclose(f);

    lv_stub_reset();
    // 0) 全局解锁:先写入独立记录,app_init 必须把它加载进来(而不是全 0 启动)。
    CHECK(gal_store_init(), "store init for seeding");
    {
        gal_unlock_t seed;
        gal_unlock_clear(&seed);
        gal_unlock_cg_set(&seed, 32);
        gal_unlock_chapter_set(&seed, 17);
        gal_unlock_ending_set(&seed, 3);
        CHECK(gal_store_save_unlock(&seed), "seed global unlock");
    }
    const gal_app_buffers_t bufs = {
        .art_px = g_art, .strip_px = g_strip,
        .scratch = g_scratch, .scratch_size = sizeof(g_scratch),
    };
    CHECK(gal_app_init(&g_app, blob, (uint32_t)len, &bufs), "app init");
    CHECK(gal_unlock_cg_get(&g_app.unlock, 32), "init restores CG unlock from store");
    CHECK(gal_unlock_chapter_get(&g_app.unlock, 17), "init restores chapter unlock");
    CHECK(gal_unlock_ending_get(&g_app.unlock, 3), "init restores ending unlock");
    CHECK(g_app.page == GAL_PAGE_TITLE, "start page %d", (int)g_app.page);

    // 1) 标题菜单:5 行都建出来,继续游戏置灰(无存档)时也能移动
    CHECK(g_app.ui.title_menu_text[4] != NULL, "title menu rows");
    CHECK(g_app.ui.notice != NULL && !lv_stub_visible(g_app.ui.notice), "toast hidden at start");

    // 2) 开始新游戏 -> 正文页
    //   title_sel 0=继续 1=新游戏
    click(BSP_BTN_DOWN);
    click(BSP_BTN_OK);
    CHECK(g_app.page == GAL_PAGE_GAME, "new game page %d", (int)g_app.page);
    // 阅读页四硬指标：名字在框外、页码在面板外右上、正文贴顶 +4 且末行不出框。
    CHECK(g_app.ui.name->y + 20 <= GAL_BOX_Y, "speaker must sit above the box");
    CHECK(g_app.ui.page_label != NULL &&
              g_app.ui.page_label->y + 18 <= GAL_BOX_Y,
          "page label must sit above the box (y=%d)", (int)g_app.ui.page_label->y);
    CHECK(g_app.ui.text->y == 4, "body must start at box_top+4 (y=%d)", (int)g_app.ui.text->y);
    CHECK(g_app.ui.text->y + 4 * 19 <= GAL_BOX_H, "last text line must stay inside the box");

    // 3) 推进到第一个选择点
    advance_to_choice();
    CHECK(g_app.choice_pending, "choice should appear");
    CHECK(choice_visible(), "choice box visible on game page");

    // 4) 选择点长按 OK 打开菜单;返回后选项仍在(且同一组)
    const uint16_t group = g_app.choice_group;
    const int count = g_app.choice_count;
    long_press(BSP_BTN_OK);
    CHECK(g_app.page == GAL_PAGE_MENU, "menu from choice page %d", (int)g_app.page);
    CHECK(lv_stub_child_count(g_app.ui.menu.page) >= 6, "menu rows %d",
          lv_stub_child_count(g_app.ui.menu.page));
    long_press(BSP_BTN_OK);
    CHECK(g_app.page == GAL_PAGE_GAME, "back to game page %d", (int)g_app.page);
    CHECK(choice_visible(), "choice still visible after menu");
    CHECK(g_app.choice_group == group && g_app.choice_count == count, "same choice group");

    // 5) 选择点存档:菜单 -> 保存进度 -> 槽 1 -> toast 在 SLOTS 页可见
    long_press(BSP_BTN_OK);
    click(BSP_BTN_DOWN);            // 保存进度(第 2 行)
    click(BSP_BTN_OK);
    CHECK(g_app.page == GAL_PAGE_SLOTS, "slots page %d", (int)g_app.page);
    click(BSP_BTN_OK);              // 空槽直接保存
    CHECK(g_app.ui.notice != NULL, "notice object");
    CHECK(lv_stub_visible(g_app.ui.notice), "toast visible on slots page (text '%s')",
          g_app.ui.notice && g_app.ui.notice->text ? g_app.ui.notice->text : "");
    CHECK(g_app.ui.notice->text && strstr(g_app.ui.notice->text, "已保存") != NULL,
          "toast text '%s'", g_app.ui.notice && g_app.ui.notice->text ? g_app.ui.notice->text : "");
    long_press(BSP_BTN_OK);         // 回菜单
    CHECK(g_app.page == GAL_PAGE_MENU, "slots back to menu %d", (int)g_app.page);
    long_press(BSP_BTN_OK);         // 回剧情
    CHECK(g_app.page == GAL_PAGE_GAME, "menu back to game");
    CHECK(choice_visible(), "choice still there");

    // 6) 确认选项后才离开选择点
    const uint16_t before_pc = g_app.play.pc;
    click(BSP_BTN_OK);
    CHECK(!g_app.choice_pending, "choice resolved");
    CHECK(g_app.play.pc != before_pc, "pc moved after choose (%u)",
          (unsigned)g_app.play.pc);

    // 7) 历史:正文页 DOWN -> 历史;行不重叠;OK 回正文且不推动剧情
    const uint16_t history_pc = g_app.play.pc;
    click(BSP_BTN_DOWN);
    CHECK(g_app.page == GAL_PAGE_HISTORY, "history page %d", (int)g_app.page);
    CHECK(g_app.ui.history_count > 0, "history entries %d", g_app.ui.history_count);
    if (g_app.ui.history_count >= 2) {
        const int32_t y0 = g_app.ui.history_rows[0]->y;
        const int32_t y1 = g_app.ui.history_rows[1]->y;
        CHECK(y1 - y0 == 20, "history row step %d", (int)(y1 - y0));
        CHECK(g_app.ui.history_rows[0]->h == 20, "history row height %d",
              (int)g_app.ui.history_rows[0]->h);
        CHECK(lv_stub_visible(g_app.ui.history_rows[1]), "history row visible");
    }
    click(BSP_BTN_OK);
    CHECK(g_app.page == GAL_PAGE_GAME, "history back to game %d", (int)g_app.page);
    CHECK(g_app.play.pc == history_pc, "history does not move the script");

    // 8) 快进跨页取消:PRESS UP -> LONG UP -> DOWN(历史) -> RELEASE UP
    press(BSP_BTN_UP);
    long_press(BSP_BTN_UP);
    CHECK(g_app.fast_forward, "fast forward on");
    CHECK(!gal_ui_typing(&g_app.ui), "fast forward shows the whole line");
    click(BSP_BTN_DOWN);
    CHECK(g_app.page == GAL_PAGE_HISTORY, "history from ff");
    release(BSP_BTN_UP);
    CHECK(!g_app.fast_forward, "fast forward released on history page");
    click(BSP_BTN_OK);
    CHECK(g_app.page == GAL_PAGE_GAME, "back to game after ff");
    for (int i = 0; i < 10; ++i) gal_app_tick(&g_app, 50);
    CHECK(!g_app.fast_forward, "fast forward stays off");

    // 8b) 点按尾部隔离:PRESS 打开选项后,同一次点按的 RELEASE/CLICK 不得改选择
    {
        // 推进到下一个选项停点;产生选项的那一次点按,尾部事件留到下面验证。
        int guard = 0;
        while (!g_app.choice_pending && g_app.page == GAL_PAGE_GAME && guard++ < 8000) {
            key(BSP_BTN_UP, BSP_BTN_PRESS);
            if (g_app.choice_pending) break;
            key(BSP_BTN_UP, BSP_BTN_RELEASE);
            key(BSP_BTN_UP, BSP_BTN_CLICK);
        }
        CHECK(g_app.choice_pending, "back at a choice for tap-tail test");
        const int sel_before = g_app.choice_sel;
        key(BSP_BTN_UP, BSP_BTN_RELEASE);   // 同一次点按的尾部
        key(BSP_BTN_UP, BSP_BTN_CLICK);
        CHECK(g_app.choice_sel == sel_before, "tap tail CLICK does not move selection");
    }

    // 8c) 正文 -> 菜单 -> 回到标题;标题 -> 读取存档 -> 长按 OK 必须回标题
    {
        long_press(BSP_BTN_OK);                     // 打开菜单(选择点也可以)
        CHECK(g_app.page == GAL_PAGE_MENU, "menu for title test");
        for (int i = 0; i < 5; ++i) click(BSP_BTN_DOWN);   // 回到标题
        click(BSP_BTN_OK);
        CHECK(g_app.page == GAL_PAGE_TITLE, "menu -> title");
        tap(BSP_BTN_DOWN);
        tap(BSP_BTN_DOWN);                          // 读取存档
        tap(BSP_BTN_OK);
        CHECK(g_app.page == GAL_PAGE_SLOTS, "title opens slots");
        long_press(BSP_BTN_OK);
        CHECK(g_app.page == GAL_PAGE_TITLE, "slots long-OK returns to title origin");
    }

    // 8d) 历史只展示最近 48 条,最后一行是最新一句
    {
        static uint16_t many[64];
        for (int i = 0; i < 64; ++i) many[i] = (uint16_t)i;
        gal_ui_set_history(&g_app.ui, &g_app.pack, many, 64);
        CHECK(g_app.ui.history_count == 48, "history window %d", g_app.ui.history_count);
        const lv_obj_t *last = g_app.ui.history_rows[47];
        CHECK(last && last->first_child && last->first_child->text &&
              strstr(last->first_child->text, gal_line_text(&g_app.pack, 63)) != NULL,
              "history keeps latest line");
        CHECK(last && last->y == 47 * 20, "history last row y=%d", last ? (int)last->y : -1);
    }

    // 9) 读档回到选择点:菜单 -> 读取进度 -> 槽 1
    long_press(BSP_BTN_OK);
    click(BSP_BTN_DOWN);
    click(BSP_BTN_DOWN);            // 读取进度(第 3 行)
    click(BSP_BTN_OK);
    CHECK(g_app.page == GAL_PAGE_SLOTS, "load slots page %d", (int)g_app.page);
    click(BSP_BTN_OK);              // 读取槽 1
    CHECK(g_app.page == GAL_PAGE_GAME, "loaded into game");
    CHECK(g_app.choice_pending, "restored at pending choice");
    CHECK(choice_visible(), "restored choice visible");

    // 10) 全局解锁合并持久化全链路:
    //     全局记录为空 + 槽位带 CG31 -> 读档合并 -> dirty -> flush -> 重启仍恢复;
    //     用同样的位再读一次不得再次置脏。
    {
        gal_unlock_t empty;
        gal_unlock_clear(&empty);
        CHECK(gal_store_save_unlock(&empty), "reset global unlock record");

        uint16_t line_ev = 0;
        while (line_ev < g_app.pack.event_count &&
               g_app.pack.events[line_ev].kind != GAL_EV_LINE) {
            ++line_ev;
        }
        gal_save_t save;
        memset(&save, 0, sizeof(save));
        save.pc = line_ev;
        save.bg = GAL_NONE;
        save.cg = GAL_NONE;
        save.spr[0] = save.spr[1] = save.spr[2] = GAL_NONE;
        gal_unlock_cg_set(&save.unlock, 31);
        snprintf(save.preview, sizeof(save.preview), "merge test");

        CHECK(gal_store_save_slot(0, &save), "slot 0 with CG31");

        CHECK(gal_app_init(&g_app, blob, (uint32_t)len, &bufs), "re-init after reset");
        CHECK(!gal_unlock_cg_get(&g_app.unlock, 31), "fresh global lacks CG31");
        CHECK(!g_app.unlock_dirty, "clean at boot");

        tap(BSP_BTN_DOWN);              // 读取存档
        tap(BSP_BTN_DOWN);
        tap(BSP_BTN_OK);
        CHECK(g_app.page == GAL_PAGE_SLOTS, "load slots for merge");
        tap(BSP_BTN_OK);                // 载入槽 1
        CHECK(g_app.page == GAL_PAGE_GAME, "merged load returns to game");
        CHECK(gal_unlock_cg_get(&g_app.unlock, 31), "merge brought CG31 into session");
        CHECK(g_app.unlock_dirty, "merge marks the global record dirty");

        gal_app_flush(&g_app);
        CHECK(!g_app.unlock_dirty, "flush persists and clears dirty");

        CHECK(gal_app_init(&g_app, blob, (uint32_t)len, &bufs), "re-init after flush");
        CHECK(gal_unlock_cg_get(&g_app.unlock, 31), "CG31 survived restart");
        CHECK(!g_app.unlock_dirty, "clean after restart");

        tap(BSP_BTN_DOWN);              // 再次读同一槽
        tap(BSP_BTN_DOWN);
        tap(BSP_BTN_OK);
        tap(BSP_BTN_OK);
        CHECK(gal_unlock_cg_get(&g_app.unlock, 31), "still unlocked");
        CHECK(!g_app.unlock_dirty, "loading the same bits does not dirty");
    }

    // ---------------------------------------------------------------- S3-3 存档光标
    // 保存成功后光标必须留在刚写的槽位（之前 slots_refresh 无条件归零）。
    CHECK(gal_store_erase_slot(2), "erase slot 3 for the cursor case");
    CHECK(gal_app_init(&g_app, blob, (uint32_t)len, &bufs), "re-init for the slot cursor");
    tap(BSP_BTN_DOWN);              // 标题第 2 项：开始新游戏
    tap(BSP_BTN_OK);
    CHECK(g_app.page == GAL_PAGE_GAME, "new game for the slot cursor");
    // 开场可能有章节过场：先短按关过场，再用长 OK 进菜单（短 OK 现在是“下一句”）。
    for (int i = 0; i < 4 && g_app.page != GAL_PAGE_GAME; i++) tap(BSP_BTN_OK);
    if (g_app.page == GAL_PAGE_GAME) long_press(BSP_BTN_OK);
    CHECK(g_app.page == GAL_PAGE_MENU, "menu for the slot cursor (got %d)", (int)g_app.page);
    // 菜单光标移到“保存进度”(索引 1)，不假设初始值。
    for (int i = 0; i < 8 && g_app.menu_sel != 1; i++) tap(BSP_BTN_DOWN);
    CHECK(g_app.menu_sel == 1, "menu cursor moved to save row (sel=%d)", g_app.menu_sel);
    tap(BSP_BTN_OK);
    CHECK(g_app.page == GAL_PAGE_SLOTS, "save page for the slot cursor (got %d, sel=%d)",
          (int)g_app.page, g_app.slots_sel);
    CHECK(g_app.slots_saving, "slots page is in save mode");
    CHECK(g_app.slots_sel == 0, "a freshly opened save page starts at slot 1");
    tap(BSP_BTN_DOWN);
    tap(BSP_BTN_DOWN);              // 存档 3
    CHECK(g_app.slots_sel == 2, "cursor moved to slot 3 (got %d)", g_app.slots_sel);
    tap(BSP_BTN_OK);                // 空槽 -> 直接保存
    gal_save_t cursor_save;
    CHECK(gal_store_load_slot(2, &cursor_save), "slot 3 saved");
    CHECK(g_app.page == GAL_PAGE_SLOTS, "still on the save page after saving");
    CHECK(g_app.slots_sel == 2, "cursor stays on the slot just written (got %d)",
          g_app.slots_sel);

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("app flows ok\n");
    free(blob);
    return 0;
}
