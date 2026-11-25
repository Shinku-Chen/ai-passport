// Broken-package behavior: a bad story pack must show a visible error and never
// enter the story; a bad image pack must show a visible error, keep the frame's
// text, and block advancing past the missing visual.
//
// Usage: test_nine_badpack <nine_script.bin> <nine_images.bin>
#include "nine_app.h"
#include "gal_ui_stub.h"
#include "bsp_button.h"

#include <stdio.h>
#include <string.h>

bool nine_host_load(const char *script, const char *images);
void nine_host_flip_script(size_t offset);
void nine_host_flip_images(size_t offset);

static nine_app_t app;
static int failures;
static int checks;

#define CHECK(cond, msg) do { \
    ++checks; \
    if (!(cond)) { ++failures; fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, msg); } \
} while (0)

static void click(uint8_t btn)
{
    nine_key_t k = { .btn = btn, .ev = BSP_BTN_PRESS };
    nine_app_key(&app, &k);
    k.ev = BSP_BTN_CLICK;
    nine_app_key(&app, &k);
    k.ev = BSP_BTN_RELEASE;
    nine_app_key(&app, &k);
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: %s <script.bin> <images.bin>\n", argv[0]); return 2; }

    // 1) Corrupt the story package CRC: the reader must not start the story.
    nvs_stub_reset();
    CHECK(nine_host_load(argv[1], argv[2]), "load real packages");
    nine_host_flip_script(16);
    CHECK(nine_app_init(&app), "ui still initializes");
    CHECK(!app.pack_ok, "bad script pack rejected");
    CHECK(host_page == GAL_PAGE_START, "bad pack stays on the start page");
    CHECK(strstr(host_notice, "资源包") != NULL, "bad pack error is visible");
    click(BSP_BTN_OK);
    CHECK(host_page == GAL_PAGE_START, "bad pack cannot enter the story");
    CHECK(strstr(host_notice, "资源包") != NULL, "bad pack error persists");

    // 2) Corrupt the image package only: text is readable, the first visual
    //    failure is visible and blocks advancing, so content is never skipped.
    nvs_stub_reset();
    CHECK(nine_host_load(argv[1], argv[2]), "reload real packages");
    nine_host_flip_images(200);
    CHECK(nine_app_init(&app), "ui initializes with a bad image pack");
    CHECK(app.pack_ok, "story pack still valid");
    CHECK(!app.images_ok, "bad image pack rejected");
    click(BSP_BTN_OK);   // start -> title
    CHECK(host_page == GAL_PAGE_TITLE, "bad images: title is reachable");
    click(BSP_BTN_OK);   // new game
    CHECK(host_page == GAL_PAGE_GAME, "bad images: text page is shown");
    CHECK(host_text[0] != 0, "bad images: dialogue text is visible");
    CHECK(app.view_error == NINE_VIEW_IMAGE_ERROR, "bad images: failure recorded");
    CHECK(strstr(host_notice, "图像") != NULL, "bad images: error is visible");
    {
        const uint32_t frame = app.last_frame;
        const uint16_t node = app.vm.state.node;
        click(BSP_BTN_OK);
        CHECK(app.last_frame == frame && app.vm.state.node == node,
              "bad images: advancing is blocked");
    }

    if (failures) { fprintf(stderr, "%d/%d checks failed\n", failures, checks); return 1; }
    printf("PASS badpack: %d checks (story/image failures visible, story not skipped)\n", checks);
    return 0;
}
