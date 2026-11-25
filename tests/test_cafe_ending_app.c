// Production cafe_app flow witness harness. UI/NVS/audio/image are controlled host stubs.
// Prelude runs a real app ending path to produce permanent bits before the tested route.
#include "cafe_app.h"
#include "cafe_store.h"
#include "cafe_prelude.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int audit_ui_page, audit_ui_rows_count, audit_ui_rows_selected;
extern int audit_ui_choice_count, audit_ui_choice_selected, audit_ui_modal, audit_ui_ending_count;
extern char audit_ui_ending[512];
static int op_skips;

static unsigned char *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f); *size = (size_t)n; return b;
}
static void tap(cafe_app_t *a, uint8_t key, uint32_t *t) {
    cafe_app_key(a, key, true, *t); cafe_app_key(a, key, false, *t + 40); *t += 80;
}
static void ok(cafe_app_t *a, uint32_t *t) { tap(a, CAFE_KEY_OK, t); }
static void down(cafe_app_t *a, uint32_t *t) { tap(a, CAFE_KEY_DOWN, t); }

static int finish_to_decision(cafe_app_t *a, uint32_t *t, int *last_nav) {
    (void)last_nav;
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
static int choose(cafe_app_t *a, uint32_t *t, int target, int *last_nav) {
    if (a->page != CAFE_PAGE_CHOICE || target < 0 || target >= a->choice_count) return 0;
    int count = a->choice_count;
    for (int i = a->choice_sel; i < target; ++i) down(a, t);
    if (target == count - 1) *last_nav = 1;
    if (a->choice_sel != target) return 0;
    ok(a, t);
    return 1;
}
static int run_path(cafe_app_t *a, uint32_t *t, const int *actions, int n, int *last_nav) {
    for (int i = 0; i < n; ++i) {
        if (finish_to_decision(a, t, last_nav) != 2) return 0;
        if (!choose(a, t, actions[i], last_nav)) return 0;
    }
    return finish_to_decision(a, t, last_nav) == 1;
}
static int start_game_from_title(cafe_app_t *a, uint32_t *t) {
    if (a->page == CAFE_PAGE_START) ok(a, t);
    if (a->page != CAFE_PAGE_TITLE) return 0;
    ok(a, t); // New Game row
    return a->page == CAFE_PAGE_GAME || a->page == CAFE_PAGE_TRANSITION || a->page == CAFE_PAGE_CHOICE;
}
static int open_prelude(cafe_app_t *a, uint32_t *t, const char *name) {
    const int *actions = NULL; int n = 0;
    if (strcmp(name, "kanna") == 0) { actions = pre_kanna; n = pre_kanna_n; }
    else if (strcmp(name, "natsume") == 0) { actions = pre_natsume; n = pre_natsume_n; }
    else if (strcmp(name, "nozomi") == 0) { actions = pre_nozomi; n = pre_nozomi_n; }
    else if (strcmp(name, "mei") == 0) { actions = pre_mei; n = pre_mei_n; }
    else if (strcmp(name, "suzune") == 0) { actions = pre_suzune; n = pre_suzune_n; }
    else return 0;
    if (!start_game_from_title(a, t)) return 0;
    int ignored = 0;
    if (!run_path(a, t, actions, n, &ignored)) return 0;
    if (a->page != CAFE_PAGE_ENDING) return 0;
    ok(a, t); // ending -> title
    return a->page == CAFE_PAGE_TITLE;
}
static int open_after(cafe_app_t *a, uint32_t *t, int idx) {
    if (a->page == CAFE_PAGE_START) ok(a, t);
    if (a->page == CAFE_PAGE_ENDING) ok(a, t);
    if (a->page != CAFE_PAGE_TITLE) return 0;
    for (int i = 0; i < 4; ++i) down(a, t); // Gallery row
    if (a->title_sel != 4) return 0;
    ok(a, t);
    if (a->page != CAFE_PAGE_GALLERY) return 0;
    down(a, t); down(a, t); // chapter/directory
    if (a->gallery_sel != 2) return 0;
    ok(a, t);
    if (a->page != CAFE_PAGE_CHAPTER) return 0;
    for (int i = 0; i < 6; ++i) down(a, t); // after route
    if (a->route_sel != 6) return 0;
    ok(a, t);
    if (a->page != CAFE_PAGE_CHAPTER || a->chapter_routes) return 0;
    for (int i = 0; i < idx; ++i) down(a, t);
    if (a->chapter_sel != idx) return 0;
    ok(a, t);
    if (a->page != CAFE_PAGE_TRANSITION) return 0;
    ok(a, t); // preview confirmation
    return a->page == CAFE_PAGE_GAME || a->page == CAFE_PAGE_CHOICE || a->page == CAFE_PAGE_TRANSITION;
}
static int expected_bit(const char *name) {
    static const char *keys[] = {"bad","normal","kanna","natsume","nozomi","mei","suzune"};
    for (int i = 0; i < 7; ++i) if (strcmp(name, keys[i]) == 0) return i;
    return -1;
}
int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <cafe.pak> <app-cases>\n", argv[0]); return 2; }
    size_t pn = 0; unsigned char *pack = read_file(argv[1], &pn);
    if (!pack || !cafe_pack_set_data(pack, pn)) { fprintf(stderr, "pack load failed\n"); return 2; }
    FILE *f = fopen(argv[2], "r"); if (!f) { fprintf(stderr, "cases open failed\n"); return 2; }
    char line[4096]; int cases = 0, failures = 0;
    while (fgets(line, sizeof(line), f)) {
        char name[96], kind[24], target[64], action_s[2048];
        unsigned bits = 0;
        if (sscanf(line, "CASE %95s %23s %u %63s %2047s", name, kind, &bits, target, action_s) != 5) {
            fprintf(stderr, "bad case: %s\n", line); return 1;
        }
        cafe_ui_t ui; cafe_bands_t bands; cafe_app_t app;
        memset(&ui, 0, sizeof(ui)); memset(&bands, 0, sizeof(bands));
        if (!cafe_app_init(&app, &ui, &bands)) { fprintf(stderr, "init fail\n"); return 1; }
        // Fresh-install profile. Prelude endings below produce all tested permanent bits.
        memset(&app.unlock, 0, sizeof(app.unlock));
        app.unlock.last_cleared = 0xFF;
        (void)cafe_store_save_unlock(&app.unlock);
        app.now_ms = 1000; app.last_activity_ms = 1000;
        uint32_t t = 1000; int last_nav = 0; int started = 0; int passed = 0;
        op_skips = 0;
        int ending_bit = expected_bit(target);
        int after_flag = strcmp(kind, "after") == 0;
        int pre_ok = 1;
        if (after_flag) {
            int idx = atoi(target);
            if (idx == 0) pre_ok &= open_prelude(&app, &t, "kanna");
            else if (idx == 1) pre_ok &= open_prelude(&app, &t, "natsume");
            else if (idx == 2) pre_ok &= open_prelude(&app, &t, "nozomi");
            else if (idx == 3) pre_ok &= open_prelude(&app, &t, "mei");
            else if (idx == 4) { pre_ok &= open_prelude(&app, &t, "kanna"); pre_ok &= open_prelude(&app, &t, "suzune"); }
        } else if (strcmp(target, "suzune") == 0) {
            pre_ok &= open_prelude(&app, &t, "kanna");
        }
        if (!pre_ok) {
            printf("PRELUDE_FAIL|%s\n", name);
        } else {
            if ((int)app.unlock.progress != (int)bits) {
                printf("BITS_MISMATCH|%s|unlock=%u|expected=%u\n", name, (unsigned)app.unlock.progress, bits);
                pre_ok = 0;
            }
        }
        if (pre_ok) {
            if (after_flag) started = open_after(&app, &t, atoi(target));
            else started = start_game_from_title(&app, &t);
        }
        if (started) {
            int actions[64], n = 0;
            if (strcmp(action_s, "-") != 0) {
                char *copy = strdup(action_s), *save = NULL, *tok = strtok_r(copy, ",", &save);
                while (tok && n < 64) { actions[n++] = atoi(tok); tok = strtok_r(NULL, ",", &save); }
                free(copy);
            }
            int reach = run_path(&app, &t, actions, n, &last_nav);
            if (reach && app.page == CAFE_PAGE_ENDING) passed = 1;
        }
        int unlock_after = (int)app.unlock.progress;
        cafe_app_flush(&app);
        cafe_unlock_t persisted; memset(&persisted, 0, sizeof(persisted));
        (void)cafe_store_load_unlock(&persisted);
        int has_bit = ending_bit >= 0 && (app.unlock.progress & (1u << ending_bit)) != 0;
        int persisted_bit = ending_bit >= 0 && (persisted.progress & (1u << ending_bit)) != 0;
        int card_ok = audit_ui_ending[0] != 0 && audit_ui_ending[0] != '|';
        int okcase = passed;
        if (after_flag) {
            okcase = okcase && app.unlock.progress == bits &&
                     strstr(audit_ui_ending, "PREVIEW END") != NULL && !has_bit;
        } else {
            okcase = okcase && app.ending_bit == ending_bit && card_ok && has_bit && persisted_bit;
        }
        if (!okcase) failures++;
        printf("APP|%s|%s|%u|%d|%d|%d|%d|%d|%d|%d|%d|%s|%s",
               name, okcase ? "PASS" : "FAIL", bits, started, passed, app.page,
               (int)app.ending_bit, unlock_after, (int)bits, has_bit, last_nav,
               after_flag ? "after" : "ending", audit_ui_ending);
        printf("|node=%u,idx=%u,pre_ok=%d,op_skips=%d,card=%d,persisted=%d\n",
               (unsigned)app.vm.node, (unsigned)app.vm.index, pre_ok, op_skips, card_ok, persisted_bit);
        cases++;
    }
    fclose(f); free(pack);
    printf("APP_REPLAY|%d|%d\n", cases, failures);
    return failures ? 1 : 0;
}
