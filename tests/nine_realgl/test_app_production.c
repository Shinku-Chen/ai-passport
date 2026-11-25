// Production app/UI startup regression: NO NINE_IMAGE_HOST, NO fake gal_ui,
// real LVGL objects/fonts and the real pack. The ESP heap allocator is tracked
// by a host shim; LVGL uses the target-like 48 KB builtin pool.
#include "nine_app.h"
#include "nine_image.h"
#include "lvgl.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)
static uint8_t *load(const char *p, size_t *n)
{
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)l); if (!b || fread(b, 1, (size_t)l, f) != (size_t)l) return NULL;
    fclose(f); *n = (size_t)l; return b;
}
static void key(nine_app_t *a, int btn, int ev) { nine_key_t k = {(uint8_t)btn, (uint8_t)ev}; nine_app_key(a, &k); }
static void click(nine_app_t *a, int btn) { key(a, btn, NINE_EV_PRESS); key(a, btn, NINE_EV_CLICK); key(a, btn, NINE_EV_RELEASE); }
static void longp(nine_app_t *a, int btn) { key(a, btn, NINE_EV_PRESS); key(a, btn, NINE_EV_LONG); key(a, btn, NINE_EV_RELEASE); }
static unsigned pool_used(void)
{
    lv_mem_monitor_t m;
    lv_mem_monitor(&m);
    printf("POOL used=%u free=%u largest=%u", (unsigned)(m.total_size - m.free_size),
           (unsigned)m.free_size, (unsigned)m.free_biggest_size);
    puts("");
    return (unsigned)(m.total_size - m.free_size);
}
int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 3) { printf("usage: %s script images\n", argv[0]); return 2; }
    size_t sn, imn;
    uint8_t *script = load(argv[1], &sn), *images = load(argv[2], &imn);
    if (!script || !images) return 2;
    lv_init();
    lv_display_t *disp = lv_display_create(320, 240);
    CHECK(disp != NULL, "display");
    printf("POOL after lv_init ");
    (void)pool_used();
    static nine_app_t app;
    CHECK(nine_app_init(&app, script, sn, images, imn), "production app init");
    CHECK(!app.fatal, "normal pack is not fatal");
    CHECK(app.page == GAL_PAGE_START, "start page");
    CHECK(nine_image_band_count() == 4, "four real image bands");
    unsigned used_init = pool_used();
    CHECK(used_init > 0 && used_init < 62000, "LVGL pool fits after init");
    click(&app, NINE_BTN_OK);
    CHECK(app.page == GAL_PAGE_TITLE, "title");
    click(&app, NINE_BTN_OK);
    CHECK(app.page == GAL_PAGE_GAME, "new game");
    CHECK(nine_app_current_text(&app)[0] != 0, "first text");
    const uint16_t *px = NULL; unsigned w = 0, h = 0;
    CHECK(nine_image_band_info(0, &px, &w, &h) && px && w == 256 && h == 36, "first canvas band");
    unsigned nonzero = 0;
    for (unsigned i = 0; px && i < w * h; ++i) nonzero += px[i] != 0;
    CHECK(nonzero > 1000, "first frame decoded");
    /* Settings return must go straight back to the reading layer. */
    longp(&app, NINE_BTN_OK);
    for (int i = 0; i < 4; ++i) click(&app, NINE_BTN_DOWN);
    click(&app, NINE_BTN_OK);          // settings
    CHECK(app.page == GAL_PAGE_SETTINGS, "settings page");
    longp(&app, NINE_BTN_OK);          // direct back to reading
    CHECK(app.page == GAL_PAGE_GAME, "settings returns to reading");
    CHECK(nine_app_current_text(&app)[0] != 0, "reading text after settings");

    /* Exercise every lazily built list/page; the 64 KB pool must survive. */
    longp(&app, NINE_BTN_OK);          // menu
    click(&app, NINE_BTN_DOWN); click(&app, NINE_BTN_OK);  // save page (7 rows)
    longp(&app, NINE_BTN_OK);          // back to reading
    longp(&app, NINE_BTN_OK);          // menu again
    for (int i = 0; i < 4; ++i) click(&app, NINE_BTN_DOWN);
    click(&app, NINE_BTN_OK);          // settings (6 rows)
    longp(&app, NINE_BTN_OK);          // direct back to reading
    longp(&app, NINE_BTN_OK);          // menu again
    for (int i = 0; i < 2; ++i) click(&app, NINE_BTN_DOWN);
    click(&app, NINE_BTN_OK);          // history
    longp(&app, NINE_BTN_OK);          // back to reading
    longp(&app, NINE_BTN_OK);          // menu
    for (int i = 0; i < 5; ++i) click(&app, NINE_BTN_DOWN);
    click(&app, NINE_BTN_OK);          // title
    for (int i = 0; i < 4; ++i) click(&app, NINE_BTN_DOWN);
    click(&app, NINE_BTN_OK);          // gallery
    click(&app, NINE_BTN_OK);          // CG list (3x3 tiles, thumbnails)
    longp(&app, NINE_BTN_OK);
    click(&app, NINE_BTN_DOWN); click(&app, NINE_BTN_OK);  // chapters
    longp(&app, NINE_BTN_OK);
    click(&app, NINE_BTN_DOWN); click(&app, NINE_BTN_OK);  // music
    longp(&app, NINE_BTN_OK);
    unsigned used_pages = pool_used();
    printf("HEAP used=%u peak=%u largest=%u", (unsigned)heap_stub_used(),
           (unsigned)heap_stub_peak(), (unsigned)heap_stub_largest());
    puts("");
    CHECK(used_pages < 62000, "LVGL pool retains room after all pages");
    CHECK(heap_stub_used() >= 73728, "four image bands allocated from ESP heap");
    printf("fails=%d\n", fails);
    free(script); free(images);
    return fails ? 1 : 0;
}
