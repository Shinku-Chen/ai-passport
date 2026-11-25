// Application key-flow regression with a controllable clock.
//
// Covers the user-facing contract: short OK/UP/DOWN all advance; long OK opens
// the in-game menu; long DOWN toggles auto; holding UP fast-forwards through
// typing and the auto wait, stops on release and stops at a choice; a long
// press does not fire a short press on release; leaving the page clears the
// transient modes; auto + a manual press still advances.
//
// Usage: test_nine_app <nine_script.bin> <nine_images.bin>
#include "nine_app.h"
#include "nine_input.h"
#include "gal_ui_stub.h"
#include "bsp_button.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool nine_host_load(const char *script, const char *images);

static nine_app_t app;
static int failures;
static int checks;
extern int nine_test_lock_fail_once;

#define CHECK(cond, msg) do { \
    ++checks; \
    if (!(cond)) { ++failures; fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, msg); } \
} while (0)

static void press(uint8_t btn) { nine_key_t k = { .btn = btn, .ev = BSP_BTN_PRESS }; nine_app_key(&app, &k); }
static void click(uint8_t btn)
{
    press(btn);
    nine_key_t k = { .btn = btn, .ev = BSP_BTN_CLICK };
    nine_app_key(&app, &k);
    k.ev = BSP_BTN_RELEASE;
    nine_app_key(&app, &k);
}
static void long_press(uint8_t btn)
{
    press(btn);
    nine_key_t k = { .btn = btn, .ev = BSP_BTN_LONG };
    nine_app_key(&app, &k);
}
static void release(uint8_t btn)
{
    nine_key_t k = { .btn = btn, .ev = BSP_BTN_RELEASE };
    nine_app_key(&app, &k);
}
static void tick_ms(uint32_t ms) { nine_app_tick(&app, ms); }
static void menu_home(void)
{
    for (int i = 0; i < 6; ++i) click(BSP_BTN_UP);
}

static bool pump_key(nine_input_t *in, nine_key_t k)
{
    in->pending = k;
    in->have_pending = true;
    for (int i = 0; i < 8 && !nine_input_process(&app, in); ++i) { }
    return !in->have_pending;
}

static void release_all(void)
{
    release(BSP_BTN_OK);
    release(BSP_BTN_UP);
    release(BSP_BTN_DOWN);
}
static const char *text_copy(char *dst, size_t cap)
{
    snprintf(dst, cap, "%s", host_text);
    return dst;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <script.bin> <images.bin>\n", argv[0]);
        return 2;
    }
    nvs_stub_reset();
    if (!nine_host_load(argv[1], argv[2])) {
        fprintf(stderr, "cannot load packages\n");
        return 2;
    }
    if (!nine_app_init(&app)) {
        fprintf(stderr, "nine_app_init failed\n");
        return 2;
    }
    CHECK(host_page == GAL_PAGE_START, "init shows start page");
    CHECK(host_row_count[GAL_LIST_START] == 1 &&
          !strcmp(host_rows[GAL_LIST_START][0], "开始游戏"), "start page prompt");
    CHECK(app.pack_ok && app.images_ok && app.media_ok, "packages accepted");

    // Start -> title. New game starts the story.
    click(BSP_BTN_OK);
    CHECK(host_page == GAL_PAGE_TITLE, "start -> title");
    CHECK(host_row_count[GAL_LIST_TITLE] == 7, "title menu has 7 rows");
    CHECK(host_rows[GAL_LIST_TITLE][5][0] != 0, "title menu has a 主视觉 row");
    CHECK(host_row_enabled[GAL_LIST_TITLE][1] == false, "continue disabled without progress");
    click(BSP_BTN_OK);   // 开始游戏
    CHECK(host_page == GAL_PAGE_GAME, "new game enters dialogue");
    CHECK(host_text[0] != 0, "first line has text");
    CHECK(host_typing, "first line starts typing");

    char a[2048], b[2048];
    text_copy(a, sizeof(a));
    click(BSP_BTN_OK);   // a short press must advance even while typing
    text_copy(b, sizeof(b));
    CHECK(strcmp(a, b) != 0, "OK advances a typing line");
    CHECK(host_typing, "the next line starts typing");
    click(BSP_BTN_UP);
    text_copy(a, sizeof(a));
    CHECK(strcmp(a, b) != 0, "UP advances a typing line");
    click(BSP_BTN_DOWN);
    text_copy(b, sizeof(b));
    CHECK(strcmp(b, a) != 0, "DOWN advances a typing line");

    // A frame whose visual fails must not be skipped: the text is visible, the
    // story freezes, and returning from the menu retries the decode.
    extern int nine_host_fail_visual;
    nine_host_fail_visual = 1;
    text_copy(a, sizeof(a));
    bool image_failed = false;
    for (int i = 0; i < 40 && !image_failed; ++i) {
        click(BSP_BTN_OK);
        image_failed = app.view_error == NINE_VIEW_IMAGE_ERROR;
    }
    CHECK(image_failed, "image failure is recorded");
    CHECK(strstr(host_notice, "图像解码失败") != NULL, "image failure is visible");
    CHECK(strcmp(a, host_text) != 0, "failed frame text is still shown");
    {
        const uint16_t n = app.vm.state.node;
        const uint32_t i = app.vm.state.index;
        click(BSP_BTN_OK);
        CHECK(app.vm.state.node == n && app.vm.state.index == i,
              "failed image blocks advancing the VM");
        nine_host_fail_visual = 0;
        long_press(BSP_BTN_OK);
        menu_home();
        click(BSP_BTN_OK);   // 返回剧情 retries the decode
        CHECK(host_page == GAL_PAGE_GAME, "menu return after image failure");
        CHECK(app.view_error == NINE_VIEW_OK, "menu return retries the visual");
        click(BSP_BTN_OK);
        CHECK(app.vm.state.node != n || app.vm.state.index != i,
              "story resumes after a successful retry");
    }

    // Long OK opens the menu; its release must not fire a short press.
    long_press(BSP_BTN_OK);
    CHECK(host_page == GAL_PAGE_MENU, "long OK opens menu");
    release(BSP_BTN_OK);
    {
        // The BSP may report CLICK on the same release. It must not fire a
        // second action for the gesture that already opened the menu.
        nine_key_t k = { .btn = BSP_BTN_OK, .ev = BSP_BTN_CLICK };
        nine_app_key(&app, &k);
    }
    CHECK(host_page == GAL_PAGE_MENU, "long-press release click is ignored");
    menu_home();
    click(BSP_BTN_OK);   // select 返回剧情
    CHECK(host_page == GAL_PAGE_GAME, "menu resumes the story");

    // Deterministic production-path HAL contention: the first lock attempt
    // fails, the same already-dequeued key is retried, and the action occurs
    // exactly once. Synthetic overflow is a separate latch test below.
    {
        nine_input_t in;
        nine_input_init(&in);
        text_copy(a, sizeof(a));
        nine_key_t k = { .btn = BSP_BTN_OK, .ev = BSP_BTN_PRESS };
        CHECK(pump_key(&in, k), "contention press consumed");
        k.ev = BSP_BTN_CLICK;
        nine_test_lock_fail_once = 1;
        CHECK(pump_key(&in, k), "contention click retried after lock timeout");
        CHECK(nine_test_lock_fail_once == 0, "first lock failure actually happened");
        k.ev = BSP_BTN_RELEASE;
        CHECK(pump_key(&in, k), "contention release consumed");
        text_copy(b, sizeof(b));
        CHECK(strcmp(a, b) != 0, "contention short acts exactly once");

        k = (nine_key_t){ .btn = BSP_BTN_OK, .ev = BSP_BTN_LONG };
        nine_test_lock_fail_once = 1;
        CHECK(pump_key(&in, k), "contention long retried");
        CHECK(host_page == GAL_PAGE_MENU, "contention long opens menu exactly once");
        k.ev = BSP_BTN_RELEASE;
        CHECK(pump_key(&in, k), "contention long release");
        CHECK(host_page == GAL_PAGE_MENU, "release after long adds no click");
        k = (nine_key_t){ .btn = BSP_BTN_OK, .ev = BSP_BTN_LONG };
        CHECK(pump_key(&in, k), "close menu");
        k.ev = BSP_BTN_RELEASE;
        CHECK(pump_key(&in, k), "close menu release");
        CHECK(host_page == GAL_PAGE_GAME, "back to reading after menu");

        k = (nine_key_t){ .btn = BSP_BTN_UP, .ev = BSP_BTN_LONG };
        nine_test_lock_fail_once = 1;
        CHECK(pump_key(&in, k), "contention long UP retried");
        CHECK(app.fast_forward, "contention long UP starts FF once");
        k.ev = BSP_BTN_RELEASE;
        CHECK(pump_key(&in, k), "contention release queued after FF");
        CHECK(!app.fast_forward, "quick release after LONG stops FF");

        // Synthetic queue-overflow latch (not the observed lock timeout).
        app.fast_forward = true;
        app.suppress_click[BSP_BTN_OK] = true;
        nine_app_input_recover(&app);
        CHECK(!app.fast_forward && !app.suppress_click[BSP_BTN_OK],
              "synthetic overflow latch recovery clears hold/latch");
        printf("INPUT_CONTENTION lock_timeout_retry=1 synthetic_overflow=1 | ");
    }

    // Auto: long DOWN enables it; the wait is 1200ms after typing completes.
    long_press(BSP_BTN_DOWN);
    release(BSP_BTN_DOWN);   // release must not add an extra step
    CHECK(app.auto_mode && host_auto, "long DOWN enables auto");
    CHECK(!host_typing, "enabling auto shows the full page");
    text_copy(a, sizeof(a));
    tick_ms(1199);
    CHECK(!strcmp(a, host_text), "long DOWN release and pre-wait do not advance");
    tick_ms(1);
    text_copy(b, sizeof(b));
    CHECK(strcmp(a, b) != 0, "auto advances at 1200ms");
    CHECK(host_typing, "the auto-advanced line is typing");
    click(BSP_BTN_OK);
    CHECK(!app.auto_mode && !host_auto, "manual press turns auto off");
    CHECK(strcmp(b, host_text) != 0, "manual press advances while auto typed");

    // Hold UP: threshold enters the next whole sentence; one sentence per
    // input tick, no 90/120ms accumulator or catch-up; release stops.
    text_copy(a, sizeof(a));
    long_press(BSP_BTN_UP);
    CHECK(app.fast_forward, "long UP starts fast-forward");
    CHECK(!host_typing, "fast-forward skips typing");
    text_copy(b, sizeof(b));
    CHECK(strcmp(a, b) != 0, "long UP immediately advances the next sentence");
    tick_ms(50);
    text_copy(a, sizeof(a));
    CHECK(strcmp(b, host_text) != 0, "one input tick advances one whole sentence");
    tick_ms(50);
    text_copy(b, sizeof(b));
    CHECK(strcmp(a, b) != 0, "sustained hold advances on each input tick");
    release(BSP_BTN_UP);
    CHECK(!app.fast_forward, "releasing UP stops fast-forward");
    tick_ms(500);
    CHECK(!strcmp(b, host_text), "released fast-forward does not advance");

    // Fast-forward to the first choice: it must stop there, not choose.
    long_press(BSP_BTN_UP);
    int guard = 0;
    while (host_page != GAL_PAGE_CHOICE && guard++ < 5000) tick_ms(50);
    CHECK(host_page == GAL_PAGE_CHOICE, "fast-forward reaches the first choice");
    CHECK(!app.fast_forward, "fast-forward stops at a choice");
    CHECK(host_choice_count >= 1, "choice has visible options");
    release(BSP_BTN_UP);
    CHECK(host_typing == false, "choice page does not type");
    // The pinned source choice panel has 打开菜单. Long OK must open the same
    // menu as the reading page, keep the VM/choice_sel, and return to the exact
    // choice. Save and Load round-trips must also stay on CHOICE/CHOICE.
    {
        const uint16_t choice_node = app.vm.state.node;
        const uint32_t choice_index = app.vm.state.index;
        const int choice_sel = app.choice_sel;
        long_press(BSP_BTN_OK);
        CHECK(host_page == GAL_PAGE_MENU, "long OK opens menu on the choice page");
        CHECK(app.vm.state.node == choice_node && app.vm.state.index == choice_index &&
              app.choice_sel == choice_sel, "choice menu does not move the VM/selection");
        release(BSP_BTN_OK);
        {
            nine_key_t k = { .btn = BSP_BTN_OK, .ev = BSP_BTN_CLICK };
            nine_app_key(&app, &k);
        }
        CHECK(host_page == GAL_PAGE_MENU, "choice menu long-release click is ignored");
        menu_home();
        click(BSP_BTN_OK);   // 返回剧情
        CHECK(host_page == GAL_PAGE_CHOICE, "choice menu direct return restores CHOICE");
        CHECK(app.vm.state.node == choice_node && app.vm.state.index == choice_index &&
              app.choice_sel == choice_sel, "direct return keeps the exact choice");

        // Save at the choice to slot 1 (slot 0 stays empty for the later flow).
        long_press(BSP_BTN_OK);
        menu_home();
        click(BSP_BTN_DOWN);
        click(BSP_BTN_OK);   // 存档
        CHECK(host_page == GAL_PAGE_SAVE, "choice menu opens save page");
        click(BSP_BTN_DOWN);
        click(BSP_BTN_OK);   // slot 1
        CHECK(!strcmp(host_notice, "已保存"), "choice save succeeds");
        long_press(BSP_BTN_OK);
        menu_home();
        click(BSP_BTN_OK);   // 返回剧情
        CHECK(host_page == GAL_PAGE_CHOICE && app.vm.state.node == choice_node &&
              app.vm.state.index == choice_index && app.choice_sel == choice_sel,
              "save return keeps the exact choice");

        // Load the choice save; it must render CHOICE, not GAME with a CHOICE VM.
        long_press(BSP_BTN_OK);
        menu_home();
        click(BSP_BTN_DOWN);
        click(BSP_BTN_DOWN);
        click(BSP_BTN_OK);   // 读档
        CHECK(host_page == GAL_PAGE_LOAD, "choice menu opens load page");
        click(BSP_BTN_OK);   // first used slot = slot 1
        CHECK(host_page == GAL_PAGE_CHOICE, "choice load restores the CHOICE page");
        CHECK(app.vm.state.node == choice_node && app.vm.state.index == choice_index &&
              app.choice_sel == choice_sel, "choice load keeps the exact choice");
    }
    click(BSP_BTN_OK);
    CHECK(host_page == GAL_PAGE_GAME, "choice selection resumes the story");

    // Walk to a frame that spans two pages. A single short press must advance
    // the page cursor even when the new page is still typing.
    bool multi = false;
    for (int i = 0; i < 12000; ++i) {
        if (host_page == GAL_PAGE_CHOICE) { click(BSP_BTN_OK); continue; }
        if (host_page == GAL_PAGE_TRANSITION) { tick_ms(90); continue; }
        if (host_page == GAL_PAGE_GAME && app.page_count > 1) {
            multi = true;
            break;
        }
        click(BSP_BTN_OK);
    }
    CHECK(multi, "found a multi-page frame");
    CHECK(app.page_count == 2 && app.text_page == 0, "multi-page frame starts on page 1");
    {
        const uint16_t page_node = app.vm.state.node;
        const uint32_t page_index = app.vm.state.index;
        click(BSP_BTN_UP);
        CHECK(app.vm.state.node == page_node && app.vm.state.index == page_index &&
              app.text_page == 1, "one short press advances the page, not just typing");

        // Save page 1 to slot 1, load it, and require the exact sub-page.
        long_press(BSP_BTN_OK);
        menu_home();
        click(BSP_BTN_DOWN);
        click(BSP_BTN_OK);   // 存档
        click(BSP_BTN_DOWN);
        click(BSP_BTN_DOWN);
        click(BSP_BTN_OK);   // slot 3 (page-1 save)
        CHECK(!strcmp(host_notice, "已保存"), "page-1 save succeeds");
        long_press(BSP_BTN_OK);
        menu_home();
        click(BSP_BTN_DOWN);
        click(BSP_BTN_DOWN);
        click(BSP_BTN_OK);   // 读档
        click(BSP_BTN_DOWN);   // first used = slot 1 (choice save); move to slot 3
        click(BSP_BTN_OK);
        CHECK(host_page == GAL_PAGE_GAME && app.vm.state.node == page_node &&
              app.vm.state.index == page_index && app.text_page == 1,
              "load restores page 1 of the same frame");

        // A CRC-valid record with an out-of-range page must clamp, never read
        // past page_offsets.
        {
            uint8_t bad[NINE_SLOT_BYTES];
            const size_t bad_len = nine_vm_save_encode(&app.vm, 99, bad, sizeof(bad));
            CHECK(bad_len > 0 && nvs_stub_store("nine3", "s07", bad, bad_len) == 0,
                  "inject an out-of-range page save");
            long_press(BSP_BTN_OK);
            menu_home();
            click(BSP_BTN_DOWN);
            click(BSP_BTN_DOWN);
            click(BSP_BTN_OK);   // 读档
            for (int i = 0; i < 5; ++i) click(BSP_BTN_DOWN);   // slot 7
            click(BSP_BTN_OK);
            CHECK(host_page == GAL_PAGE_GAME && app.vm.state.node == page_node &&
                  app.vm.state.index == page_index && app.text_page == 1,
                  "out-of-range saved page clamps to the last page");
        }

        // Continue/restart from the automatic record must also restore page 1.
        nine_app_flush(&app);
        {
            nine_app_t app2;
            CHECK(nine_app_init(&app2), "restart init succeeds");
            app = app2;
            CHECK(host_page == GAL_PAGE_START, "restart shows start page");
            click(BSP_BTN_OK);   // start -> title
            CHECK(host_page == GAL_PAGE_TITLE, "restart reaches title");
            CHECK(host_row_enabled[GAL_LIST_TITLE][1], "continue enabled after restart");
            click(BSP_BTN_DOWN);
            click(BSP_BTN_OK);   // 继续游戏
            CHECK(host_page == GAL_PAGE_GAME && app.vm.state.node == page_node &&
                  app.vm.state.index == page_index && app.text_page == 1,
                  "continue/restart restores page 1");
        }
        // Fast-forward from page 1 of a multi-page sentence must skip the
        // remaining page(s) and land on page 0 of the next sentence.
        long_press(BSP_BTN_UP);
        CHECK(app.vm.state.node != page_node || app.vm.state.index != page_index,
              "FF from page 1 advances the VM sentence");
        CHECK(app.text_page == 0, "FF from page 1 lands on next page 0");
        release(BSP_BTN_UP);
        CHECK(!app.fast_forward, "release after multi-page FF stops");
        click(BSP_BTN_DOWN);
        CHECK(app.vm.state.node != page_node || app.vm.state.index != page_index,
              "the next press leaves the last page");
    }
    char chosen[2048];
    text_copy(chosen, sizeof(chosen));

    // Save slot 0, advance, then load it back.
    long_press(BSP_BTN_OK);
    menu_home();
    click(BSP_BTN_DOWN);   // 存档
    click(BSP_BTN_OK);
    CHECK(host_page == GAL_PAGE_SAVE, "menu opens save page");
    click(BSP_BTN_OK);     // empty slot 0
    CHECK(!strcmp(host_notice, "已保存"), "save reports success");
    CHECK(nvs_stub_set_count() > 0, "save wrote a record");
    long_press(BSP_BTN_OK);   // back to menu
    menu_home();
    click(BSP_BTN_OK);        // 返回剧情
    CHECK(host_page == GAL_PAGE_GAME, "back to story after save");
    click(BSP_BTN_OK);        // reveal/advance a little
    click(BSP_BTN_OK);
    text_copy(a, sizeof(a));
    CHECK(strcmp(a, chosen) != 0, "story advanced after the save");

    long_press(BSP_BTN_OK);
    menu_home();
    click(BSP_BTN_DOWN);
    click(BSP_BTN_DOWN);   // 读档
    click(BSP_BTN_OK);
    CHECK(host_page == GAL_PAGE_LOAD, "menu opens load page");
    click(BSP_BTN_OK);     // slot 0
    CHECK(host_page == GAL_PAGE_GAME, "load returns to the story");
    CHECK(!strcmp(host_text, chosen), "load restores the saved line");

    // A corrupt slot is reported and never applied to the live state.
    {
        const uint8_t junk[37] = { 0x4e, 0x56, 0x53, 0x31 };
        CHECK(nvs_stub_store("nine3", "s04", junk, sizeof(junk)) == 0, "inject corrupt slot");
    }
    text_copy(a, sizeof(a));
    long_press(BSP_BTN_OK);
    menu_home();
    click(BSP_BTN_DOWN);
    click(BSP_BTN_DOWN);
    click(BSP_BTN_OK);
    CHECK(host_page == GAL_PAGE_LOAD, "load page for corrupt slot");
    click(BSP_BTN_DOWN);
    click(BSP_BTN_DOWN);
    click(BSP_BTN_DOWN);   // move to slot 4 (index 3)
    click(BSP_BTN_OK);
    CHECK(!strcmp(host_notice, "存档数据校验失败"), "corrupt save is reported");
    CHECK(!strcmp(a, host_text), "corrupt save does not change live state");
    CHECK(app.page == GAL_PAGE_LOAD, "corrupt load stays on the load page");

    // Back to the story and finish: ending grants the permanent record.
    long_press(BSP_BTN_OK);   // load -> menu
    menu_home();
    click(BSP_BTN_OK);        // 返回剧情
    CHECK(host_page == GAL_PAGE_GAME, "back to story before ending");
    const int true_key = nine_pack_find_key(&app.pack, "true");
    const int bg_key = nine_pack_find_key(&app.pack, "titleBg");
    const int flavor_key = nine_pack_find_key(&app.pack, "flavor_n2");
    CHECK(true_key >= 0 && bg_key >= 0 && flavor_key >= 0, "story keys present");
    guard = 0;
    while (host_page != GAL_PAGE_ENDING && guard++ < 30000) {
        if (host_page == GAL_PAGE_CHOICE) {
            click(BSP_BTN_OK);   // choose the only option and keep going
            continue;
        }
        if (host_page == GAL_PAGE_TRANSITION) {   // opening movie: our stub ends it on tick
            tick_ms(90);
            continue;
        }
        if (host_page == GAL_PAGE_GAME && !app.fast_forward) long_press(BSP_BTN_UP);
        tick_ms(90);
    }
    CHECK(host_page == GAL_PAGE_ENDING, "fast-forward reaches the ending");
    CHECK(!strcmp(host_ending_eyebrow, "TRUE END"), "true ending label");
    CHECK(strstr(host_ending_title, "香坂春风") != NULL, "true ending title");
    CHECK(app.vm.state.progress[true_key] > 0, "true ending recorded permanently");
    CHECK(app.vm.state.progress[bg_key] == 2, "title visual 3 unlocked");
    CHECK((app.vm.state.flag_set[flavor_key / 8] >> (flavor_key % 8)) & 1,
          "run flag was written in this run");
    release(BSP_BTN_UP);

    // Ending -> title -> load the old manual slot: position/flags return, but the
    // newer permanent record survives (manual and permanent state are separate).
    click(BSP_BTN_OK);
    CHECK(host_page == GAL_PAGE_TITLE, "ending returns to title");
    CHECK(strstr(host_values[GAL_LIST_TITLE][5], "/ 3") != NULL,
          "all three title visuals unlocked");
    for (int i = 0; i < 5; ++i) click(BSP_BTN_DOWN);
    click(BSP_BTN_OK);   // 主视觉: cycle to visual 2
    CHECK(!strncmp(host_values[GAL_LIST_TITLE][5], "2 / 3", 5), "title visual switches");
    click(BSP_BTN_OK);   // visual 3
    CHECK(!strncmp(host_values[GAL_LIST_TITLE][5], "3 / 3", 5), "title visual 3 selected");
    for (int i = 0; i < 6; ++i) click(BSP_BTN_UP);   // back to row 0
    click(BSP_BTN_DOWN);
    click(BSP_BTN_DOWN);   // 读档
    click(BSP_BTN_OK);
    CHECK(host_page == GAL_PAGE_LOAD, "title load page");
    click(BSP_BTN_OK);
    CHECK(host_page == GAL_PAGE_GAME, "old save loads after the ending");
    CHECK(!strcmp(host_text, chosen), "old save restores its story position");
    CHECK(app.vm.state.progress[true_key] > 0, "permanent ending survives an old load");
    CHECK(app.vm.state.progress[bg_key] == 2, "permanent title visual survives an old load");
    CHECK(app.vm.state.preview == false, "normal load is not a preview");

    // Chapter preview: save it, unlock nothing, then load it after the ending.
    // The newest permanent record must survive both the preview and the load.
    long_press(BSP_BTN_OK);
    menu_home();
    for (int i = 0; i < 5; ++i) click(BSP_BTN_DOWN);
    click(BSP_BTN_OK);   // 返回标题
    CHECK(host_page == GAL_PAGE_TITLE, "title before preview");
    for (int i = 0; i < 4; ++i) click(BSP_BTN_DOWN);
    click(BSP_BTN_OK);   // 鉴赏
    CHECK(host_page == GAL_PAGE_GALLERY, "gallery opened");
    click(BSP_BTN_DOWN);
    click(BSP_BTN_OK);   // 剧情鉴赏
    CHECK(host_page == GAL_PAGE_CHAPTER, "chapter list opened");
    click(BSP_BTN_OK);   // preview chapter 1
    CHECK(host_page == GAL_PAGE_GAME, "chapter preview starts");
    CHECK(app.vm.state.preview, "chapter preview flag set");
    long_press(BSP_BTN_OK);
    menu_home();
    click(BSP_BTN_DOWN);
    click(BSP_BTN_OK);   // 存档
    click(BSP_BTN_DOWN);
    click(BSP_BTN_DOWN);
    click(BSP_BTN_DOWN);
    click(BSP_BTN_DOWN);
    click(BSP_BTN_OK);   // slot 5 (preview save)
    CHECK(!strcmp(host_notice, "已保存"), "preview save succeeds");
    long_press(BSP_BTN_OK);
    menu_home();
    for (int i = 0; i < 5; ++i) click(BSP_BTN_DOWN);
    click(BSP_BTN_OK);   // 返回标题
    click(BSP_BTN_DOWN);
    click(BSP_BTN_DOWN);
    click(BSP_BTN_OK);   // 读档
    click(BSP_BTN_DOWN);
    click(BSP_BTN_DOWN);
    click(BSP_BTN_DOWN);
    click(BSP_BTN_DOWN);
    click(BSP_BTN_OK);   // slot 5 (preview save)
    CHECK(host_page == GAL_PAGE_GAME, "preview save loads");
    CHECK(app.vm.state.preview, "loaded preview keeps its mode");
    CHECK(app.vm.state.progress[true_key] > 0, "preview load keeps the ending");
    CHECK(app.vm.state.progress[bg_key] == 2, "preview load keeps title visuals");

    // New game clears run flags but keeps the permanent record.
    long_press(BSP_BTN_OK);
    menu_home();
    for (int i = 0; i < 5; ++i) click(BSP_BTN_DOWN);
    click(BSP_BTN_OK);   // 返回标题
    CHECK(host_page == GAL_PAGE_TITLE, "menu returns to title");
    click(BSP_BTN_OK);     // 开始游戏
    CHECK(host_page == GAL_PAGE_GAME, "new game after ending starts");
    bool flags_clear = true;
    for (uint32_t i = 0; i < app.pack.key_count; ++i) {
        if (!nine_pack_key_persistent(&app.pack, (uint16_t)i) && app.vm.state.flag[i] != 0) {
            flags_clear = false;
        }
    }
    CHECK(flags_clear, "new game clears every per-run variable");
    CHECK(app.vm.state.progress[true_key] > 0, "new game keeps the permanent ending");
    CHECK(app.vm.state.progress[bg_key] == 2, "new game keeps the permanent title visual");

    // Continue after an ending is still available from the automatic record.
    long_press(BSP_BTN_OK);
    menu_home();
    click(BSP_BTN_DOWN);
    click(BSP_BTN_DOWN);
    click(BSP_BTN_DOWN);
    click(BSP_BTN_DOWN);
    click(BSP_BTN_DOWN);
    click(BSP_BTN_OK);
    CHECK(host_page == GAL_PAGE_TITLE, "returned to title for continue");
    CHECK(host_row_enabled[GAL_LIST_TITLE][1], "continue is enabled");
    click(BSP_BTN_DOWN);   // 继续游戏
    click(BSP_BTN_OK);
    CHECK(host_page == GAL_PAGE_GAME || host_page == GAL_PAGE_ENDING,
          "continue restores a reading position");

    // Second legal NewGame after TRUE END: n40 overwrites titleBg=1, but the
    // source rule still unlocks all three visuals from persistent loop+true.
    // The choice long-OK menu is exercised at n40 as well.
    long_press(BSP_BTN_OK);
    menu_home();
    for (int i = 0; i < 5; ++i) click(BSP_BTN_DOWN);
    click(BSP_BTN_OK);   // 返回标题
    CHECK(host_page == GAL_PAGE_TITLE, "title before the second NewGame");
    click(BSP_BTN_OK);   // 开始游戏
    CHECK(host_page == GAL_PAGE_GAME, "second NewGame starts");
    guard = 0;
    while (guard++ < 30000) {
        if (host_page == GAL_PAGE_CHOICE && app.vm.state.node == 40) break;
        if (host_page == GAL_PAGE_CHOICE) { click(BSP_BTN_OK); continue; }
        if (host_page == GAL_PAGE_TRANSITION) { tick_ms(90); continue; }
        if (host_page == GAL_PAGE_GAME && !app.fast_forward) long_press(BSP_BTN_UP);
        tick_ms(90);
    }
    CHECK(host_page == GAL_PAGE_CHOICE && app.vm.state.node == 40,
          "second run reaches the n40 choice");
    CHECK(app.vm.state.progress[true_key] > 0, "second run keeps the permanent true ending");
    CHECK(app.vm.state.progress[bg_key] == 1, "n40 writes the transient titleBg=1");
    long_press(BSP_BTN_OK);
    CHECK(host_page == GAL_PAGE_MENU, "long OK opens the menu at the n40 choice");
    menu_home();
    click(BSP_BTN_OK);   // 返回剧情
    CHECK(host_page == GAL_PAGE_CHOICE && app.vm.state.node == 40,
          "n40 menu direct return restores CHOICE");
    long_press(BSP_BTN_OK);
    menu_home();
    for (int i = 0; i < 5; ++i) click(BSP_BTN_DOWN);
    click(BSP_BTN_OK);   // 返回标题
    CHECK(host_page == GAL_PAGE_TITLE, "second run returns to title");
    CHECK(strstr(host_values[GAL_LIST_TITLE][5], "/ 3") != NULL,
          "second run after n40 keeps all three title visuals");
    CHECK(strstr(host_values[GAL_LIST_TITLE][5], "/ 2") == NULL,
          "transient titleBg=1 does not revoke visual 3");

    release_all();
    if (failures) {
        fprintf(stderr, "%d/%d checks failed\n", failures, checks);
        return 1;
    }
    printf("PASS app flow: %d checks\n", checks);
    return 0;
}
