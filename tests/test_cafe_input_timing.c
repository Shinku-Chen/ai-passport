// tests/test_cafe_input_timing.c — event time must survive queue/render delay.
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

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    size_t n = 0;
    unsigned char *pack = read_file(argv[1], &n);
    CHECK(pack && cafe_pack_set_data(pack, n));
    cafe_ui_t ui; cafe_bands_t bands; cafe_app_t app;
    memset(&ui, 0, sizeof(ui)); memset(&bands, 0, sizeof(bands));
    CHECK(cafe_app_init(&app, &ui, &bands));
    app.now_ms = 1000; app.last_activity_ms = 1000;

    // A 150 ms physical press is applied at 1000 ms after a render/lock stall.
    // The event timestamp must keep it short, so START -> TITLE.
    cafe_app_key_at(&app, CAFE_KEY_OK, true, 1000, 1000);
    cafe_app_key_at(&app, CAFE_KEY_OK, false, 1150, 1000);
    CHECK(app.page == CAFE_PAGE_TITLE);

    // The old wrapper used the processing timestamp for both edges; retain it as
    // the negative control: a delayed release becomes long and START does not advance.
    memset(&ui, 0, sizeof(ui)); memset(&bands, 0, sizeof(bands));
    CHECK(cafe_app_init(&app, &ui, &bands));
    app.now_ms = 1000; app.last_activity_ms = 1000;
    cafe_app_key(&app, CAFE_KEY_OK, true, 1000);
    cafe_app_key(&app, CAFE_KEY_OK, false, 6000); // processing time delay: old-style long
    CHECK(app.page == CAFE_PAGE_START);

    free(pack);
    printf("cafe input event-time PASS (short delayed release; old timestamp negative control)\n");
    return 0;
}
