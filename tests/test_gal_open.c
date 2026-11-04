// Source index.ux semantics: all 61 gallery groups / 172 members and all 371
// chapter entries are open from first launch; unlock metadata is not a gate.
#include "gal_app.h"
#include "gal_pack.h"
#include "gal_store.h"
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
static uint32_t now = 1000;

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
static void title_select(int idx)
{
    app.page = APP_PAGE_TITLE;
    for (int i = 0; i < 20 && app.title_sel > idx; ++i) tap(BSP_BTN_UP);
    for (int i = 0; i < 20 && app.title_sel < idx; ++i) tap(BSP_BTN_DOWN);
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    uint32_t size = 0;
    uint8_t *data = load_file(argv[1], &size);
    const char *err = NULL;
    if (!gal_pack_open(&pack, data, size, &err)) { fprintf(stderr, "pack: %s\n", err); return 1; }
    lv_stub_reset();
    lv_stub_require_lock(true);
    nvs_stub_reset();
    esp_timer_stub_set_us(1000000);
    CHECK(gal_app_init(&app, &pack, art, strip, scratch, sizeof(scratch)), "init");
    app.settings.text_speed = 0;
    for (size_t i = 0; i < sizeof(app.unlock.gallery); ++i) CHECK(app.unlock.gallery[i] == 0, "unlock seed");
    for (size_t i = 0; i < sizeof(app.unlock.chapters); ++i) CHECK(app.unlock.chapters[i] == 0, "chapter seed");

    const int groups = (int)pack.count[GAL_SEC_GALLERY];
    int members = 0;
    for (int g = 0; g < groups; ++g) {
        title_select(3);
        tap(BSP_BTN_OK);
        CHECK(app.page == APP_PAGE_GALLERY, "gallery page=%s", gal_app_page_name(app.page));
        app.gallery_sel = g;
        tap(BSP_BTN_OK);
        CHECK(app.page == APP_PAGE_CG, "group %d page=%s", g, gal_app_page_name(app.page));
        CHECK(app.cg_sel == g, "group %d cg_sel=%d", g, app.cg_sel);
        const gal_gallery_group_t *grp = gal_pack_gallery_group(&pack, (uint16_t)g);
        CHECK(grp && app.cg_variant == grp->first, "group %d first=%d", g, app.cg_variant);
        if (!grp) continue;
        for (uint16_t i = 0; i < grp->count; ++i) {
            CHECK(app.cg_variant == grp->first + i, "group %d member %u got %d", g, i,
                  app.cg_variant);
            members++;
            if (i + 1 < grp->count) {
                tap(BSP_BTN_OK);
                CHECK(app.page == APP_PAGE_CG, "group %d cycle page", g);
            }
        }
        long_ok();  // CG -> gallery
        CHECK(app.page == APP_PAGE_GALLERY, "group %d back page=%s", g,
              gal_app_page_name(app.page));
    }
    printf("open gallery groups=%d members=%d\n", groups, members);
    CHECK(members == 172, "members=%d", members);

    int chapters = 0;
    const int routes = (int)pack.count[GAL_SEC_ROUTES];
    for (int r = 0; r < routes; ++r) {
        const gal_route_t *route = (const gal_route_t *)pack.sec[GAL_SEC_ROUTES] + r;
        title_select(4);
        tap(BSP_BTN_OK);
        CHECK(app.page == APP_PAGE_ROUTES, "routes page=%s", gal_app_page_name(app.page));
        app.route_sel = r;
        tap(BSP_BTN_OK);
        CHECK(app.page == APP_PAGE_CHAPTERS, "chapters route %d page=%s", r,
              gal_app_page_name(app.page));
        const int samples[3] = { 0, route->count / 2, route->count - 1 };
        for (int s = 0; s < 3; ++s) {
            const int idx = samples[s];
            if (idx < 0 || idx >= route->count) continue;
            app.chapter_sel = idx;
            tap(BSP_BTN_OK);
            CHECK(app.page == APP_PAGE_GAME, "route %d chapter %d page=%s", r, idx,
                  gal_app_page_name(app.page));
            CHECK(app.vm_started, "route %d chapter %d vm", r, idx);
            chapters++;
            long_ok();  // menu
            CHECK(app.page == APP_PAGE_MENU, "chapter menu");
            app.menu_sel = 8;
            tap(BSP_BTN_OK);  // title
            CHECK(app.page == APP_PAGE_TITLE, "chapter title page=%s",
                  gal_app_page_name(app.page));
        }
    }
    printf("open chapters samples=%d\n", chapters);
    CHECK(chapters >= 30, "chapter samples=%d", chapters);
    CHECK(lv_stub_lock_violations() == 0, "lock violations=%d", lv_stub_lock_violations());
    printf("test_gal_open: %s\n", failures ? "FAIL" : "PASS");
    free(data);
    return failures ? 1 : 0;
}
