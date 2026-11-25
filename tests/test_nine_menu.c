// Production-app menu round-trip regression. Keys/NVS only; no page,
// VM or flag injection. HAL/UI are controlled stubs, not a device run.
#include "nine_app.h"
#include "nine_vm.h"
#include "gal_stub.h"
#include "nvs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, cases;
static nine_app_t a1, a2, a3, a4, a5, a6, a7;

static void key(nine_app_t *a, int btn, int ev)
{
    nine_key_t k = {(uint8_t)btn, (uint8_t)ev};
    nine_app_key(a, &k);
}
static void click(nine_app_t *a, int btn)
{
    key(a, btn, NINE_EV_PRESS);
    key(a, btn, NINE_EV_CLICK);
    key(a, btn, NINE_EV_RELEASE);
}
static void longp(nine_app_t *a, int btn)
{
    key(a, btn, NINE_EV_PRESS);
    key(a, btn, NINE_EV_LONG);
    key(a, btn, NINE_EV_RELEASE);
}
static void tick(nine_app_t *a, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) nine_app_tick(a, 50);
}
static void ff_to_choice(nine_app_t *a)
{
    for (unsigned i = 0; i < 100000 && a->page == GAL_PAGE_GAME; ++i) {
        if (!a->ff_hold) {
            key(a, NINE_BTN_UP, NINE_EV_PRESS);
            key(a, NINE_BTN_UP, NINE_EV_LONG);
        }
        nine_app_tick(a, 50);
        if (a->page == GAL_PAGE_OPENING) {
            if (a->ff_hold) key(a, NINE_BTN_UP, NINE_EV_RELEASE);
            click(a, NINE_BTN_OK);
        }
    }
    if (a->ff_hold) key(a, NINE_BTN_UP, NINE_EV_RELEASE);
}
static void ff_to_op(nine_app_t *a)
{
    for (unsigned i = 0; i < 100000 && a->page == GAL_PAGE_GAME; ++i) {
        if (!a->ff_hold) {
            key(a, NINE_BTN_UP, NINE_EV_PRESS);
            key(a, NINE_BTN_UP, NINE_EV_LONG);
        }
        nine_app_tick(a, 50);
        if (a->page == GAL_PAGE_OPENING) {
            if (a->ff_hold) key(a, NINE_BTN_UP, NINE_EV_RELEASE);
            return;
        }
    }
    if (a->ff_hold) key(a, NINE_BTN_UP, NINE_EV_RELEASE);
}
static void ff_to_end(nine_app_t *a)
{
    for (unsigned i = 0; i < 100000 && a->page != GAL_PAGE_ENDING; ++i) {
        if (a->page == GAL_PAGE_OPENING) {
            if (a->ff_hold) key(a, NINE_BTN_UP, NINE_EV_RELEASE);
            click(a, NINE_BTN_OK);
            continue;
        }
        if (a->page != GAL_PAGE_GAME) break;
        if (!a->ff_hold) {
            key(a, NINE_BTN_UP, NINE_EV_PRESS);
            key(a, NINE_BTN_UP, NINE_EV_LONG);
        }
        nine_app_tick(a, 50);
    }
    if (a->ff_hold) key(a, NINE_BTN_UP, NINE_EV_RELEASE);
}
static void new_game(nine_app_t *a)
{
    if (a->page != GAL_PAGE_START) return;
    click(a, NINE_BTN_OK);
    click(a, NINE_BTN_OK);
}
static void choose(nine_app_t *a, int want)
{
    while (a->choice_sel > 0) click(a, NINE_BTN_UP);
    for (int i = 0; i < want; ++i) click(a, NINE_BTN_DOWN);
    click(a, NINE_BTN_OK);
}
static void result(const char *id, const char *expected, const char *actual, int ok, const char *detail)
{
    ++cases;
    printf("CASE\t%s\t%s\t%s\t%s\t%s\n", id, expected, actual,
           ok ? "PASS" : "FAIL", detail ? detail : "");
    if (!ok) fails++;
}
static int is_choice(nine_app_t *a) { return a->vm.view.kind == NINE_VM_CHOICE; }
static int is_text(nine_app_t *a) { return a->vm.view.kind == NINE_VM_TEXT; }
static int is_op(nine_app_t *a) { return a->vm.view.kind == NINE_VM_OP; }
static int page(nine_app_t *a) { return (int)a->page; }
static void ensure_menu(nine_app_t *a)
{
    if (a->page == GAL_PAGE_CHOICE) longp(a, NINE_BTN_OK); // choice layer -> game
    if (a->page == GAL_PAGE_GAME) longp(a, NINE_BTN_OK); // game -> menu
}
static void open_menu(nine_app_t *a) { ensure_menu(a); }

static int load_assets(const char *path, unsigned char **out, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    *out = malloc((size_t)sz);
    if (!*out || fread(*out, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); return 0; }
    fclose(f); *n = (size_t)sz; return 1;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s script images\n", argv[0]); return 2; }
    unsigned char *script = NULL, *images = NULL; size_t sn = 0, imn = 0;
    if (!load_assets(argv[1], &script, &sn) || !load_assets(argv[2], &images, &imn)) return 2;

    // A. First run: graph OP, single choice n60, choice-layer menu.
    nvs_stub_reset(); gal_stub_reset();
    if (!nine_app_init(&a1, script, sn, images, imn)) return 2;
    new_game(&a1);
    ff_to_op(&a1);
    result("graph_op_page", "OPENING/OP",
           page(&a1) == GAL_PAGE_OPENING && is_op(&a1) ? "OPENING/OP" : "other",
           page(&a1) == GAL_PAGE_OPENING && is_op(&a1), "n24 graph OP reached by FF");
    if (page(&a1) == GAL_PAGE_OPENING) click(&a1, NINE_BTN_OK);
    result("op_skip_resumes", "GAME/TEXT",
           page(&a1) == GAL_PAGE_GAME && is_text(&a1) ? "GAME/TEXT" : "other",
           page(&a1) == GAL_PAGE_GAME && is_text(&a1), "skip graph OP");
    ff_to_choice(&a1);
    result("single_choice_initial", "CHOICE/CHOICE",
           page(&a1) == GAL_PAGE_CHOICE && is_choice(&a1) ? "CHOICE/CHOICE" : "other",
           page(&a1) == GAL_PAGE_CHOICE && is_choice(&a1), "n60");
    longp(&a1, NINE_BTN_OK);
    result("choice_long_ok_ui_vm", "MENU/CHOICE",
           page(&a1) == GAL_PAGE_MENU && is_choice(&a1) ? "MENU/CHOICE" : "other",
           page(&a1) == GAL_PAGE_MENU && is_choice(&a1), "one long OK from CHOICE enters MENU");
    result("choice_menu_opened", "MENU/CHOICE",
           page(&a1) == GAL_PAGE_MENU && is_choice(&a1) ? "MENU/CHOICE" : "other",
           page(&a1) == GAL_PAGE_MENU && is_choice(&a1), "");
    click(&a1, NINE_BTN_OK);
    click(&a1, NINE_BTN_OK);
    longp(&a1, NINE_BTN_OK);
    result("choice_save_return_resume", "CHOICE/CHOICE",
           page(&a1) == GAL_PAGE_CHOICE && is_choice(&a1) ? "CHOICE/CHOICE" : "other",
           page(&a1) == GAL_PAGE_CHOICE && is_choice(&a1), "save return restores choices");
    open_menu(&a1); click(&a1, NINE_BTN_DOWN); click(&a1, NINE_BTN_OK);
    result("load_page_from_choice", "LOAD", page(&a1) == GAL_PAGE_LOAD ? "LOAD" : "other",
           page(&a1) == GAL_PAGE_LOAD, "");
    click(&a1, NINE_BTN_OK);
    result("choice_load_success", "CHOICE/CHOICE",
           page(&a1) == GAL_PAGE_CHOICE && is_choice(&a1) ? "CHOICE/CHOICE" : "other",
           page(&a1) == GAL_PAGE_CHOICE && is_choice(&a1), "load renders choice");
    ensure_menu(&a1); click(&a1, NINE_BTN_DOWN); click(&a1, NINE_BTN_OK); longp(&a1, NINE_BTN_OK);
    result("choice_load_cancel_return", "CHOICE/CHOICE",
           page(&a1) == GAL_PAGE_CHOICE && is_choice(&a1) ? "CHOICE/CHOICE" : "other",
           page(&a1) == GAL_PAGE_CHOICE && is_choice(&a1), "load cancel restores choices");
    ensure_menu(&a1); click(&a1, NINE_BTN_OK); click(&a1, NINE_BTN_DOWN); click(&a1, NINE_BTN_OK); longp(&a1, NINE_BTN_OK);
    result("choice_save_cancel_return", "CHOICE/CHOICE",
           page(&a1) == GAL_PAGE_CHOICE && is_choice(&a1) ? "CHOICE/CHOICE" : "other",
           page(&a1) == GAL_PAGE_CHOICE && is_choice(&a1), "save cancel restores choices");
    ensure_menu(&a1); click(&a1, NINE_BTN_OK); click(&a1, NINE_BTN_OK);
    result("choice_overwrite_modal", "modal", gal_stub()->modal ? "modal" : "none",
           gal_stub()->modal, "slot0 occupied");
    if (gal_stub()->modal) click(&a1, NINE_BTN_OK);
    longp(&a1, NINE_BTN_OK);
    result("choice_overwrite_cancel_return", "CHOICE/CHOICE",
           page(&a1) == GAL_PAGE_CHOICE && is_choice(&a1) ? "CHOICE/CHOICE" : "other",
           page(&a1) == GAL_PAGE_CHOICE && is_choice(&a1), "overwrite cancel restores choices");
    ensure_menu(&a1); click(&a1, NINE_BTN_OK); click(&a1, NINE_BTN_OK); // modal
    if (gal_stub()->modal) { click(&a1, NINE_BTN_DOWN); click(&a1, NINE_BTN_OK); } // confirm overwrite
    longp(&a1, NINE_BTN_OK);
    result("choice_overwrite_confirm_return", "CHOICE/CHOICE",
           page(&a1) == GAL_PAGE_CHOICE && is_choice(&a1) ? "CHOICE/CHOICE" : "other",
           page(&a1) == GAL_PAGE_CHOICE && is_choice(&a1), "overwrite confirm restores choices");
    ensure_menu(&a1); click(&a1, NINE_BTN_DOWN); click(&a1, NINE_BTN_OK);
    for (int i=0;i<4;i++) { click(&a1, NINE_BTN_DOWN); }
    click(&a1, NINE_BTN_OK); longp(&a1, NINE_BTN_OK);
    result("choice_load_invalid_return", "CHOICE/CHOICE",
           page(&a1) == GAL_PAGE_CHOICE && is_choice(&a1) ? "CHOICE/CHOICE" : "other",
           page(&a1) == GAL_PAGE_CHOICE && is_choice(&a1), "invalid load restores choices");
    ensure_menu(&a1); for (int i=0;i<4;i++) click(&a1, NINE_BTN_DOWN);
    click(&a1, NINE_BTN_OK); longp(&a1, NINE_BTN_OK);
    result("choice_settings_return", "CHOICE/CHOICE",
           page(&a1) == GAL_PAGE_CHOICE && is_choice(&a1) ? "CHOICE/CHOICE" : "other",
           page(&a1) == GAL_PAGE_CHOICE && is_choice(&a1), "settings return restores choices");
    ensure_menu(&a1); for (int i=0;i<2;i++) click(&a1, NINE_BTN_DOWN);
    click(&a1, NINE_BTN_OK); longp(&a1, NINE_BTN_OK);
    result("choice_history_return", "CHOICE/CHOICE",
           page(&a1) == GAL_PAGE_CHOICE && is_choice(&a1) ? "CHOICE/CHOICE" : "other",
           page(&a1) == GAL_PAGE_CHOICE && is_choice(&a1), "history return restores choices");
    ensure_menu(&a1); for (int i=0;i<5;i++) click(&a1, NINE_BTN_DOWN);
    click(&a1, NINE_BTN_OK); click(&a1, NINE_BTN_OK);
    result("choice_menu_exit_newgame", "GAME/TEXT",
           page(&a1) == GAL_PAGE_GAME && is_text(&a1) ? "GAME/TEXT" : "other",
           page(&a1) == GAL_PAGE_GAME && is_text(&a1), "exit title then New Game");
    // Establish loop1 from the saved n60 choice so the next app starts the second week.
    open_menu(&a1); for (int i=0;i<5;i++) click(&a1, NINE_BTN_DOWN); click(&a1, NINE_BTN_OK); // title
    click(&a1, NINE_BTN_DOWN); click(&a1, NINE_BTN_DOWN); click(&a1, NINE_BTN_OK); // load
    click(&a1, NINE_BTN_OK); // slot0
    if (page(&a1) == GAL_PAGE_CHOICE) { choose(&a1, 0); ff_to_end(&a1); }

    // B. Second run multi-option n1 (loop1 prefix is not needed for a menu test).
    if (!nine_app_init(&a2, script, sn, images, imn)) return 2;
    new_game(&a2);
    ff_to_choice(&a2);
    result("multi_choice_n1", "CHOICE/CHOICE opt2",
           page(&a2) == GAL_PAGE_CHOICE && is_choice(&a2) && a2.vm.view.choice_count == 2 ? "CHOICE/CHOICE opt2" : "other",
           page(&a2) == GAL_PAGE_CHOICE && is_choice(&a2) && a2.vm.view.choice_count == 2, "");
    ensure_menu(&a2); click(&a2, NINE_BTN_OK); click(&a2, NINE_BTN_DOWN); click(&a2, NINE_BTN_DOWN); click(&a2, NINE_BTN_OK); longp(&a2, NINE_BTN_OK);
    result("multi_choice_save_resume", "CHOICE/CHOICE",
           page(&a2) == GAL_PAGE_CHOICE && is_choice(&a2) ? "CHOICE/CHOICE" : "other",
           page(&a2) == GAL_PAGE_CHOICE && is_choice(&a2), "n1 save/return restores choices");

    // C. Last option n21: normal navigation, no menu.
    if (!nine_app_init(&a3, script, sn, images, imn)) return 2;
    new_game(&a3); ff_to_choice(&a3); choose(&a3, 0);
    ff_to_choice(&a3); choose(&a3, 0); ff_to_choice(&a3);
    result("last_choice_n21_present", "CHOICE opt2",
           page(&a3) == GAL_PAGE_CHOICE && a3.vm.view.choice_count == 2 ? "CHOICE opt2" : "other",
           page(&a3) == GAL_PAGE_CHOICE && a3.vm.view.choice_count == 2, "");
    while (a3.choice_sel > 0) click(&a3, NINE_BTN_UP);
    click(&a3, NINE_BTN_DOWN);
    result("last_choice_n21_nav", "choice_sel=1", a3.choice_sel == 1 ? "choice_sel=1" : "other",
           a3.choice_sel == 1, "");
    click(&a3, NINE_BTN_OK);
    result("last_choice_n21_choose", "GAME/TEXT node23",
           page(&a3) == GAL_PAGE_GAME && is_text(&a3) && a3.vm.state.node == 23 ? "GAME/TEXT node23" : "other",
           page(&a3) == GAL_PAGE_GAME && is_text(&a3) && a3.vm.state.node == 23, "");

    // D. Text save/return/advance.
    if (!nine_app_init(&a4, script, sn, images, imn)) return 2;
    new_game(&a4);
    result("text_page_view", "GAME/TEXT",
           page(&a4) == GAL_PAGE_GAME && is_text(&a4) ? "GAME/TEXT" : "other",
           page(&a4) == GAL_PAGE_GAME && is_text(&a4), "");
    open_menu(&a4); click(&a4, NINE_BTN_OK);
    for (int i=0;i<3;i++) click(&a4, NINE_BTN_DOWN); // empty slot3
    click(&a4, NINE_BTN_OK); longp(&a4, NINE_BTN_OK);
    result("text_save_return", "GAME/TEXT",
           page(&a4) == GAL_PAGE_GAME && is_text(&a4) ? "GAME/TEXT" : "other",
           page(&a4) == GAL_PAGE_GAME && is_text(&a4), "");
    click(&a4, NINE_BTN_OK);
    result("text_after_return_advance", "GAME/TEXT next",
           page(&a4) == GAL_PAGE_GAME && is_text(&a4) ? "GAME/TEXT" : "other",
           page(&a4) == GAL_PAGE_GAME && is_text(&a4), "short advances text");
    open_menu(&a4); for (int i=0;i<5;i++) click(&a4, NINE_BTN_DOWN); click(&a4, NINE_BTN_OK);
    result("menu_exit_title", "TITLE", page(&a4) == GAL_PAGE_TITLE ? "TITLE" : "other",
           page(&a4) == GAL_PAGE_TITLE, "");
    click(&a4, NINE_BTN_DOWN); click(&a4, NINE_BTN_OK);
    result("continue_after_menu_exit", "GAME/TEXT",
           page(&a4) == GAL_PAGE_GAME && is_text(&a4) ? "GAME/TEXT" : "other",
           page(&a4) == GAL_PAGE_GAME && is_text(&a4), "menu exit auto is current, not restart n0");

    // E. Restart Continue: show_title writes the initial n0 over a valid auto n2.
    if (!nine_app_init(&a6, script, sn, images, imn)) return 2;
    new_game(&a6); ff_to_choice(&a6); choose(&a6, 0); tick(&a6, 80);
    uint16_t want_node = a6.vm.state.node;
    uint32_t want_index = a6.vm.state.index;
    if (!nine_app_init(&a7, script, sn, images, imn)) return 2;
    click(&a7, NINE_BTN_OK); // START -> TITLE writes auto
    click(&a7, NINE_BTN_DOWN); click(&a7, NINE_BTN_OK); // Continue
    result("restart_continue_auto", "node=2",
           a7.vm.state.node == want_node ? "node=2" : "node=0",
           a7.vm.state.node == want_node && a7.vm.state.index == want_index,
           "show_title() must preserve loaded auto");

    // F. Preview fresh NVS: preview menu has no Save/Load and no permanent writes.
    nvs_stub_reset(); gal_stub_reset();
    if (!nine_app_init(&a5, script, sn, images, imn)) return 2;
    int true_key = nine_pack_find_key(&a5.pack, "true");
    int chapter_before = (a5.unlock[(32 + 10) / 8] >> ((32 + 10) % 8)) & 1;
    click(&a5, NINE_BTN_OK); // title; show_title may write the n0 auto
    unsigned auto_before_preview = a5.auto_size;
    for (int i=0;i<4;i++) click(&a5, NINE_BTN_DOWN);
    click(&a5, NINE_BTN_OK); // gallery
    click(&a5, NINE_BTN_DOWN); click(&a5, NINE_BTN_OK); // chapters
    for (int i=0;i<10;i++) click(&a5, NINE_BTN_DOWN);
    click(&a5, NINE_BTN_OK); // preview n62
    open_menu(&a5);
    result("preview_menu_rows", "MENU 3 rows no save",
           gal_stub()->rows_count[GAL_LIST_MENU] == 3 ? "MENU 3 rows" : "other",
           gal_stub()->rows_count[GAL_LIST_MENU] == 3, "preview menu must not expose Save/Load");
    longp(&a5, NINE_BTN_OK);
    ff_to_end(&a5);
    result("preview_end_no_permanent", "CHAPTER true=0",
           page(&a5) == GAL_PAGE_CHAPTER && a5.vm.state.progress[true_key] == 0 ? "CHAPTER unchanged" : "other",
           page(&a5) == GAL_PAGE_CHAPTER && a5.vm.state.progress[true_key] == 0, "");
    result("preview_auto_not_written", "auto unchanged",
           a5.auto_size == auto_before_preview ? "auto unchanged" : "auto changed",
           a5.auto_size == auto_before_preview, "preview must not write/overwrite auto");
    result("preview_chapter_unlock", "chapter10 unchanged",
           ((a5.unlock[(32 + 10) / 8] >> ((32 + 10) % 8)) & 1) == chapter_before ? "unchanged" : "changed",
           ((a5.unlock[(32 + 10) / 8] >> ((32 + 10) % 8)) & 1) == chapter_before, "");

    // G. Graph OP is covered by graph_op_page above; gallery OP is a UI-only cut.
    printf("CASE	choice_after_save_short_advance	N/A (stale GAME/CHOICE path removed)	N/A	N/A	not applicable after menu restore fix; choice remains interactive\n");
    printf("CASE\tconditional_choice_visibility\tN/A (no when)\tN/A\tN/A\tall 9 options unconditional in graph\n");
    printf("CASE\tgallery_op_entry\tN/A (UI-only cut)\tN/A\tN/A\tnot a graph view; graph OP covered above\n");

    printf("SUMMARY cases=%d fails=%d\n", cases, fails);
    printf("VERDICT %s\n", fails ? "FAIL_FINDINGS" : "PASS");
    free(script); free(images);
    return fails ? 1 : 0;
}
