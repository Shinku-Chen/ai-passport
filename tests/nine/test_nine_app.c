// Production reader/state-machine regression on the real Episode 4 pack.
// Covers short-key advancement while typing, auto/fast-forward cadence with a
// controlled clock, long-press suppression, choice stops, save/load/continue,
// chapter-preview isolation and permanent ending gates.
#include "nine_app.h"
#include "nine_image.h"
#include "nine_media.h"
#include "nine_vm.h"
#include "nine_assets_host.h"
#include "nine_ui_stub.h"
#include "bsp_button.h"
#include "gal_ui.h"
#include "nvs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int nine_nvs_host_get(const char *key, void *out, size_t cap, size_t *len);
int nine_nvs_host_put(const char *key, const void *data, size_t len);
void nine_nvs_host_reset(void);
void nine_store_test_clear(void);

const lv_font_t nine_cjk_16;
const lv_font_t nine_cjk_20;

static int failures;
#define CHECK(c, m) do { if (!(c)) { failures++; printf("FAIL: %s\n", m); } } while (0)

static nine_app_t app;

static void key(nine_app_t *a, int btn, int ev)
{
    nine_key_t k = { (uint8_t)btn, (uint8_t)ev };
    nine_app_key(a, &k);
}

static void click(nine_app_t *a, int btn) { key(a, btn, BSP_BTN_CLICK); }

static void long_press(nine_app_t *a, int btn)
{
    key(a, btn, BSP_BTN_LONG);
    key(a, btn, BSP_BTN_RELEASE);
}

// A genuinely new short press after a long press (the release click, if any,
// is swallowed by the app; the next physical gesture starts with PRESS).
static void new_click(nine_app_t *a, int btn)
{
    key(a, btn, BSP_BTN_PRESS);
    click(a, btn);
}

static void long_down_only(nine_app_t *a, int btn) { key(a, btn, BSP_BTN_LONG); }

typedef struct { uint16_t node; uint32_t index; uint16_t page; int kind; } pos_t;

static pos_t pos(const nine_app_t *a)
{
    pos_t p = { a->vm.state.node, a->vm.state.index, a->text_page, (int)a->vm.view.kind };
    return p;
}

static bool same(pos_t a, pos_t b)
{
    return a.node == b.node && a.index == b.index && a.page == b.page && a.kind == b.kind;
}

static void to_title(nine_app_t *a)
{
    if (a->page == GAL_PAGE_START) click(a, BSP_BTN_OK);
    if (a->page == GAL_PAGE_TITLE) return;
    if (a->page == GAL_PAGE_GAME || a->page == GAL_PAGE_CHOICE) {
        key(a, BSP_BTN_OK, BSP_BTN_LONG);
        key(a, BSP_BTN_OK, BSP_BTN_RELEASE);
    }
    if (a->page == GAL_PAGE_MENU) {
        a->menu_sel = 5;
        new_click(a, BSP_BTN_OK);
    }
}

static bool ensure_text(nine_app_t *a)
{
    for (int i = 0; i < 400; ++i) {
        if (a->page == GAL_PAGE_GAME) return true;
        if (a->page == GAL_PAGE_CHOICE) {
            click(a, BSP_BTN_OK);
            continue;
        }
        return false;
    }
    return false;
}

static void new_game(nine_app_t *a)
{
    to_title(a);
    a->title_sel = 0;
    click(a, BSP_BTN_OK);
}

static void open_menu(nine_app_t *a)
{
    if (a->page != GAL_PAGE_MENU) long_press(a, BSP_BTN_OK);
}

static void save_slot(nine_app_t *a, int slot)
{
    ensure_text(a);
    open_menu(a);
    a->menu_sel = 1;
    new_click(a, BSP_BTN_OK);
    CHECK(a->page == GAL_PAGE_SAVE, "save: slot page");
    a->save_sel = slot;
    click(a, BSP_BTN_OK);
    CHECK(a->slot_used[slot], "save: slot written");
    key(a, BSP_BTN_OK, BSP_BTN_LONG);
    key(a, BSP_BTN_OK, BSP_BTN_RELEASE);
    if (a->page == GAL_PAGE_MENU) {
        a->menu_sel = 0;
        new_click(a, BSP_BTN_OK);
    }
}

static void load_slot(nine_app_t *a, int slot)
{
    open_menu(a);
    a->menu_sel = 2;
    new_click(a, BSP_BTN_OK);
    CHECK(a->page == GAL_PAGE_LOAD, "load: page");
    a->load_sel = slot;
    click(a, BSP_BTN_OK);
}

static int drive_to_ending(nine_app_t *a)
{
    for (int i = 0; i < 30000; ++i) {
        if (a->page == GAL_PAGE_ENDING) return 1;
        if (a->page == GAL_PAGE_GAME) {
            click(a, BSP_BTN_OK);
            continue;
        }
        if (a->page == GAL_PAGE_CHOICE) {
            a->choice_sel = 0;
            click(a, BSP_BTN_OK);
            continue;
        }
        return 0;
    }
    return 0;
}

static void test_short_keys(void)
{
    new_game(&app);
    CHECK(app.page == GAL_PAGE_GAME, "short: new game starts reading");
    CHECK(app.text[0] != '\0', "short: real text loaded");
    CHECK(app.vm.state.flag[0] == 0, "short: fresh run flags");

    const int buttons[3] = { BSP_BTN_OK, BSP_BTN_UP, BSP_BTN_DOWN };
    for (int i = 0; i < 3; ++i) {
        CHECK(ensure_text(&app), "short: text page available");
        // Force the typewriter flag: the press must still move the cursor.
        app.ui.typing_active = true;
        const pos_t before = pos(&app);
        click(&app, buttons[i]);
        const pos_t after = pos(&app);
        CHECK(!same(before, after), "short: every short key advances while typing");
        CHECK(!gal_ui_typing(&app.ui), "short: destination page is fully revealed");
        if (app.page == GAL_PAGE_CHOICE) click(&app, BSP_BTN_OK);
    }

    // Pagination: a short press on a multi-page line moves to the next page,
    // not just to the end of the typewriter. Use the 20px tier, where real
    // source lines do split (16px lines fit in one 4-line page).
    new_game(&app);
    app.cfg.display.font_large = 1;
    app.shown_index = UINT32_MAX;
    bool found = false;
    for (int i = 0; i < 20 && !found; ++i) {
        CHECK(ensure_text(&app), "pagination: text page");
        click(&app, BSP_BTN_OK);
        if (app.page_count > 1 && app.text_page == 0) {
            found = true;
            const uint16_t page = app.text_page;
            app.ui.typing_active = true;
            click(&app, BSP_BTN_OK);
            CHECK(app.text_page == page + 1, "pagination: next complete page");
            CHECK(!gal_ui_typing(&app.ui), "pagination: complete page shown");
            break;
        }
    }
    CHECK(found, "pagination: real source line splits at the 20px tier");
    app.cfg.display.font_large = 0;
    app.shown_index = UINT32_MAX;
}

static void test_pagination_last_save_load(void)
{
    new_game(&app);
    app.cfg.display.font_large = 1;
    app.shown_index = UINT32_MAX;
    bool found = false;
    for (int i = 0; i < 80 && !found; ++i) {
        CHECK(ensure_text(&app), "pagelast: text");
        if (app.page_count > 1) { found = true; break; }
        click(&app, BSP_BTN_OK);
    }
    CHECK(found, "pagelast: multi-page line found");
    if (!found) return;
    const uint16_t last = (uint16_t)(app.page_count - 1);
    for (int i = 0; i < 20 && app.text_page < last; ++i) click(&app, BSP_BTN_OK);
    CHECK(app.text_page == last, "pagelast: reached last page");
    save_slot(&app, 2);
    CHECK(app.slot_used[2], "pagelast: saved last page");
    load_slot(&app, 2);
    CHECK(app.page == GAL_PAGE_GAME && app.text_page == last,
          "pagelast: load restores last page");
    app.cfg.display.font_large = 0;
    app.shown_index = UINT32_MAX;
    ensure_text(&app);
    CHECK(app.text_page < (uint16_t)app.page_count, "pagelast: font-switch page clamp");
    app.cfg.display.font_large = 0;
    app.shown_index = UINT32_MAX;
}

static void test_auto_and_manual(void)
{
    CHECK(ensure_text(&app), "auto: text page");
    long_press(&app, BSP_BTN_DOWN);
    CHECK(app.auto_mode, "auto: long DOWN enables");
    CHECK(!gal_ui_typing(&app.ui), "auto: current page revealed");
    const pos_t before = pos(&app);
    nine_app_tick(&app, 100);
    CHECK(same(before, pos(&app)), "auto: waits for the 900ms interval");
    nine_app_tick(&app, 800);
    CHECK(!same(before, pos(&app)), "auto: advances after interval");
    const pos_t after = pos(&app);
    nine_app_tick(&app, 50);
    CHECK(same(after, pos(&app)), "auto: release does not add a step");

    // Manual short press while auto is enabled: stops auto AND advances.
    const pos_t manual = pos(&app);
    click(&app, BSP_BTN_OK);
    CHECK(!app.auto_mode, "auto: short press exits auto");
    CHECK(!same(manual, pos(&app)), "auto: short press still advances");
}

static void test_fast_forward(void)
{
    CHECK(ensure_text(&app), "ff: text page");
    if (app.page == GAL_PAGE_CHOICE) click(&app, BSP_BTN_OK);
    const pos_t before = pos(&app);
    long_down_only(&app, BSP_BTN_UP);
    CHECK(app.fast_forward, "ff: long UP starts");
    CHECK(!same(before, pos(&app)), "ff: threshold advances next sentence immediately");
    CHECK(!app.auto_mode, "ff: auto cleared");
    app.ui.typing_active = true;
    const pos_t first = pos(&app);
    nine_app_tick(&app, 1);
    CHECK(!same(first, pos(&app)), "ff: one sentence per tick without 90ms wait");
    CHECK(!gal_ui_typing(&app.ui), "ff: destination revealed");
    CHECK(!gal_ui_typing(&app.ui), "ff: destination revealed");
    const pos_t running = pos(&app);
    key(&app, BSP_BTN_UP, BSP_BTN_RELEASE);
    CHECK(!app.fast_forward, "ff: release stops");
    nine_app_tick(&app, 500);
    CHECK(same(running, pos(&app)), "ff: no movement after release");
}

static void test_instant_ff_pages(void)
{
    new_game(&app);
    CHECK(ensure_text(&app), "ffpages: text");
    app.cfg.display.font_large = 1;
    app.shown_index = UINT32_MAX;
    bool found = false;
    for (int i = 0; i < 80 && !found; ++i) {
        CHECK(ensure_text(&app), "ffpages: text probe");
        if (app.page_count > 1 && app.text_page == 0) { found = true; break; }
        click(&app, BSP_BTN_OK);
    }
    CHECK(found, "ffpages: multi-page line found");
    if (found) {
        click(&app, BSP_BTN_OK);
        CHECK(app.text_page == 1, "ffpages: moved to middle page");
        const pos_t before = pos(&app);
        long_down_only(&app, BSP_BTN_UP);
        CHECK(app.fast_forward, "ffpages: hold starts");
        CHECK(app.text_page == 0, "ffpages: remaining pages skipped");
        CHECK(!same(before, pos(&app)), "ffpages: next sentence at threshold");
        const pos_t one = pos(&app);
        nine_app_tick(&app, 1);
        CHECK(!same(one, pos(&app)), "ffpages: continuous tick next sentence");
        const pos_t held = pos(&app);
        key(&app, BSP_BTN_UP, BSP_BTN_RELEASE);
        CHECK(!app.fast_forward, "ffpages: release stops");
        nine_app_tick(&app, 500);
        CHECK(same(held, pos(&app)), "ffpages: no extra after release");
    }
    app.cfg.display.font_large = 0;
    app.shown_index = UINT32_MAX;

    // Fast-forward from the beginning until the first decision; the destination
    // choice must stop the hold and no option may be selected by FF itself.
    new_game(&app);
    CHECK(ensure_text(&app), "ffstop: text");
    long_down_only(&app, BSP_BTN_UP);
    for (int i = 0; i < 5000 && app.page == GAL_PAGE_GAME && app.fast_forward; ++i)
        nine_app_tick(&app, 1);
    CHECK(app.page == GAL_PAGE_CHOICE || app.page == GAL_PAGE_ENDING,
          "ffstop: reaches decision/end");
    CHECK(!app.fast_forward, "ffstop: hold stops at destination");
    if (app.page == GAL_PAGE_CHOICE) {
        CHECK(app.choice_sel == 0, "ffstop: no option auto-selected");
        const pos_t before = pos(&app);
        nine_app_tick(&app, 500);
        CHECK(same(before, pos(&app)), "ffstop: no post-stop movement");
    }
    key(&app, BSP_BTN_UP, BSP_BTN_RELEASE);
}

static void test_long_press_and_menu(void)
{
    CHECK(ensure_text(&app), "menu: text page");
    const pos_t before = pos(&app);
    key(&app, BSP_BTN_OK, BSP_BTN_LONG);
    CHECK(app.page == GAL_PAGE_MENU, "menu: long OK opens");
    // The component may emit a click on release; it must not advance or run a row.
    key(&app, BSP_BTN_OK, BSP_BTN_RELEASE);
    click(&app, BSP_BTN_OK);
    CHECK(app.page == GAL_PAGE_MENU, "menu: release click suppressed");
    // A real new press clears the suppression and resumes.
    key(&app, BSP_BTN_OK, BSP_BTN_PRESS);
    click(&app, BSP_BTN_OK);
    CHECK(app.page == GAL_PAGE_GAME, "menu: new short press resumes");
    CHECK(same(before, pos(&app)), "menu: resume keeps position");
}

static void test_choice_stop(void)
{
    for (int i = 0; i < 4000 && app.page != GAL_PAGE_CHOICE; ++i) {
        CHECK(ensure_text(&app), "choice: walk text");
        click(&app, BSP_BTN_OK);
    }
    CHECK(app.page == GAL_PAGE_CHOICE, "choice: reached a choice");
    const pos_t before = pos(&app);
    key(&app, BSP_BTN_UP, BSP_BTN_LONG);
    nine_app_tick(&app, 500);
    CHECK(app.page == GAL_PAGE_CHOICE, "choice: fast-forward stops");
    CHECK(same(before, pos(&app)), "choice: no option auto-selected");
    key(&app, BSP_BTN_UP, BSP_BTN_RELEASE);

    const int32_t flags = app.vm.state.flag[0];
    click(&app, BSP_BTN_OK);
    CHECK(app.page != GAL_PAGE_CHOICE, "choice: short OK commits");
    CHECK(flags != app.vm.state.flag[0] || app.vm.state.index != before.index ||
          app.vm.state.node != before.node, "choice: state changed");
}

static void test_save_load_and_corruption(void)
{
    new_game(&app);
    CHECK(ensure_text(&app), "save: text");
    for (int i = 0; i < 5; ++i) click(&app, BSP_BTN_OK);
    ensure_text(&app);
    save_slot(&app, 0);
    const pos_t saved = pos(&app);

    // A save on a later page of a paginated line must restore that page.
    app.cfg.display.font_large = 1;
    app.shown_index = UINT32_MAX;
    bool multipage = false;
    for (int i = 0; i < 40 && !multipage; ++i) {
        CHECK(ensure_text(&app), "multipage: text");
        if (app.page_count > 1) {
            click(&app, BSP_BTN_OK);
            CHECK(app.text_page == 1, "multipage: moved to page 2");
            save_slot(&app, 1);
            const uint16_t page = app.text_page;
            click(&app, BSP_BTN_OK);
            ensure_text(&app);
            load_slot(&app, 1);
            CHECK(app.text_page == page, "multipage: load restores page");
            multipage = true;
            break;
        }
        click(&app, BSP_BTN_OK);
    }
    CHECK(multipage, "multipage: found a paginated line");
    app.cfg.display.font_large = 0;
    app.shown_index = UINT32_MAX;

    for (int i = 0; i < 7; ++i) {
        if (app.page == GAL_PAGE_CHOICE) click(&app, BSP_BTN_OK);
        else click(&app, BSP_BTN_OK);
    }
    ensure_text(&app);
    CHECK(!same(saved, pos(&app)), "load: advanced away");
    load_slot(&app, 0);
    CHECK(app.page == GAL_PAGE_GAME, "load: returns to reader");
    CHECK(same(saved, pos(&app)), "load: exact position restored");

    // Corrupt the stored node and verify the live state does not move.
    uint8_t blob[1024];
    size_t len = 0;
    CHECK(nine_nvs_host_get("slot0", blob, sizeof(blob), &len) == 0 && len > 40,
          "corrupt: read slot");
    blob[20] ^= 0x7f;
    CHECK(nine_nvs_host_put("slot0", blob, len) == 0, "corrupt: write slot");
    const pos_t live = pos(&app);
    open_menu(&app);
    app.menu_sel = 2;
    new_click(&app, BSP_BTN_OK);
    app.load_sel = 0;
    click(&app, BSP_BTN_OK);
    CHECK(same(live, pos(&app)), "corrupt: live state untouched");
    CHECK(strstr(nine_ui_notice, "损坏") != NULL, "corrupt: visible error");
}

static void test_continue(void)
{
    new_game(&app);
    for (int i = 0; i < 20; ++i) click(&app, BSP_BTN_OK);
    ensure_text(&app);
    nine_app_flush(&app);
    uint8_t auto_blob[1024];
    size_t auto_len = 0;
    CHECK(nine_nvs_host_get("auto", auto_blob, sizeof(auto_blob), &auto_len) == 0,
          "continue: auto record exists");
    const pos_t expected = pos(&app);

    nine_image_deinit();
    nine_media_deinit();
    nine_store_test_clear();
    nine_app_t app2;
    CHECK(nine_app_init(&app2), "continue: second boot");
    nine_image_test_disable_decode(1);
    click(&app2, BSP_BTN_OK);
    CHECK(app2.page == GAL_PAGE_TITLE, "continue: title");
    app2.title_sel = 1;
    click(&app2, BSP_BTN_OK);
    CHECK(app2.page == GAL_PAGE_GAME, "continue: resumes reading");
    CHECK(app2.vm.state.node == expected.node && app2.vm.state.index == expected.index,
          "continue: exact cursor");
    CHECK(app2.text_page == expected.page, "continue: exact page");
}

static void test_ending_and_preview(void)
{
    nine_image_deinit();
    nine_media_deinit();
    nine_store_test_clear();
    CHECK(nine_app_init(&app), "ending: boot");
    nine_image_test_disable_decode(1);
    click(&app, BSP_BTN_OK);
    new_game(&app);
    CHECK(drive_to_ending(&app), "ending: reached @end");
    CHECK(app.page == GAL_PAGE_ENDING, "ending: page");
    CHECK((app.unlock.endings & 0x3u) == 0x3u, "ending: both permanent gates");
    CHECK(app.cfg.title_bg == 2, "ending: true visual unlocked");
    CHECK(strstr(nine_ui_ending[0], "TRUE") != NULL, "ending: true ending card");

    click(&app, BSP_BTN_OK);
    CHECK(app.page == GAL_PAGE_TITLE, "ending: returns to title");
    click(&app, BSP_BTN_OK);
    new_game(&app);
    CHECK(app.page == GAL_PAGE_GAME, "ending: new game after clear");
    bool any_flag = false;
    for (uint32_t i = 0; i < app.pack.key_count; ++i) any_flag |= app.vm.state.flag[i] != 0;
    CHECK(!any_flag, "ending: new run flags cleared");
    CHECK(app.vm.state.progress[app.key_loop1] != 0, "ending: loop1 progress retained");
    CHECK(app.vm.state.progress[app.key_true] != 0, "ending: true progress retained");

    // Chapter preview must not touch the automatic save or permanent record.
    uint8_t auto_before[1024], perm_before[1024];
    size_t auto_len = 0, perm_len = 0;
    CHECK(nine_nvs_host_get("auto", auto_before, sizeof(auto_before), &auto_len) == 0,
          "preview: auto record");
    if (nine_nvs_host_get("perm", perm_before, sizeof(perm_before), &perm_len) != 0) {
        perm_len = 0;
    }
    to_title(&app);
    app.title_sel = 4;
    click(&app, BSP_BTN_OK);
    CHECK(app.page == GAL_PAGE_GALLERY, "preview: gallery");
    app.gallery_sel = 1;
    click(&app, BSP_BTN_OK);
    CHECK(app.page == GAL_PAGE_CHAPTER, "preview: chapter list");
    app.chapter_sel = 3;
    click(&app, BSP_BTN_OK);
    CHECK(app.preview && app.page == GAL_PAGE_GAME, "preview: chapter starts isolated");
    for (int i = 0; i < 6; ++i) click(&app, BSP_BTN_OK);
    key(&app, BSP_BTN_OK, BSP_BTN_LONG);
    key(&app, BSP_BTN_OK, BSP_BTN_RELEASE);
    app.menu_sel = 5;
    new_click(&app, BSP_BTN_OK);
    CHECK(app.page == GAL_PAGE_TITLE, "preview: exits to title");
    uint8_t after[1024];
    size_t after_len = 0;
    CHECK(nine_nvs_host_get("auto", after, sizeof(after), &after_len) == 0 &&
          after_len == auto_len && memcmp(after, auto_before, auto_len) == 0,
          "preview: automatic save unchanged");
    if (perm_len) {
        CHECK(nine_nvs_host_get("perm", after, sizeof(after), &after_len) == 0 &&
              after_len == perm_len && memcmp(after, perm_before, perm_len) == 0,
              "preview: permanent record unchanged");
    }
}

static bool drive_to_opening(nine_app_t *a)
{
    for (int i = 0; i < 8000; ++i) {
        if (a->opening) return true;
        if (a->page == GAL_PAGE_GAME) {
            click(a, BSP_BTN_OK);
        } else if (a->page == GAL_PAGE_CHOICE) {
            a->choice_sel = 0;
            click(a, BSP_BTN_OK);
        } else {
            return false;
        }
    }
    return false;
}

static void test_op_gallery_and_story(void)
{
    nine_image_deinit();
    nine_media_deinit();
    nine_store_test_clear();
    nine_app_t a;
    CHECK(nine_app_init(&a), "op: boot");
    nine_image_test_disable_decode(1);
    nine_ui_stub_reset();

    click(&a, BSP_BTN_OK);
    CHECK(a.page == GAL_PAGE_TITLE, "op: title");
    a.title_sel = 4;
    click(&a, BSP_BTN_OK);
    CHECK(a.page == GAL_PAGE_GALLERY, "op: gallery");
    a.gallery_sel = 3;
    click(&a, BSP_BTN_OK);
    CHECK(a.page == GAL_PAGE_CG && a.opening && a.op_gallery,
          "op: Gallery OP launches real playback");
    CHECK(a.op_duration >= 110180u, "op: full source duration retained");
    CHECK(nine_media_current_track() == 1u, "op: opening track starts");
    const nine_state_t vm_before = a.vm.state;
    const nine_unlock_blob_t unlock_before = a.unlock;
    uint8_t auto_before[1024], perm_before[1024];
    size_t auto_len = 0, perm_len = 0;
    CHECK(nine_nvs_host_get("auto", auto_before, sizeof(auto_before), &auto_len) == 0,
          "op: auto baseline");
    CHECK(nine_nvs_host_get("perm", perm_before, sizeof(perm_before), &perm_len) == 0,
          "op: permanent baseline");
    const int selected = a.gallery_sel;

    // A long late interval must select only the current frame; the bounded
    // diagnostic counts skipped source frames but there is no catch-up burst.
    nine_media_test_advance(1200);
    nine_app_tick(&a, 1);
    CHECK(a.opening && a.op_frames_drawn >= 1, "op: current frame shown");
    CHECK(a.op_late_skips >= 1, "op: late frames counted");
    nine_media_test_advance(110200);
    nine_app_tick(&a, 1);
    CHECK(a.page == GAL_PAGE_GALLERY && !a.opening && !a.op_gallery,
          "op: natural end returns Gallery");
    CHECK(a.gallery_sel == selected, "op: Gallery selection preserved");
    CHECK(memcmp(&a.vm.state, &vm_before, sizeof(vm_before)) == 0,
          "op: live VM untouched");
    CHECK(memcmp(&a.unlock, &unlock_before, sizeof(unlock_before)) == 0,
          "op: unlock untouched");
    uint8_t after[1024];
    size_t after_len = 0;
    CHECK(nine_nvs_host_get("auto", after, sizeof(after), &after_len) == 0 &&
          after_len == auto_len && memcmp(after, auto_before, auto_len) == 0,
          "op: auto save untouched");
    CHECK(nine_nvs_host_get("perm", after, sizeof(after), &after_len) == 0 &&
          after_len == perm_len && memcmp(after, perm_before, perm_len) == 0,
          "op: permanent record untouched");

    // Replay and skip by a short OK; no phantom release click and no state write.
    a.gallery_sel = 3;
    click(&a, BSP_BTN_OK);
    CHECK(a.opening && a.op_gallery, "op: replay starts");
    click(&a, BSP_BTN_OK);
    CHECK(a.page == GAL_PAGE_GALLERY && !a.opening, "op: short OK returns Gallery");
    key(&a, BSP_BTN_UP, BSP_BTN_RELEASE);
    CHECK(a.page == GAL_PAGE_GALLERY && a.gallery_sel == 3,
          "op: release does not add a phantom click");
    CHECK(memcmp(&a.vm.state, &vm_before, sizeof(vm_before)) == 0,
          "op: replay/skip leaves VM untouched");

    // A failed first frame must not leave stale art, advance the VM, or start a
    // silent audio-only OP.
    nine_image_test_fail_show(1);
    a.gallery_sel = 3;
    click(&a, BSP_BTN_OK);
    CHECK(a.page == GAL_PAGE_GALLERY && !a.opening && !a.op_gallery,
          "op: failed Gallery frame returns with no playback");
    CHECK(strstr(nine_ui_notice, "片头") != NULL, "op: failed Gallery frame visible");
    CHECK(memcmp(&a.vm.state, &vm_before, sizeof(vm_before)) == 0,
          "op: failed Gallery frame leaves VM untouched");
    nine_image_test_fail_show(0);

    // The graph OP must advance the live VM exactly once on natural end.
    key(&a, BSP_BTN_OK, BSP_BTN_LONG);
    key(&a, BSP_BTN_OK, BSP_BTN_RELEASE);
    CHECK(a.page == GAL_PAGE_TITLE, "op: back to title");
    a.title_sel = 0;
    new_click(&a, BSP_BTN_OK);
    CHECK(drive_to_opening(&a), "op: story reaches graph OP");
    CHECK(!a.op_gallery, "op: graph OP is not Gallery mode");
    const uint16_t node_before = a.vm.state.node;
    const uint32_t index_before = a.vm.state.index;
    nine_media_test_advance(110200);
    nine_app_tick(&a, 1);
    CHECK(!a.opening, "op: graph OP natural end");
    CHECK(a.vm.state.node != node_before || a.vm.state.index != index_before,
          "op: graph OP advanced the VM once");
    CHECK(a.page == GAL_PAGE_GAME || a.page == GAL_PAGE_CHOICE ||
          a.page == GAL_PAGE_ENDING, "op: graph OP lands on next view");

    nine_media_deinit();
    nine_image_deinit();
}

static void boot_to_graph_op(nine_app_t *a)
{
    nine_image_deinit();
    nine_media_deinit();
    nine_store_test_clear();
    CHECK(nine_app_init(a), "opusfault: boot");
    nine_image_test_disable_decode(1);
    nine_ui_stub_reset();
    click(a, BSP_BTN_OK);
    CHECK(a->page == GAL_PAGE_TITLE, "opusfault: title");
    a->title_sel = 0;
    new_click(a, BSP_BTN_OK);
    CHECK(drive_to_opening(a), "opusfault: legal NewGame reaches OP");
    CHECK(a->media_ok && a->opening && nine_media_current_track() == 1u,
          "opusfault: opening audio active");
}

static void test_op_audio_fault_wins(void)
{
    nine_app_t a;
    boot_to_graph_op(&a);
    const nine_state_t vm_before = a.vm.state;
    const nine_unlock_blob_t unlock_before = a.unlock;
    uint8_t auto_before[1024], perm_before[1024];
    size_t auto_len = 0, perm_len = 0;
    CHECK(nine_nvs_host_get("auto", auto_before, sizeof(auto_before), &auto_len) == 0,
          "opusfault: auto baseline");
    CHECK(nine_nvs_host_get("perm", perm_before, sizeof(perm_before), &perm_len) == 0,
          "opusfault: perm baseline");

    // Simulated HAL fault arrives before the legal short OK that would skip.
    nine_media_test_fail(1);
    click(&a, BSP_BTN_OK);
    CHECK(!a.opening, "opusfault: short key consumed by audio fault");
    CHECK(a.resource_error && a.page == GAL_PAGE_GAME, "opusfault: visible fault");
    CHECK(strstr(a.error, "音频") != NULL, "opusfault: retained audio error");
    CHECK(memcmp(&a.vm.state, &vm_before, sizeof(vm_before)) == 0,
          "opusfault: fault-before-skip does not advance VM");
    CHECK(memcmp(&a.unlock, &unlock_before, sizeof(unlock_before)) == 0,
          "opusfault: no unlock");
    uint8_t after[1024];
    size_t after_len = 0;
    CHECK(nine_nvs_host_get("auto", after, sizeof(after), &after_len) == 0 &&
          after_len == auto_len && memcmp(after, auto_before, auto_len) == 0,
          "opusfault: no automatic save");
    CHECK(nine_nvs_host_get("perm", after, sizeof(after), &after_len) == 0 &&
          after_len == perm_len && memcmp(after, perm_before, perm_len) == 0,
          "opusfault: no permanent progress");
    const nine_state_t after_release = a.vm.state;
    key(&a, BSP_BTN_UP, BSP_BTN_RELEASE);
    CHECK(memcmp(&a.vm.state, &after_release, sizeof(after_release)) == 0,
          "opusfault: release after fault has no extra action");

    // Fresh legal NewGame -> OP: fault must also beat natural end in the tick.
    boot_to_graph_op(&a);
    const nine_state_t end_before = a.vm.state;
    nine_media_test_advance(110200);
    nine_media_test_fail(1);
    nine_app_tick(&a, 1);
    CHECK(!a.opening && a.resource_error, "opusfault: fault wins natural end");
    CHECK(strstr(a.error, "音频") != NULL, "opusfault: natural-end error retained");
    CHECK(memcmp(&a.vm.state, &end_before, sizeof(end_before)) == 0,
          "opusfault: fault-before-end does not advance VM");
    nine_media_test_fail(0);
    nine_media_deinit();
    nine_image_deinit();
}

static void test_image_failure_blocks(void)
{
    nine_image_deinit();
    nine_media_deinit();
    nine_store_test_clear();
    nine_app_t a;
    nine_image_test_disable_decode(1);
    CHECK(nine_app_init(&a), "imagefail: boot");
    CHECK(a.image_ok, "imagefail: band layer available");
    click(&a, BSP_BTN_OK);
    new_game(&a);
    CHECK(a.page == GAL_PAGE_GAME, "imagefail: reader");
    uint8_t auto_before[1024], perm_before[1024];
    size_t auto_len = 0, perm_len = 0;
    CHECK(nine_nvs_host_get("auto", auto_before, sizeof(auto_before), &auto_len) == 0,
          "imagefail: auto baseline");
    CHECK(nine_nvs_host_get("perm", perm_before, sizeof(perm_before), &perm_len) == 0,
          "imagefail: perm baseline");
    const nine_unlock_blob_t unlock_before = a.unlock;

    a.visual_valid = false;
    nine_image_test_fail_show(1);
    click(&a, BSP_BTN_OK);
    CHECK(a.image_error && a.resource_error, "imagefail: error state set");
    CHECK(!a.progress_dirty, "imagefail: failed frame not marked dirty");
    CHECK(strstr(nine_ui_notice, "图像") != NULL, "imagefail: visible notice");
    const pos_t blocked = pos(&a);
    long_down_only(&a, BSP_BTN_UP);
    CHECK(!a.fast_forward, "imagefail: FF refused on failed frame");
    CHECK(same(blocked, pos(&a)), "imagefail: FF does not advance bad resource");
    nine_app_flush(&a);
    uint8_t after[1024];
    size_t after_len = 0;
    CHECK(nine_nvs_host_get("auto", after, sizeof(after), &after_len) == 0 &&
          after_len == auto_len && memcmp(after, auto_before, auto_len) == 0,
          "imagefail: no automatic save from failed frame");
    CHECK(nine_nvs_host_get("perm", after, sizeof(after), &after_len) == 0 &&
          after_len == perm_len && memcmp(after, perm_before, perm_len) == 0,
          "imagefail: no permanent progress from failed frame");
    CHECK(memcmp(&unlock_before, &a.unlock, sizeof(a.unlock)) == 0,
          "imagefail: no unlock from failed frame");

    // Manual save must also refuse the undisplayed frame.
    open_menu(&a);
    a.menu_sel = 1;
    new_click(&a, BSP_BTN_OK);
    CHECK(a.page == GAL_PAGE_SAVE, "imagefail: save page opens for message");
    a.save_sel = 0;
    click(&a, BSP_BTN_OK);
    CHECK(!a.slot_used[0], "imagefail: manual save refused");
    CHECK(strstr(nine_ui_notice, "不能存档") != NULL, "imagefail: save refusal visible");
    long_press(&a, BSP_BTN_OK);
    a.menu_sel = 0;
    new_click(&a, BSP_BTN_OK);
    CHECK(same(blocked, pos(&a)), "imagefail: return to failed frame");

    click(&a, BSP_BTN_OK);
    CHECK(same(blocked, pos(&a)), "imagefail: retry does not advance the cursor");
    CHECK(strstr(nine_ui_notice, "图像") != NULL, "imagefail: error remains visible");
    nine_image_test_fail_show(0);
    click(&a, BSP_BTN_OK);
    CHECK(same(blocked, pos(&a)), "imagefail: successful retry restores the same frame");
    CHECK(!a.image_error && !a.resource_error, "imagefail: retry clears resource error");
    CHECK(a.progress_dirty, "imagefail: restored frame can now commit progress");
    click(&a, BSP_BTN_OK);
    CHECK(!same(blocked, pos(&a)), "imagefail: following press advances normally");
    nine_image_deinit();
    nine_media_deinit();
}

static void test_image_init_failure(void)
{
    nine_image_deinit();
    nine_media_deinit();
    nine_store_test_clear();
    nine_app_t a;
    nine_image_test_fail_allocation(1);
    CHECK(nine_app_init(&a), "imageinit: boot returns with visible error");
    CHECK(a.fatal && a.resource_fatal && !a.image_ok, "imageinit: unreadable state");
    CHECK(nine_image_canvas_bytes() == 0, "imageinit: failed allocation released bands");
    CHECK(strstr(a.error, "图像") != NULL, "imageinit: CJK error visible");
    const pos_t before = pos(&a);
    new_game(&a);
    CHECK(a.page == GAL_PAGE_START, "imageinit: New Game refused");
    click(&a, BSP_BTN_OK);
    CHECK(same(before, pos(&a)), "imageinit: short keys refused");
    nine_image_test_fail_allocation(0);
    nine_image_deinit();
    nine_media_deinit();
}

static void test_save_blob_layout(void)
{
    nine_vm_t v, v2;
    CHECK(nine_vm_init(&v, &app.pack), "blob: vm init");
    CHECK(nine_vm_init(&v2, &app.pack), "blob: vm2 init");
    v.state.node = 0;
    v.state.index = 0;
    for (uint32_t i = 0; i < app.pack.key_count; ++i) {
        v.state.flag[i] = (int32_t)(i * 3 - 7);
        v.state.progress[i] = (int32_t)(i * 5 + 1);
    }
    v.state.flag_set[3] = 0x5a;
    v.state.progress_set[4] = 0xa5;
    uint8_t blob[1600];
    const size_t n = nine_vm_save_encode(&v, 2, blob, sizeof(blob));
    CHECK(n > 0 && n < sizeof(blob), "blob: encoded");
    uint16_t node, page;
    uint32_t index;
    bool preview;
    CHECK(nine_vm_save_peek(&app.pack, blob, n, &node, &index, &page, &preview),
          "blob: peek validates");
    CHECK(nine_vm_save_decode(&v2, blob, n, &page), "blob: decodes");
    bool same = true;
    for (uint32_t i = 0; i < app.pack.key_count; ++i)
        same = same && v.state.flag[i] == v2.state.flag[i] &&
               v.state.progress[i] == v2.state.progress[i];
    CHECK(same, "blob: all variables round-trip");
    CHECK(memcmp(v.state.flag_set, v2.state.flag_set, sizeof(v.state.flag_set)) == 0,
          "blob: flag bitset round-trip");
    CHECK(memcmp(v.state.progress_set, v2.state.progress_set,
                 sizeof(v.state.progress_set)) == 0,
          "blob: progress bitset round-trip");

    nine_vm_t before = v2;
    blob[n - 1] ^= 0x40;
    CHECK(!nine_vm_save_decode(&v2, blob, n, &page), "blob: bad CRC rejected");
    CHECK(memcmp(&before.state, &v2.state, sizeof(v2.state)) == 0,
          "blob: bad CRC leaves live state unchanged");

    uint8_t progress[600];
    const size_t pn = nine_vm_progress_encode(&v, progress, sizeof(progress));
    CHECK(pn > 0 && pn < sizeof(progress), "blob: progress encoded");
    CHECK(nine_vm_progress_decode(&v2, progress, pn), "blob: progress decodes");
    same = true;
    for (uint32_t i = 0; i < app.pack.key_count; ++i)
        same = same && v.state.progress[i] == v2.state.progress[i];
    CHECK(same, "blob: progress values round-trip");
    CHECK(memcmp(v.state.progress_set, v2.state.progress_set,
                 sizeof(v.state.progress_set)) == 0,
          "blob: progress bitset round-trip");
}

static void test_bad_pack(void)
{
    FILE *f = fopen("assets/nine/yukiiro/nine_script.bin", "rb");
    CHECK(f != NULL, "pack: open fixture");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = (uint8_t *)malloc((size_t)n);
    CHECK(data && fread(data, 1, (size_t)n, f) == (size_t)n, "pack: read fixture");
    fclose(f);
    if (!data) return;
    nine_pack_t pack;
    CHECK(nine_pack_open(&pack, data, (size_t)n), "pack: valid opens");
    data[100] ^= 0x55;
    CHECK(!nine_pack_open(&pack, data, (size_t)n), "pack: corrupt CRC rejected");
    data[100] ^= 0x55;
    CHECK(!nine_pack_open(&pack, data, (size_t)n - 4), "pack: short data rejected");
    free(data);
}

int main(void)
{
    nine_nvs_host_reset();
    nine_store_test_clear();
    nine_ui_stub_reset();
    CHECK(nine_assets_host_load("assets/nine/yukiiro/nine_script.bin",
                                "assets/nine/yukiiro/nine_images.bin"),
          "assets: load real packs");
    CHECK(nine_app_init(&app), "app: init");
    CHECK(!app.fatal, "app: pack accepted");
    // State-machine tests keep the image layer logically ready but skip real
    // pixel decoding; the real-LVGL test covers the production compositor.
    nine_image_test_disable_decode(1);
    nine_ui_stub_reset();

    test_short_keys();
    test_pagination_last_save_load();
    test_auto_and_manual();
    test_fast_forward();
    test_instant_ff_pages();
    test_long_press_and_menu();
    test_choice_stop();
    test_save_load_and_corruption();
    test_continue();
    test_ending_and_preview();
    test_op_gallery_and_story();
    test_op_audio_fault_wins();
    test_image_failure_blocks();
    test_image_init_failure();
    test_save_blob_layout();
    test_bad_pack();

    nine_media_deinit();
    nine_image_deinit();
    if (failures) {
        printf("FAILED: %d checks\n", failures);
        return 1;
    }
    printf("PASS: nine reader integration\n");
    return 0;
}
