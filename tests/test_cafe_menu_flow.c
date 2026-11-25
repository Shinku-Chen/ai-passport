// Supplemental menu/UI-vs-VM audit for Cafe Stella.
// All arrivals use the production app's key path; no app.page/vm/flags are assigned by the driver.
#include "cafe_app.h"
#include "cafe_store.h"
#include "cafe_prelude.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int audit_ui_page, audit_ui_rows_count, audit_ui_rows_selected;
extern int audit_ui_choice_count, audit_ui_choice_selected, audit_ui_modal, audit_ui_ending_count;
extern char audit_ui_ending[512];

static int failures;
static int checks;
static char current_case[80];
static int op_skips;

static const char *pn(int p) {
    static const char *names[] = {"START","TITLE","GAME","CHOICE","MENU","SAVE","LOAD",
        "SETTINGS","HISTORY","GALLERY","CG_LIST","RECALL","MUSIC","CHAPTER",
        "TRANSITION","ENDING","ABOUT","CG","COUNT"};
    return p >= 0 && p < (int)(sizeof(names)/sizeof(names[0])) ? names[p] : "?";
}
static void report(const char *stage, int ok, const char *detail) {
    checks++;
    if (!ok) failures++;
    printf("MENU|%s|%s|%s|%s|page=%s|node=%u|idx=%u|choice=%d|sel=%d|prog=%08x|det=%s\n",
           current_case, stage, ok ? "PASS" : "FAIL", ok ? "ok" : "FAIL",
           pn(0), 0u, 0u, 0, 0, 0u, detail ? detail : "");
}
static void report_app(const char *stage, int ok, const cafe_app_t *a, const char *detail) {
    checks++;
    if (!ok) failures++;
    printf("MENU|%s|%s|%s|%s|page=%s|node=%u|idx=%u|choice=%d|sel=%d|prog=%08x|det=%s\n",
           current_case, stage, ok ? "PASS" : "FAIL", ok ? "ok" : "FAIL",
           pn(a->page), (unsigned)a->vm.node, (unsigned)a->vm.index,
           a->choice_count, a->choice_sel, a->vm.progress, detail ? detail : "");
}
static void tap(cafe_app_t *a, uint8_t key, uint32_t *t) {
    cafe_app_key(a, key, true, *t);
    cafe_app_key(a, key, false, *t + 40);
    *t += 80;
}
static void ok(cafe_app_t *a, uint32_t *t) { tap(a, CAFE_KEY_OK, t); }
static void down(cafe_app_t *a, uint32_t *t) { tap(a, CAFE_KEY_DOWN, t); }
static void long_ok(cafe_app_t *a, uint32_t *t) {
    cafe_app_key(a, CAFE_KEY_OK, true, *t);
    cafe_app_tick(a, *t + 520, 520);
    *t += 520;
    cafe_app_key(a, CAFE_KEY_OK, false, *t);
    *t += 40;
}
static void long_key(cafe_app_t *a, uint8_t key, uint32_t *t) {
    cafe_app_key(a, key, true, *t);
    cafe_app_tick(a, *t + 520, 520);
    *t += 520;
    cafe_app_key(a, key, false, *t);
    *t += 40;
}
static int finish_to_decision(cafe_app_t *a, uint32_t *t) {
    int fast = 0;
    for (int i = 0; i < 300000; ++i) {
        if (a->page != CAFE_PAGE_GAME && fast) {
            cafe_app_key(a, CAFE_KEY_UP, false, *t); *t += 40; fast = 0;
        }
        if (a->page == CAFE_PAGE_ENDING) return 1;
        if (a->page == CAFE_PAGE_CHOICE) return 2;
        if (a->page == CAFE_PAGE_TRANSITION) {
            if (a->op_pending) op_skips++;
            ok(a, t);
            continue;
        }
        if (a->page != CAFE_PAGE_GAME) return 0;
        if (!fast) {
            cafe_app_key(a, CAFE_KEY_UP, true, *t);
            cafe_app_tick(a, *t + 520, 520); *t += 520; fast = 1;
        }
        cafe_app_tick(a, *t, 60); *t += 60;
        if (a->page != CAFE_PAGE_GAME) {
            cafe_app_key(a, CAFE_KEY_UP, false, *t); *t += 40;
        }
    }
    return 0;
}
static int choose(cafe_app_t *a, uint32_t *t, int idx) {
    if (a->page != CAFE_PAGE_CHOICE || idx < 0 || idx >= a->choice_count) return 0;
    for (int i = a->choice_sel; i < idx; ++i) down(a, t);
    if (a->choice_sel != idx) return 0;
    ok(a, t);
    return 1;
}
static int start_new(cafe_app_t *a, uint32_t *t) {
    if (a->page == CAFE_PAGE_START) ok(a, t);
    if (a->page != CAFE_PAGE_TITLE) return 0;
    ok(a, t); // title row 0 New Game
    return a->page == CAFE_PAGE_GAME || a->page == CAFE_PAGE_TRANSITION || a->page == CAFE_PAGE_CHOICE;
}
static int reach_choice(cafe_app_t *a, uint32_t *t, const int *acts, int n, uint16_t target) {
    if (!start_new(a, t)) return 0;
    for (int i = 0; i < n; ++i) {
        if (finish_to_decision(a, t) != 2 || !choose(a, t, acts[i])) return 0;
    }
    if (finish_to_decision(a, t) != 2) return 0;
    return a->vm.node == target;
}
static int menu_at_choice(cafe_app_t *a, uint32_t *t, int *direct) {
    const uint16_t n = a->vm.node, idx = a->vm.index;
    long_ok(a, t); // requested one long OK from CHOICE
    *direct = a->page == CAFE_PAGE_MENU;
    if (a->page == CAFE_PAGE_GAME) {
        // The port currently turns CHOICE into GAME first; a second long OK opens the menu.
        long_ok(a, t);
    }
    return a->page == CAFE_PAGE_MENU && a->vm.node == n && a->vm.index == idx;
}
static int menu_select(cafe_app_t *a, uint32_t *t, int idx) {
    if (a->page != CAFE_PAGE_MENU) return 0;
    for (int i = a->menu_sel; i < idx; ++i) down(a, t);
    if (a->menu_sel != idx) return 0;
    ok(a, t);
    return 1;
}
static int save_slot(cafe_app_t *a, uint32_t *t, int slot) {
    if (a->page != CAFE_PAGE_SAVE) return 0;
    for (int i = a->save_sel; i < slot; ++i) down(a, t);
    if (a->save_sel != slot) return 0;
    ok(a, t);
    return a->page == CAFE_PAGE_SAVE;
}
static int load_slot(cafe_app_t *a, uint32_t *t, int slot) {
    if (a->page != CAFE_PAGE_LOAD) return 0;
    for (int i = a->load_sel; i < slot; ++i) down(a, t);
    if (a->load_sel != slot) return 0;
    ok(a, t);
    return 1;
}
static void reset_ui(void) {
    audit_ui_ending[0] = 0; audit_ui_ending_count = 0; audit_ui_modal = 0;
}
static void begin_case(cafe_app_t *a, cafe_ui_t *ui, cafe_bands_t *bands, uint32_t *t) {
    memset(ui, 0, sizeof(*ui)); memset(bands, 0, sizeof(*bands));
    if (!cafe_app_init(a, ui, bands)) { printf("INIT_FAIL|%s\n", current_case); exit(2); }
    a->now_ms = 1000; a->last_activity_ms = 1000; *t = 1000;
    reset_ui(); op_skips = 0;
}
static int run_prelude(cafe_app_t *a, uint32_t *t, const int *acts, int n) {
    if (!start_new(a, t)) return 0;
    for (int i = 0; i < n; ++i) {
        if (finish_to_decision(a, t) != 2 || !choose(a, t, acts[i])) return 0;
    }
    if (finish_to_decision(a, t) != 1) return 0;
    ok(a, t); // ending -> title
    return a->page == CAFE_PAGE_TITLE;
}
static int open_after_gallery(cafe_app_t *a, uint32_t *t, int idx) {
    if (a->page == CAFE_PAGE_START) ok(a, t);
    if (a->page != CAFE_PAGE_TITLE) return 0;
    for (int i = 0; i < 4; ++i) down(a, t);
    if (a->title_sel != 4) return 0;
    ok(a, t);
    if (a->page != CAFE_PAGE_GALLERY) return 0;
    down(a, t); down(a, t);
    if (a->gallery_sel != 2) return 0;
    ok(a, t);
    if (a->page != CAFE_PAGE_CHAPTER) return 0;
    for (int i = 0; i < 6; ++i) down(a, t);
    if (a->route_sel != 6) return 0;
    ok(a, t);
    for (int i = 0; i < idx; ++i) down(a, t);
    if (a->chapter_sel != idx) return 0;
    ok(a, t);
    if (a->page != CAFE_PAGE_TRANSITION) return 0;
    ok(a, t);
    return a->page == CAFE_PAGE_GAME || a->page == CAFE_PAGE_CHOICE || a->page == CAFE_PAGE_TRANSITION;
}
static int reach_op(cafe_app_t *a, uint32_t *t) {
    if (!start_new(a, t)) return 0;
    int fast = 0;
    for (int i = 0; i < 100000; ++i) {
        if (a->page != CAFE_PAGE_GAME && fast) {
            cafe_app_key(a, CAFE_KEY_UP, false, *t); *t += 40; fast = 0;
        }
        if (a->page == CAFE_PAGE_TRANSITION && a->op_pending) return 1;
        if (a->page == CAFE_PAGE_CHOICE) {
            if (!choose(a, t, 0)) return 0;
            continue;
        }
        if (a->page == CAFE_PAGE_TRANSITION) { ok(a, t); continue; }
        if (a->page != CAFE_PAGE_GAME) return 0;
        if (!fast) {
            cafe_app_key(a, CAFE_KEY_UP, true, *t);
            cafe_app_tick(a, *t + 520, 520); *t += 520; fast = 1;
        }
        cafe_app_tick(a, *t, 60); *t += 60;
        if (a->page != CAFE_PAGE_GAME) {
            cafe_app_key(a, CAFE_KEY_UP, false, *t); *t += 40;
        }
    }
    return 0;
}
static uint16_t expected_target(int choice_node, int option) {
    // Stable expected targets used by the source/C witnesses in the first audit.
    if (choice_node == 7) return option == 0 ? 8 : 8;
    if (choice_node == 55) return option == 0 ? 56 : (option == 1 ? 58 : 60);
    if (choice_node == 67) return option == 0 ? 68 : (option == 1 ? 70 : (option == 2 ? 72 : 74));
    return 0xFFFF;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <cafe.pak>\n", argv[0]); return 2; }
    size_t pnbytes = 0; FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "pack open failed\n"); return 2; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *pack = malloc((size_t)sz);
    if (!pack || fread(pack, 1, (size_t)sz, f) != (size_t)sz) return 2;
    fclose(f); pnbytes = (size_t)sz;
    if (!cafe_pack_set_data(pack, pnbytes)) return 2;

    cafe_ui_t ui; cafe_bands_t bands; cafe_app_t a; uint32_t t;

    // A. Menu entry from a real CHOICE and UI/VM consistency after direct exit.
    strcpy(current_case, "choice_n7_menu_direct");
    begin_case(&a, &ui, &bands, &t);
    if (!reach_choice(&a, &t, NULL, 0, 7)) {
        report_app("reach_choice", 0, &a, "n7");
    } else {
        long_key(&a, CAFE_KEY_DOWN, &t);
        report_app("choice_long_down", a.choice_sel == 1 && audit_ui_choice_selected == 1,
                   &a, "choice cursor");
        long_key(&a, CAFE_KEY_UP, &t);
        report_app("choice_long_up", a.choice_sel == 0 && audit_ui_choice_selected == 0,
                   &a, "choice cursor");
        int direct = 0;
        int same = menu_at_choice(&a, &t, &direct);
        report_app("long_OK_is_menu", direct, &a, direct ? "direct" : "first long went to GAME; second opened MENU");
        report_app("menu_keeps_vm", same, &a, "VM node/index must not change");
        if (a.page == CAFE_PAGE_MENU) {
            long_ok(&a, &t); // direct close
            report_app("menu_close_page", a.page == CAFE_PAGE_CHOICE && a.vm.node == 7,
                       &a, "close should restore CHOICE (VM is CHOICE)");
            report_app("choice_interactive_after_close", a.page == CAFE_PAGE_CHOICE && a.vm.node == 7,
                       &a, "close must leave the real CHOICE page");
            report_app("choice_count_preserved", a.choice_count == 2, &a, "choice set preserved");
            report_app("choice_sel_preserved", a.choice_sel == 0, &a, "choice selection preserved");
            if (a.page == CAFE_PAGE_CHOICE) {
                int last = a.choice_count - 1;
                for (int i = 0; i < last; ++i) down(&a, &t);
                int sel = a.choice_sel;
                ok(&a, &t);
                report_app("last_option", sel == last && a.vm.node == expected_target(7, last),
                           &a, "navigate to last option and choose");
            }
        }
    }

    // A2. Multi-option cases: 3- and 4-option groups, first/last navigation.
    strcpy(current_case, "choice_n55_menu_last");
    begin_case(&a, &ui, &bands, &t);
    {
        const int acts[] = {0, 1, 0};
        if (!reach_choice(&a, &t, acts, 3, 55)) report_app("reach_choice", 0, &a, "n55");
        else {
            int direct = 0;
            if (!menu_at_choice(&a, &t, &direct)) report_app("open_menu", 0, &a, "menu");
            else {
                long_ok(&a, &t); // close (known B3 candidate)
                report_app("menu_close_page", a.page == CAFE_PAGE_CHOICE && a.vm.node == 55,
                           &a, "close should restore CHOICE");
                ok(&a, &t);
                if (a.page == CAFE_PAGE_CHOICE) {
                    int last = a.choice_count - 1;
                    for (int i = 0; i < last; ++i) down(&a, &t);
                    int sel = a.choice_sel;
                    ok(&a, &t);
                    report_app("last_option", sel == last && a.vm.node == expected_target(55, last),
                               &a, "n55 last option target");
                }
            }
        }
    }
    strcpy(current_case, "choice_n67_menu_first");
    begin_case(&a, &ui, &bands, &t);
    {
        const int acts[] = {0, 1, 0, 0};
        if (!reach_choice(&a, &t, acts, 4, 67)) report_app("reach_choice", 0, &a, "n67");
        else {
            int direct = 0;
            if (!menu_at_choice(&a, &t, &direct)) report_app("open_menu", 0, &a, "menu");
            else {
                long_ok(&a, &t); // close
                report_app("menu_close_page", a.page == CAFE_PAGE_CHOICE && a.vm.node == 67,
                           &a, "close should restore CHOICE");
                ok(&a, &t);
                if (a.page == CAFE_PAGE_CHOICE) {
                    int sel = a.choice_sel;
                    ok(&a, &t);
                    report_app("first_option", sel == 0 && a.vm.node == expected_target(67, 0),
                               &a, "n67 first option target");
                }
            }
        }
    }

    // B. Save/cancel/overwrite from a real CHOICE, then close and resume.
    strcpy(current_case, "choice_n7_save_roundtrip");
    begin_case(&a, &ui, &bands, &t);
    if (!reach_choice(&a, &t, NULL, 0, 7)) report_app("reach_choice", 0, &a, "n7");
    else {
        int direct = 0;
        if (!menu_at_choice(&a, &t, &direct)) report_app("open_menu", 0, &a, "menu");
        else {
            if (!menu_select(&a, &t, 1)) report_app("menu_save", 0, &a, "select save");
            else {
                report_app("save_page", a.page == CAFE_PAGE_SAVE, &a, "save list");
                long_key(&a, CAFE_KEY_DOWN, &t);
                report_app("save_long_down", a.save_sel == 1 && audit_ui_rows_selected == 1,
                           &a, "save cursor");
                long_key(&a, CAFE_KEY_UP, &t);
                report_app("save_long_up", a.save_sel == 0 && audit_ui_rows_selected == 0,
                           &a, "save cursor");
                if (save_slot(&a, &t, 0)) report_app("save_new_slot", 1, &a, "slot 0");
                else report_app("save_new_slot", 0, &a, "slot 0");
                long_ok(&a, &t); // save list -> menu
                report_app("save_back_menu", a.page == CAFE_PAGE_MENU, &a, "save back");
                long_ok(&a, &t); // menu -> game (bug candidate)
                report_app("menu_close_after_save", a.page == CAFE_PAGE_CHOICE && a.vm.node == 7,
                           &a, "close should restore CHOICE");
                report_app("choice_interactive_after_menu_save", a.page == CAFE_PAGE_CHOICE && a.vm.node == 7,
                           &a, "menu save/close must leave the real CHOICE page");
                if (a.page == CAFE_PAGE_CHOICE) {
                    int last = a.choice_count - 1;
                    for (int i = 0; i < last; ++i) down(&a, &t);
                    if (ok(&a, &t), a.vm.node == expected_target(7, last))
                        report_app("choose_after_menu_save", 1, &a, "last option target");
                    else report_app("choose_after_menu_save", 0, &a, "last option target");
                }
            }
        }
    }

    // C. Save cancel/overwrite confirmation, then Load success and invalid slot.
    strcpy(current_case, "choice_n7_save_load_modal");
    begin_case(&a, &ui, &bands, &t);
    if (!reach_choice(&a, &t, NULL, 0, 7)) report_app("reach_choice", 0, &a, "n7");
    else {
        int direct = 0;
        if (!menu_at_choice(&a, &t, &direct) || !menu_select(&a, &t, 1) || !save_slot(&a, &t, 2)) {
            report_app("setup_save", 0, &a, "save slot2");
        } else {
            cafe_save_t dbg; int dbgok = cafe_store_load_slot(2, &dbg);
            printf("SAVEDBG|%d|node=%u|idx=%u|prog=%u|progress=%08x", dbgok, (unsigned)dbg.node,
                   (unsigned)dbg.index, (unsigned)dbg.progression, dbg.progress);
            printf("\n");
            long_ok(&a, &t); // menu
            if (!menu_select(&a, &t, 1)) report_app("reopen_save", 0, &a, "save");
            else {
                for (int i = a.save_sel; i < 0; ++i) { }
                ok(&a, &t); // existing slot0 -> overwrite modal
                report_app("overwrite_modal", a.modal_open, &a, "overwrite modal");
                long_ok(&a, &t); // cancel
                report_app("overwrite_cancel", !a.modal_open && a.page == CAFE_PAGE_SAVE, &a, "cancel keeps list");
                ok(&a, &t); // modal again
                if (a.modal_open) {
                    down(&a, &t); // cancel -> overwrite
                    ok(&a, &t);
                    report_app("overwrite_confirm", !a.modal_open && a.page == CAFE_PAGE_SAVE, &a, "confirm");
                } else report_app("overwrite_confirm", 0, &a, "modal missing");
                long_ok(&a, &t); // menu
                if (!menu_select(&a, &t, 2)) report_app("open_load", 0, &a, "load");
                else {
                    report_app("load_page", a.page == CAFE_PAGE_LOAD, &a, "load list");
                    if (!load_slot(&a, &t, 2)) report_app("load_success", 0, &a, "slot2");
                    else report_app("load_success", a.page == CAFE_PAGE_CHOICE && a.vm.node == 7,
                                    &a, "load must restore CHOICE");
                    // Return to load list/menu, then try an empty slot (slot1).
                    if (a.page == CAFE_PAGE_CHOICE) { int dd = 0; menu_at_choice(&a, &t, &dd); }
                    if (a.page == CAFE_PAGE_MENU) {
                        if (!menu_select(&a, &t, 2)) report_app("reopen_load", 0, &a, "load");
                        else {
                            cafe_page_t before_page = a.page;
                            load_slot(&a, &t, 1);
                            report_app("load_empty_stays", a.page == CAFE_PAGE_LOAD || a.page == before_page,
                                       &a, "empty slot must not advance VM");
                        }
                    } else report_app("reopen_load", 0, &a, "not menu");
                }
            }
        }
    }

    // D. Settings/History roundtrip from a real CHOICE.
    strcpy(current_case, "choice_n7_settings_history");
    begin_case(&a, &ui, &bands, &t);
    if (!reach_choice(&a, &t, NULL, 0, 7)) report_app("reach_choice", 0, &a, "n7");
    else {
        int direct = 0;
        if (!menu_at_choice(&a, &t, &direct)) report_app("open_menu", 0, &a, "menu");
        else {
            if (!menu_select(&a, &t, 4)) report_app("settings_open", 0, &a, "settings");
            else {
                report_app("settings_page", a.page == CAFE_PAGE_SETTINGS, &a, "settings");
                long_ok(&a, &t);
                report_app("settings_back_menu", a.page == CAFE_PAGE_MENU, &a, "settings back");
                if (!menu_select(&a, &t, 3)) report_app("history_open", 0, &a, "history");
                else {
                    report_app("history_page", a.page == CAFE_PAGE_HISTORY, &a, "history");
                    long_ok(&a, &t);
                    report_app("history_back_menu", a.page == CAFE_PAGE_MENU, &a, "history back");
                    report_app("menu_vm_unchanged", a.vm.node == 7, &a, "VM still choice");
                }
            }
        }
    }

    // D2. OP: a long press must not advance; a short OK legally skips.
    strcpy(current_case, "op_menu_no_advance");
    begin_case(&a, &ui, &bands, &t);
    if (!reach_op(&a, &t)) report_app("reach_op", 0, &a, "OP");
    else {
        uint16_t n = a.vm.node, i = a.vm.index;
        report_app("op_page", a.page == CAFE_PAGE_TRANSITION && a.op_pending, &a, "OP page");
        long_ok(&a, &t);
        report_app("op_long_no_advance", a.page == CAFE_PAGE_TRANSITION && a.op_pending &&
                   a.vm.node == n && a.vm.index == i, &a, "long OK must not advance OP");
        ok(&a, &t);
        report_app("op_short_skip", a.vm.node != n || a.vm.index != i || !a.op_pending,
                   &a, "short OK skips OP legally");
    }

    // E. Text-page menu roundtrip: opening/returning must not advance VM.
    strcpy(current_case, "text_menu_roundtrip");
    begin_case(&a, &ui, &bands, &t);
    if (!start_new(&a, &t) || finish_to_decision(&a, &t) != 2) report_app("reach_text", 0, &a, "n7");
    else {
        // At n7 CHOICE, make a regular text state first: choose a branch, then reach next text.
        if (!choose(&a, &t, 0)) report_app("choose_branch", 0, &a, "n7");
        else {
            // We are now on text after n8; use current game page.
            uint16_t n0 = a.vm.node, i0 = a.vm.index;
            long_ok(&a, &t);
            report_app("text_menu_open", a.page == CAFE_PAGE_MENU && a.vm.node == n0 && a.vm.index == i0,
                       &a, "menu from GAME text");
            long_ok(&a, &t);
            report_app("text_menu_close", a.page == CAFE_PAGE_GAME && a.vm.node == n0 && a.vm.index == i0,
                       &a, "close to same GAME text");
            ok(&a, &t);
            report_app("text_short_advance", a.vm.node != n0 || a.vm.index != i0,
                       &a, "short after menu advances");
        }
    }

    // F. Auto save -> menu -> title -> Continue at a real choice.
    strcpy(current_case, "choice_n7_continue");
    begin_case(&a, &ui, &bands, &t);
    if (!reach_choice(&a, &t, NULL, 0, 7)) report_app("reach_choice", 0, &a, "n7");
    else {
        cafe_app_tick(&a, t + 3100, 3100); t += 3100; // progress dirty -> auto save
        int direct = 0;
        if (!menu_at_choice(&a, &t, &direct)) report_app("open_menu", 0, &a, "menu");
        else {
            if (!menu_select(&a, &t, 5)) report_app("menu_to_title", 0, &a, "title");
            else {
                report_app("title_after_menu", a.page == CAFE_PAGE_TITLE, &a, "title");
                // Continue row is title_sel 1; navigate with real DOWN.
                down(&a, &t);
                ok(&a, &t);
                report_app("continue_restores_choice", a.page == CAFE_PAGE_CHOICE && a.vm.node == 7,
                           &a, "title Continue must restore CHOICE");
            }
        }
    }

    // G. Restart from NVS and Continue.
    strcpy(current_case, "restart_continue");
    begin_case(&a, &ui, &bands, &t);
    if (!reach_choice(&a, &t, NULL, 0, 7)) report_app("reach_choice", 0, &a, "n7");
    else {
        cafe_app_tick(&a, t + 3100, 3100); t += 3100;
        cafe_app_flush(&a);
        begin_case(&a, &ui, &bands, &t); // new app instance, same in-memory NVS
        ok(&a, &t); // START -> TITLE
        down(&a, &t);
        ok(&a, &t);
        report_app("restart_continue_choice", a.page == CAFE_PAGE_CHOICE && a.vm.node == 7,
                   &a, "restart Continue");
    }

    // H. Preview save/load must stay chapter preview and must not grant *_after.
    strcpy(current_case, "after_preview_menu_save_load");
    begin_case(&a, &ui, &bands, &t);
    if (!run_prelude(&a, &t, pre_kanna, pre_kanna_n)) report_app("prelude_kanna", 0, &a, "prelude");
    else if (!open_after_gallery(&a, &t, 0)) report_app("open_after", 0, &a, "gallery");
    else if (finish_to_decision(&a, &t) != 2) report_app("after_choice", 0, &a, "n496 choice");
    else {
        uint32_t before = a.unlock.progress;
        int direct = 0;
        if (!menu_at_choice(&a, &t, &direct) || !menu_select(&a, &t, 1) || !save_slot(&a, &t, 1)) {
            report_app("preview_save", 0, &a, "save preview");
        } else {
            long_ok(&a, &t); long_ok(&a, &t); // list -> menu -> game (port close)
            if (!menu_at_choice(&a, &t, &direct)) report_app("preview_reopen_menu", 0, &a, "menu");
            else if (!menu_select(&a, &t, 2)) report_app("preview_load_open", 0, &a, "load");
            else if (!load_slot(&a, &t, 1)) report_app("preview_load", 0, &a, "slot1");
            else {
                report_app("preview_progression", a.vm.progression == CAFE_PROGRESSION_CHAPTER,
                           &a, "chapter preview");
                report_app("preview_unlock_unchanged", a.unlock.progress == before,
                           &a, "no after bit");
                report_app("preview_card", (a.unlock.progress & (1u << CAFE_PROG_KANNA_AFTER)) == 0, &a, "no after bit");
            }
        }
    }

    printf("MENU_SUMMARY|%d|%d|op_skips=%d|ui_ending_count=%d\n", checks, failures, op_skips, audit_ui_ending_count);
    free(pack);
    return failures ? 1 : 0;
}
