// Production nine_app input/state machine driven on the host with a controlled
// clock and a persistent NVS stub. No alternate host behavior is compiled in.
#include "nine_app.h"
#include "nine_image.h"
#include "gal_ui.h"
#include "gal_stub.h"
#include "nvs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

static uint8_t *load(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) exit(2);
    fclose(f); *size = (size_t)n; return b;
}
static void key(nine_app_t *a, int btn, int ev) { nine_key_t k = {(uint8_t)btn, (uint8_t)ev}; nine_app_key(a, &k); }
static void click(nine_app_t *a, int btn) { key(a, btn, NINE_EV_PRESS); key(a, btn, NINE_EV_CLICK); key(a, btn, NINE_EV_RELEASE); }
static void longp(nine_app_t *a, int btn) { key(a, btn, NINE_EV_PRESS); key(a, btn, NINE_EV_LONG); key(a, btn, NINE_EV_RELEASE); }
static void copy_text(const nine_app_t *a, char *out, size_t cap)
{
    snprintf(out, cap, "%s", nine_app_current_text(a));
}
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t crc32_bytes(const uint8_t *p, size_t n)
{
    uint32_t c = ~0u;
    while (n--) {
        c ^= *p++;
        for (unsigned b = 0; b < 8; ++b) c = (c >> 1) ^ ((0u - (c & 1u)) & 0xedb88320u);
    }
    return ~c;
}
static uint32_t crc32_zero(const uint8_t *p, size_t n, size_t zoff, size_t zn)
{
    uint32_t c = ~0u;
    for (size_t i = 0; i < n; ++i) {
        uint8_t b = i >= zoff && i - zoff < zn ? 0 : p[i];
        c ^= b;
        for (unsigned j = 0; j < 8; ++j) c = (c >> 1) ^ ((0u - (c & 1u)) & 0xedb88320u);
    }
    return ~c;
}
static void seed_loop_progress(const uint8_t *script, size_t sn)
{
    static nine_pack_t pack;
    static nine_vm_t vm;
    static nine_store_t store;
    CHECK(nine_pack_open(&pack, script, sn), "seed pack");
    CHECK(nine_vm_init(&vm, &pack), "seed vm");
    int k = nine_pack_find_key(&pack, "loop_my");
    CHECK(k >= 0, "seed key");
    vm.state.progress[k] = 1;
    vm.state.progress_set[k / 8] |= (uint8_t)(1u << (k % 8));
    uint8_t buf[NINE_PROGRESS_RECORD_MAX];
    size_t n = nine_vm_progress_encode(&vm, buf, sizeof(buf));
    CHECK(nine_store_init(&store, true), "seed store");
    CHECK(nine_store_progress_save(&store, buf, n), "seed progress");
}

int main(int argc, char **argv)
{
    if (argc < 3) { printf("usage: %s script images\n", argv[0]); return 2; }
    size_t sn, imn;
    uint8_t *script = load(argv[1], &sn);
    uint8_t *images = load(argv[2], &imn);

    CHECK(nine_app_landscape_ok(false, true, 320, 240) == false, "landscape lock gate");
    CHECK(nine_app_landscape_ok(true, false, 320, 240) == false, "landscape set gate");
    CHECK(nine_app_landscape_ok(true, true, 240, 320) == false, "landscape size gate");
    CHECK(nine_app_landscape_ok(true, true, 320, 240) == true, "landscape ready");
    CHECK(!nine_app_first_frame_ready(false, true), "first frame lock gate");
    CHECK(!nine_app_first_frame_ready(true, false), "first frame timeout gate");
    CHECK(nine_app_first_frame_ready(true, true), "first frame submitted gate");

    nvs_stub_reset();
    gal_stub_reset();
    seed_loop_progress(script, sn);

    static nine_app_t app;
    CHECK(nine_app_init(&app, script, sn, images, imn), "init");
    CHECK(!app.fatal, "not fatal");
    CHECK(nine_image_band_count() == 4, "four production image bands");
    CHECK(app.page == GAL_PAGE_START, "start page");
    click(&app, NINE_BTN_OK);
    CHECK(app.page == GAL_PAGE_TITLE, "title after start");
    click(&app, NINE_BTN_OK);
    CHECK(app.page == GAL_PAGE_GAME, "new game");
    CHECK(nine_app_current_text(&app)[0] != 0, "first text");
    printf("first: %s\n", nine_app_current_text(&app));

    /* Every reading key is a real advance even while the typewriter is active.
     * A single press must move to the next pagination unit/line, never only
     * complete the animation. */
    char b[2048], c[2048];
    CHECK(gal_ui_typing(&app.ui), "typing at first frame");
    copy_text(&app, b, sizeof(b));
    click(&app, NINE_BTN_OK);
    copy_text(&app, c, sizeof(c));
    CHECK(strcmp(b, c) != 0, "short OK advances while typing");
    CHECK(gal_ui_typing(&app.ui), "next unit starts typing");
    copy_text(&app, b, sizeof(b));
    click(&app, NINE_BTN_UP);
    copy_text(&app, c, sizeof(c));
    CHECK(strcmp(b, c) != 0, "short UP advances while typing");
    copy_text(&app, b, sizeof(b));
    click(&app, NINE_BTN_DOWN);
    copy_text(&app, c, sizeof(c));
    CHECK(strcmp(b, c) != 0, "short DOWN advances while typing");
    copy_text(&app, b, sizeof(b));
    key(&app, NINE_BTN_OK, NINE_EV_PRESS);
    key(&app, NINE_BTN_OK, NINE_EV_DOUBLE);
    key(&app, NINE_BTN_OK, NINE_EV_RELEASE);
    copy_text(&app, c, sizeof(c));
    CHECK(strcmp(b, c) != 0, "DOUBLE acts as one advance");

    /* A long line is paged; one short press advances one full page. Use a
     * fresh app so the first known 80-character line (n0 frame 2) is reached. */
    static nine_app_t pageapp;
    gal_stub_reset();
    CHECK(nine_app_init(&pageapp, script, sn, images, imn), "page app init");
    click(&pageapp, NINE_BTN_OK);
    click(&pageapp, NINE_BTN_OK);
    // Use the 20 px tier: the first 60-character line then needs two pages.
    longp(&pageapp, NINE_BTN_OK);
    for (int i = 0; i < 4; ++i) click(&pageapp, NINE_BTN_DOWN);
    click(&pageapp, NINE_BTN_OK);   // settings
    click(&pageapp, NINE_BTN_DOWN); // font row
    click(&pageapp, NINE_BTN_OK);   // 16 -> 20 px
    longp(&pageapp, NINE_BTN_OK);   // back directly to game
    // At 20 px the first long line itself has several pages; keep pressing
    // until the known 80-character frame is actually reached, not a page.
    for (int i = 0; i < 30 && pageapp.vm.view.frame < 2; ++i) click(&pageapp, NINE_BTN_OK);
    CHECK(pageapp.vm.view.frame == 2, "reached long frame");
    CHECK(pageapp.page_count > 1, "long line has multiple pages");
    CHECK(pageapp.text_page == 0, "long line starts on page 0");
    if (pageapp.page_count > 1 && pageapp.text_page == 0) {
        copy_text(&pageapp, b, sizeof(b));
        click(&pageapp, NINE_BTN_OK);
        CHECK(pageapp.text_page == 1, "short press advances one page");
        CHECK(strcmp(b, nine_app_current_text(&pageapp)) != 0, "page content changed");
    }

    /* long DOWN toggles auto and does not advance; long OK opens the menu and
     * does not advance. Releasing either long press adds no short advance. */
    copy_text(&app, b, sizeof(b));
    longp(&app, NINE_BTN_DOWN);
    CHECK(nine_app_auto_on(&app), "long DOWN enables auto");
    CHECK(strcmp(b, nine_app_current_text(&app)) == 0, "long DOWN does not advance");
    copy_text(&app, b, sizeof(b));
    longp(&app, NINE_BTN_DOWN);
    CHECK(!nine_app_auto_on(&app), "long DOWN disables auto");
    CHECK(strcmp(b, nine_app_current_text(&app)) == 0, "long DOWN release does not advance");
    copy_text(&app, b, sizeof(b));
    longp(&app, NINE_BTN_OK);
    CHECK(app.page == GAL_PAGE_MENU, "long OK opens menu");
    CHECK(strcmp(b, nine_app_current_text(&app)) == 0, "long OK does not advance");
    longp(&app, NINE_BTN_OK);
    CHECK(app.page == GAL_PAGE_GAME, "long OK returns to reading");

    /* Auto advances on its own clock; a short press stops it and advances. */
    longp(&app, NINE_BTN_DOWN);
    CHECK(nine_app_auto_on(&app), "auto re-enabled");
    if (gal_ui_typing(&app.ui)) gal_ui_show_full_text(&app.ui);
    copy_text(&app, b, sizeof(b));
    int auto_advanced = 0;
    for (int i = 0; i < 20; ++i) {
        nine_app_tick(&app, 500);
        if (strcmp(b, nine_app_current_text(&app)) != 0) { auto_advanced = 1; break; }
    }
    CHECK(auto_advanced, "auto advances without a key");
    CHECK(app.idle_ms == 0, "auto advance resets idle");
    copy_text(&app, b, sizeof(b));
    click(&app, NINE_BTN_DOWN);
    CHECK(!nine_app_auto_on(&app), "short press stops auto");
    CHECK(strcmp(b, nine_app_current_text(&app)) != 0, "short press during auto advances");
    /* Each reading key must both stop auto and advance. */
    for (int k = 0; k < 3; ++k) {
        const int btn = k == 0 ? NINE_BTN_OK : (k == 1 ? NINE_BTN_UP : NINE_BTN_DOWN);
        longp(&app, NINE_BTN_DOWN);
        CHECK(nine_app_auto_on(&app), "auto on for key loop");
        if (gal_ui_typing(&app.ui)) gal_ui_show_full_text(&app.ui);
        copy_text(&app, b, sizeof(b));
        click(&app, btn);
        CHECK(!nine_app_auto_on(&app), "short key stops auto");
        CHECK(strcmp(b, nine_app_current_text(&app)) != 0, "short key during auto advances");
    }
    longp(&app, NINE_BTN_DOWN);
    CHECK(nine_app_auto_on(&app), "auto on before page switch");
    longp(&app, NINE_BTN_OK);
    CHECK(app.page == GAL_PAGE_MENU && !nine_app_auto_on(&app), "page switch clears auto");
    longp(&app, NINE_BTN_OK);
    CHECK(app.page == GAL_PAGE_GAME, "back from menu after auto test");

    /* Long UP skips typing and the auto delay and advances on the LONG event
     * itself; releasing stops it; a page switch clears the hold. */
    if (gal_ui_typing(&app.ui)) gal_ui_show_full_text(&app.ui);
    copy_text(&app, b, sizeof(b));
    key(&app, NINE_BTN_UP, NINE_EV_PRESS);
    key(&app, NINE_BTN_UP, NINE_EV_LONG);
    CHECK(nine_app_fast_forwarding(&app), "ff active");
    CHECK(strcmp(b, nine_app_current_text(&app)) != 0, "ff advances on the long press");
    key(&app, NINE_BTN_UP, NINE_EV_RELEASE);
    CHECK(!nine_app_fast_forwarding(&app), "ff release stops");
    copy_text(&app, b, sizeof(b));
    nine_app_tick(&app, 2000);
    CHECK(strcmp(b, nine_app_current_text(&app)) == 0, "no advance after release");
    key(&app, NINE_BTN_UP, NINE_EV_PRESS);
    key(&app, NINE_BTN_UP, NINE_EV_LONG);
    longp(&app, NINE_BTN_OK);
    CHECK(app.page == GAL_PAGE_MENU, "long OK during ff opens menu");
    CHECK(!nine_app_fast_forwarding(&app), "page switch clears ff");
    longp(&app, NINE_BTN_OK);
    CHECK(app.page == GAL_PAGE_GAME, "back to reading");

    /* Save slot 0, then load it through the menu. */
    longp(&app, NINE_BTN_OK);
    CHECK(app.page == GAL_PAGE_MENU, "menu for save");
    click(&app, NINE_BTN_OK);
    CHECK(app.page == GAL_PAGE_SAVE, "save page");
    click(&app, NINE_BTN_OK);
    CHECK(app.store.writes > 0, "save wrote nvs");
    CHECK(app.slots[0].occupied, "slot occupied");
    longp(&app, NINE_BTN_OK);
    CHECK(app.page == GAL_PAGE_GAME, "back from save");
    longp(&app, NINE_BTN_OK);
    click(&app, NINE_BTN_DOWN); click(&app, NINE_BTN_OK);
    CHECK(app.page == GAL_PAGE_LOAD, "load page");
    click(&app, NINE_BTN_OK);
    CHECK(app.page == GAL_PAGE_GAME, "loaded");

    /* Chapter preview from the gallery; it uses the 4-row preview menu. */
    longp(&app, NINE_BTN_OK);
    CHECK(app.page == GAL_PAGE_MENU, "menu for title");
    for (int i = 0; i < 5; ++i) click(&app, NINE_BTN_DOWN);
    click(&app, NINE_BTN_OK);
    CHECK(app.page == GAL_PAGE_TITLE, "returned to title");
    for (int i = 0; i < 4; ++i) click(&app, NINE_BTN_DOWN);
    click(&app, NINE_BTN_OK);
    CHECK(app.page == GAL_PAGE_GALLERY, "gallery");
    click(&app, NINE_BTN_DOWN); click(&app, NINE_BTN_OK);
    CHECK(app.page == GAL_PAGE_CHAPTER, "chapters");
    click(&app, NINE_BTN_OK);
    CHECK(app.page == GAL_PAGE_GAME, "chapter preview");
    longp(&app, NINE_BTN_OK);
    CHECK(app.page == GAL_PAGE_MENU, "preview menu");
    CHECK(gal_stub()->rows_count[GAL_LIST_MENU] == 3, "preview menu has 3 rows");
    for (int i = 0; i < 4; ++i) click(&app, NINE_BTN_DOWN);
    CHECK(app.menu_sel >= 0 && app.menu_sel < 3, "preview menu wraps within 3 rows");
    longp(&app, NINE_BTN_OK);
    CHECK(app.page == GAL_PAGE_GAME, "preview long OK back");

    /* Fast-forward must stop at the graph OP, not run through it. Skip the OP
     * with a normal short press, then continue to the choice. */
    static nine_app_t ffapp;
    gal_stub_reset();
    CHECK(nine_app_init(&ffapp, script, sn, images, imn), "ff app init");
    click(&ffapp, NINE_BTN_OK);
    click(&ffapp, NINE_BTN_OK);
    CHECK(ffapp.page == GAL_PAGE_GAME, "ff app game");
    key(&ffapp, NINE_BTN_UP, NINE_EV_PRESS);
    key(&ffapp, NINE_BTN_UP, NINE_EV_LONG);
    for (int i = 0; i < 5000 && ffapp.page == GAL_PAGE_GAME; ++i) nine_app_tick(&ffapp, 50);
    CHECK(ffapp.page == GAL_PAGE_CHOICE || ffapp.page == GAL_PAGE_OPENING,
          "ff stops at choice or OP");
    CHECK(!nine_app_fast_forwarding(&ffapp), "ff cleared at stop");
    key(&ffapp, NINE_BTN_UP, NINE_EV_RELEASE);
    if (ffapp.page == GAL_PAGE_OPENING) {
        click(&ffapp, NINE_BTN_OK);   // skip OP
        key(&ffapp, NINE_BTN_UP, NINE_EV_PRESS);
        key(&ffapp, NINE_BTN_UP, NINE_EV_LONG);
        for (int i = 0; i < 5000 && ffapp.page == GAL_PAGE_GAME; ++i) nine_app_tick(&ffapp, 50);
    }
    CHECK(ffapp.page == GAL_PAGE_CHOICE, "ff stops at choice");
    CHECK(!nine_app_fast_forwarding(&ffapp), "ff cleared at choice");
    CHECK(ffapp.vm.view.choice_count == 2, "choice has two options");
    key(&ffapp, NINE_BTN_UP, NINE_EV_RELEASE);
    click(&ffapp, NINE_BTN_OK);
    CHECK(ffapp.page == GAL_PAGE_GAME, "choice selection resumes reading");

    /* A fresh boot continues from automatic progress and loads a manual slot. */
    static nine_app_t again;
    gal_stub_reset();
    CHECK(nine_app_init(&again, script, sn, images, imn), "second boot init");
    click(&again, NINE_BTN_OK);
    CHECK(again.page == GAL_PAGE_TITLE, "second boot title");
    CHECK(again.auto_size > 0, "automatic progress present");
    click(&again, NINE_BTN_DOWN);
    click(&again, NINE_BTN_OK);
    CHECK(again.page == GAL_PAGE_GAME, "continue resumes");
    CHECK(nine_app_current_text(&again)[0] != 0, "continue text");
    longp(&again, NINE_BTN_OK);
    click(&again, NINE_BTN_DOWN); click(&again, NINE_BTN_OK);
    click(&again, NINE_BTN_OK);
    CHECK(again.page == GAL_PAGE_GAME, "manual slot loads on second boot");

    /* A corrupt script produces a visible error and blocks further advancing. */
    static nine_app_t badapp;
    uint8_t *bad = malloc(sn);
    CHECK(bad != NULL, "bad copy");
    memcpy(bad, script, sn);
    bad[300] ^= 0x01;
    gal_stub_reset();
    CHECK(nine_app_init(&badapp, bad, sn, images, imn), "bad init returns");
    CHECK(badapp.fatal && badapp.page == GAL_PAGE_TITLE, "bad pack error screen");
    CHECK(strstr(gal_stub()->brand[0], "资源包") != NULL, "bad pack visible message");
    free(bad);

    /* A corrupt image package fails init before any story can be shown. */
    static nine_app_t badimg;
    uint8_t *bad_images = malloc(imn);
    CHECK(bad_images != NULL, "bad image copy");
    memcpy(bad_images, images, imn);
    uint32_t ioff = (uint32_t)bad_images[24] | (uint32_t)bad_images[25] << 8 |
                    (uint32_t)bad_images[26] << 16 | (uint32_t)bad_images[27] << 24;
    uint32_t poff = (uint32_t)bad_images[ioff + 12] | (uint32_t)bad_images[ioff + 13] << 8 |
                    (uint32_t)bad_images[ioff + 14] << 16 | (uint32_t)bad_images[ioff + 15] << 24;
    bad_images[poff + 3] ^= 0xff;
    gal_stub_reset();
    CHECK(nine_app_init(&badimg, script, sn, bad_images, imn), "bad image init returns");
    CHECK(badimg.fatal && badimg.page == GAL_PAGE_TITLE, "bad image error screen");
    CHECK(strstr(gal_stub()->brand[0], "资源包") != NULL, "bad image visible message");
    free(bad_images);

    /* A valid-CRC but undecodable JPEG is rejected when the first story frame
       is shown; the error screen blocks any further advance. */
    static nine_app_t badjpeg;
    uint8_t *bad_jpeg = malloc(imn);
    CHECK(bad_jpeg != NULL, "bad jpeg copy");
    memcpy(bad_jpeg, images, imn);
    uint32_t img_off = rd32(bad_jpeg + 24);
    uint32_t img_data = rd32(bad_jpeg + img_off + 12);
    uint32_t img_len = rd32(bad_jpeg + img_off + 16);
    bad_jpeg[img_data] = 0x00;   // break SOI
    wr32(bad_jpeg + img_off + 20, crc32_bytes(bad_jpeg + img_data, img_len));
    wr32(bad_jpeg + 16, crc32_zero(bad_jpeg, imn, 16, 4));
    gal_stub_reset();
    CHECK(nine_app_init(&badjpeg, script, sn, bad_jpeg, imn), "bad jpeg init returns");
    click(&badjpeg, NINE_BTN_OK);
    click(&badjpeg, NINE_BTN_OK);
    CHECK(badjpeg.fatal && badjpeg.page == GAL_PAGE_TITLE, "image decode error screen");
    CHECK(badjpeg.vm.state.node == 0 && badjpeg.vm.state.index == 0,
          "image failure does not skip the first line");
    click(&badjpeg, NINE_BTN_OK);
    CHECK(badjpeg.page == GAL_PAGE_TITLE, "image error blocks advancing");
    free(bad_jpeg);

    /* Corrupt a not-yet-cached text block after init: the line must fail
       visibly at the failing frame and never advance past it. */
    static nine_app_t textfail;
    uint8_t *tscript = malloc(sn);
    CHECK(tscript != NULL, "text copy");
    memcpy(tscript, script, sn);
    gal_stub_reset();
    CHECK(nine_app_init(&textfail, tscript, sn, images, imn), "textfail init");
    click(&textfail, NINE_BTN_OK);
    click(&textfail, NINE_BTN_OK);
    CHECK(textfail.vm.state.node == 0 && textfail.vm.state.index == 0, "textfail first frame");
    uint32_t fcount = rd32(tscript + 44), foff = rd32(tscript + 48);
    uint32_t boff = rd32(tscript + 56);
    uint32_t fail_index = 0, fail_block = 0, fail_comp = 0;
    for (uint32_t i = 0; i < fcount && !fail_block; ++i) {
        uint32_t kind = rd32(tscript + foff + i * 24);
        uint32_t blk = rd32(tscript + foff + i * 24 + 8);
        if (kind == 0 && blk == 1) { fail_index = i; fail_block = blk; }
    }
    CHECK(fail_block == 1, "found block 1");
    fail_comp = rd32(tscript + boff + fail_block * 16);
    tscript[fail_comp + 10] ^= 0xff;
    for (int i = 0; i < 200 && !textfail.fatal; ++i) click(&textfail, NINE_BTN_OK);
    CHECK(textfail.fatal, "text block failure is fatal");
    CHECK(textfail.page == GAL_PAGE_TITLE, "text failure error screen");
    CHECK(textfail.vm.state.node == 0 && textfail.vm.state.index == fail_index,
          "text failure stops at the failing frame");
    click(&textfail, NINE_BTN_OK);
    CHECK(textfail.vm.state.index == fail_index, "text failure blocks advancing");
    free(tscript);

    printf("fails=%d\n", fails);
    free(script); free(images);
    return fails ? 1 : 0;
}
