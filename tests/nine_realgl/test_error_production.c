// Production error path with real LVGL/fonts: a bad script must show the CJK
// error page and must not create image bands or enter the story.
#include "nine_app.h"
#include "nine_image.h"
#include "lvgl.h"
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
int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    size_t sn, imn;
    uint8_t *script = load(argv[1], &sn), *images = load(argv[2], &imn);
    if (!script || !images) return 2;
    lv_init();
    lv_display_t *disp = lv_display_create(320, 240);
    CHECK(disp != NULL, "display");
    script[300] ^= 0x01;
    static nine_app_t app;
    CHECK(nine_app_init(&app, script, sn, images, imn), "bad init returns");
    CHECK(app.fatal, "bad script is fatal");
    CHECK(app.page == GAL_PAGE_TITLE, "error page is title");
    CHECK(nine_image_band_count() == 0, "no image bands on fatal pack");
    const char *brand = lv_label_get_text(app.ui.brand_title);
    CHECK(brand && strstr(brand, "资源包") != NULL, "CJK error title visible");
    const char *sub = lv_label_get_text(app.ui.brand_sub);
    CHECK(sub && sub[0] != 0, "CJK error detail visible");
    printf("brand=%s sub=%s\n", brand, sub);
    printf("fails=%d\n", fails);
    return fails ? 1 : 0;
}
