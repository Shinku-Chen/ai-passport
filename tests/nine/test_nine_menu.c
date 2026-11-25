// Strict production regression for the choice/menu return path.
// All navigation is through nine_app_key(); app/VM/flag fields are read only.
#include "nine_app.h"
#include "nine_assets_host.h"
#include "nine_image.h"
#include "nine_media.h"
#include "nine_ui_stub.h"
#include "bsp_button.h"
#include "gal_ui.h"
#include "nvs.h"

#include <stdio.h>
#include <string.h>

int nine_nvs_host_get(const char *key, void *out, size_t cap, size_t *len);
void nine_nvs_host_reset(void);
void nine_store_test_clear(void);
const lv_font_t nine_cjk_16;
const lv_font_t nine_cjk_20;

static nine_app_t app;
static int failures;
#define CHECK(c, m) do { if (!(c)) { failures++; printf("FAIL: %s\n", m); } } while (0)

static void key(int btn, int ev) { nine_key_t k = { (uint8_t)btn, (uint8_t)ev }; nine_app_key(&app, &k); }
static void click(int btn) { key(btn, BSP_BTN_PRESS); key(btn, BSP_BTN_CLICK); }
static void long_press(int btn) { key(btn, BSP_BTN_LONG); key(btn, BSP_BTN_RELEASE); }
static int flag(const char *name) { int k = nine_pack_find_key(&app.pack, name); return k < 0 ? -999 : app.vm.state.flag[k]; }
static const char *node_name(void)
{
    static char b[32]; nine_node_t n;
    if (app.vm.state.node == NINE_NODE_NONE) return "@end";
    if (!nine_pack_node(&app.pack, app.vm.state.node, &n)) return "?";
    snprintf(b, sizeof(b), "%s", nine_pack_string(&app.pack, n.name));
    return b;
}
static int node_is(const char *n) { return strcmp(node_name(), n) == 0; }

typedef struct { uint16_t node; uint32_t index; uint16_t text_page; uint8_t kind; bool preview; int sel; int flag[128], progress[128]; } snap_t;
static void take(snap_t *s)
{
    memset(s, 0, sizeof(*s));
    s->node = app.vm.state.node; s->index = app.vm.state.index; s->text_page = app.text_page;
    s->kind = (uint8_t)app.vm.view.kind; s->preview = app.preview; s->sel = app.choice_sel;
    memcpy(s->flag, app.vm.state.flag, sizeof(s->flag));
    memcpy(s->progress, app.vm.state.progress, sizeof(s->progress));
}
static int same_vm(const snap_t *a, const snap_t *b)
{
    return a->node == b->node && a->index == b->index && a->kind == b->kind &&
           a->preview == b->preview && a->sel == b->sel &&
           memcmp(a->flag, b->flag, sizeof(a->flag)) == 0 &&
           memcmp(a->progress, b->progress, sizeof(a->progress)) == 0;
}
static int consistent(void)
{
    int want = app.vm.view.kind == NINE_VM_CHOICE ? GAL_PAGE_CHOICE :
               (app.vm.view.kind == NINE_VM_TEXT || app.vm.view.kind == NINE_VM_OP ? GAL_PAGE_GAME :
                (app.vm.view.kind == NINE_VM_END ? GAL_PAGE_ENDING : -1));
    return want == (int)app.page && nine_ui_page == (int)app.page;
}
static void fresh(void)
{
    nine_image_deinit(); nine_media_deinit(); nine_store_test_clear(); nine_nvs_host_reset();
    nine_ui_stub_reset(); nine_image_test_disable_decode(1);
    CHECK(nine_app_init(&app), "boot"); CHECK(!app.fatal, "readable"); nine_ui_stub_reset();
}
static void restart(void)
{
    nine_image_deinit(); nine_media_deinit(); nine_ui_stub_reset();
    CHECK(nine_app_init(&app), "restart"); nine_ui_stub_reset();
}
static void start_new(void)
{
    if (app.page == GAL_PAGE_START) click(BSP_BTN_OK);
    CHECK(app.page == GAL_PAGE_TITLE, "title");
    click(BSP_BTN_OK); CHECK(app.page == GAL_PAGE_GAME, "new game");
}
static void down(int n) { for (int i = 0; i < n; i++) click(BSP_BTN_DOWN); }
static int drive_choice(const char *target)
{
    for (int i = 0; i < 200000; i++) {
        if (app.page == GAL_PAGE_CHOICE) {
            if (node_is(target)) return 1;
            click(BSP_BTN_OK);
            continue;
        }
        if (app.page == GAL_PAGE_GAME) { click(BSP_BTN_OK); continue; }
        return 0;
    }
    return 0;
}
static int drive_node(const char *target)
{
    for (int i = 0; i < 200000; i++) {
        if (app.page == GAL_PAGE_GAME) {
            if (node_is(target)) return 1;
            click(BSP_BTN_OK);
            continue;
        }
        return 0;
    }
    return 0;
}
static int choose(int visible)
{
    if (app.page != GAL_PAGE_CHOICE || visible < 0 || visible >= app.vm.view.choice_count) return 0;
    for (int i = 0; i < visible; i++) click(BSP_BTN_DOWN);
    if (app.choice_sel != visible) return 0;
    click(BSP_BTN_OK);
    return 1;
}
static void menu_open(void) { CHECK(app.page == GAL_PAGE_GAME || app.page == GAL_PAGE_CHOICE, "menu origin"); long_press(BSP_BTN_OK); CHECK(app.page == GAL_PAGE_MENU, "menu"); }
static void menu_item(int target)
{
    for (int i = 0; i < 20 && app.menu_sel != target; i++) click(app.menu_sel < target ? BSP_BTN_DOWN : BSP_BTN_UP);
    CHECK(app.menu_sel == target, "menu item"); click(BSP_BTN_OK);
}
static void back_menu(void) { long_press(BSP_BTN_OK); CHECK(app.page == GAL_PAGE_MENU, "back menu"); }
static void resume(void) { long_press(BSP_BTN_OK); }
static void slot(int n)
{
    for (int i = 0; i < 8; i++) {
        int cur = app.page == GAL_PAGE_SAVE ? app.save_sel : app.load_sel;
        if (cur == n) break;
        click(cur < n ? BSP_BTN_DOWN : BSP_BTN_UP);
    }
}

static void at_n30(void) { fresh(); start_new(); CHECK(drive_choice("n30"), "reach n30"); CHECK(app.vm.view.choice_count == 3, "n30 count"); }
static void check_choice(const snap_t *before, const char *label)
{
    snap_t after; take(&after);
    CHECK(app.page == GAL_PAGE_CHOICE, label);
    CHECK(consistent(), "choice page/view");
    CHECK(same_vm(before, &after), "choice VM/selection unchanged");
}
static void test_save_resume(void)
{
    at_n30(); down(1); snap_t b; take(&b);
    menu_open(); menu_item(1); CHECK(app.page == GAL_PAGE_SAVE, "save page"); click(BSP_BTN_OK);
    back_menu(); resume(); check_choice(&b, "save resume choice");
    click(BSP_BTN_DOWN); click(BSP_BTN_OK);
    CHECK(flag("D02_branch_3") == 1, "last visible after restore");
}
static void test_save_cancel(void)
{
    at_n30(); down(1); snap_t b; take(&b);
    menu_open(); menu_item(1); back_menu(); resume(); check_choice(&b, "save cancel choice");
    click(BSP_BTN_UP); click(BSP_BTN_OK);
    CHECK(flag("D02_branch_1") == 1, "first visible after cancel");
}
static void test_overwrite(void)
{
    at_n30(); down(1); snap_t b; take(&b);
    menu_open(); menu_item(1); click(BSP_BTN_OK); back_menu(); resume();
    menu_open(); menu_item(1); slot(0); click(BSP_BTN_OK); CHECK(app.modal, "overwrite modal");
    click(BSP_BTN_OK); CHECK(!app.modal, "overwrite cancel closes"); back_menu(); resume(); check_choice(&b, "overwrite cancel choice");
}
static void test_load_cancel_invalid(void)
{
    at_n30(); down(1); snap_t b; take(&b);
    menu_open(); menu_item(2); back_menu(); resume(); check_choice(&b, "load cancel choice");
    at_n30(); down(1); take(&b);
    menu_open(); menu_item(2); slot(2); click(BSP_BTN_OK); CHECK(app.page == GAL_PAGE_LOAD, "empty stays load");
    back_menu(); resume(); check_choice(&b, "load empty choice");
}
static void test_settings_history(void)
{
    at_n30(); down(1); snap_t b; take(&b);
    menu_open(); menu_item(4); click(BSP_BTN_OK); back_menu(); resume(); check_choice(&b, "settings choice");
    at_n30(); down(1); take(&b);
    menu_open(); menu_item(3); back_menu(); resume(); check_choice(&b, "history choice");
}
static void test_text(void)
{
    fresh(); start_new();
    CHECK(drive_node("n2"), "text n2");
    snap_t b; take(&b);
    menu_open(); menu_item(1); click(BSP_BTN_OK); back_menu(); resume();
    snap_t a; take(&a); CHECK(same_vm(&b, &a) && app.page == GAL_PAGE_GAME, "text save round-trip");
    click(BSP_BTN_OK); menu_open(); menu_item(2); slot(0); click(BSP_BTN_OK);
    snap_t c; take(&c); CHECK(same_vm(&b, &c), "text load exact");
}
static void test_preview(void)
{
    fresh();
    if (app.page == GAL_PAGE_START) click(BSP_BTN_OK);
    down(4); click(BSP_BTN_OK); click(BSP_BTN_DOWN); click(BSP_BTN_OK);
    down(13); click(BSP_BTN_OK);
    CHECK(app.preview && app.page == GAL_PAGE_GAME, "preview start");
    const char *cs[] = { "n105", "n107", "n109", "n111" };
    for (size_t i = 0; i < sizeof(cs)/sizeof(cs[0]); i++) { CHECK(drive_choice(cs[i]), "preview choice"); CHECK(choose(0), "preview choose"); }
    CHECK(drive_node("n114"), "preview n114");
    menu_open(); menu_item(1); click(BSP_BTN_OK); back_menu(); resume();
    menu_open(); menu_item(2); slot(0); click(BSP_BTN_OK);
    uint8_t buf[64]; size_t len = 0;
    CHECK(app.preview, "preview retained"); CHECK(app.unlock.endings == 0, "preview no unlock");
    CHECK(nine_nvs_host_get("perm", buf, sizeof(buf), &len) != 0, "preview no perm");
    CHECK(nine_nvs_host_get("auto", buf, sizeof(buf), &len) != 0, "preview no auto");
}
static void test_continue(void)
{
    fresh(); start_new();
    CHECK(drive_node("n2"), "continue n2");
    snap_t b; take(&b); nine_app_flush(&app); restart();
    if (app.page == GAL_PAGE_START) click(BSP_BTN_OK);
    click(BSP_BTN_DOWN); click(BSP_BTN_OK);
    snap_t a; take(&a); CHECK(same_vm(&b, &a), "continue exact");
}

int main(void)
{
    nine_nvs_host_reset(); nine_store_test_clear(); nine_ui_stub_reset();
    CHECK(nine_assets_host_load("assets/nine/yukiiro/nine_script.bin",
                                "assets/nine/yukiiro/nine_images.bin"), "assets");
    nine_image_test_disable_decode(1);
    test_save_resume();
    test_save_cancel();
    test_overwrite();
    test_load_cancel_invalid();
    test_settings_history();
    test_text();
    test_preview();
    test_continue();
    nine_image_deinit(); nine_media_deinit();
    if (failures) { printf("FAILED: nine menu return (%d)\n", failures); return 1; }
    printf("PASS: nine menu return\n");
    return 0;
}
