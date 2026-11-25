// Production-path regression for the instant fast-forward contract.
// The real production app/nine_reader/nine_vm/pack code is linked; only the
// board/LVGL edges are stubbed (same harness as test_nine_app_menu.c).
#include "nine_app.h"
#include "bsp_button.h"
#include "gal_text.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

extern const char *nine_menu_script_path;
extern const char *nine_menu_images_path;

static void key(nine_app_t *app, int button, int event)
{
    nine_key_t k = { (uint8_t)button, (uint8_t)event };
    nine_app_key(app, &k);
}
static void click(nine_app_t *app, int button) { key(app, button, BSP_BTN_CLICK); }
static void long_(nine_app_t *app, int button) { key(app, button, BSP_BTN_LONG); }
static void release(nine_app_t *app, int button) { key(app, button, BSP_BTN_RELEASE); }

static void seek_state(nine_app_t *app, uint16_t node, uint32_t index,
                       uint16_t page, int page_count)
{
    assert(nine_vm_seek(&app->vm, node, index, true, true));
    assert(nine_vm_locate(&app->vm) != NULL);
    app->has_session = true;
    app->page = GAL_PAGE_GAME;
    app->text_page = page;
    app->page_count = page_count;
    app->render_error = false;
    app->reader.auto_mode = false;
    app->reader.typing = false;
    app->reader.fast_forward = false;
    app->reader.auto_deadline_valid = false;
    app->clock_ms = 1000;
    nine_reader_set_reading(&app->reader, true, app->clock_ms);
}

static int sentence_pages(nine_app_t *app)
{
    nine_frame_t frame;
    assert(nine_vm_current_frame(&app->vm, &frame));
    const char *text = nine_pack_text(&app->pack, &frame);
    assert(text);
    return gal_text_pages(text, 26, GAL_TEXT_LINES_SMALL, NULL, 0);
}

static void assert_next_sentence(nine_app_t *app, uint16_t node, uint32_t index)
{
    assert(app->vm.state.node == node);
    assert(app->vm.state.index == index);
    assert(app->text_page == 0);
    assert(app->page == GAL_PAGE_GAME);
    assert(!app->render_error);
    assert(app->reader.fast_forward);
    assert(!app->reader.auto_mode);
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    nine_menu_script_path = argv[1];
    nine_menu_images_path = argv[2];

    nine_app_t app;
    assert(nine_app_init(&app));
    assert(!app.error);

    // Node46/index88 is the long paginated sentence in this pack. Its page
    // count follows the real 320px/3-line layout; starting on page 1, a long
    // UP must skip every remaining page and enter index89 immediately.
    seek_state(&app, 46, 88, 0, 1);
    const int pages = sentence_pages(&app);
    assert(pages >= 2);
    app.page_count = (uint16_t)pages;
    nine_reader_set_typing(&app.reader, true, app.clock_ms);
    nine_reader_set_auto(&app.reader, true, app.clock_ms);
    assert(app.reader.typing);
    assert(app.reader.auto_mode);
    long_(&app, BSP_BTN_UP);
    assert_next_sentence(&app, 46, 89);
    assert(app.vm.state.index == 89);
    release(&app, BSP_BTN_UP);
    assert(!app.reader.fast_forward);

    // Same multi-page sentence from its middle page: still one sentence jump,
    // not just the next page.
    seek_state(&app, 46, 88, 1, pages);
    long_(&app, BSP_BTN_UP);
    assert_next_sentence(&app, 46, 89);
    release(&app, BSP_BTN_UP);

    // Normal short clicks remain complete pagination units. Walk every page
    // of the same sentence, then the next click advances to index89.
    seek_state(&app, 46, 88, 0, pages);
    for (int p = 1; p < pages; ++p) {
        click(&app, BSP_BTN_OK);
        assert(app.vm.state.node == 46);
        assert(app.vm.state.index == 88);
        assert(app.text_page == p);
    }
    click(&app, BSP_BTN_OK);
    assert(app.vm.state.node == 46);
    assert(app.vm.state.index == 89);
    assert(app.text_page == 0);

    // Held UP: every control tick advances exactly one sentence, with no
    // 90/120 ms staging and no catch-up burst on a large elapsed value.
    seek_state(&app, 0, 0, 0, 1);
    long_(&app, BSP_BTN_UP);
    assert(app.vm.state.node == 0 && app.vm.state.index == 1);
    const uint32_t before_tick = app.vm.state.index;
    nine_app_tick(&app, 1);
    assert(app.vm.state.node == 0);
    assert(app.vm.state.index == before_tick + 1);
    const uint32_t before_big_tick = app.vm.state.index;
    nine_app_tick(&app, 500);
    assert(app.vm.state.node == 0);
    assert(app.vm.state.index == before_big_tick + 1);
    release(&app, BSP_BTN_UP);
    assert(!app.reader.fast_forward);
    const uint16_t stopped_node = app.vm.state.node;
    const uint32_t stopped_index = app.vm.state.index;
    nine_app_tick(&app, 5000);
    assert(app.vm.state.node == stopped_node);
    assert(app.vm.state.index == stopped_index);

    // Choice / ending / OP / render error are stop states and never consume a
    // held-UP sentence jump.
    assert(nine_vm_seek(&app.vm, 1, 0, true, true));
    app.page = GAL_PAGE_CHOICE;
    app.has_session = true;
    const uint16_t choice_node = app.vm.state.node;
    long_(&app, BSP_BTN_UP);
    assert(app.page == GAL_PAGE_CHOICE);
    assert(app.vm.state.node == choice_node);
    assert(!app.reader.fast_forward);

    app.page = GAL_PAGE_ENDING;
    const uint16_t ending_node = app.vm.state.node;
    long_(&app, BSP_BTN_UP);
    assert(app.page == GAL_PAGE_ENDING);
    assert(app.vm.state.node == ending_node);

    app.page = GAL_PAGE_CG;
    app.opening = true;
    const uint16_t op_node = app.vm.state.node;
    long_(&app, BSP_BTN_UP);
    assert(app.page == GAL_PAGE_CG);
    assert(app.opening);
    assert(app.vm.state.node == op_node);

    seek_state(&app, 46, 88, 0, 2);
    app.render_error = true;
    const uint16_t error_node = app.vm.state.node;
    long_(&app, BSP_BTN_UP);
    assert(app.render_error);
    assert(!app.reader.fast_forward);
    assert(app.vm.state.node == error_node);
    assert(app.vm.state.index == 88);

    puts("nine app instant fast-forward contract: PASS");
    return 0;
}
