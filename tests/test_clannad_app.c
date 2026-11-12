// tests/test_clannad_app.c —— 生产 app/UI/VM 的宿主回归：真实按键序列、菜单/存读档、
// 失败注入、全局通关记录、图片失败与坏包。NVS/LVGL/定时器为薄桩。
#include "clannad_app.h"
#include "clannad_image.h"

#include "bsp_button.h"
#include "lvgl.h"
#include "nvs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const lv_font_t clannad_cjk_16 = {0};

static int failures;
static int checks;
#define CHECK(cond, msg) do { checks++; if (!(cond)) { failures++; \
    printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); } } while (0)

static uint8_t pack_data[8u * 1024u * 1024u];
static size_t pack_size;
static clannad_app_t app;
static clannad_app_t app2;
static clannad_app_t bad_app;
static clannad_app_t chapter_app;
static clannad_app_t checkpoint_app;
static clannad_app_t slot_app;

static void send_key_on(clannad_app_t *target, uint8_t btn, uint8_t ev)
{
    clannad_key_t key = {btn, ev};
    clannad_app_key(target, &key);
}

static void send_key(uint8_t btn, uint8_t ev)
{
    send_key_on(&app, btn, ev);
}

static void click_on(clannad_app_t *target, uint8_t btn)
{
    send_key_on(target, btn, BSP_BTN_PRESS);
    send_key_on(target, btn, BSP_BTN_RELEASE);
    send_key_on(target, btn, BSP_BTN_CLICK);
}

static void long_press_on(clannad_app_t *target, uint8_t btn)
{
    send_key_on(target, btn, BSP_BTN_PRESS);
    send_key_on(target, btn, BSP_BTN_LONG);
    send_key_on(target, btn, BSP_BTN_RELEASE);
}

static void click(uint8_t btn)
{
    click_on(&app, btn);
}

static void long_press(uint8_t btn)
{
    send_key(btn, BSP_BTN_PRESS);
    send_key(btn, BSP_BTN_LONG);
    send_key(btn, BSP_BTN_RELEASE);
}

static void tick_ms(int ms)
{
    extern int64_t esp_timer_stub_now_us;
    esp_timer_stub_now_us += (int64_t)ms * 1000;
    clannad_app_tick(&app, (uint32_t)ms);
}

static int rows_visible_count(void)
{
    char rows[8][96];
    return clannad_app_ui_rows(&app, rows, 8);
}

static bool rows_contain(const char *needle)
{
    char rows[8][96];
    const int n = clannad_app_ui_rows(&app, rows, 8);
    for (int i = 0; i < n; ++i) {
        if (strstr(rows[i], needle)) return true;
    }
    return false;
}

static uint16_t find_scene_bg(const clannad_pack_t *pack)
{
    for (uint16_t i = 0; i < pack->asset_count; ++i) {
        clannad_asset_t a;
        if (!clannad_pack_asset(pack, i, &a)) continue;
        if (a.kind == CLAN_ASSET_BG && a.mask_len == 0 && a.w >= 200 && a.h >= 150) {
            return i;
        }
    }
    return 0xFFFF;
}

static int find_scene_sprites(const clannad_pack_t *pack, uint16_t out[3])
{
    int n = 0;
    for (uint16_t i = 0; i < pack->asset_count && n < 3; ++i) {
        clannad_asset_t a;
        if (!clannad_pack_asset(pack, i, &a)) continue;
        const uint32_t stride = ((uint32_t)a.w + 7u) / 8u;
        if (a.kind == CLAN_ASSET_SPRITE && a.mask_len == stride * a.h &&
            a.w >= 50 && a.h >= 50) {
            out[n++] = i;
        }
    }
    return n;
}

static bool read_nvs_blob(const char *ns, const char *key, uint8_t *out,
                          size_t cap, size_t *out_len)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = 0;
    esp_err_t err = nvs_get_blob(h, key, NULL, &len);
    if (err == ESP_OK && len > 0 && len <= cap) {
        err = nvs_get_blob(h, key, out, &len);
        if (err == ESP_OK && out_len) *out_len = len;
    } else {
        err = ESP_FAIL;
    }
    nvs_close(h);
    return err == ESP_OK;
}

static bool auto_checkpoint(clannad_app_t *target, uint16_t *cursor,
                            uint16_t *page)
{
    static uint8_t packed[8192];
    static uint8_t raw[16384];
    size_t packed_len = 0;
    if (!read_nvs_blob("clannad_auto", "auto", packed, sizeof(packed),
                       &packed_len)) {
        return false;
    }
    uint32_t out = 0;
    if (!clannad_inflate_raw(packed, (uint32_t)packed_len, raw, sizeof(raw), &out)) {
        return false;
    }
    clannad_save_info_t info;
    if (!clannad_vm_save_validate(&target->vm, raw, out, &info)) return false;
    if (cursor) *cursor = info.cursor;
    if (page) *page = info.page;
    return true;
}

static bool make_checkpoint_app(clannad_app_t *target)
{
    char why[128] = {0};
    lv_stub_reset();
    if (!clannad_app_init(target, pack_data, (uint32_t)pack_size, why,
                          sizeof(why))) {
        return false;
    }
    target->settings.speed = 3;
    click_on(target, BSP_BTN_OK);           // 新游戏
    for (int i = 0; i < 3 && target->page == CLAN_PAGE_GAME; ++i) {
        click_on(target, BSP_BTN_UP);
    }
    return target->page == CLAN_PAGE_GAME;
}

static void open_checkpoint_path(clannad_app_t *target, int path)
{
    switch (path) {
    case 0: break;                          // GAME
    case 1: long_press_on(target, BSP_BTN_OK); break;   // MENU
    case 2: click_on(target, BSP_BTN_DOWN); break;     // HISTORY
    case 3:
        long_press_on(target, BSP_BTN_OK);
        target->list_sel = 6;
        click_on(target, BSP_BTN_OK);
        break;                              // SETTINGS
    case 4:
        long_press_on(target, BSP_BTN_OK);
        target->list_sel = 1;
        click_on(target, BSP_BTN_OK);
        break;                              // SAVE
    case 5:
        long_press_on(target, BSP_BTN_OK);
        target->list_sel = 2;
        click_on(target, BSP_BTN_OK);
        break;                              // LOAD
    default:
        long_press_on(target, BSP_BTN_OK);
        target->list_sel = 7;
        click_on(target, BSP_BTN_OK);
        break;                              // TITLE
    }
}

static bool find_chapter_entry(const clannad_pack_t *pack, uint16_t scene,
                               int *route_out, int *chapter_out)
{
    for (uint16_t r = 0; r < pack->route_count; ++r) {
        clannad_route_t route;
        if (!clannad_pack_route(pack, r, &route)) continue;
        for (uint16_t i = 0; i < route.chapter_count; ++i) {
            clannad_chapter_t chapter;
            if (!clannad_pack_chapter(pack, (uint16_t)(route.first_chapter + i),
                                      &chapter)) {
                continue;
            }
            if (chapter.scene_id == scene) {
                *route_out = (int)r;
                *chapter_out = (int)i;
                return true;
            }
        }
    }
    return false;
}

static bool advance_until_choice(int max_clicks)
{
    for (int i = 0; i < max_clicks; ++i) {
        if (app.page == CLAN_PAGE_CHOICE) return true;
        if (app.page != CLAN_PAGE_GAME) return false;
        click(BSP_BTN_UP);
    }
    return app.page == CLAN_PAGE_CHOICE;
}

static void open_menu(void)
{
    long_press(BSP_BTN_OK);
}

static void goto_title(void)
{
    open_menu();
    for (int i = 0; i < 7; ++i) click(BSP_BTN_DOWN);   // 返回标题
    click(BSP_BTN_OK);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "assets/clannad/clannad_pack.bin";
    FILE *fh = fopen(path, "rb");
    if (!fh) { fprintf(stderr, "cannot open %s\n", path); return 2; }
    pack_size = fread(pack_data, 1, sizeof(pack_data), fh);
    fclose(fh);
    nvs_stub_reset();
    char why[128] = {0};
    CHECK(clannad_app_init(&app, pack_data, (uint32_t)pack_size, why, sizeof(why)),
          "app init");
    app.settings.speed = 3;
    CHECK(app.page == CLAN_PAGE_TITLE, "boots to title");
    CHECK(rows_contain("新游戏"), "title shows new game");
    {
        // 标题画按 1:1 铺满 320 宽、纵向居中裁切：画布四角都必须是图，
        // 不应再退回主题底色（旧版 224 宽只在中间，两侧是 48px 空条）。
        static uint16_t clear_ref[CLAN_IMG_W * CLAN_IMG_H];
        clannad_image_t ref = {.canvas = app.ui.image.canvas, .pixels = clear_ref};
        clannad_image_clear(&ref);
        const int last = CLAN_IMG_W * (CLAN_IMG_H - 1);
        CHECK(app.ui.image.pixels[0] != clear_ref[0] &&
                  app.ui.image.pixels[CLAN_IMG_W - 1] != clear_ref[CLAN_IMG_W - 1] &&
                  app.ui.image.pixels[last] != clear_ref[last],
              "title art must cover the whole 320x168 canvas (full-bleed)");
    }
    CHECK(app.light_count == 0 && !app.after_unlocked && !app.true_ending,
          "fresh progress");

    // 标题 → 新游戏 → 正文；画面真实解码
    click(BSP_BTN_OK);
    CHECK(app.page == CLAN_PAGE_GAME, "new game enters reader");
    // 背景铺满可视画面区：左右 0/47 列都必须是图，不是主题底色（旧版 224 宽留 48px 黑边）。
    {
        static uint16_t theme_ref[CLAN_IMG_W * CLAN_IMG_H];
        clannad_image_t ref = {.canvas = app.ui.image.canvas, .pixels = theme_ref};
        clannad_image_clear(&ref);
        CHECK(app.ui.image.pixels[0] != theme_ref[0] &&
                  app.ui.image.pixels[CLAN_IMG_W - 1] != theme_ref[CLAN_IMG_W - 1],
              "background must fill the 320-wide art window");
    }
    // 阅读页四硬指标：名字在框外（<CLAN_PANEL_Y）、页码独占面板内右上、末行离 240 ≥2px。
    CHECK(app.ui.speaker->y + app.ui.speaker->h <= CLAN_PANEL_Y,
          "speaker must sit above the panel");
    // 文本框背景必须真的是 LV_OPA_TRANSP(0)；正文纯白 + 1px 描边 + 置顶。
    CHECK(app.ui.game_panel != NULL && app.ui.game_panel->bg_opa == 170,
          "game panel must be COL_PANEL @170 for readability");
    CHECK(app.ui.lines[0] != NULL && app.ui.lines[0]->bg_opa == 0,
          "body line must not paint its own background");
    // 桩里 LV_SIZE_CONTENT 是哨兵值；真机由 LVGL 解析为文字宽度。
    CHECK(app.ui.chapter->w == LV_SIZE_CONTENT || app.ui.chapter->w <= 320,
          "chapter bar must hug its text");
    CHECK(app.ui.page_mark->y >= CLAN_PANEL_Y &&
              app.ui.page_mark->y + app.ui.page_mark->h <= CLAN_LINES_TOP,
          "page mark must sit between panel top and first text line");
    CHECK(CLAN_LINES_TOP + CLAN_TEXT_LINES * 18 <= 238,
          "last text line must keep 2px above the 240 edge");
    CHECK(app.text[0] != '\0', "first line has text");
    CHECK(app.vm.mode == CLAN_MODE_STORY && app.vm.root == 414,
          "story from 414 (root chapter)");
    {
        CHECK(app.cache_valid && !app.image_failed,
              "scene canvas decoded (image cache valid)");
    }

    // 普通对白手动槽：列表预览来自真实对白，加载回到同一句/同一页。
    {
        char text_before[1024];
        char speaker_before[128];
        snprintf(text_before, sizeof(text_before), "%s", app.text);
        snprintf(speaker_before, sizeof(speaker_before), "%s", app.speaker);
        const uint16_t page_before = (uint16_t)app.page_index;
        CHECK(text_before[0] != '\0', "text slot has real text");
        open_menu();
        click(BSP_BTN_DOWN);                  // 保存进度
        click(BSP_BTN_OK);
        CHECK(app.page == CLAN_PAGE_SAVE, "text save opens slot list");
        for (int i = 0; i < 4; ++i) click(BSP_BTN_DOWN);
        click(BSP_BTN_OK);                    // 空栏位 4 直接保存
        CHECK(app.slot_ok[4], "text save marks slot valid");
        CHECK(app.slot_info[4].preview[0] != '\0',
              "text save list preview is non-empty");
        CHECK(strncmp(app.slot_info[4].preview, text_before,
                      strlen(app.slot_info[4].preview)) == 0,
              "text save preview is the same sentence");
        CHECK(app.page == CLAN_PAGE_MENU, "text save returns to menu");
        long_press(BSP_BTN_OK);
        CHECK(app.page == CLAN_PAGE_GAME, "text save returns to reader");

        open_menu();
        click(BSP_BTN_DOWN);
        click(BSP_BTN_DOWN);                  // 读取进度
        click(BSP_BTN_OK);
        CHECK(app.page == CLAN_PAGE_LOAD, "text load opens slot list");
        for (int i = 0; i < 4; ++i) click(BSP_BTN_DOWN);
        click(BSP_BTN_OK);
        CHECK(app.page == CLAN_PAGE_GAME, "text load returns to reader");
        CHECK(strcmp(app.text, text_before) == 0, "text load restores the same sentence");
        CHECK(strcmp(app.speaker, speaker_before) == 0, "text load restores the speaker");
        CHECK(app.page_index == page_before, "text load restores the same page");
    }

    // 自动档 → 标题“继续阅读”恢复同一句；不依赖手动槽。
    {
        char text_before[1024];
        snprintf(text_before, sizeof(text_before), "%s", app.text);
        const uint16_t page_before = (uint16_t)app.page_index;
        clannad_app_flush(&app);
        goto_title();
        CHECK(app.page == CLAN_PAGE_TITLE, "auto save returns to title");
        click(BSP_BTN_DOWN);                  // 继续
        click(BSP_BTN_OK);
        CHECK(app.page == CLAN_PAGE_GAME, "continue loads automatic save");
        CHECK(strcmp(app.text, text_before) == 0,
              "automatic save restores the same sentence");
        CHECK(app.page_index == page_before,
              "automatic save restores the same page");
    }

    // 子页：手动槽独立于 flush/auto，页码也随槽恢复。
    if (app.page == CLAN_PAGE_GAME && app.page_count > 1) {
        click(BSP_BTN_UP);
        if (app.page == CLAN_PAGE_GAME && app.page_index == 1) {
            char text_before[1024];
            snprintf(text_before, sizeof(text_before), "%s", app.text);
            open_menu();
            click(BSP_BTN_DOWN);
            click(BSP_BTN_OK);
            for (int i = 0; i < 5; ++i) click(BSP_BTN_DOWN);
            click(BSP_BTN_OK);
            CHECK(app.slot_ok[5], "subpage slot saves");
            CHECK(app.page == CLAN_PAGE_GAME, "subpage save returns");
            open_menu();
            click(BSP_BTN_DOWN);
            click(BSP_BTN_DOWN);
            click(BSP_BTN_OK);
            for (int i = 0; i < 5; ++i) click(BSP_BTN_DOWN);
            click(BSP_BTN_OK);
            CHECK(app.page_index == 1 && strcmp(app.text, text_before) == 0,
                  "subpage slot restores page 1 and text");
        }
    }

    // 快进：长按 UP 时当前页必须瞬显（typing=false、shown_bytes==页尾）。
    {
        app.typing = true;               // 人为制造打字中
        app.shown_bytes = app.page_offsets[app.page_index];
        send_key(BSP_BTN_UP, BSP_BTN_LONG);
        CHECK(app.fast_forward, "long UP enters fast forward");
        CHECK(!app.typing, "fast forward must not type");
        CHECK(app.shown_bytes == app.page_offsets[app.page_index + 1],
              "fast forward shows the whole page");
        send_key(BSP_BTN_UP, BSP_BTN_RELEASE);
        CHECK(!app.fast_forward, "release leaves fast forward");
    }

    // 真实短按序列推进；长按吞掉尾随 CLICK，且不吞下一次独立短按
    // 真实短按序列推进；长按吞掉尾随 CLICK，且不吞下一次独立短按
    {
        char before[64];
        snprintf(before, sizeof(before), "%s", app.text);
        send_key(BSP_BTN_UP, BSP_BTN_PRESS);
        send_key(BSP_BTN_UP, BSP_BTN_RELEASE);
        send_key(BSP_BTN_UP, BSP_BTN_CLICK);
        CHECK(strcmp(before, app.text) != 0 || app.page == CLAN_PAGE_CHOICE,
              "short press advances");
    }
    {
        const char *before = app.text;
        char snapshot[64];
        snprintf(snapshot, sizeof(snapshot), "%s", before);
        long_press(BSP_BTN_UP);
        send_key(BSP_BTN_UP, BSP_BTN_CLICK);   // 驱动可能补发的尾随 CLICK
        CHECK(strcmp(snapshot, app.text) == 0, "long press trailing click swallowed");
        click(BSP_BTN_UP);
        CHECK(strcmp(snapshot, app.text) != 0 || app.page == CLAN_PAGE_CHOICE,
              "independent short press still works after long press");
    }

    // 历史：DOWN 打开，行内可见正文，长 OK 返回
    {
        const int before = app.history_count;
        click(BSP_BTN_DOWN);
        CHECK(app.page == CLAN_PAGE_HISTORY, "DOWN opens history");
        CHECK(rows_visible_count() > 0, "history rows visible");
        CHECK(before > 0, "history recorded");
        long_press(BSP_BTN_OK);
        CHECK(app.page == CLAN_PAGE_GAME, "history returns to reader");
    }

    // 选择：跑到一个待选；在待选处保存（choice_sel=1）→ 选择 → 读档回到同一待选
    CHECK(advance_until_choice(2000), "reach a choice");
    if (app.page == CLAN_PAGE_CHOICE) {
        const uint16_t choice_cursor = app.vm.cursor;
        click(BSP_BTN_DOWN);   // 非零光标
        CHECK(app.choice_sel == 1, "choice cursor moved");
        open_menu();
        CHECK(app.page == CLAN_PAGE_MENU, "long OK opens menu at choice");
        click(BSP_BTN_DOWN);   // 保存进度
        click(BSP_BTN_OK);
        CHECK(app.page == CLAN_PAGE_SAVE, "save page");
        click(BSP_BTN_OK);     // 空栏位 0 直接保存
        CHECK(nvs_stub_count("clannad") >= 1, "save written to NVS");
        CHECK(rows_contain("已保存") || app.notice_ms > 0, "save notice");
        if (app.page == CLAN_PAGE_MENU) long_press(BSP_BTN_OK);
        CHECK(app.page == CLAN_PAGE_CHOICE && app.vm.cursor == choice_cursor,
              "menu round-trip keeps pending choice");
        CHECK(app.choice_sel == 1, "menu round-trip keeps choice cursor");
        click(BSP_BTN_OK);     // 确认选择
        CHECK(app.page != CLAN_PAGE_CHOICE, "choice confirmed");
        // 读档回到同一待选
        open_menu();
        click(BSP_BTN_DOWN);
        click(BSP_BTN_DOWN);   // 读取进度
        click(BSP_BTN_OK);
        CHECK(app.page == CLAN_PAGE_LOAD, "load page");
        click(BSP_BTN_OK);     // 栏位 0
        CHECK(app.page == CLAN_PAGE_CHOICE, "load returns to pending choice");
        CHECK(app.vm.cursor == choice_cursor, "load restores the same op");
        CHECK(app.choice_sel == 1, "load restores the choice cursor");
        // 坏档：直接写垃圾到栏位 2，再尝试读取，不得改变当前待选
        {
            nvs_handle_t h;
            CHECK(nvs_open("clannad", NVS_READWRITE, &h) == ESP_OK, "nvs open");
            const uint8_t junk[6] = {1, 2, 3, 4, 5, 6};
            CHECK(nvs_set_blob(h, "s02", junk, sizeof(junk)) == ESP_OK, "junk write");
            nvs_close(h);
        }
        const uint16_t guard_cursor = app.vm.cursor;
        open_menu();
        click(BSP_BTN_DOWN);
        click(BSP_BTN_DOWN);
        click(BSP_BTN_OK);     // load page
        for (int i = 0; i < 2; ++i) click(BSP_BTN_DOWN);
        click(BSP_BTN_OK);     // slot 2
        CHECK(app.vm.cursor == guard_cursor, "bad save leaves cursor unchanged");
        CHECK(app.notice_ms > 0, "bad save shows a notice");
        long_press(BSP_BTN_OK);
        if (app.page == CLAN_PAGE_MENU) long_press(BSP_BTN_OK);
        CHECK(app.page == CLAN_PAGE_CHOICE, "bad save keeps pending choice");
    }

    // 存档写入失败：旧栏位与当前状态不变
    if (app.page == CLAN_PAGE_CHOICE) {
        const int before = nvs_stub_count("clannad");
        nvs_stub_fail_write = 1;
        open_menu();
        click(BSP_BTN_DOWN);
        click(BSP_BTN_OK);
        for (int i = 0; i < 3; ++i) click(BSP_BTN_DOWN);
        click(BSP_BTN_OK);     // 空栏位 3
        CHECK(app.notice_ms > 0, "failed save shows notice");
        CHECK(nvs_stub_count("clannad") == before, "failed save wrote nothing");
        CHECK(app.page == CLAN_PAGE_SAVE, "failed save keeps slot page");
        nvs_stub_fail_write = 0;
        long_press(BSP_BTN_OK);   // 回菜单
        if (app.page == CLAN_PAGE_MENU) long_press(BSP_BTN_OK);
        CHECK(app.page == CLAN_PAGE_CHOICE, "failed save keeps pending choice");
    }

    // 自动播放/快进遇选择停止
    if (app.page == CLAN_PAGE_CHOICE) {
        click(BSP_BTN_OK);
        if (app.page == CLAN_PAGE_GAME) {
            send_key(BSP_BTN_UP, BSP_BTN_PRESS);
            send_key(BSP_BTN_UP, BSP_BTN_LONG);
            for (int i = 0; i < 4000 && app.page == CLAN_PAGE_GAME; ++i) tick_ms(60);
            send_key(BSP_BTN_UP, BSP_BTN_RELEASE);
            CHECK(app.page != CLAN_PAGE_GAME, "fast-forward stops at choice/end");
        }
    }

    // 全局通关记录：设置 g[]，flush，重新初始化后标题读数与 After Story 生效
    app.vm.mode = CLAN_MODE_STORY;
    clannad_vm_var_set_int(&app.vm, 1, 0, 13);
    for (int bit = 1; bit <= 31; ++bit) clannad_vm_var_set_int(&app.vm, 1, bit, 1);
    clannad_vm_var_set_int(&app.vm, 1, 45, 1);
    nvs_stub_fail_write = 1;
    clannad_app_flush(&app);
    CHECK(app.progress_dirty && app.notice_ms > 0,
          "failed progress write stays dirty and notices");
    nvs_stub_fail_write = 0;
    app.progress_ms = 0;
    clannad_app_tick(&app, 2500);
    CHECK(!app.progress_dirty, "progress retry succeeds");
    CHECK(nvs_stub_count("clannad") >= 2, "progress/auto written");
    CHECK(clannad_app_init(&app2, pack_data, (uint32_t)pack_size, why, sizeof(why)),
          "second app init");
    CHECK(app2.light_count == 13, "global light count loaded");
    CHECK(app2.true_ending, "true ending flag loaded");
    CHECK(app2.after_unlocked, "after story unlocked from global flags");
    CHECK(app2.page == CLAN_PAGE_TITLE && app2.title_art_for_ending,
          "restarted title displays the true-ending art");

    // 标题 → CG 鉴赏 → 全屏 CG → 返回；图片失败提示
    {
        goto_title();
        CHECK(app.page == CLAN_PAGE_TITLE, "returned to title");
        bool found = false;
        for (int i = 0; i < CLAN_UI_TITLE_ROWS; ++i) {
            if (strstr(app.ui.title_main[i], "CG")) found = true;
        }
        CHECK(found, "title has CG entry");
        for (int i = 0; i < 4; ++i) click(BSP_BTN_DOWN);
        click(BSP_BTN_OK);
        CHECK(app.page == CLAN_PAGE_GALLERY, "gallery opens");
        CHECK(rows_visible_count() > 0, "gallery rows visible");
        click(BSP_BTN_OK);
        CHECK(app.page == CLAN_PAGE_CG, "CG viewer opens");
        long_press(BSP_BTN_OK);
        CHECK(app.page == CLAN_PAGE_GALLERY, "CG returns to list");
        long_press(BSP_BTN_OK);
        CHECK(app.page == CLAN_PAGE_TITLE, "gallery returns to title");
    }
    {
        // 破坏标题背景的声明长度，再用同一包初始化：必须提示且不崩溃
        const uint16_t title_assets[2] = {
            (uint16_t)clannad_pack_meta_int(&app.pack, "title_before", 0),
            (uint16_t)clannad_pack_meta_int(&app.pack, "title_after", 0),
        };
        uint32_t asset_off = 0;
        const uint16_t n = clannad_rd16(pack_data + 16);
        for (uint16_t i = 0; i < n; ++i) {
            const uint8_t *s = pack_data + 24 + i * 16;
            if (clannad_rd32(s) == CLAN_SEC_ASSET) asset_off = clannad_rd32(s + 4);
        }
        uint8_t *recs[2] = {0, 0};
        uint32_t saved_lens[2] = {0, 0};
        for (int i = 0; i < 2; ++i) {
            if (!title_assets[i]) continue;
            recs[i] = pack_data + asset_off +
                      (uint32_t)title_assets[i] * CLAN_ASSET_BYTES;
            saved_lens[i] = clannad_rd32(recs[i] + 4);
            const uint32_t quarter = saved_lens[i] / 4;
            recs[i][4] = (uint8_t)quarter;
            recs[i][5] = (uint8_t)(quarter >> 8);
            recs[i][6] = (uint8_t)(quarter >> 16);
            recs[i][7] = (uint8_t)(quarter >> 24);
        }
        lv_stub_invalidate_count = 0;
        CHECK(clannad_app_init(&bad_app, pack_data, (uint32_t)pack_size, why,
                               sizeof(why)), "app init with damaged title");
        CHECK(strstr(bad_app.notice, "标题图片") != NULL || bad_app.notice_ms > 0,
              "damaged title image reports a notice");
        CHECK(bad_app.image_failed && !bad_app.cache_valid,
              "damaged title marks image failure and invalid cache");
        CHECK(lv_stub_invalidate_count > 0, "damaged title invalidates canvas");
        for (int i = 0; i < 2; ++i) {
            if (!recs[i]) continue;
            recs[i][4] = (uint8_t)saved_lens[i];
            recs[i][5] = (uint8_t)(saved_lens[i] >> 8);
            recs[i][6] = (uint8_t)(saved_lens[i] >> 16);
            recs[i][7] = (uint8_t)(saved_lens[i] >> 24);
        }
    }

    // 场景失败：app 层必须保留提醒、cache 无效、image_failed=true，并在
    // 同一 id 重试成功后恢复；图像层已在 test_clannad_image 里检查全画布 clear。
    if (bad_app.ui.image.pixels) {
        uint16_t bodies[3] = {0, 0, 0};
        const uint16_t bg = find_scene_bg(&bad_app.pack);
        const int nb = find_scene_sprites(&bad_app.pack, bodies);
        CHECK(bg != 0xFFFF && nb == 3, "find app scene assets");
        if (bg != 0xFFFF && nb == 3) {
            uint8_t *rec = (uint8_t *)bad_app.pack.asset +
                           (size_t)bodies[1] * CLAN_ASSET_BYTES;
            const uint32_t saved_mask = clannad_rd32(rec + 16);
            CHECK(saved_mask > 8, "app second sprite has mask");
            const uint32_t bad_mask = saved_mask - 8;
            rec[16] = (uint8_t)bad_mask;
            rec[17] = (uint8_t)(bad_mask >> 8);
            rec[18] = (uint8_t)(bad_mask >> 16);
            rec[19] = (uint8_t)(bad_mask >> 24);
            bad_app.page = CLAN_PAGE_GAME;
            bad_app.vm.background = bg;
            bad_app.vm.body_count = 3;
            bad_app.vm.bodies[0] = bodies[0];
            bad_app.vm.bodies[1] = bodies[1];
            bad_app.vm.bodies[2] = bodies[2];
            bad_app.cache_valid = false;
            bad_app.image_failed = true;
            bad_app.notice[0] = '\0';
            bad_app.notice_ms = 0;
            lv_stub_invalidate_count = 0;
            click_on(&bad_app, BSP_BTN_UP);
            CHECK(!bad_app.cache_valid && bad_app.image_failed,
                  "bad app scene leaves cache invalid and image_failed");
            CHECK(strstr(bad_app.notice, "图片读取失败") != NULL,
                  "bad app scene keeps the image error notice");
            CHECK(lv_stub_invalidate_count > 0,
                  "bad app scene invalidates canvas");

            rec[16] = (uint8_t)saved_mask;
            rec[17] = (uint8_t)(saved_mask >> 8);
            rec[18] = (uint8_t)(saved_mask >> 16);
            rec[19] = (uint8_t)(saved_mask >> 24);
            click_on(&bad_app, BSP_BTN_UP);
            CHECK(bad_app.cache_valid && !bad_app.image_failed,
                  "same app scene retries successfully after restore");
            CHECK(bad_app.notice[0] == '\0',
                  "successful retry clears the image failure notice");
        }
    }

    // 真实 6729 章节入口：源 selectBrowse 的 withProgress(g2=1,g32=0,g0=5)
    // 必须复现 cursor 114；章节局部改动不得写回全局 progress。
    {
        static uint8_t progress_raw[32768];
        static clannad_vm_t progress_vm;
        lv_stub_reset();
        CHECK(clannad_vm_init(&progress_vm, pack_data, (uint32_t)pack_size,
                              progress_raw, sizeof(progress_raw), why, sizeof(why)),
              "chapter witness progress vm");
        CHECK(clannad_vm_var_set_int(&progress_vm, 1, 2, 1), "witness g2=1");
        CHECK(clannad_vm_var_set_int(&progress_vm, 1, 32, 0), "witness g32=0");
        CHECK(clannad_vm_var_set_int(&progress_vm, 1, 0, 5), "witness g0=5");
        const size_t plen = clannad_vm_progress_encode(
            &progress_vm, chapter_app.progress, sizeof(chapter_app.progress));
        CHECK(plen > 0 && clannad_vm_progress_validate(chapter_app.progress, plen),
              "witness progress encodes");
        chapter_app.progress_len = (uint16_t)plen;
        chapter_app.progress_valid = true;
        uint8_t progress_before[CLAN_VM_PROGRESS_MAX];
        memcpy(progress_before, chapter_app.progress, plen);
        CHECK(clannad_app_init(&chapter_app, pack_data, (uint32_t)pack_size, why,
                               sizeof(why)), "chapter witness app init");
        chapter_app.settings.speed = 3;
        chapter_app.progress_len = (uint16_t)plen;
        chapter_app.progress_valid = true;
        memcpy(chapter_app.progress, progress_before, plen);
        int route_idx = -1, chapter_idx = -1;
        CHECK(find_chapter_entry(&chapter_app.pack, 6729, &route_idx, &chapter_idx),
              "6729 chapter entry exists");
        chapter_app.title_sel = 5;            // 章节 / 路线
        click_on(&chapter_app, BSP_BTN_OK);
        CHECK(chapter_app.page == CLAN_PAGE_ROUTES, "6729 route list opens");
        chapter_app.list_sel = route_idx;
        click_on(&chapter_app, BSP_BTN_OK);
        CHECK(chapter_app.page == CLAN_PAGE_CHAPTERS, "6729 chapter list opens");
        chapter_app.list_sel = chapter_idx;
        click_on(&chapter_app, BSP_BTN_OK);
        CHECK(chapter_app.page == CLAN_PAGE_GAME && chapter_app.vm.mode == CLAN_MODE_CHAPTER,
              "6729 chapter starts in CHAPTER mode");
        for (int guard = 0; guard < 500 && chapter_app.page == CLAN_PAGE_GAME &&
                          chapter_app.vm.cursor < 114; ++guard) {
            click_on(&chapter_app, BSP_BTN_UP);
        }
        while (chapter_app.page == CLAN_PAGE_CHOICE) {
            click_on(&chapter_app, BSP_BTN_OK);
            for (int guard = 0; guard < 500 && chapter_app.page == CLAN_PAGE_GAME &&
                              chapter_app.vm.cursor < 114; ++guard) {
                click_on(&chapter_app, BSP_BTN_UP);
            }
        }
        const uint16_t witness_cursor = (uint16_t)chapter_app.vm.cursor;
        CHECK(witness_cursor == 114,
              "6729 source withProgress first text >=114 cursor 114");
        CHECK(strstr(chapter_app.text, "是吗") != NULL,
              "6729 source witness first text");
        int32_t g2 = -1, g32 = -1, g0 = -1;
        CHECK(clannad_vm_var_int(&chapter_app.vm, 1, 2, &g2) && g2 == 1 &&
                  clannad_vm_var_int(&chapter_app.vm, 1, 32, &g32) && g32 == 0 &&
                  clannad_vm_var_int(&chapter_app.vm, 1, 0, &g0) && g0 == 5,
              "6729 chapter retains injected g2/g32/g0");
        // Run the chapter for a bounded window; local values may change, but the
        // saved global progress blob must remain byte-identical.
        for (int i = 0; i < 4000 && chapter_app.page == CLAN_PAGE_GAME; ++i) {
            if (chapter_app.typing) {
                chapter_app.typing = false;
            }
            click_on(&chapter_app, BSP_BTN_UP);
        }
        clannad_app_flush(&chapter_app);
        CHECK(chapter_app.progress_len == plen &&
                  memcmp(chapter_app.progress, progress_before, plen) == 0,
              "6729 chapter flush does not write global progress");
        printf("6729 witness: cursor=%u g2=%ld g32=%ld g0=%ld progress_same=%d\n",
               (unsigned)witness_cursor, (long)g2, (long)g32, (long)g0,
               chapter_app.progress_len == plen &&
                       memcmp(chapter_app.progress, progress_before, plen) == 0);
        CHECK(chapter_app.slot_state[0] == CLAN_SLOT_EMPTY ||
                  chapter_app.slot_state[0] == CLAN_SLOT_VALID ||
                  chapter_app.slot_state[0] == CLAN_SLOT_CORRUPT ||
                  chapter_app.slot_state[0] == CLAN_SLOT_IO_ERROR,
              "slot state enum is initialized");
    }

    // D 段：6 个显示页面的 flush 都必须保存当前阅读 checkpoint，而不是显示页。
    {
        const int paths = 7;
        for (int path = 0; path < paths; ++path) {
            CHECK(make_checkpoint_app(&checkpoint_app), "checkpoint app starts");
            const uint16_t expected = checkpoint_app.vm.cursor;
            open_checkpoint_path(&checkpoint_app, path);
            clannad_app_flush(&checkpoint_app);
            uint16_t got = 0xFFFF;
            CHECK(auto_checkpoint(&checkpoint_app, &got, NULL),
                  "checkpoint auto exists");
            CHECK(got == expected, "checkpoint flush uses current VM position");
        }
        // 自动档写失败保留 dirty，定时器节流后重试成功。
        CHECK(make_checkpoint_app(&checkpoint_app), "dirty checkpoint app");
        nvs_stub_fail_write = 1;
        clannad_app_flush(&checkpoint_app);
        CHECK(checkpoint_app.auto_dirty, "failed autosave keeps auto_dirty");
        nvs_stub_fail_write = 0;
        clannad_app_tick(&checkpoint_app, 2000);
        CHECK(!checkpoint_app.auto_dirty, "autosave retry clears auto_dirty");
        uint16_t retry_cursor = 0;
        CHECK(auto_checkpoint(&checkpoint_app, &retry_cursor, NULL) &&
                  retry_cursor == checkpoint_app.vm.cursor,
              "autosave retry writes the same checkpoint");

        // 初始标题没有 checkpoint，不能写空 auto。
        nvs_stub_reset();
        char why[128] = {0};
        CHECK(clannad_app_init(&checkpoint_app, pack_data, (uint32_t)pack_size,
                               why, sizeof(why)), "title checkpoint app");
        clannad_app_flush(&checkpoint_app);
        uint16_t unused = 0;
        CHECK(!auto_checkpoint(&checkpoint_app, &unused, NULL),
              "fresh title writes no auto checkpoint");
    }

    // F 段：get 失败不得把占用槽显示为空、不得无确认覆盖；解除后重试可确认。
    {
        nvs_stub_reset();
        char why[128] = {0};
        lv_stub_reset();
        CHECK(clannad_app_init(&slot_app, pack_data, (uint32_t)pack_size, why,
                               sizeof(why)), "slot fault app init");
        slot_app.settings.speed = 3;
        click_on(&slot_app, BSP_BTN_OK);       // 新游戏
        long_press_on(&slot_app, BSP_BTN_OK);  // 菜单
        slot_app.list_sel = 1;
        click_on(&slot_app, BSP_BTN_OK);       // 保存页
        slot_app.list_sel = 3;
        click_on(&slot_app, BSP_BTN_OK);       // 空槽 3 直接保存
        CHECK(slot_app.slot_state[3] == CLAN_SLOT_VALID, "slot 3 valid before fault");
        uint8_t old_blob[4096];
        size_t old_len = 0;
        CHECK(read_nvs_blob("clannad", "s03", old_blob, sizeof(old_blob),
                            &old_len), "read old slot 3");
        slot_app.list_sel = 0;
        click_on(&slot_app, BSP_BTN_OK);       // 返回游戏
        click_on(&slot_app, BSP_BTN_UP);       // 改变阅读位置
        long_press_on(&slot_app, BSP_BTN_OK);
        slot_app.list_sel = 1;
        click_on(&slot_app, BSP_BTN_OK);       // 保存页
        nvs_stub_fail_read = 1;
        slot_app.list_sel = 3;
        click_on(&slot_app, BSP_BTN_OK);
        CHECK(slot_app.page == CLAN_PAGE_SAVE, "IO error stays on save page");
        CHECK(slot_app.slot_state[3] == CLAN_SLOT_IO_ERROR,
              "IO error state distinct from empty");
        CHECK(!slot_app.slot_ok[3], "IO error slot is not shown as valid");
        CHECK(slot_app.slot_action == 1, "save action retained");
        CHECK(slot_app.page != CLAN_PAGE_CONFIRM,
              "IO error does not silently save or bypass confirm");
        nvs_stub_fail_read = 0;
        uint8_t after_blob[4096];
        size_t after_len = 0;
        CHECK(read_nvs_blob("clannad", "s03", after_blob, sizeof(after_blob),
                            &after_len), "read slot after IO error");
        CHECK(after_len == old_len &&
                  memcmp(old_blob, after_blob, old_len) == 0,
              "IO error never overwrites old slot");
        slot_app.list_sel = 3;
        click_on(&slot_app, BSP_BTN_OK);
        CHECK(slot_app.page == CLAN_PAGE_CONFIRM,
              "retry after IO recovery opens overwrite confirm");
        CHECK(slot_app.confirm_sel == 1, "overwrite confirm defaults to cancel");
        CHECK(read_nvs_blob("clannad", "s03", after_blob, sizeof(after_blob),
                            &after_len) && after_len == old_len &&
                  memcmp(old_blob, after_blob, old_len) == 0,
              "confirm page still leaves old slot unchanged");
    }

    // 导航回归 A：菜单第 5 项“历史记录”必须真的打开历史，并能回到菜单。
    // 修复前 open_history() 只收 GAME/CHOICE，菜单调用被静默拒绝。
    {
        nvs_stub_reset();
        lv_stub_reset();
        CHECK(clannad_app_init(&app, pack_data, (uint32_t)pack_size, why, sizeof(why)),
              "nav menu history app init");
        app.settings.speed = 3;
        click(BSP_BTN_OK);                       // 新游戏
        CHECK(app.page == CLAN_PAGE_GAME, "nav menu history starts in game");
        CHECK(strstr(app.notice, "OK 菜单") != NULL,
              "nav game start shows key hint");
        open_menu();
        app.list_sel = 4;                        // 历史记录
        click(BSP_BTN_OK);
        CHECK(app.page == CLAN_PAGE_HISTORY, "nav menu history opens history");
        click(BSP_BTN_OK);
        CHECK(app.page == CLAN_PAGE_MENU, "nav menu history returns to menu");
    }

    // 导航回归 B：设置→操作说明→返回后，设置页仍能回到标题（长 OK 路径）。
    // 修复前 open_about() 把共享 return_page 改成 SETTINGS，设置页自锁。
    {
        nvs_stub_reset();
        lv_stub_reset();
        CHECK(clannad_app_init(&app, pack_data, (uint32_t)pack_size, why, sizeof(why)),
              "nav settings about app init");
        app.settings.speed = 3;
        app.title_sel = 6;                       // 设置
        click(BSP_BTN_OK);
        CHECK(app.page == CLAN_PAGE_SETTINGS, "nav settings about opens settings");
        app.list_sel = 3;                        // 操作说明
        click(BSP_BTN_OK);
        CHECK(app.page == CLAN_PAGE_ABOUT, "nav settings about opens about");
        long_press(BSP_BTN_OK);
        CHECK(app.page == CLAN_PAGE_SETTINGS, "nav settings about returns to settings");
        long_press(BSP_BTN_OK);
        CHECK(app.page == CLAN_PAGE_TITLE, "nav settings about exits to title");
    }

    // 导航回归 C：同一路径下用设置页第 5 项“返回”也要能出去。
    {
        nvs_stub_reset();
        lv_stub_reset();
        CHECK(clannad_app_init(&app, pack_data, (uint32_t)pack_size, why, sizeof(why)),
              "nav settings return row app init");
        app.settings.speed = 3;
        app.title_sel = 6;
        click(BSP_BTN_OK);
        app.list_sel = 3;
        click(BSP_BTN_OK);
        long_press(BSP_BTN_OK);                  // 关于 → 设置
        CHECK(app.page == CLAN_PAGE_SETTINGS, "nav settings return row at settings");
        app.list_sel = 4;                        // 返回
        click(BSP_BTN_OK);
        CHECK(app.page == CLAN_PAGE_TITLE, "nav settings return row exits to title");
    }

    // 坏包：魔数/长度
    {
        uint8_t copy[64];
        memcpy(copy, pack_data, sizeof(copy));
        copy[0] = 'X';
        CHECK(!clannad_app_init(&bad_app, copy, sizeof(copy), why, sizeof(why)),
              "bad pack rejected");
        CHECK(why[0] != '\0', "bad pack gives a reason");
        CHECK(!clannad_app_init(&bad_app, pack_data, (uint32_t)pack_size - 8, why,
                                sizeof(why)), "truncated pack rejected");
    }

    // 上机 5 条修复的回归断言：面板 @170、章节条只包文字、OK 短按推进 /
    // OK 长按进菜单、立绘脚落在面板上沿、列表垂直居中。
    {
        CHECK(app.ui.game_panel->bg_opa == 170, "panel opacity is 170");
        CHECK(app.ui.lines[0]->bg_opa == 0, "body line has no own background");
        CHECK(app.ui.chapter->w == LV_SIZE_CONTENT, "chapter width is content-only");

        // 立绘：可见底边 == 面板上沿；pack 图裁过边(h<full_h)，用 h 回推
        clannad_asset_t fake = {0};
        fake.w = 148; fake.h = 160; fake.ow = 180; fake.oh = 194;
        int px, py, pw, ph;
        if (clannad_image_portrait_box(1, 0, &fake, &px, &py, &pw, &ph)) {
            CHECK(py + ph == CLAN_PANEL_Y, "portrait feet sit on panel top");
            CHECK(py >= 0, "portrait stays inside canvas");
            CHECK(ph == (int)(160 * ((float)248 / 180 < (float)160 / 194
                                         ? (float)248 / 180 : (float)160 / 194) + 0.5f),
                  "portrait height uses stored pixels");
        } else {
            CHECK(0, "portrait box computed");
        }

        // 按键：短按 OK 推进；长按 OK 进菜单
        if (app.page == CLAN_PAGE_GAME) {
            const uint16_t page_before = app.page_index;
            char text_before[64];
            snprintf(text_before, sizeof(text_before), "%s", app.text);
            click(BSP_BTN_OK);
            CHECK(app.page == CLAN_PAGE_GAME, "short OK stays in game");
            CHECK(app.page_index != page_before || strcmp(text_before, app.text) != 0,
                  "short OK advances text");
            long_press(BSP_BTN_OK);
            CHECK(app.page == CLAN_PAGE_MENU, "long OK opens menu");
            app.list_sel = 0;
            click(BSP_BTN_OK);              // 菜单第 0 行 = 继续阅读
            CHECK(app.page == CLAN_PAGE_GAME, "menu row 0 back to game");
        }

        // 列表垂直居中：首行 y == (240 - rows*38)/2
        {
            static clannad_ui_row_t rows4[4];
            for (int i = 0; i < 4; ++i) {
                snprintf(rows4[i].main, sizeof(rows4[i].main), "row%d", i);
                rows4[i].value[0] = 0;
                rows4[i].sub[0] = 0;
            }
            clannad_ui_list(&app.ui, "设置", rows4, 4, 0, 0, "hint");
            CHECK(app.ui.list_rows[0][0]->y == (240 - 4 * 38) / 2,
                  "list rows vertically centered");
            CHECK(app.ui.list_sel->y == (240 - 4 * 38) / 2 - 2,
                  "list selection follows centered rows");
        }
    }

    printf("%s: checks %d, failures %d\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
