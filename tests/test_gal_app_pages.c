// Host test: title -> chapters/settings/about/gallery/book page navigation.
#include "gal_app.h"
#include "gal_pack.h"
#include "bsp_button.h"
#include "esp_timer.h"
#include "jpeg_decoder.h"
#include "lvgl.h"
#include "nvs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const lv_font_t manosaba_16 = { 0 };
static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); failures++; } \
} while (0)

static uint16_t s_art[320 * 150], s_strip[160 * 45];
static uint8_t s_scratch[150 * 150 * 2];
static gal_pack_t s_pack;
static gal_app_t s_app;
static uint32_t s_now = 1000;

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
static void key(uint8_t btn, uint8_t ev) { const gal_key_t k = { btn, ev }; gal_app_key(&s_app, &k); }
static void tap(uint8_t btn) { key(btn, BSP_BTN_PRESS); key(btn, BSP_BTN_RELEASE); key(btn, BSP_BTN_CLICK); s_now += 40; gal_app_tick(&s_app, s_now); }
static void long_ok(void) { key(BSP_BTN_OK, BSP_BTN_PRESS); key(BSP_BTN_OK, BSP_BTN_LONG); key(BSP_BTN_OK, BSP_BTN_RELEASE); s_now += 40; gal_app_tick(&s_app, s_now); }
static const char *row(int i) { return lv_stub_text(s_app.ui.list_labels[i]); }

static void title_select(int idx)
{
    for (int i = 0; i < 20 && s_app.title_sel < idx; ++i) tap(BSP_BTN_DOWN);
    for (int i = 0; i < 20 && s_app.title_sel > idx; ++i) tap(BSP_BTN_UP);
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    puts("pages start");
    uint32_t size = 0;
    uint8_t *data = load_file(argv[1], &size);
    const char *err = NULL;
    if (!gal_pack_open(&s_pack, data, size, &err)) { fprintf(stderr, "pack %s\n", err); return 1; }
    lv_stub_reset();
    lv_stub_require_lock(true);
    nvs_stub_reset();
    esp_timer_stub_set_us(1000000);
    // 启动时实际加载版本化全局解锁
    gal_store_t st;
    gal_unlock_t seeded;
    gal_unlock_init(&seeded);
    seeded.gallery[0] |= 0x02;
    seeded.chapters[0] |= 0x04;
    uint8_t ub[GAL_UNLOCK_BLOB];
    const size_t ulen = gal_unlock_encode(&seeded, ub, sizeof(ub));
    if (gal_store_init(&st)) CHECK(gal_store_save_unlock(&st, ub, ulen), "seed unlock");
    if (!gal_app_init(&s_app, &s_pack, s_art, s_strip, s_scratch, sizeof(s_scratch))) return 1;
    const int initial_objs = lv_stub_obj_count();
    CHECK(gal_unlock_gallery(&s_app.unlock, 1), "startup gallery unlock");
    CHECK(gal_unlock_chapter(&s_app.unlock, 2), "startup chapter unlock");
    s_app.settings.text_speed = 0;

    // Photo regression: the empty toast was created last and stayed visible
    // forever because notice_until_ms started at 0. It covered title rows 0/1.
    CHECK(!lv_stub_visible(s_app.ui.notice_bg), "empty notice visible at boot");
    CHECK(lv_stub_text(s_app.ui.notice)[0] == 0, "boot notice text not empty");
    CHECK(lv_screen_active()->bg_opa == LV_OPA_COVER, "root backdrop not opaque");
    CHECK(lv_screen_active()->bg_color == 0x1A1319, "root backdrop color=%06x",
          lv_screen_active()->bg_color);
    CHECK(lv_stub_visible(s_app.ui.title_box), "title page hidden at boot");
    CHECK(!lv_stub_visible(s_app.ui.panel), "game panel visible at boot");
    CHECK(!lv_stub_visible(s_app.ui.list_page), "list page visible at boot");
    CHECK(!lv_stub_visible(s_app.ui.text_page), "text page visible at boot");
    CHECK(!lv_stub_visible(s_app.ui.cg_bar), "cg bar visible at boot");
    CHECK(lv_stub_visible(s_app.image.art_canvas), "art canvas hidden at boot");
    CHECK(lv_stub_visible(s_app.image.strip_canvas), "strip canvas hidden at boot");
    lv_stub_set_locked(true);
    gal_ui_show(&s_app.ui, GAL_UI_GAME);
    CHECK(!lv_stub_visible(s_app.ui.title_box) && lv_stub_visible(s_app.ui.panel) &&
          !lv_stub_visible(s_app.ui.list_page) && !lv_stub_visible(s_app.ui.text_page) &&
          !lv_stub_visible(s_app.ui.cg_bar), "game page visibility");
    gal_ui_show(&s_app.ui, GAL_UI_LIST);
    CHECK(!lv_stub_visible(s_app.ui.title_box) && !lv_stub_visible(s_app.ui.panel) &&
          lv_stub_visible(s_app.ui.list_page) && !lv_stub_visible(s_app.ui.text_page) &&
          !lv_stub_visible(s_app.ui.cg_bar), "list page visibility");
    gal_ui_show(&s_app.ui, GAL_UI_TEXT);
    CHECK(!lv_stub_visible(s_app.ui.title_box) && !lv_stub_visible(s_app.ui.panel) &&
          !lv_stub_visible(s_app.ui.list_page) && lv_stub_visible(s_app.ui.text_page) &&
          !lv_stub_visible(s_app.ui.cg_bar), "text page visibility");
    gal_ui_show(&s_app.ui, GAL_UI_CG);
    CHECK(!lv_stub_visible(s_app.ui.title_box) && !lv_stub_visible(s_app.ui.panel) &&
          !lv_stub_visible(s_app.ui.list_page) && !lv_stub_visible(s_app.ui.text_page) &&
          lv_stub_visible(s_app.ui.cg_bar), "cg page visibility");
    gal_ui_show(&s_app.ui, GAL_UI_TITLE);
    CHECK(lv_stub_visible(s_app.ui.title_box) && !lv_stub_visible(s_app.ui.panel) &&
          !lv_stub_visible(s_app.ui.list_page) && !lv_stub_visible(s_app.ui.text_page) &&
          !lv_stub_visible(s_app.ui.cg_bar), "title restore visibility");
    CHECK(!lv_stub_visible(s_app.ui.notice_bg), "empty notice after page matrix");
    gal_ui_notice(&s_app.ui, "提示", 1000, 100);
    CHECK(lv_stub_visible(s_app.ui.notice_bg), "shown notice hidden");
    CHECK(strcmp(lv_stub_text(s_app.ui.notice), "提示") == 0, "notice text");
    gal_ui_tick(&s_app.ui, 1099);
    CHECK(lv_stub_visible(s_app.ui.notice_bg), "notice expired early");
    // A page switch must not leave the toast permanent; it still owns a deadline.
    gal_ui_show(&s_app.ui, GAL_UI_TITLE);
    CHECK(lv_stub_visible(s_app.ui.notice_bg), "page switch hid active notice");
    gal_ui_tick(&s_app.ui, 1100);
    CHECK(!lv_stub_visible(s_app.ui.notice_bg), "expired notice visible");
    CHECK(s_app.ui.notice_until_ms == 0, "expired notice deadline=%d", s_app.ui.notice_until_ms);
    gal_ui_notice(&s_app.ui, "clear-me", 1200, 1000);
    gal_ui_notice(&s_app.ui, "", 1200, 1000);
    CHECK(!lv_stub_visible(s_app.ui.notice_bg), "empty notice after clear visible");
    CHECK(s_app.ui.notice_until_ms == 0, "clear notice deadline=%d", s_app.ui.notice_until_ms);
    lv_stub_set_locked(false);

    // 章节选择:路线 -> 章节 -> 以章节模式开始
    printf("title page=%s sel=%d\n", gal_app_page_name(s_app.page), s_app.title_sel);
    title_select(4);
    printf("after select sel=%d page=%s\n", s_app.title_sel, gal_app_page_name(s_app.page));
    tap(BSP_BTN_OK);
    CHECK(s_app.page == APP_PAGE_ROUTES, "routes page=%s", gal_app_page_name(s_app.page));
    CHECK(row(0)[0] != 0, "route row empty");
    tap(BSP_BTN_OK);
    CHECK(s_app.page == APP_PAGE_CHAPTERS, "chapters page=%s", gal_app_page_name(s_app.page));
    s_app.unlock.chapters[0] |= 1;   // test fixture: unlock the first chapter entry
    CHECK(row(0)[0] != 0, "chapter row empty");
    printf("chapter row0: %s\n", row(0));
    tap(BSP_BTN_OK);
    CHECK(s_app.page == APP_PAGE_GAME, "chapter start page=%s err=%s", gal_app_page_name(s_app.page), s_app.error);
    CHECK(s_app.vm.debug_direct == 1, "chapter mode should set isDebugDirectJump");
    CHECK(s_app.vm_started, "chapter vm not started");
    long_ok();       // menu
    CHECK(s_app.page == APP_PAGE_MENU, "chapter menu");
    // 回标题:菜单第 9 项
    while (s_app.menu_sel < 8) tap(BSP_BTN_DOWN);
    tap(BSP_BTN_OK);
    CHECK(s_app.page == APP_PAGE_TITLE, "return title page=%s", gal_app_page_name(s_app.page));

    // 鉴赏室(初始未玩,应显示未解锁)
    title_select(3);
    tap(BSP_BTN_OK);
    CHECK(s_app.page == APP_PAGE_GALLERY, "gallery page=%s", gal_app_page_name(s_app.page));
    CHECK(row(0)[0] != 0, "gallery row empty");
    printf("gallery row0: %s\n", row(0));
    tap(BSP_BTN_OK);                 // 打开第 1 组 CG
    CHECK(s_app.page == APP_PAGE_CG, "cg page=%s", gal_app_page_name(s_app.page));
    CHECK(s_app.ui.cg_hint != NULL &&
              strstr(lv_stub_text(s_app.ui.cg_hint), "差分") != NULL,
          "cg hint mentions diff: %s", lv_stub_text(s_app.ui.cg_hint));
    CHECK(strstr(lv_stub_text(s_app.ui.cg_hint), "长OK") != NULL,
          "cg hint mentions long-OK");
    tap(BSP_BTN_OK);                 // OK = 切差分，不退出
    CHECK(s_app.page == APP_PAGE_CG, "cg OK does not exit page=%s",
          gal_app_page_name(s_app.page));
    long_ok();                       // 长 OK 回鉴赏
    CHECK(s_app.page == APP_PAGE_GALLERY, "cg long OK returns gallery page=%s",
          gal_app_page_name(s_app.page));
    long_ok();
    CHECK(s_app.page == APP_PAGE_TITLE, "gallery back page=%s", gal_app_page_name(s_app.page));

    // 设置:Win32 NVS 失败注入;长 OK 返回
    title_select(5);
    tap(BSP_BTN_OK);
    CHECK(s_app.page == APP_PAGE_SETTINGS, "settings page=%s", gal_app_page_name(s_app.page));
    CHECK(strstr(row(0), "文字速度") != NULL, "settings row0=%s", row(0));
    nvs_stub_fail_set(true);
    tap(BSP_BTN_OK);
    nvs_stub_fail_set(false);
    long_ok();
    CHECK(s_app.page == APP_PAGE_TITLE, "settings back page=%s", gal_app_page_name(s_app.page));

    // 关于两页
    title_select(6);
    tap(BSP_BTN_OK);
    CHECK(s_app.page == APP_PAGE_ABOUT, "about page=%s", gal_app_page_name(s_app.page));
    CHECK(strstr(lv_stub_text(s_app.ui.text_body), "魔法少女") != NULL, "about body");
    tap(BSP_BTN_OK);
    CHECK(strstr(lv_stub_text(s_app.ui.text_body), "当前源快照") != NULL, "about page1");
    long_ok();
    CHECK(s_app.page == APP_PAGE_TITLE, "about back");

    // 新游戏 -> 魔女手册
    title_select(0);
    tap(BSP_BTN_OK);
    CHECK(s_app.page == APP_PAGE_GAME, "new game page=%s", gal_app_page_name(s_app.page));
    long_ok();       // menu
    while (s_app.menu_sel < 4) tap(BSP_BTN_DOWN);
    tap(BSP_BTN_OK);
    CHECK(s_app.page == APP_PAGE_BOOK, "book page=%s", gal_app_page_name(s_app.page));
    CHECK(row(0)[0] != 0, "book row empty: %s", row(0));
    printf("book row0: %s\n", row(0));
    tap(BSP_BTN_OK);
    CHECK(s_app.page == APP_PAGE_BOOK_DETAIL, "book detail page=%s", gal_app_page_name(s_app.page));
    CHECK(lv_stub_text(s_app.ui.text)[0] != 0, "book detail text empty");
    // 导航回归：正文页提示必须写明长 OK 返回，且末页单击 OK 必须能回到手册。
    // 修复前提示只有“OK 下一页”，末页单击只弹“已经是最后一页”。
    CHECK(strstr(lv_stub_text(s_app.ui.hint), "长OK") != NULL,
          "book detail hint mentions long-OK: %s", lv_stub_text(s_app.ui.hint));
    for (int i = 0; i < (int)s_app.book_pages && s_app.page == APP_PAGE_BOOK_DETAIL; ++i) {
        tap(BSP_BTN_OK);                                           // 走到末页（末页这次应退出）
    }
    if (s_app.page == APP_PAGE_BOOK_DETAIL) tap(BSP_BTN_OK);       // 单页记录也覆盖一次末页按键
    CHECK(s_app.page == APP_PAGE_BOOK, "book detail click exits page=%s",
          gal_app_page_name(s_app.page));
    if (s_app.page == APP_PAGE_BOOK) tap(BSP_BTN_OK);                 // 重进详情，保留长 OK 覆盖
    long_ok();
    CHECK(s_app.page == APP_PAGE_BOOK, "book detail back");
    long_ok();
    CHECK(s_app.page == APP_PAGE_MENU, "book back menu");
    long_ok();
    CHECK(s_app.page == APP_PAGE_GAME, "menu resume");
    CHECK(lv_stub_lock_violations() == 0, "lock violations=%d", lv_stub_lock_violations());

    CHECK(lv_stub_obj_count() == initial_objs, "UI object leak %d -> %d",
          initial_objs, lv_stub_obj_count());
    printf("test_gal_app_pages: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
