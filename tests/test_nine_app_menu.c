// Production-path regression for the choice-page long-OK contract:
// one long OK must open the menu; returning must restore the same choice_sel.
#include "nine_app.h"
#include "bsp_button.h"

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

static void click(nine_app_t *app, int button)
{
    key(app, button, BSP_BTN_CLICK);
}

static void long_(nine_app_t *app, int button)
{
    key(app, button, BSP_BTN_LONG);
}

static int vm_kind(nine_app_t *app)
{
    const nine_view_t *view = nine_vm_locate(&app->vm);
    return view ? (int)view->kind : -1;
}

static void advance_to_choice(nine_app_t *app, int node, int count)
{
    for (int i = 0; i < 20000; ++i) {
        if (app->page == GAL_PAGE_CHOICE) break;
        if (app->page == GAL_PAGE_GAME) click(app, BSP_BTN_OK);
        else if (app->page == GAL_PAGE_CG && app->opening) click(app, BSP_BTN_OK);
        else break;
    }
    assert(app->page == GAL_PAGE_CHOICE);
    assert(app->vm.state.node == node);
    assert(vm_kind(app) == NINE_VM_CHOICE);
    assert(nine_vm_locate(&app->vm)->choice_count == (uint32_t)count);
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    nine_menu_script_path = argv[1];
    nine_menu_images_path = argv[2];

    nine_app_t app;
    assert(nine_app_init(&app));
    assert(!app.error);
    click(&app, BSP_BTN_OK);              // START -> TITLE
    assert(app.page == GAL_PAGE_TITLE);
    click(&app, BSP_BTN_OK);              // New Game -> GAME
    assert(app.page == GAL_PAGE_GAME);

    advance_to_choice(&app, 1, 1);
    click(&app, BSP_BTN_OK);              // n1: choose visible 0
    advance_to_choice(&app, 4, 3);

    // Keep a non-zero selection position across the menu round trip.
    click(&app, BSP_BTN_DOWN);
    click(&app, BSP_BTN_DOWN);
    assert(app.choice_sel == 2);

    long_(&app, BSP_BTN_OK);              // REQUIRED: opens MENU directly
    assert(app.page == GAL_PAGE_MENU);
    assert(vm_kind(&app) == NINE_VM_CHOICE);
    assert(app.vm.state.node == 4);

    long_(&app, BSP_BTN_OK);              // MENU -> restore CHOICE
    assert(app.page == GAL_PAGE_CHOICE);
    assert(vm_kind(&app) == NINE_VM_CHOICE);
    assert(app.vm.state.node == 4);
    assert(app.choice_sel == 2);

    click(&app, BSP_BTN_OK);              // last visible choice (n4 target n5)
    advance_to_choice(&app, 35, 2);       // continue the selected branch
    assert(app.vm.state.node == 35);

    puts("nine app choice long-OK menu contract: PASS");
    return 0;
}
