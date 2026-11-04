// Checkpoint regression: a pending choice/objection/text position must survive
// entering BOOK/BOOK_DETAIL/HISTORY/SETTINGS/LOAD/SAVE, flushing, and a fresh
// app boot + "continue". Uses only real key sequences and the production app.
#include "gal_app.h"
#include "gal_pack.h"
#include "gal_save.h"
#include "gal_store.h"
#include "gal_vm.h"
#include "bsp_button.h"
#include "esp_timer.h"
#include "jpeg_decoder.h"
#include "lvgl.h"
#include "nvs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const lv_font_t manosaba_16 = {0};

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); failures++; } \
} while (0)

static uint16_t art[320 * 150], strip[160 * 45];
static uint8_t scratch[150 * 150 * 2];
static gal_pack_t pack;
static gal_app_t app;
static uint8_t *pdata;
static uint32_t psize;
static uint32_t now = 1000;
static gal_vm_t tmp;

static uint8_t *load_file(const char *path, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) exit(2);
    fclose(f);
    *size = (uint32_t)n;
    return b;
}

static void key(uint8_t btn, uint8_t ev) { const gal_key_t k = { btn, ev }; gal_app_key(&app, &k); }
static void tick(void) { now += 40; esp_timer_stub_set_us((uint64_t)now * 1000); gal_app_tick(&app, now); }
static void tap(uint8_t btn) { key(btn, BSP_BTN_PRESS); key(btn, BSP_BTN_RELEASE); key(btn, BSP_BTN_CLICK); tick(); }
static void long_ok(void) { key(BSP_BTN_OK, BSP_BTN_PRESS); key(BSP_BTN_OK, BSP_BTN_LONG); key(BSP_BTN_OK, BSP_BTN_RELEASE); tick(); }

static void boot(void)
{
    lv_stub_reset();
    lv_stub_require_lock(true);
    nvs_stub_reset();
    esp_timer_stub_set_us(1000000);
    now = 1000;
    CHECK(gal_app_init(&app, &pack, art, strip, scratch, sizeof(scratch)), "init");
    app.settings.text_speed = 0;
}
static void reinit(void)
{
    lv_stub_reset();
    lv_stub_require_lock(true);
    esp_timer_stub_set_us((uint64_t)now * 1000);
    CHECK(gal_app_init(&app, &pack, art, strip, scratch, sizeof(scratch)), "reinit");
    app.settings.text_speed = 0;
}
static void start(void) { app.title_sel = 0; tap(BSP_BTN_OK); }

static bool reach_choice(void)
{
    start();
    for (int i = 0; i < 6000; ++i) {
        if (app.page == APP_PAGE_CHOICE) return true;
        if (app.page == APP_PAGE_ENDING || app.page == APP_PAGE_ERROR) return false;
        tap(BSP_BTN_UP);
    }
    return false;
}

static void open_menu_from_choice(void)
{
    long_ok();
    CHECK(app.page == APP_PAGE_MENU, "menu from choice page=%s", gal_app_page_name(app.page));
}
static void choose_menu(int item)
{
    for (int i = 0; i < 20 && app.menu_sel < item; ++i) tap(BSP_BTN_DOWN);
    for (int i = 0; i < 20 && app.menu_sel > item; ++i) tap(BSP_BTN_UP);
    tap(BSP_BTN_OK);
}
static void open_sub(int sub)
{
    open_menu_from_choice();
    if (sub == 0) {
        choose_menu(4);              // book
        CHECK(app.page == APP_PAGE_BOOK, "book page=%s", gal_app_page_name(app.page));
        tap(BSP_BTN_OK);             // book detail
        CHECK(app.page == APP_PAGE_BOOK_DETAIL, "book detail page=%s",
              gal_app_page_name(app.page));
    } else if (sub == 1) {
        choose_menu(3);
        CHECK(app.page == APP_PAGE_HISTORY, "history page=%s", gal_app_page_name(app.page));
    } else if (sub == 2) {
        choose_menu(7);
        CHECK(app.page == APP_PAGE_SETTINGS, "settings page=%s", gal_app_page_name(app.page));
    } else if (sub == 3) {
        choose_menu(2);
        CHECK(app.page == APP_PAGE_LOAD, "load page=%s", gal_app_page_name(app.page));
    } else {
        choose_menu(1);
        CHECK(app.page == APP_PAGE_SAVE, "save page=%s", gal_app_page_name(app.page));
    }
}

static bool decode_auto(gal_save_ui_t *ui)
{
    size_t len = 0;
    if (gal_store_read_slot(&app.store, GAL_SLOT_AUTO, app.save_blob,
                            sizeof(app.save_blob), &len) != GAL_STORE_OK) return false;
    static uint8_t blob[GAL_SAVE_MAX];
    memcpy(blob, app.save_blob, len);
    const char *err = NULL;
    if (!gal_save_decode(blob, len, &pack, &tmp, ui, &err)) {
        fprintf(stderr, "decode auto: %s\n", err ? err : "?");
        return false;
    }
    return true;
}

static void continue_auto(void)
{
    reinit();
    app.title_sel = 1;
    tap(BSP_BTN_OK);
}

static void run_choice_matrix(void)
{
    boot();
    if (!reach_choice()) { CHECK(false, "no choice reached"); return; }
    tap(BSP_BTN_DOWN);
    CHECK(app.choice_sel == 1, "choice sel=%d", app.choice_sel);
    if (app.vm.book_count == 0) {
        app.vm.book_count = 1;
        app.vm.book_order[0] = 0;
        app.vm.book[0] = 1;
    }
    const uint16_t cursor = app.vm.cursor;
    const int row = app.choice_sel;
    for (int sub = 0; sub < 5; ++sub) {
        open_sub(sub);
        gal_app_flush(&app);
        gal_save_ui_t ui;
        CHECK(decode_auto(&ui), "choice sub=%d auto decode", sub);
        CHECK(ui.scene_mode == 1, "choice sub=%d scene_mode=%u", sub, ui.scene_mode);
        CHECK(ui.choice_row == row, "choice sub=%d row=%u want=%d", sub, ui.choice_row, row);
        CHECK(tmp.cursor == cursor, "choice sub=%d cursor=%u want=%u", sub, tmp.cursor, cursor);
        continue_auto();
        CHECK(app.page == APP_PAGE_CHOICE, "choice sub=%d restore page=%s", sub,
              gal_app_page_name(app.page));
        CHECK(app.choice_sel == row, "choice sub=%d restore row=%d", sub, app.choice_sel);
        CHECK(app.vm.cursor == cursor, "choice sub=%d restore cursor=%u/%u", sub,
              app.vm.cursor, cursor);
    }
}

static bool make_objection_save(void)
{
    // Find a LINK_OPS op and its scene, run a VM to its text frame, and save
    // that exact VM with scene_mode=2 (objection page).
    for (uint32_t i = 0; i < pack.count[GAL_SEC_LINK_OPS]; ++i) {
        const gal_link_op_t *lo = gal_pack_link_op(&pack, (uint16_t)i);
        if (!lo || lo->count == 0) continue;
        int scene = -1;
        for (uint32_t s = 0; s < pack.count[GAL_SEC_SCENES]; ++s) {
            const gal_scene_t *sc = gal_pack_scene(&pack, (uint16_t)s);
            if (sc && lo->op_index >= sc->op_start &&
                lo->op_index < sc->op_start + sc->op_count) {
                scene = (int)s;
                break;
            }
        }
        if (scene < 0) continue;
        const gal_scene_t *sc = gal_pack_scene(&pack, (uint16_t)scene);
        gal_vm_t vm;
        gal_vm_init(&vm, &pack);
        const char *err = NULL;
        if (!gal_vm_start(&vm, (uint16_t)scene, true, false, &err)) continue;
        vm.cursor = (uint16_t)(lo->op_index - sc->op_start);
        vm.root = (uint16_t)scene;
        gal_frame_t f;
        if (gal_vm_read(&vm, &f) != GAL_FRAME_TEXT || f.link_count == 0) continue;
        gal_save_ui_t ui = { .scene_mode = 2, .page = 0, .choice_row = 1 };
        uint8_t blob[GAL_SAVE_MAX];
        size_t len = 0;
        if (!gal_save_encode(&vm, &ui, blob, sizeof(blob), &len)) continue;
        if (!gal_store_save_slot(&app.store, 0, blob, len)) return false;
        return true;
    }
    return false;
}

static void load_fixture_slot0(void)
{
    app.title_sel = 2;
    tap(BSP_BTN_OK);
    CHECK(app.page == APP_PAGE_LOAD, "fixture load page=%s", gal_app_page_name(app.page));
    tap(BSP_BTN_OK);
    CHECK(app.page == APP_PAGE_CHOICE, "fixture restored page=%s", gal_app_page_name(app.page));
    CHECK(app.objection_mode, "fixture not objection mode");
    CHECK(app.choice_sel == 1, "fixture choice_sel=%d", app.choice_sel);
    if (app.vm.book_count == 0) {
        app.vm.book_count = 1;
        app.vm.book_order[0] = 0;
        app.vm.book[0] = 1;
    }
}

static void run_objection_matrix(void)
{
    boot();
    CHECK(make_objection_save(), "objection fixture");
    load_fixture_slot0();
    const uint16_t cursor = app.vm.cursor;
    const int count = app.choice_count;
    for (int sub = 0; sub < 5; ++sub) {
        open_sub(sub);
        gal_app_flush(&app);
        gal_save_ui_t ui;
        CHECK(decode_auto(&ui), "objection sub=%d auto decode", sub);
        CHECK(ui.scene_mode == 2, "objection sub=%d scene_mode=%u", sub, ui.scene_mode);
        CHECK(ui.choice_row == 1, "objection sub=%d row=%u", sub, ui.choice_row);
        CHECK(tmp.cursor == cursor, "objection sub=%d cursor=%u want=%u", sub, tmp.cursor, cursor);
        continue_auto();
        CHECK(app.page == APP_PAGE_CHOICE, "objection sub=%d restore page=%s", sub,
              gal_app_page_name(app.page));
        CHECK(app.objection_mode, "objection sub=%d restore mode", sub);
        CHECK(app.choice_sel == 1, "objection sub=%d restore sel=%d", sub, app.choice_sel);
        CHECK(app.choice_count == count, "objection sub=%d count=%d/%d", sub,
              app.choice_count, count);
        CHECK(app.vm.cursor == cursor, "objection sub=%d restore cursor=%u/%u", sub,
              app.vm.cursor, cursor);
    }
}

static void run_text_page(void)
{
    boot();
    start();
    for (int i = 0; i < 6000; ++i) {
        if (app.page == APP_PAGE_ERROR || app.page == APP_PAGE_ENDING) break;
        if (app.page == APP_PAGE_GAME && app.text_pages > 1) {
            const uint16_t cursor = app.vm.cursor;
            tap(BSP_BTN_UP);  // advance to page 1
            if (app.page != APP_PAGE_GAME || app.text_page == 0) continue;
            open_sub(1);      // history
            gal_app_flush(&app);
            gal_save_ui_t ui;
            CHECK(decode_auto(&ui), "text auto decode");
            CHECK(ui.scene_mode == 0, "text scene_mode=%u", ui.scene_mode);
            CHECK(ui.page == app.text_page, "text page=%u/%u", ui.page, app.text_page);
            CHECK(tmp.cursor == cursor, "text cursor=%u/%u", tmp.cursor, cursor);
            continue_auto();
            CHECK(app.page == APP_PAGE_GAME, "text restore page=%s", gal_app_page_name(app.page));
            CHECK(app.text_page == ui.page, "text restore page=%u/%u", app.text_page, ui.page);
            CHECK(app.vm.cursor == cursor, "text restore cursor=%u/%u", app.vm.cursor, cursor);
            return;
        }
        tap(BSP_BTN_UP);
    }
    CHECK(false, "no multi-page text found");
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (argc < 2) return 2;
    pdata = load_file(argv[1], &psize);
    const char *err = NULL;
    if (!gal_pack_open(&pack, pdata, psize, &err)) {
        fprintf(stderr, "pack: %s\n", err);
        return 1;
    }
    run_choice_matrix();
    run_objection_matrix();
    run_text_page();
    printf("test_gal_checkpoint: %s\n", failures ? "FAIL" : "PASS");
    free(pdata);
    return failures ? 1 : 0;
}
