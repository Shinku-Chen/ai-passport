// Production-path fast-forward contract: sentence-granular hold, immediate
// first step, one bounded step per control tick, choices stop, and normal
// short-press pagination remains page-granular. Host HAL/NVS only.
#include "nine_app.h"
#include "nine_media.h"
#include "nine_vm.h"
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
static void key(nine_app_t *a, int btn, int ev)
{
    nine_key_t k = {(uint8_t)btn, (uint8_t)ev};
    nine_app_key(a, &k);
}
static void click(nine_app_t *a, int btn)
{
    key(a, btn, NINE_EV_PRESS); key(a, btn, NINE_EV_CLICK); key(a, btn, NINE_EV_RELEASE);
}
static void hold_up(nine_app_t *a)
{
    key(a, NINE_BTN_UP, NINE_EV_PRESS); key(a, NINE_BTN_UP, NINE_EV_LONG);
}
static void release_up(nine_app_t *a) { key(a, NINE_BTN_UP, NINE_EV_RELEASE); }
static void tick(nine_app_t *a, unsigned ms) { nine_app_tick(a, ms); }

static void boot(nine_app_t *a, const uint8_t *s, size_t sn, const uint8_t *im, size_t imn)
{
    nvs_stub_reset(); gal_stub_reset();
    CHECK(nine_app_init(a, s, sn, im, imn), "init");
    click(a, NINE_BTN_OK);
    click(a, NINE_BTN_OK);
    CHECK(a->page == GAL_PAGE_GAME, "new game");
}
static void to_long_line(nine_app_t *a)
{
    for (int i = 0; i < 30 && a->page == GAL_PAGE_GAME && a->page_count <= 1; ++i)
        click(a, NINE_BTN_OK);
    CHECK(a->page_count > 1, "multi-page sentence");
}

int main(int argc, char **argv)
{
    if (argc < 3) { printf("usage: %s script images\n", argv[0]); return 2; }
    size_t sn, imn;
    uint8_t *script = load(argv[1], &sn);
    uint8_t *images = load(argv[2], &imn);
    static nine_app_t a;

    // Short press remains one pagination unit.
    boot(&a, script, sn, images, imn);
    to_long_line(&a);
    uint32_t f = a.vm.view.frame;
    uint16_t p = a.text_page;
    click(&a, NINE_BTN_OK);
    CHECK(a.vm.view.frame == f && a.text_page == p + 1, "short press advances one page");
    CHECK(nine_media_deinit(), "media deinit after normal page");

    // Long UP from a multi-page sentence starts at the next sentence, page 0,
    // and does not replay the remaining page(s).
    boot(&a, script, sn, images, imn);
    to_long_line(&a);
    f = a.vm.view.frame;
    p = a.text_page;
    hold_up(&a);
    CHECK(a.page == GAL_PAGE_GAME, "long hold stays in reading");
    CHECK(a.vm.view.frame != f, "long hold immediately reaches next sentence");
    CHECK(a.text_page == 0, "next sentence starts at page 0");
    CHECK(a.page_count >= 1, "next sentence has pagination");
    uint32_t f1 = a.vm.view.frame;
    tick(&a, 50);
    CHECK(a.vm.view.frame != f1, "one control tick advances one sentence");
    release_up(&a);
    uint32_t f2 = a.vm.view.frame;
    tick(&a, 2000);
    CHECK(a.vm.view.frame == f2, "release stops without a short-press advance");
    CHECK(nine_media_deinit(), "media deinit after hold");

    // OP and choice both stop the hold; no option is selected by FF.
    boot(&a, script, sn, images, imn);
    hold_up(&a);
    for (int i = 0; i < 5000 && a.page == GAL_PAGE_GAME; ++i) tick(&a, 50);
    CHECK(a.page == GAL_PAGE_OPENING, "FF stops at graph OP");
    CHECK(!a.ff_hold, "FF cleared at OP");
    release_up(&a);
    click(&a, NINE_BTN_OK); // skip OP
    hold_up(&a);
    for (int i = 0; i < 5000 && a.page == GAL_PAGE_GAME; ++i) tick(&a, 50);
    CHECK(a.page == GAL_PAGE_CHOICE, "FF stops at choice");
    CHECK(!a.ff_hold, "FF cleared at choice");
    CHECK(a.vm.view.choice_count >= 1, "choice remains available");
    CHECK(a.choice_sel == 0, "FF does not inject a choice");
    release_up(&a);
    CHECK(nine_media_deinit(), "media deinit after choice");

    printf("fails=%d\n", fails);
    free(script); free(images);
    return fails ? 1 : 0;
}
