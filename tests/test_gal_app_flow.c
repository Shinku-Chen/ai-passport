#include "gal_app.h"
#include "gal_content.h"
#include "bsp_button.h"
#include "lvgl.h"
#include "nvs.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c, m) do { if (!(c)) { failures++; printf("FAIL: %s\n", m); } } while (0)

static void key(gal_app_t *app, int btn, int ev)
{
    gal_key_t k = { (uint8_t)btn, (uint8_t)ev };
    gal_app_key(app, &k);
}

static bool advance_until(gal_app_t *app, gal_page_t target)
{
    for (int i = 0; i < 2000 && app->page != target; ++i) {
        if (app->page == GAL_PAGE_GAME || app->page == GAL_PAGE_TRANSITION) {
            key(app, BSP_BTN_OK, BSP_BTN_CLICK);
        } else {
            return false;
        }
    }
    return app->page == target;
}

static void to_title(gal_app_t *app)
{
    for (int guard = 0; guard < 24 && app->page != GAL_PAGE_TITLE; ++guard) {
        switch (app->page) {
        case GAL_PAGE_START:
            key(app, BSP_BTN_OK, BSP_BTN_CLICK);
            break;
        case GAL_PAGE_GAME:
            key(app, BSP_BTN_OK, BSP_BTN_LONG);
            break;
        case GAL_PAGE_CHOICE:
            key(app, BSP_BTN_OK, BSP_BTN_LONG);
            break;
        case GAL_PAGE_TRANSITION:
            key(app, BSP_BTN_OK, BSP_BTN_CLICK);
            break;
        case GAL_PAGE_ENDING:
            key(app, BSP_BTN_OK, BSP_BTN_CLICK);
            break;
        case GAL_PAGE_MENU:
            app->menu_sel = GAL_MENU_TITLE;
            key(app, BSP_BTN_OK, BSP_BTN_CLICK);
            break;
        case GAL_PAGE_GALLERY:
        case GAL_PAGE_SAVE:
        case GAL_PAGE_LOAD:
        case GAL_PAGE_SETTINGS:
        case GAL_PAGE_HISTORY:
        case GAL_PAGE_ABOUT:
            key(app, BSP_BTN_OK, BSP_BTN_LONG);
            break;
        case GAL_PAGE_CHAPTER:
            key(app, BSP_BTN_OK, BSP_BTN_LONG);
            break;
        default:
            key(app, BSP_BTN_OK, BSP_BTN_LONG);
            break;
        }
    }
}

static void start_new_game(gal_app_t *app)
{
    to_title(app);
    app->title_sel = GAL_TITLE_MENU_NEW;
    key(app, BSP_BTN_OK, BSP_BTN_CLICK);
}

static void open_chapter_page(gal_app_t *app)
{
    to_title(app);
    app->title_sel = GAL_TITLE_MENU_GALLERY;
    key(app, BSP_BTN_OK, BSP_BTN_CLICK);
    app->gallery_sel = GAL_GALLERY_STORY;
    key(app, BSP_BTN_OK, BSP_BTN_CLICK);
}

static void save_slot0(gal_app_t *app)
{
    CHECK(app->page == GAL_PAGE_GAME, "save: starts in reader");
    key(app, BSP_BTN_OK, BSP_BTN_LONG);
    CHECK(app->page == GAL_PAGE_MENU, "save: game -> menu");
    app->menu_sel = GAL_MENU_SAVE;
    key(app, BSP_BTN_OK, BSP_BTN_CLICK);
    CHECK(app->page == GAL_PAGE_SAVE, "save: menu -> slot page");
    app->save_sel = 0;
    key(app, BSP_BTN_OK, BSP_BTN_CLICK);
    CHECK(app->save.slots[0].occupied, "save: slot 0 occupied");
    key(app, BSP_BTN_OK, BSP_BTN_LONG);
    key(app, BSP_BTN_OK, BSP_BTN_LONG);
    CHECK(app->page == GAL_PAGE_GAME, "save: return to game");
}

static bool history_has(const gal_app_t *app, const char *needle)
{
    for (int i = 0; i < app->history_count; ++i) {
        if (app->history[i] && strstr(app->history[i]->text, needle)) return true;
    }
    return false;
}

static void run_first_playthrough(gal_app_t *app)
{
    start_new_game(app);
    CHECK(app->page == GAL_PAGE_GAME, "first run: new game");
    CHECK(advance_until(app, GAL_PAGE_CHOICE), "first run: first choice");
    app->choice_sel = 0;
    key(app, BSP_BTN_OK, BSP_BTN_CLICK);
    CHECK(app->flags == (1u << GAL_FLAG_CHOICE_A), "first run: choice A flag");
    save_slot0(app);
    CHECK(app->save.slots[0].flags == (1u << GAL_FLAG_CHOICE_A),
          "first run: manual slot stores run flags");

    CHECK(advance_until(app, GAL_PAGE_CHOICE), "first run: second choice");
    CHECK(history_has(app, "追上去的那一步"), "first run: A line visible");
    CHECK(!history_has(app, "留在原地也不算坏"), "first run: B line absent");
    CHECK(app->ui.lists[GAL_LIST_CHOICE].disabled[0],
          "first run: CLEARED option is disabled before ending");
    app->choice_sel = 0;
    key(app, BSP_BTN_OK, BSP_BTN_CLICK);
    CHECK(app->page == GAL_PAGE_CHOICE, "first run: locked CLEARED option cannot be chosen");
    CHECK(strstr(app->notice, "条件不足") != NULL,
          "first run: locked CLEARED option shows condition notice");

    app->choice_sel = 1;
    key(app, BSP_BTN_OK, BSP_BTN_CLICK);
    CHECK(advance_until(app, GAL_PAGE_ENDING), "first run: reaches ending");
    CHECK(gal_unlock_has_ending(&app->unlock, 0), "first run: permanent ending granted");
    CHECK(gal_unlock_has_any_ending(&app->unlock), "first run: CLEARED qualification granted");
    CHECK((app->flags & (1u << GAL_FLAG_CLEARED)) == 0,
          "first run: CLEARED is not stored as a run flag");
    CHECK(gal_unlock_has_cg(&app->unlock, 6), "first run: later CG granted");
    CHECK(gal_unlock_has_chapter(&app->unlock, 1), "first run: later chapter granted");
}

static void run_second_playthrough(gal_app_t *app)
{
    key(app, BSP_BTN_OK, BSP_BTN_CLICK);   // ending -> title
    CHECK(app->page == GAL_PAGE_TITLE, "ending -> title");
    start_new_game(app);
    CHECK(app->page == GAL_PAGE_GAME, "second run: new game");
    CHECK(app->flags == 0, "second run: NewGame clears run flags");
    CHECK(gal_unlock_has_ending(&app->unlock, 0), "second run: ending survives NewGame");
    CHECK(gal_unlock_has_cg(&app->unlock, 6), "second run: later CG survives NewGame");
    CHECK(gal_unlock_has_chapter(&app->unlock, 1), "second run: later chapter survives NewGame");
    CHECK(!history_has(app, "追上去的那一步"), "second run: history reset");

    CHECK(advance_until(app, GAL_PAGE_CHOICE), "second run: first choice");
    app->choice_sel = 1;
    key(app, BSP_BTN_OK, BSP_BTN_CLICK);
    CHECK(app->flags == (1u << GAL_FLAG_CHOICE_B), "second run: choice B flag");
    CHECK(advance_until(app, GAL_PAGE_CHOICE), "second run: second choice");
    CHECK(history_has(app, "留在原地也不算坏"), "second run: B line visible");
    CHECK(!history_has(app, "追上去的那一步"), "second run: A line absent");
    CHECK(!app->ui.lists[GAL_LIST_CHOICE].disabled[0],
          "second run: CLEARED option is visible and enabled");

    app->choice_sel = 0;
    key(app, BSP_BTN_OK, BSP_BTN_CLICK);
    CHECK(app->page == GAL_PAGE_ENDING, "second run: CLEARED option is choosable");
    CHECK(advance_until(app, GAL_PAGE_ENDING), "second run: hidden option reaches ending");
}

static void check_hidden_chapter(gal_app_t *app)
{
    key(app, BSP_BTN_OK, BSP_BTN_CLICK);   // ending -> title
    open_chapter_page(app);
    CHECK(app->page == GAL_PAGE_CHAPTER, "chapter page opens");
    app->chapter_sel = 2;
    key(app, BSP_BTN_OK, BSP_BTN_CLICK);
    CHECK(app->page == GAL_PAGE_GAME,
          "hidden chapter enters after a clear");
    CHECK(gal_unlock_has_chapter(&app->unlock, 1),
          "hidden chapter does not consume permanent later chapter");
}

static void seed_v1_progress(void)
{
    // v1: magic, version, reserved[2], {valid, pad, event:u16, page:u16}
    const uint8_t v1[10] = { 0x50, 1, 0, 0, 1, 0, 15, 0, 0, 0 };
    nvs_handle_t h = 0;
    CHECK(nvs_open("galprog", NVS_READWRITE, &h) == ESP_OK, "open v1 progress");
    CHECK(nvs_set_blob(h, "pos", v1, sizeof(v1)) == ESP_OK, "seed v1 progress");
    CHECK(nvs_commit(h) == ESP_OK, "commit v1 progress");
}

static void seed_v1_save(void)
{
    // v1 save: 4-byte header + 3 slots of 36 bytes; slot 0 at event 15.
    uint8_t v1[4 + 3 * 36] = { 0 };
    v1[0] = 0x53;
    v1[1] = 1;
    v1[4] = 1;             // occupied
    v1[6] = 15;            // event_index
    memcpy(v1 + 10, "legacy", 6);
    nvs_handle_t h = 0;
    CHECK(nvs_open("galsave", NVS_READWRITE, &h) == ESP_OK, "open v1 save");
    CHECK(nvs_set_blob(h, "slots", v1, sizeof(v1)) == ESP_OK, "seed v1 save");
    CHECK(nvs_commit(h) == ESP_OK, "commit v1 save");
}

int main(void)
{
    gal_app_t app;
    CHECK(gal_app_init(&app), "app init");
    CHECK(app.page == GAL_PAGE_START, "starts at start page");
    key(&app, BSP_BTN_OK, BSP_BTN_CLICK);
    CHECK(app.page == GAL_PAGE_TITLE, "start -> title");

    // No ending yet: hidden chapter is still locked.
    open_chapter_page(&app);
    CHECK(app.page == GAL_PAGE_CHAPTER, "fresh chapter page opens");
    app.chapter_sel = 2;
    key(&app, BSP_BTN_OK, BSP_BTN_CLICK);
    CHECK(app.page == GAL_PAGE_CHAPTER, "fresh hidden chapter stays locked");
    CHECK(strstr(app.notice, "章节未解锁") != NULL,
          "fresh hidden chapter shows lock notice");
    to_title(&app);
    CHECK(app.page == GAL_PAGE_TITLE, "fresh chapter -> title");

    run_first_playthrough(&app);

    // Second playthrough: A/B exclusivity, permanent CLEARED option/chapter.
    run_second_playthrough(&app);
    check_hidden_chapter(&app);

    // Manual load/continue/restart still restore the current run and keep
    // permanent qualifications independent from v1 flags=0.
    gal_app_flush(&app);
    to_title(&app);
    app.title_sel = GAL_TITLE_MENU_LOAD;
    key(&app, BSP_BTN_OK, BSP_BTN_CLICK);
    CHECK(app.page == GAL_PAGE_LOAD, "title -> load");
    app.load_sel = 0;
    key(&app, BSP_BTN_OK, BSP_BTN_CLICK);
    CHECK(app.page == GAL_PAGE_GAME, "manual load returns to saved reader");
    CHECK(app.flags == (1u << GAL_FLAG_CHOICE_A), "manual load restores run flags");
    gal_app_flush(&app);

    to_title(&app);
    app.flags = 0;   // stale in-memory title state must not override progress
    app.title_sel = GAL_TITLE_MENU_CONTINUE;
    key(&app, BSP_BTN_OK, BSP_BTN_CLICK);
    CHECK(app.page == GAL_PAGE_GAME, "continue returns to saved reader");
    CHECK(app.flags == (1u << GAL_FLAG_CHOICE_A),
          "continue restores run flags from auto progress");

    CHECK(advance_until(&app, GAL_PAGE_CHOICE), "load path reaches second choice");
    CHECK(!app.ui.lists[GAL_LIST_CHOICE].disabled[0],
          "manual load keeps permanent CLEARED option enabled");
    app.choice_sel = 0;
    key(&app, BSP_BTN_OK, BSP_BTN_CLICK);
    CHECK(app.page == GAL_PAGE_ENDING, "load path can choose CLEARED option");
    key(&app, BSP_BTN_OK, BSP_BTN_CLICK);   // ending -> title
    gal_app_flush(&app);

    gal_app_t app2;
    CHECK(gal_app_init(&app2), "second app init");
    CHECK((app2.flags & (1u << GAL_FLAG_CHOICE_A)) != 0,
          "restart loads choice A run flag");
    CHECK((app2.flags & (1u << GAL_FLAG_CHOICE_B)) == 0,
          "restart does not leak choice B");
    CHECK((app2.flags & (1u << GAL_FLAG_CHAPTER_2)) != 0,
          "restart keeps reached chapter flag");
    CHECK(gal_unlock_has_ending(&app2.unlock, 0), "restart keeps ending");
    CHECK(gal_unlock_has_cg(&app2.unlock, 6), "restart keeps later CG");
    CHECK(gal_unlock_has_chapter(&app2.unlock, 1), "restart keeps later chapter");
    open_chapter_page(&app2);
    app2.chapter_sel = 2;
    key(&app2, BSP_BTN_OK, BSP_BTN_CLICK);
    CHECK(app2.page == GAL_PAGE_GAME, "restart keeps hidden chapter usable");

    // v1 progress has no flags field. It must not erase the permanent ending
    // (CLEARED) qualification when the app is rebuilt.
    seed_v1_progress();
    gal_app_t app3;
    CHECK(gal_app_init(&app3), "v1 progress app init");
    CHECK(app3.flags == 0, "v1 progress loads flags=0");
    CHECK(gal_unlock_has_any_ending(&app3.unlock),
          "v1 progress keeps permanent CLEARED qualification");
    key(&app3, BSP_BTN_OK, BSP_BTN_CLICK);   // start -> title
    app3.title_sel = GAL_TITLE_MENU_CONTINUE;
    key(&app3, BSP_BTN_OK, BSP_BTN_CLICK);
    CHECK(app3.page == GAL_PAGE_CHOICE, "v1 continue reaches saved choice");
    CHECK(!app3.ui.lists[GAL_LIST_CHOICE].disabled[0],
          "v1 flags=0 does not lock permanent CLEARED option");

    // The same holds for an old v1 manual slot: loading flags=0 must not
    // remove the permanent ending qualification.
    seed_v1_save();
    gal_app_t app4;
    CHECK(gal_app_init(&app4), "v1 save app init");
    CHECK(app4.flags == 0, "v1 save loads flags=0");
    key(&app4, BSP_BTN_OK, BSP_BTN_CLICK);   // start -> title
    app4.title_sel = GAL_TITLE_MENU_LOAD;
    key(&app4, BSP_BTN_OK, BSP_BTN_CLICK);
    app4.load_sel = 0;
    key(&app4, BSP_BTN_OK, BSP_BTN_CLICK);
    CHECK(app4.page == GAL_PAGE_CHOICE, "v1 manual load reaches saved choice");
    CHECK(app4.flags == 0, "v1 manual load restores flags=0");
    CHECK(!app4.ui.lists[GAL_LIST_CHOICE].disabled[0],
          "v1 manual load keeps permanent CLEARED option");

    printf("%s: failures=%d\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
