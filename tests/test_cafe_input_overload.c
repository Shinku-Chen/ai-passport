// tests/test_cafe_input_overload.c — incomplete input cycles must not stick.
#include "cafe_app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #c); return 1; } } while (0)

static unsigned char *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f); *size = (size_t)n; return b;
}
static void press_release(cafe_app_t *a, uint8_t key, uint32_t t)
{
    cafe_app_key(a, key, true, t); cafe_app_key(a, key, false, t + 40);
}
int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    size_t n = 0; unsigned char *pack = read_file(argv[1], &n);
    CHECK(pack && cafe_pack_set_data(pack, n));
    cafe_ui_t ui; cafe_bands_t bands; cafe_app_t app;
    memset(&ui, 0, sizeof(ui)); memset(&bands, 0, sizeof(bands));
    CHECK(cafe_app_init(&app, &ui, &bands));
    app.now_ms = 1000; app.last_activity_ms = 1000;
    press_release(&app, CAFE_KEY_OK, 1000); // START -> TITLE

    // A press that is aborted by queue recovery must not become a ghost short.
    cafe_app_key(&app, CAFE_KEY_DOWN, true, 1100);
    cafe_app_cancel_input(&app);
    cafe_app_key(&app, CAFE_KEY_DOWN, false, 1150);
    CHECK(app.page == CAFE_PAGE_TITLE);
    CHECK(app.title_sel == 0);
    // The next complete press works exactly once.
    press_release(&app, CAFE_KEY_DOWN, 1200);
    CHECK(app.page == CAFE_PAGE_TITLE);
    CHECK(app.title_sel == 1);

    // Recovery also clears a partial long/FF cycle; the next complete key is clean.
    cafe_app_key(&app, CAFE_KEY_UP, true, 1300);
    cafe_app_cancel_input(&app);
    CHECK(!app.fast_forward);
    cafe_app_key(&app, CAFE_KEY_UP, false, 1800);
    CHECK(app.page == CAFE_PAGE_TITLE);
    press_release(&app, CAFE_KEY_UP, 1900);
    CHECK(app.title_sel == 0);

    printf("cafe input overflow recovery PASS (no ghost, next full key once)\n");
    free(pack);
    return 0;
}
