// Host test: drive the production gal_app/gal_ui/gal_image with the real pack.
#include "gal_app.h"
#include "gal_pack.h"
#include "gal_vm.h"
#include "gal_save.h"
#include "gal_unlock.h"
#include "gal_text.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "jpeg_decoder.h"
#include "nvs.h"
#include "esp_timer.h"
#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const lv_font_t manosaba_16 = { 0 };

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); failures++; } \
} while (0)

static uint16_t s_art[320 * 150];
static uint16_t s_strip[160 * 45];
static uint8_t s_scratch[150 * 150 * 2];
static gal_pack_t s_pack;
static gal_app_t s_app;
static uint32_t s_now = 1000;
static uint8_t *s_pack_data;
static uint32_t s_pack_size;

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

static void key(uint8_t btn, uint8_t ev)
{
    const gal_key_t k = { btn, ev };
    gal_app_key(&s_app, &k);
}
static void tick(void)
{
    s_now += 40;
    esp_timer_get_time_set_ms(s_now);
    gal_app_tick(&s_app, s_now);
}
static void tap(uint8_t btn) { key(btn, BSP_BTN_PRESS); key(btn, BSP_BTN_RELEASE); key(btn, BSP_BTN_CLICK); tick(); }
static void long_ok(void) { key(BSP_BTN_OK, BSP_BTN_PRESS); key(BSP_BTN_OK, BSP_BTN_LONG); key(BSP_BTN_OK, BSP_BTN_RELEASE); tick(); }

static const char *row_text(int i) { return lv_stub_text(s_app.ui.list_labels[i]); }
static const char *game_text(void) { return lv_stub_text(s_app.ui.text); }

static void advance_until_choice(void)
{
    for (int i = 0; i < 4000; ++i) {
        if (s_app.page == APP_PAGE_CHOICE || s_app.page == APP_PAGE_ENDING ||
            s_app.page == APP_PAGE_ERROR) return;
        tap(BSP_BTN_UP);
    }
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (argc < 2) { fprintf(stderr, "usage: %s pack\n", argv[0]); return 2; }
    s_pack_data = load_file(argv[1], &s_pack_size);
    const char *err = NULL;
    if (!gal_pack_open(&s_pack, s_pack_data, s_pack_size, &err)) {
        fprintf(stderr, "pack open: %s\n", err);
        return 1;
    }
    lv_stub_reset();
    lv_stub_require_lock(true);
    nvs_stub_reset();
    esp_timer_stub_set_us(1000000);
    if (!gal_app_init(&s_app, &s_pack, s_art, s_strip, s_scratch, sizeof(s_scratch))) {
        fprintf(stderr, "app init failed\n");
        return 1;
    }
    s_app.settings.text_speed = 0;   // instant text for deterministic key flow
    CHECK(s_app.page == APP_PAGE_TITLE, "boot page=%s", gal_app_page_name(s_app.page));
    CHECK(lv_stub_visible(s_app.ui.title_box), "title box hidden");
    CHECK(!lv_stub_visible(s_app.ui.panel), "game panel visible on title");
    printf("title image meta=%u,%u shown=%u art=%d strip=%d jpeg=%d\n",
           s_pack.meta[10], s_pack.meta[11], s_app.image.shown,
           s_app.image.art_valid, s_app.image.strip_valid, jpeg_stub_decode_count());
    CHECK(s_app.image.art_valid && s_app.image.strip_valid, "title art decode failed");
    CHECK(lv_stub_lock_violations() == 0, "LVGL lock violations=%d", lv_stub_lock_violations());

    // 新游戏:短 OK 一次
    tap(BSP_BTN_OK);
    CHECK(s_app.page == APP_PAGE_GAME, "new game page=%s", gal_app_page_name(s_app.page));
    CHECK(s_app.vm_started, "vm not started");
    CHECK(game_text()[0] != 0, "empty game text");
    CHECK(s_app.image.art_valid, "game art failed");
    // 阅读页四硬指标：名字是根层 label 且在面板上方、页码独占面板内右上、正文不出面板。
    CHECK(s_app.ui.speaker->parent == s_app.ui.root &&
              s_app.ui.speaker->y < GAL_UI_PANEL_Y,
          "speaker must be a root label above the panel (y=%d)", s_app.ui.speaker->y);
    CHECK(s_app.ui.page_label->parent == s_app.ui.root &&
              s_app.ui.page_label->x == GAL_UI_TOP_PAGE_X &&
              s_app.ui.page_label->y == 2,
          "page label not at top bar (x=%d,y=%d)", s_app.ui.page_label->x,
          s_app.ui.page_label->y);
    CHECK(s_app.ui.hp->parent == s_app.ui.root &&
              s_app.ui.hp->x == GAL_UI_TOP_HP_X && s_app.ui.hp->y == 2,
          "HP not at top bar (x=%d,y=%d)", s_app.ui.hp->x, s_app.ui.hp->y);
    CHECK(s_app.ui.panel->bg_opa == 180, "panel opacity changed: %d", s_app.ui.panel->bg_opa);
    CHECK(s_app.ui.panel->y == 176 && s_app.ui.panel->h == 64,
          "panel geometry=%d/%d", s_app.ui.panel->y, s_app.ui.panel->h);
    CHECK(s_app.ui.text->y + s_app.ui.text->h <= GAL_UI_PANEL_H,
          "text bottom %d exceeds panel height %d", s_app.ui.text->y + s_app.ui.text->h,
          GAL_UI_PANEL_H);
    const uint16_t first_cursor = s_app.vm.cursor;
    const char *first_text = strdup(game_text());

    // 短按 UP:PRESS 推进,尾随 CLICK 不得再推一次
    tap(BSP_BTN_UP);
    CHECK(s_app.vm.cursor != first_cursor || strcmp(game_text(), first_text) != 0,
          "UP did not advance");
    const uint16_t after_one = s_app.vm.cursor;
    tick();
    CHECK(s_app.vm.cursor == after_one, "tail CLICK advanced twice (%u->%u)", after_one, s_app.vm.cursor);

    // 图像失败不解锁;重试成功后恢复解锁
    const gal_imgrec_t *bgrec = gal_pack_image(&s_pack, s_app.vm.background);
    if (bgrec && bgrec->gallery != GAL_NONE) {
        const uint16_t g = (uint16_t)bgrec->gallery;
        CHECK((s_app.unlock.gallery[g >> 3] & (1u << (g & 7))) != 0, "bg CG not unlocked");
        s_app.unlock.gallery[g >> 3] &= (uint8_t)~(1u << (g & 7));
        s_app.vm.unlock_gallery[g >> 3] &= (uint8_t)~(1u << (g & 7));
        s_app.unlock.dirty = 0;
        s_app.art_ok = false;
        s_app.art_bg = 0xFFFFFFFFu;
        s_app.art_bodies = 0;
        s_app.image_dirty = true;
        s_app.image_retry_ms = s_now;
        jpeg_stub_fail_next(1);
        gal_app_tick(&s_app, s_now + 40);
        CHECK(!s_app.art_ok, "failed decode left art_ok");
        CHECK((s_app.unlock.gallery[g >> 3] & (1u << (g & 7))) == 0, "failed decode unlocked CG");
        CHECK(s_app.notice[0] != 0, "no image failure notice");
        gal_app_tick(&s_app, s_now + 3000);
        CHECK(s_app.art_ok, "image retry did not recover");
        CHECK((s_app.unlock.gallery[g >> 3] & (1u << (g & 7))) != 0, "retry did not unlock CG");
    }

    // 下条带失败也必须清空两段并保持缓存无效。
    if (bgrec && bgrec->gallery != GAL_NONE) {
        const uint16_t g = (uint16_t)bgrec->gallery;
        s_app.unlock.gallery[g >> 3] &= (uint8_t)~(1u << (g & 7));
        s_app.art_ok = false;
        s_app.image_dirty = true;
        s_app.image_retry_ms = s_now;
        memset(s_art, 0xAA, sizeof(s_art));
        memset(s_strip, 0xAA, sizeof(s_strip));
        jpeg_stub_fail_after(1);
        gal_app_tick(&s_app, s_now + 40);
        CHECK(!s_app.art_ok, "strip failure left art_ok");
        CHECK(!s_app.image.art_valid && !s_app.image.strip_valid,
              "strip failure flags art=%d strip=%d", s_app.image.art_valid,
              s_app.image.strip_valid);
        bool zero = true;
        for (size_t i = 0; i < sizeof(s_art) / sizeof(s_art[0]); ++i) {
            if (s_art[i] != 0) zero = false;
        }
        for (size_t i = 0; i < sizeof(s_strip) / sizeof(s_strip[0]); ++i) {
            if (s_strip[i] != 0) zero = false;
        }
        CHECK(zero, "strip failure left a half frame");
        CHECK(strstr(s_app.notice, "图片") != NULL, "strip failure notice=%s", s_app.notice);
        gal_app_tick(&s_app, s_now + 3000);
        CHECK(s_app.art_ok, "strip retry did not recover");
    }

    // 全局解锁:写失败保持 dirty,重试成功后落盘
    s_app.unlock.dirty = 1;
    s_app.unlock_retry_ms = 0;
    nvs_stub_fail_set(true);
    gal_app_tick(&s_app, s_now + 10);
    CHECK(s_app.unlock.dirty, "unlock dirty cleared on failed write");
    nvs_stub_fail_set(false);
    gal_app_tick(&s_app, s_now + 7000);
    CHECK(!s_app.unlock.dirty, "unlock retry did not clear dirty");
    CHECK((int)nvs_stub_key_size("unlock") == 81, "unlock blob size=%d",
          (int)nvs_stub_key_size("unlock"));

    // 长 OK 打开菜单,长 OK 返回游戏
    long_ok();
    CHECK(s_app.page == APP_PAGE_MENU, "long OK menu page=%s", gal_app_page_name(s_app.page));
    long_ok();
    CHECK(s_app.page == APP_PAGE_GAME, "long OK resume page=%s", gal_app_page_name(s_app.page));

    // 历史与返回
    tap(BSP_BTN_DOWN);
    CHECK(s_app.page == APP_PAGE_HISTORY, "history page=%s", gal_app_page_name(s_app.page));
    long_ok();
    CHECK(s_app.page == APP_PAGE_GAME, "history back page=%s", gal_app_page_name(s_app.page));

    // 到第一个选项
    advance_until_choice();
    CHECK(s_app.page == APP_PAGE_CHOICE || s_app.page == APP_PAGE_ENDING,
          "no choice/end reached, page=%s err=%s", gal_app_page_name(s_app.page), s_app.error);
    if (s_app.page == APP_PAGE_CHOICE) {
        CHECK(s_app.choice_count >= 2, "choice count=%d", s_app.choice_count);
        CHECK(row_text(0)[0] != 0, "choice row 0 empty");
        // 选择第二项,验证显式确认
        tap(BSP_BTN_DOWN);
        CHECK(s_app.choice_sel == 1, "choice sel=%d", s_app.choice_sel);
        // 菜单往返保留选择
        long_ok();
        CHECK(s_app.page == APP_PAGE_MENU, "choice menu page=%s", gal_app_page_name(s_app.page));
        long_ok();
        CHECK(s_app.page == APP_PAGE_CHOICE, "choice resume page=%s", gal_app_page_name(s_app.page));
        CHECK(s_app.choice_sel == 1, "choice sel lost=%d", s_app.choice_sel);
        // 存档到 0 号槽
        long_ok();
        tap(BSP_BTN_DOWN); // 保存进度
        tap(BSP_BTN_OK);
        CHECK(s_app.page == APP_PAGE_SAVE, "save page=%s", gal_app_page_name(s_app.page));
        tap(BSP_BTN_OK);
        printf("save result notice=%s slot0=%d keys=%d\n", s_app.notice, s_app.slots[0].state, nvs_stub_key_count());
        CHECK(s_app.slots[0].state == 1, "slot 0 not saved");
        CHECK(s_app.page == APP_PAGE_MENU, "save return page=%s", gal_app_page_name(s_app.page));
        long_ok();
        CHECK(s_app.page == APP_PAGE_CHOICE, "back to choice page=%s", gal_app_page_name(s_app.page));
        CHECK(s_app.choice_sel == 1, "choice sel after save=%d", s_app.choice_sel);
        // 读取 0 号槽
        long_ok();
        tap(BSP_BTN_DOWN); tap(BSP_BTN_DOWN); // 读取进度
        tap(BSP_BTN_OK);
        CHECK(s_app.page == APP_PAGE_LOAD, "load page=%s", gal_app_page_name(s_app.page));
        tap(BSP_BTN_OK);
        CHECK(s_app.page == APP_PAGE_CHOICE, "load restore page=%s", gal_app_page_name(s_app.page));
        CHECK(s_app.choice_sel == 1, "load choice sel=%d", s_app.choice_sel);
    }

    // 失败注入:保存失败不得覆盖旧档
    const size_t before = nvs_stub_key_size("s00");
    nvs_stub_fail_set(true);
    long_ok();
    tap(BSP_BTN_DOWN); tap(BSP_BTN_OK);          // menu -> save
    tap(BSP_BTN_OK);                              // commit (fails)
    CHECK(s_app.notice[0] != 0, "no failure notice");
    CHECK((int)nvs_stub_key_size("s00") == (int)before, "failed save changed slot");
    nvs_stub_fail_set(false);

    // 文本分页纯逻辑
    char page[256];
    int pages = 0;
    CHECK(gal_text_page("第一行\n第二行内容很长很长很长很长很长很长", 0, 304, 16, 3, page, sizeof(page), &pages),
          "text page 0");
    CHECK(pages >= 1, "pages=%d", pages);
    CHECK(lv_stub_lock_violations() == 0, "LVGL lock violations=%d", lv_stub_lock_violations());

    // Runtime objections defense: 33 links fail closed without touching
    // choice_rows[0]; 31 links + the fixed return row fit exactly in 32 rows.
    {
        lv_stub_reset();
        lv_stub_require_lock(true);
        nvs_stub_reset();
        esp_timer_stub_set_us(1000000);
        CHECK(gal_app_init(&s_app, &s_pack, s_art, s_strip, s_scratch, sizeof(s_scratch)),
              "objection init");
        s_app.page = APP_PAGE_GAME;
        s_app.vm_started = true;
        s_app.frame.link_count = 33;
        s_app.frame.link_first = 0;
        s_app.choice_count = 0;
        const char *sentinel = (const char *)(uintptr_t)0x12345678u;
        s_app.choice_rows[0].text = sentinel;
        key(BSP_BTN_OK, BSP_BTN_PRESS);
        key(BSP_BTN_OK, BSP_BTN_RELEASE);
        key(BSP_BTN_OK, BSP_BTN_CLICK);
        CHECK(s_app.page == APP_PAGE_ERROR, "33-link frame page=%s",
              gal_app_page_name(s_app.page));
        CHECK(s_app.choice_count == 0, "33-link frame choice_count=%d", s_app.choice_count);
        CHECK(s_app.choice_rows[0].text == sentinel, "33-link frame wrote row0");
        CHECK(strstr(s_app.error, "容量") != NULL, "33-link frame error=%s", s_app.error);

        lv_stub_reset();
        lv_stub_require_lock(true);
        nvs_stub_reset();
        esp_timer_stub_set_us(1000000);
        CHECK(gal_app_init(&s_app, &s_pack, s_art, s_strip, s_scratch, sizeof(s_scratch)),
              "31-link init");
        s_app.page = APP_PAGE_GAME;
        s_app.vm_started = true;
        s_app.frame.link_count = 31;
        s_app.frame.link_first = 0;
        key(BSP_BTN_OK, BSP_BTN_PRESS);
        key(BSP_BTN_OK, BSP_BTN_RELEASE);
        key(BSP_BTN_OK, BSP_BTN_CLICK);
        CHECK(s_app.page == APP_PAGE_CHOICE, "31-link page=%s", gal_app_page_name(s_app.page));
        CHECK(s_app.choice_count == 32, "31-link choice_count=%d", s_app.choice_count);
        CHECK(strcmp(s_app.choice_rows[31].text, "返回发言") == 0,
              "31-link last row=%s", s_app.choice_rows[31].text);
    }

    // Store read failures must not look like empty slots and must require an
    // overwrite confirmation on the save page.
    {
        lv_stub_reset();
        lv_stub_require_lock(true);
        nvs_stub_reset();
        esp_timer_stub_set_us(1000000);
        CHECK(gal_app_init(&s_app, &s_pack, s_art, s_strip, s_scratch, sizeof(s_scratch)),
              "io init");
        gal_save_ui_t seed_ui = { .scene_mode = 0, .page = 0, .choice_row = 0 };
        size_t seed_len = 0;
        CHECK(gal_save_encode(&s_app.vm, &seed_ui, s_app.save_blob,
                              sizeof(s_app.save_blob), &seed_len), "io seed encode");
        CHECK(gal_store_save_slot(&s_app.store, 0, s_app.save_blob, seed_len), "io seed save");
        nvs_stub_fail_open(true);
        s_app.page = APP_PAGE_TITLE;
        s_app.title_sel = 2;
        tap(BSP_BTN_OK);
        CHECK(s_app.page == APP_PAGE_LOAD, "io load page=%s", gal_app_page_name(s_app.page));
        CHECK(s_app.slots[0].state == 3, "io slot state=%u", s_app.slots[0].state);
        CHECK(strstr(row_text(0), "读取失败") != NULL, "io slot row=%s", row_text(0));
        tap(BSP_BTN_OK);
        CHECK(s_app.page == APP_PAGE_LOAD, "io slot was treated as loadable");
        CHECK(strstr(s_app.notice, "读取") != NULL && strstr(s_app.notice, "失败") != NULL,
              "io notice=%s", s_app.notice);
        s_app.page = APP_PAGE_SAVE;
        s_app.vm_started = true;
        s_app.slot_sel = 0;
        s_app.slots[0].state = 3;
        tap(BSP_BTN_OK);
        CHECK(s_app.page == APP_PAGE_CONFIRM, "io save overwrite page=%s",
              gal_app_page_name(s_app.page));
        nvs_stub_fail_open(false);
    }
    {
        lv_stub_reset();
        lv_stub_require_lock(true);
        nvs_stub_reset();
        esp_timer_stub_set_us(1000000);
        nvs_stub_fail_open(true);
        CHECK(gal_app_init(&s_app, &s_pack, s_art, s_strip, s_scratch, sizeof(s_scratch)),
              "auto io init");
        CHECK(s_app.auto_status == GAL_STORE_IO_ERROR, "auto io status=%d", s_app.auto_status);
        s_app.title_sel = 1;
        tap(BSP_BTN_OK);
        CHECK(strstr(s_app.notice, "读取") != NULL && strstr(s_app.notice, "失败") != NULL,
              "auto io notice=%s", s_app.notice);
        nvs_stub_fail_open(false);
    }

    printf("test_gal_app: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
