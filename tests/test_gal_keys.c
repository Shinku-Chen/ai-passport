// tests/test_gal_keys.c —— 三键语义与快进/自动状态机的 host 回归。
//
// 用真实生成的 gal_content.c（1,537 段、四章、一个结局）驱动生产状态机，
// 时间完全由 gal_app_tick(elapsed_ms) 控制，不依赖真实定时器：
//   · 阅读页短按 OK / 上 / 下 都推进一句
//   · 长按 OK 进菜单，菜单长按 OK 返回
//   · 长按下切换自动；自动开启时短按仍然推进，不被吞掉
//   · 长按上立即推进（不等打字机、不等自动间隔），按住按节拍持续快进，松手即停
//   · 长按只触发长按动作，松手不再补一次短按
//   · 切页清理快进/自动状态
//   · 章节试读不能授予永久通关；完整读完全部四章才记结局
#include "gal_app.h"
#include "gal_content.h"
#include "gal_text.h"
#include "bsp_button.h"
#include "lvgl.h"
#include "nvs.h"

#include <stdio.h>
#include <string.h>

// 定义在 tests/gal_app_stubs/gal_app_hal_stub.c
extern int g_media_init_result;
extern int g_media_faulted;
extern int g_lock_failures_left;
extern int g_typing_active;
extern int g_show_full_text_calls;
extern char g_last_asset_error[96];

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

static void tick(gal_app_t *app, uint32_t ms) { gal_app_tick(app, ms); }

static void to_game(gal_app_t *app)
{
    if (app->page == GAL_PAGE_START) press(app, BSP_BTN_OK);
    CHECK(app->page == GAL_PAGE_TITLE, "start -> title");
    app->title_sel = GAL_TITLE_MENU_NEW;
    press(app, BSP_BTN_OK);
    CHECK(app->page == GAL_PAGE_TRANSITION, "new game shows the first chapter card");
    press(app, BSP_BTN_OK);
    CHECK(app->page == GAL_PAGE_GAME, "chapter card -> reader");
    CHECK(app->event_index == 1, "reader starts at the first dialogue event");
}

static void test_short_presses_advance(gal_app_t *app)
{
    const int base = app->event_index;
    press(app, BSP_BTN_OK);
    CHECK(app->event_index == base + 1, "short OK advances one line");
    press(app, BSP_BTN_UP);
    CHECK(app->event_index == base + 2, "short UP advances one line");
    press(app, BSP_BTN_DOWN);
    CHECK(app->event_index == base + 3, "short DOWN advances one line");
    CHECK(app->page == GAL_PAGE_GAME, "short presses stay in the reader");
}

static void test_typing_completion(gal_app_t *app)
{
    // 打字中短按：同一次按键补全当前页并推进到下一句（用户要求“下一句”，
    // 不能要求再按一次；长句仍按分页单元逐页推进，不丢正文）。
    const int base = app->event_index;
    const int calls = g_show_full_text_calls;
    g_typing_active = 1;
    press(app, BSP_BTN_OK);
    CHECK(g_show_full_text_calls == calls + 1, "typing: short press completes the page");
    CHECK(app->event_index == base + 1, "typing: same short press advances one line");
    CHECK(g_typing_active == 0, "typing: page is fully shown before moving on");
    g_typing_active = 1;
    press(app, BSP_BTN_UP);
    CHECK(app->event_index == base + 2, "typing: short UP also advances one line");
    g_typing_active = 1;
    press(app, BSP_BTN_DOWN);
    CHECK(app->event_index == base + 3, "typing: short DOWN also advances one line");
}

static void test_long_ok_menu(gal_app_t *app)
{
    const int base = app->event_index;
    hold(app, BSP_BTN_OK);
    CHECK(app->page == GAL_PAGE_MENU, "long OK opens the in-game menu");
    release(app, BSP_BTN_OK);
    CHECK(app->page == GAL_PAGE_MENU, "releasing after long OK does not advance");
    CHECK(app->event_index == base, "long OK does not consume a dialogue step");
    hold(app, BSP_BTN_OK);
    CHECK(app->page == GAL_PAGE_GAME, "long OK in the menu returns to the reader");
    release(app, BSP_BTN_OK);
    CHECK(app->page == GAL_PAGE_GAME && app->event_index == base,
          "menu round trip leaves the dialogue position unchanged");
}

static void test_auto_toggle_and_manual_advance(gal_app_t *app)
{
    const int base = app->event_index;
    hold(app, BSP_BTN_DOWN);
    CHECK(app->auto_mode, "long DOWN turns auto on");
    release(app, BSP_BTN_DOWN);
    CHECK(app->auto_mode, "release after long DOWN keeps auto on");

    // 自动开启时的短按必须推进，而不是只退出自动。
    press(app, BSP_BTN_OK);
    CHECK(app->event_index == base + 1, "short press advances while auto is on");
    CHECK(app->auto_mode, "manual advance keeps auto mode on");

    // 自动节拍：补全后等 900ms 自动推进一句。
    const int auto_base = app->event_index;
    tick(app, 400);
    CHECK(app->event_index == auto_base, "auto does not advance before the interval");
    tick(app, 500);
    CHECK(app->event_index == auto_base + 1, "auto advances after the interval");

    hold(app, BSP_BTN_DOWN);
    CHECK(!app->auto_mode, "long DOWN turns auto off");
    release(app, BSP_BTN_DOWN);
    CHECK(!app->auto_mode, "release after turning auto off keeps it off");
}

static void test_fast_forward(gal_app_t *app)
{
    const int base = app->event_index;
    g_typing_active = 1;
    hold(app, BSP_BTN_UP);
    CHECK(app->fast_forward, "long UP starts fast-forward");
    CHECK(app->event_index == base + 1, "long UP advances immediately, before any tick");
    CHECK(g_typing_active == 0, "fast-forward clears the typewriter");

    // 自动开启时快进也不等 900ms 的自动间隔。
    app->auto_mode = true;
    const int ff_base = app->event_index;
    for (int i = 0; i < 5; ++i) tick(app, 90);
    CHECK(app->event_index == ff_base + 5, "fast-forward advances every task tick");
    app->auto_mode = false;

    // 按住快进期间不能被空闲策略调暗/熄屏。
    for (int i = 0; i < 20; ++i) tick(app, 90);
    CHECK(gal_app_idle_ms(app) == 0, "fast-forward keeps the screen awake");

    release(app, BSP_BTN_UP);
    CHECK(!app->fast_forward, "releasing UP stops fast-forward");
    const int stopped = app->event_index;
    tick(app, 2000);
    CHECK(app->event_index == stopped, "no further advance after release");
    CHECK(app->page == GAL_PAGE_GAME, "release keeps the reader page");
    CHECK(gal_app_idle_ms(app) >= 2000, "idle accounting resumes after release");
}

static void test_fast_forward_skips_pages(void)
{
    // 用真实内容表找一段 20px 档下需要两页的正文；本作正文里确实有 50 字的长句。
    uint32_t offsets[GAL_TEXT_MAX_PAGES + 1];
    int target = -1;
    for (int i = 0; i < gal_content_event_count; ++i) {
        const gal_event_t *ev = &gal_content_events[i];
        if (ev->kind != GAL_EV_LINE) continue;
        const int pages = gal_text_pages(gal_content_lines[ev->index].text, 30,
                                         GAL_TEXT_LINES_LARGE, offsets,
                                         GAL_TEXT_MAX_PAGES + 1);
        if (pages >= 2) {
            target = i;
            break;
        }
    }
    CHECK(target > 0, "found a two-page paragraph in the real content");
    if (target <= 0) return;

    gal_app_t app;
    g_media_init_result = 1;
    g_media_faulted = 0;
    g_lock_failures_left = 0;
    CHECK(gal_app_init(&app), "ff-page app init");
    press(&app, BSP_BTN_OK);
    app.title_sel = GAL_TITLE_MENU_NEW;
    press(&app, BSP_BTN_OK);
    press(&app, BSP_BTN_OK);
    CHECK(app.page == GAL_PAGE_GAME, "ff-page reader");

    // 20px 档下把当前句放到第 0 页（还没翻页）。
    app.settings.font_large = 1;
    app.event_index = target;
    app.page_index = 0;
    app.page_count = 2;
    app.shown_line = -1;

    const int next_event = target + 1;
    hold(&app, BSP_BTN_UP);
    CHECK(app.fast_forward, "long UP starts fast-forward on a two-page paragraph");
    CHECK(app.event_index == next_event,
          "long UP jumps to the next sentence, not the next page of this one");
    CHECK(app.page_index == 0, "fast-forward lands on page 0 of the next sentence");

    // 每个输入拍只推进一句；10ms 的拍也推进，不再等 90ms 累积。
    const int tick_base = app.event_index;
    tick(&app, 10);
    CHECK(app.event_index == tick_base + 1, "one sentence per tick, no 90ms wait");
    release(&app, BSP_BTN_UP);

    // 已经停在本句第 2 页时，长按也要直接到下一句。
    app.event_index = target;
    app.page_index = 1;
    app.page_count = 2;
    app.shown_line = -1;
    hold(&app, BSP_BTN_UP);
    CHECK(app.event_index == next_event,
          "long UP from page 2 of the same sentence still goes to the next sentence");
    CHECK(app.page_index == 0, "fast-forward from page 2 resets to page 0");
    release(&app, BSP_BTN_UP);

    // 负控：普通短按仍然逐页推进，不能被快进语义带跑。
    app.event_index = target;
    app.page_index = 0;
    app.page_count = 2;
    app.shown_line = -1;
    press(&app, BSP_BTN_OK);
    CHECK(app.event_index == target && app.page_index == 1,
          "short press still advances one page, not one sentence");
    CHECK(!app.fast_forward, "short press does not start fast-forward");
}

static void test_page_change_clears_state(gal_app_t *app)
{
    hold(app, BSP_BTN_DOWN);
    CHECK(app->auto_mode, "auto on before page change");
    release(app, BSP_BTN_DOWN);
    hold(app, BSP_BTN_OK);
    CHECK(app->page == GAL_PAGE_MENU, "menu opened");
    CHECK(!app->auto_mode, "opening the menu clears auto");
    CHECK(!app->fast_forward, "opening the menu clears fast-forward");
    release(app, BSP_BTN_OK);
    hold(app, BSP_BTN_OK);
    CHECK(app->page == GAL_PAGE_GAME, "back to reader");
    CHECK(!app->auto_mode, "auto stays cleared after returning");
}

// 快进直到离开阅读页（结局）；返回推进的节拍数。
static int read_to_ending(gal_app_t *app)
{
    int ticks = 0;
    hold(app, BSP_BTN_UP);
    while (ticks < 5000 &&
           (app->page == GAL_PAGE_GAME || app->page == GAL_PAGE_TRANSITION)) {
        tick(app, 90);
        ticks++;
    }
    release(app, BSP_BTN_UP);
    return ticks;
}

static void open_story_page(gal_app_t *app)
{
    if (app->page == GAL_PAGE_GAME) {
        hold(app, BSP_BTN_OK);
        app->menu_sel = GAL_MENU_TITLE;
        press(app, BSP_BTN_OK);
    }
    CHECK(app->page == GAL_PAGE_TITLE, "menu -> title");
    app->title_sel = GAL_TITLE_MENU_GALLERY;
    press(app, BSP_BTN_OK);
    CHECK(app->page == GAL_PAGE_GALLERY, "title -> gallery");
    app->gallery_sel = GAL_GALLERY_STORY;
    press(app, BSP_BTN_OK);
    CHECK(app->page == GAL_PAGE_CHAPTER, "gallery -> story");
}

static void test_preview_then_full_run(gal_app_t *app)
{
    CHECK(app->run_mask == 0x1u, "only chapter 1 has been visited so far");
    CHECK(!app->preview, "a New Game run is not a preview");

    // 先读到第四章，让四章都解锁（正式周目的一部分）。
    while (app->event_index < 1071 && (app->page == GAL_PAGE_GAME ||
                                       app->page == GAL_PAGE_TRANSITION)) {
        press(app, BSP_BTN_OK);
    }
    CHECK(app->event_index >= 1071, "reached the fourth chapter");
    CHECK(app->run_mask == 0xFu, "continuous reading marks all four chapters");
    CHECK(gal_unlock_has_chapter(&app->unlock, 3), "fourth chapter unlocked");

    // 负控 1：从第四章章节入口试读到结局。
    open_story_page(app);
    app->chapter_sel = 3;
    press(app, BSP_BTN_OK);
    CHECK(app->page == GAL_PAGE_TRANSITION, "chapter 4 preview shows its chapter card");
    CHECK(app->preview, "chapter entry sets the preview state");
    press(app, BSP_BTN_OK);
    CHECK(app->page == GAL_PAGE_GAME, "chapter 4 preview starts reading");
    CHECK(read_to_ending(app) > 0, "chapter 4 preview read to the ending");
    CHECK(app->page == GAL_PAGE_ENDING, "chapter 4 preview reaches the ending page");
    CHECK(app->ending_preview, "chapter 4 preview ending is marked as a preview");
    CHECK(!gal_unlock_has_any_ending(&app->unlock),
          "chapter 4 preview does not grant the permanent ending");

    // 负控 2：从第一章章节入口试读，读完整四章也不授正式结局。
    press(app, BSP_BTN_OK);   // ending -> title
    CHECK(app->page == GAL_PAGE_TITLE, "ending -> title");
    open_story_page(app);
    app->chapter_sel = 0;
    press(app, BSP_BTN_OK);
    CHECK(app->preview, "chapter 1 preview sets the preview state");
    press(app, BSP_BTN_OK);
    CHECK(app->page == GAL_PAGE_GAME, "chapter 1 preview starts reading");

    // 试读中途存一档，再回到标题读档继续；preview 必须随存档往返。
    for (int i = 0; i < 40; ++i) press(app, BSP_BTN_OK);
    hold(app, BSP_BTN_OK);
    app->menu_sel = GAL_MENU_SAVE;
    press(app, BSP_BTN_OK);
    app->save_sel = 1;
    press(app, BSP_BTN_OK);
    CHECK(app->save.slots[1].preview == 1, "preview flag travels with the manual save");
    hold(app, BSP_BTN_OK);
    hold(app, BSP_BTN_OK);
    hold(app, BSP_BTN_OK);
    app->menu_sel = GAL_MENU_TITLE;
    press(app, BSP_BTN_OK);
    CHECK(app->page == GAL_PAGE_TITLE, "preview run -> title");
    app->title_sel = GAL_TITLE_MENU_LOAD;
    press(app, BSP_BTN_OK);
    app->load_sel = 1;
    press(app, BSP_BTN_OK);
    CHECK(app->page == GAL_PAGE_GAME, "preview slot loads");
    CHECK(app->preview, "preview flag restored from the save slot");
    CHECK(read_to_ending(app) > 0, "preview run continues to the ending");
    CHECK(app->page == GAL_PAGE_ENDING, "restored preview reaches the ending");
    CHECK(app->ending_preview, "restored preview ending is still a preview");
    CHECK(!gal_unlock_has_any_ending(&app->unlock),
          "reading all four chapters from a preview never grants the ending");

    // 正控：New Game 从第一句读完整四章才授永久结局。
    press(app, BSP_BTN_OK);   // ending -> title
    to_game(app);
    CHECK(!app->preview, "New Game clears the preview state");
    CHECK(app->run_mask == 0x1u, "new game clears the run coverage");
    CHECK(read_to_ending(app) > 0, "formal run read to the ending");
    CHECK(app->page == GAL_PAGE_ENDING, "formal run reaches the ending");
    CHECK(!app->ending_preview, "formal run ending is not a preview");
    CHECK(gal_unlock_has_any_ending(&app->unlock), "formal run grants the permanent ending");
    CHECK(app->run_mask == 0xFu, "formal run covers all four chapters");
}

static void test_save_load_and_continue(gal_app_t *app)
{
    press(app, BSP_BTN_OK);            // ending -> title
    to_game(app);
    for (int i = 0; i < 5; ++i) press(app, BSP_BTN_OK);
    const int saved_event = app->event_index;
    hold(app, BSP_BTN_OK);
    app->menu_sel = GAL_MENU_SAVE;
    press(app, BSP_BTN_OK);
    CHECK(app->page == GAL_PAGE_SAVE, "menu -> save");
    app->save_sel = 0;
    press(app, BSP_BTN_OK);
    CHECK(app->save.slots[0].occupied, "slot 0 written");
    CHECK(app->save.slots[0].run_mask == 0x1u, "slot stores the run coverage");
    hold(app, BSP_BTN_OK);
    hold(app, BSP_BTN_OK);
    CHECK(app->page == GAL_PAGE_GAME, "save page -> reader");

    hold(app, BSP_BTN_OK);
    app->menu_sel = GAL_MENU_TITLE;
    press(app, BSP_BTN_OK);
    app->title_sel = GAL_TITLE_MENU_LOAD;
    press(app, BSP_BTN_OK);
    CHECK(app->page == GAL_PAGE_LOAD, "title -> load");
    app->load_sel = 0;
    press(app, BSP_BTN_OK);
    CHECK(app->page == GAL_PAGE_GAME, "load -> reader");
    CHECK(app->event_index == saved_event, "load restores the saved event");
    CHECK(app->run_mask == 0x1u, "load restores the run coverage");

    gal_app_flush(app);
    hold(app, BSP_BTN_OK);
    app->menu_sel = GAL_MENU_TITLE;
    press(app, BSP_BTN_OK);
    app->run_mask = 0;
    app->title_sel = GAL_TITLE_MENU_CONTINUE;
    press(app, BSP_BTN_OK);
    CHECK(app->page == GAL_PAGE_GAME, "continue returns to the reader");
    CHECK(app->run_mask == 0x1u, "continue restores coverage from automatic progress");
    CHECK(gal_unlock_has_any_ending(&app->unlock), "permanent ending survives save/load");
}

static void test_corrupt_save_rejected(gal_app_t *app)
{
    // 事件下标越界的槽位：长度与版本都对，但绝不能应用到运行状态。
    hold(app, BSP_BTN_OK);
    app->menu_sel = GAL_MENU_TITLE;
    press(app, BSP_BTN_OK);
    CHECK(app->page == GAL_PAGE_TITLE, "corrupt-save: back at title");
    to_game(app);
    const int before = app->event_index;
    app->save.slots[0].occupied = 1;
    app->save.slots[0].event_index = 0xFFF0u;
    app->save.slots[0].run_mask = 0;
    app->title_sel = GAL_TITLE_MENU_LOAD;
    hold(app, BSP_BTN_OK);
    app->menu_sel = GAL_MENU_TITLE;
    press(app, BSP_BTN_OK);
    app->title_sel = GAL_TITLE_MENU_LOAD;
    press(app, BSP_BTN_OK);
    CHECK(app->page == GAL_PAGE_LOAD, "corrupt-save: load page opens");
    app->load_sel = 0;
    press(app, BSP_BTN_OK);
    CHECK(app->page == GAL_PAGE_LOAD, "corrupt-save: stays on the load page");
    CHECK(strstr(app->notice, "存档损坏") != NULL, "corrupt-save: visible error");
    CHECK(app->event_index == before, "corrupt-save: position unchanged");
    app->save.slots[0].occupied = 0;
}

// Lead 复现的时序：fault 已报告时，input_task 会先送按键再 tick；按键处理
// 本身必须收口，不能把这次按键用在推进上。另测错误层首次拿锁失败后的重试。
static void test_fault_before_key_and_lock_retry(void)
{
    gal_app_t app;
    g_media_faulted = 0;
    g_lock_failures_left = 0;
    g_media_init_result = 1;
    CHECK(gal_app_init(&app), "fault-timing app init");
    press(&app, BSP_BTN_OK);
    app.title_sel = GAL_TITLE_MENU_NEW;
    press(&app, BSP_BTN_OK);
    press(&app, BSP_BTN_OK);
    CHECK(app.page == GAL_PAGE_GAME, "fault-timing reader");
    const int before = app.event_index;
    g_last_asset_error[0] = '\0';
    g_media_faulted = 1;
    press(&app, BSP_BTN_OK);   // 没有 tick
    CHECK(app.event_index == before, "reported fault blocks the next key before any tick");
    CHECK(!app.media_ok, "reported fault disables reading immediately");
    CHECK(app.error_shown && g_last_asset_error[0] != '\0',
          "reported fault submits the error layer from the key path");

    // 首次错误屏拿锁失败：不许放弃，也不许恢复 media_ok。
    gal_app_t retry;
    g_media_faulted = 0;
    g_lock_failures_left = 0;
    CHECK(gal_app_init(&retry), "lock-retry app init");
    press(&retry, BSP_BTN_OK);
    retry.title_sel = GAL_TITLE_MENU_NEW;
    press(&retry, BSP_BTN_OK);
    press(&retry, BSP_BTN_OK);
    CHECK(retry.page == GAL_PAGE_GAME, "lock-retry reader");
    g_last_asset_error[0] = '\0';
    g_media_faulted = 1;
    g_lock_failures_left = 1;
    tick(&retry, 1);
    CHECK(!retry.media_ok, "fault disables reading on the first tick");
    CHECK(!retry.error_shown, "first error-layer lock failure is not committed");
    CHECK(g_last_asset_error[0] == '\0', "no error UI yet while the lock fails");
    tick(&retry, 1);
    CHECK(retry.error_shown && g_last_asset_error[0] != '\0',
          "error layer is retried and committed on the next tick");
    CHECK(!retry.media_ok, "retry does not restore reading");
    const int frozen = retry.event_index;
    press(&retry, BSP_BTN_OK);
    CHECK(retry.event_index == frozen, "reading stays blocked after the error layer appears");
}

static void test_media_fault_at_runtime(void)
{
    gal_app_t app;
    g_media_init_result = 1;
    g_media_faulted = 0;
    CHECK(gal_app_init(&app), "fault-test app init");
    press(&app, BSP_BTN_OK);
    app.title_sel = GAL_TITLE_MENU_NEW;
    press(&app, BSP_BTN_OK);
    press(&app, BSP_BTN_OK);
    CHECK(app.page == GAL_PAGE_GAME, "fault-test reader");
    const int before = app.event_index;

    // 解码中途失败（条带 CRC）：tick 收口到可见错误并停止推进。
    g_media_faulted = 1;
    tick(&app, 50);
    CHECK(!app.media_ok, "runtime decode fault flips media_ok");
    CHECK(strstr(g_last_asset_error, "stub strip failure") != NULL,
          "runtime decode fault shows the visible error panel");
    press(&app, BSP_BTN_OK);
    CHECK(app.event_index == before, "runtime decode fault blocks further reading");
    g_media_faulted = 0;
}

static void test_bad_media_blocks_reading(void)
{
    gal_app_t app;
    g_media_init_result = 0;
    g_media_faulted = 0;
    g_lock_failures_left = 0;
    g_last_asset_error[0] = '\0';
    CHECK(gal_app_init(&app), "app init still builds the UI with a broken pack");
    CHECK(!app.media_ok, "broken pack is reported");
    CHECK(strstr(g_last_asset_error, "stub media error") != NULL,
          "broken pack produces a visible error panel");
    press(&app, BSP_BTN_OK);             // start -> title
    CHECK(app.page == GAL_PAGE_TITLE, "broken pack reaches the title");
    app.title_sel = GAL_TITLE_MENU_NEW;
    press(&app, BSP_BTN_OK);
    CHECK(app.page == GAL_PAGE_TITLE, "broken pack refuses to start a new game");
    CHECK(strstr(app.notice, "资源损坏") != NULL, "broken pack explains why reading cannot start");
    CHECK(app.event_index == 0, "broken pack does not enter the story");
    g_media_init_result = 1;
}

int main(void)
{
    gal_app_t app;
    CHECK(gal_app_init(&app), "app init");
    CHECK(app.media_ok, "media pack available");
    CHECK(app.page == GAL_PAGE_START, "starts at the start page");
    press(&app, BSP_BTN_OK);
    to_game(&app);

    test_short_presses_advance(&app);
    test_typing_completion(&app);
    test_long_ok_menu(&app);
    test_auto_toggle_and_manual_advance(&app);
    test_fast_forward(&app);
    test_fast_forward_skips_pages();
    test_page_change_clears_state(&app);
    test_preview_then_full_run(&app);
    test_save_load_and_continue(&app);
    test_corrupt_save_rejected(&app);
    test_fault_before_key_and_lock_retry();
    test_media_fault_at_runtime();
    test_bad_media_blocks_reading();

    if (failures) {
        printf("FAIL: %d checks\n", failures);
        return 1;
    }
    printf("PASS: gal key semantics\n");
    return 0;
}
