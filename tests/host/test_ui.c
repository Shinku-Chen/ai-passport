// tests/host/test_ui.c —— 用真实按键序列驱动生产 UI 状态机（LVGL 薄桩记录可见行）。
#include "gal_app.h"

#include "bsp_button.h"
#include "nvs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void lv_stub_reset(void);
extern const lv_obj_t *lv_stub_find_text(const char *text);
extern const lv_obj_t *lv_stub_find_containing(const char *needle);
extern int host_lvgl_violations(void);
extern int lv_stub_count(void);
extern void host_lvgl_reset_violations(void);
extern int host_lvgl_depth(void);
extern bool host_landscape(void);

static int g_fail;
static int g_checks;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        g_checks++;                                                                      \
        if (!(cond)) {                                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                       \
            g_fail++;                                                                    \
        }                                                                                \
    } while (0)

#define CHECK_EQ_INT(a, b)                                                               \
    do {                                                                                 \
        g_checks++;                                                                      \
        long long _a = (long long)(a), _b = (long long)(b);                              \
        if (_a != _b) {                                                                  \
            printf("FAIL %s:%d: %s=%lld != %s=%lld\n", __FILE__, __LINE__, #a, _a, #b,   \
                   _b);                                                                  \
            g_fail++;                                                                    \
        }                                                                                \
    } while (0)

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *fh = fopen(path, "rb");
    if (!fh) {
        printf("cannot open %s\n", path);
        exit(2);
    }
    fseek(fh, 0, SEEK_END);
    long n = ftell(fh);
    fseek(fh, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (fread(buf, 1, (size_t)n, fh) != (size_t)n) {
        printf("short read %s\n", path);
        exit(2);
    }
    fclose(fh);
    *size = (size_t)n;
    return buf;
}

static void send_key(gal_app_t *app, bsp_btn_t btn, bsp_btn_ev_t ev)
{
    gal_key_t key = {(uint8_t)btn, (uint8_t)ev};
    gal_app_key(app, &key);
}

// 真实短按：PRESS → RELEASE → 延迟 CLICK
static void short_press(gal_app_t *app, bsp_btn_t btn)
{
    send_key(app, btn, BSP_BTN_PRESS);
    send_key(app, btn, BSP_BTN_RELEASE);
    send_key(app, btn, BSP_BTN_CLICK);
}

// 真实长按：PRESS → LONG → RELEASE
static void long_press(gal_app_t *app, bsp_btn_t btn)
{
    send_key(app, btn, BSP_BTN_PRESS);
    send_key(app, btn, BSP_BTN_LONG);
    send_key(app, btn, BSP_BTN_RELEASE);
    send_key(app, btn, BSP_BTN_CLICK);   // 驱动常补发一次 CLICK，必须被吞掉
}

static void press_n(gal_app_t *app, bsp_btn_t btn, int n)
{
    for (int i = 0; i < n; i++) {
        short_press(app, btn);
    }
}

static void tick_ms(gal_app_t *app, uint32_t ms) { gal_app_tick(app, ms); }

static int utf8_chars(const char *s)
{
    int n = 0;
    for (int i = 0; s && s[i];) {
        unsigned char c = (unsigned char)s[i];
        i += c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        n++;
    }
    return n;
}

static void test_title_and_reader(gal_app_t *app)
{
    CHECK_EQ_INT(app->page, PQ_PAGE_TITLE);
    CHECK(lv_stub_find_text("START") != NULL);
    CHECK(lv_stub_find_text("SIDE STORY") != NULL);
    CHECK(lv_stub_find_text("AFTER STORY") == NULL);   // 未通关不显示

    short_press(app, BSP_BTN_OK);   // START
    CHECK_EQ_INT(app->page, PQ_PAGE_READER);
    CHECK(app->lbl_text != NULL);
    CHECK(strlen(app->text) > 0);
    CHECK_EQ_INT(app->game.scene, 3);   // 首次 START → 前传（1→2→3 是纯跳转场景）
    CHECK(app->draw_error == false);
    CHECK(lv_stub_find_text("pre story") != NULL);
    // 阅读页四硬指标：名字在框外、页码独立且在框内右上、末行不超面板底。
    CHECK(app->lbl_speaker != NULL && app->lbl_speaker->y + app->lbl_speaker->h <= 150);
    // 正文贴面板顶：首行 y = 面板顶 + 4；末行底 <= 面板底 - 2。
    CHECK_EQ_INT(app->lbl_text->y, 150 + 4);
    CHECK(app->lbl_text->y + 3 * 19 <= 240 - 2);
    // 页码独立：挪到面板外的右上，不与正文/名字相交。
    CHECK(app->lbl_page != NULL && app->lbl_page->y + app->lbl_page->h <= 150);
    CHECK(app->lbl_page->x >= app->lbl_speaker->x + app->lbl_speaker->w);
    // 名字框只包文字：宽度为 LV_SIZE_CONTENT（不再固定 160），且 y 在框外。
    CHECK(app->lbl_speaker->w == LV_SIZE_CONTENT);

    // 阅读页背景必须铺满 0..239（双带跨带）：y=150/200/239 都要有画面像素，
    // 且 119/120 这条带边界两侧都连续有内容。
    {
        static uint16_t flat[320 * 240];
        pq_gfx_test_flatten(flat);
        const int rows[] = {119, 120, 150, 200, 239};
        for (size_t k = 0; k < sizeof(rows) / sizeof(rows[0]); ++k) {
            const int y = rows[k];
            int lit = 0;
            for (int x = 0; x < 320; ++x) {
                if (flat[y * 320 + x] != 0) lit++;
            }
            if (lit <= 60) {
                printf("  reader bg row y=%d only %d lit pixels\n", y, lit);
            }
            CHECK(lit > 60);
        }
    }

    // UP 推进：句子/页必须变化
    char first[256];
    snprintf(first, sizeof(first), "%s", app->text);
    uint32_t op_first = app->shown_op;
    for (int i = 0; i < 5 && app->shown_op == op_first; i++) {
        short_press(app, BSP_BTN_UP);
    }
    CHECK(app->shown_op != op_first || strcmp(app->text, first) != 0);

    // 长按 UP = 快进；松手停止。速度：60ms 一次、每次最多 2 页（原 120ms/页）。
    tick_ms(app, 5000);   // 先让打字机走完，否则 tick 走的是 typing 分支
    send_key(app, BSP_BTN_UP, BSP_BTN_PRESS);
    send_key(app, BSP_BTN_UP, BSP_BTN_LONG);
    CHECK(app->fast_forward == true);
    // 快进：当前页必须瞬显（不等打字机）+ typed == 当前页长度。
    {
        const int page_len = gal_app_test_page_len(app);
        CHECK(app->typing == false);
        CHECK_EQ_INT(app->typed, page_len);
        CHECK(page_len > 0);
    }
    {
        const int page0 = app->page_index;
        const uint32_t op0 = app->shown_op;
        gal_app_tick(app, 60);
        const bool moved1 = app->page_index != page0 || app->shown_op != op0;
        CHECK(moved1);
        gal_app_tick(app, 60);
        CHECK(app->page_index != page0 || app->shown_op != op0);
    }
    send_key(app, BSP_BTN_UP, BSP_BTN_RELEASE);
    CHECK(app->fast_forward == false);

    // DOWN = 历史
    short_press(app, BSP_BTN_DOWN);
    CHECK_EQ_INT(app->page, PQ_PAGE_HISTORY);
    CHECK(app->game.hist_count > 0);
    CHECK(lv_stub_find_text("历史文本") != NULL);
    long_press(app, BSP_BTN_OK);
    CHECK_EQ_INT(app->page, PQ_PAGE_READER);

    // 短 OK = 下一句（长 OK 才进菜单）
    tick_ms(app, 5000);
    {
        const int page0 = app->page_index;
        const uint32_t op0 = app->shown_op;
        char first2[256];
        snprintf(first2, sizeof(first2), "%s", app->text);
        short_press(app, BSP_BTN_OK);
        CHECK_EQ_INT(app->page, PQ_PAGE_READER);
        CHECK(app->page_index != page0 || app->shown_op != op0 ||
              strcmp(app->text, first2) != 0);
        tick_ms(app, 5000);
    }
    // 长 OK = 菜单；长按后的补发 CLICK 不能再推进一句
    {
        const uint32_t op_before = app->shown_op;
        long_press(app, BSP_BTN_OK);
        CHECK_EQ_INT(app->page, PQ_PAGE_MENU);
        CHECK_EQ_INT(app->menu_sel, 0);
        CHECK(app->shown_op == op_before);
    }
    CHECK(lv_stub_find_text("保存进度") != NULL);
    CHECK(lv_stub_find_text("读取存档") != NULL);
    CHECK(lv_stub_find_text("CG 鉴赏") != NULL);
}


// 立绘底边必须 = 对话框顶（面板顶 = PQ_ART_H）：任何尺寸都成立。
// 抽最小与普通立绘各一个，断言 draw.y + draw.h == 面板顶。
static void check_portrait_bottom(gal_app_t *app)
{
    pq_pack_t *pack = &app->game.pack;
    int small = -1, small_h = 1 << 20, normal = -1;
    for (uint16_t i = 0; i < pack->sprite_count; i++) {
        const pq_sprite_t *sp = pq_asset_sprite(pack, (uint16_t)i);
        if (!sp) continue;
        const pq_img_entry_t *body = pq_img_entry(pack, sp->body);
        if (!body) continue;
        if ((int)body->h < small_h) { small_h = (int)body->h; small = (int)i; }
        if (normal < 0 && body->h >= 120) normal = (int)i;
    }
    CHECK(small >= 0);
    CHECK(normal >= 0);
    const int picks[2] = {small, normal};
    for (int k = 0; k < 2; k++) {
        app->bodies[0] = (uint16_t)picks[k];
        app->bodies[1] = PQ_NO_IMAGE;
        app->bodies[2] = PQ_NO_IMAGE;
        app->cutin = PQ_NO_IMAGE;
        gal_app_test_draw_scene(app);
        int px = 0, py = 0, pw = 0, ph = 0;
        pq_gfx_test_last_portrait(&px, &py, &pw, &ph);
        char detail[80];
        snprintf(detail, sizeof(detail), "sprite %d h=%d y=%d bottom=%d panel_top=%d",
                 picks[k], ph, py, py + ph, PQ_ART_H);
        if (py + ph != PQ_ART_H) { printf("  %s\n", detail); }
        CHECK(py + ph == PQ_ART_H);
        CHECK(py >= 0);
        CHECK(pw > 0);
    }
}

// 章节自动续播：同路线相邻章节，末行之后直接落到下一章开头。
static void check_auto_next_chapter(gal_app_t *app)
{
    pq_pack_t *pack = &app->game.pack;
    for (uint16_t i = 0; i + 1 < pack->chapter_count; i++) {
        if (pq_chapter_route(pack, i) != pq_chapter_route(pack, (uint16_t)(i + 1))) continue;
        const uint16_t scene = pq_chapter_scene(pack, i);
        const uint16_t next = pq_chapter_scene(pack, (uint16_t)(i + 1));
        if (scene == 0xFFFF || next == 0xFFFF) continue;
        // 先记下“正常从下一章开头启动”时的游标，作为对照。
        gal_app_test_start_chapter(app, next);
        const uint32_t expect_cursor = app->game.cursor;
        gal_app_test_start_chapter(app, scene);
        app->game.scene = scene;                  // 模拟该章已播到末行
        const bool moved = gal_app_test_auto_next_chapter(app);
        char detail[80];
        snprintf(detail, sizeof(detail), "ch%u scene %u->%u cursor=%u", (unsigned)i,
                 (unsigned)scene, (unsigned)app->game.scene, (unsigned)app->game.cursor);
        if (!(moved && app->game.scene == next && app->game.cursor == expect_cursor)) {
            printf("  %s\n", detail);
        }
        CHECK(moved && app->game.scene == next && app->game.cursor == expect_cursor);
        // 末章不能自动续播（应走结局页，而不是回主菜单的自动跳章）。
        const uint16_t last = (uint16_t)(pack->chapter_count - 1);
        const uint16_t last_scene = pq_chapter_scene(pack, last);
        if (last_scene != 0xFFFF) {
            gal_app_test_start_chapter(app, last_scene);
            app->game.scene = last_scene;
            const uint32_t scene_before = app->game.scene;
            CHECK(!gal_app_test_auto_next_chapter(app));
            CHECK(app->game.scene == scene_before);
        }
        return;
    }
    CHECK(0);
}


// 快进：同一背景连续多句只解码一次（不重建对象、不重复 draw_scene）。
static void check_fast_forward_no_redecode(gal_app_t *app)
{
    gal_app_test_goto_title(app);
    short_press(app, BSP_BTN_OK);           // 开始游戏
    tick_ms(app, 3000);
    gal_app_test_reset_draw_scene_count();
    uint16_t prev_visual = app->visual_fp;
    int changes = 0;
    send_key(app, BSP_BTN_UP, BSP_BTN_PRESS);
    send_key(app, BSP_BTN_UP, BSP_BTN_LONG);
    for (int i = 0; i < 20; ++i) {
        gal_app_tick(app, 60);
        if (app->visual_fp != prev_visual) {
            changes++;
            prev_visual = app->visual_fp;
        }
    }
    send_key(app, BSP_BTN_UP, BSP_BTN_RELEASE);
    const int calls = gal_app_test_draw_scene_count();
    char detail[96];
    snprintf(detail, sizeof(detail), "draw_scene=%d visual_changes=%d over 20 ticks",
             calls, changes);
    if (calls > changes + 1) printf("  %s\n", detail);
    // 每次 visual 变化才允许解码一次（+1 容差：起点的首帧）
    CHECK(calls <= changes + 1);
}

static void test_menu_save_load(gal_app_t *app)
{
    // 菜单光标：继续阅读(0) → 保存进度(1)
    CHECK_EQ_INT(app->menu_sel, 0);
    short_press(app, BSP_BTN_DOWN);
    CHECK_EQ_INT(app->menu_sel, 1);
    short_press(app, BSP_BTN_OK);
    CHECK_EQ_INT(app->page, PQ_PAGE_SAVE);

    uint16_t scene_saved = app->game.scene;
    uint32_t op_saved = app->game.text_op;
    uint8_t page_saved = 0;
    short_press(app, BSP_BTN_OK);
    CHECK_EQ_INT(app->page, PQ_PAGE_SAVE);
    CHECK(strstr(app->notice, "已保存") != NULL);
    CHECK(app->notice_ms > 0);
    CHECK(lv_stub_find_text("已保存") != NULL);
    CHECK(pq_game_slot_exists(0));

    // 保存失败：NVS 写失败 → 提示失败，旧档不被改写
    host_nvs_set_fail(HOST_NVS_FAIL_WRITE);
    short_press(app, BSP_BTN_DOWN);
    short_press(app, BSP_BTN_OK);
    CHECK(strstr(app->notice, "保存失败") != NULL);
    CHECK(lv_stub_find_text("保存失败") != NULL);
    CHECK(!pq_game_slot_exists(1));
    host_nvs_set_fail(HOST_NVS_OK);

    // 覆盖确认：先取消，再确认
    short_press(app, BSP_BTN_UP);
    short_press(app, BSP_BTN_OK);
    CHECK_EQ_INT(app->page, PQ_PAGE_OVERWRITE);
    CHECK(lv_stub_find_text("确认覆盖") != NULL);
    short_press(app, BSP_BTN_DOWN);   // 取消
    short_press(app, BSP_BTN_OK);
    CHECK_EQ_INT(app->page, PQ_PAGE_SAVE);
    short_press(app, BSP_BTN_OK);
    CHECK_EQ_INT(app->page, PQ_PAGE_OVERWRITE);
    short_press(app, BSP_BTN_OK);     // 确认覆盖
    CHECK_EQ_INT(app->page, PQ_PAGE_SAVE);

    // 长按 OK 返回菜单，菜单光标保持
    long_press(app, BSP_BTN_OK);
    CHECK_EQ_INT(app->page, PQ_PAGE_MENU);
    CHECK_EQ_INT(app->menu_sel, 1);
    short_press(app, BSP_BTN_DOWN);   // 读取存档
    short_press(app, BSP_BTN_OK);
    CHECK_EQ_INT(app->page, PQ_PAGE_LOAD);
    CHECK(lv_stub_find_containing("自动存档") != NULL);
    CHECK(lv_stub_find_containing("存档 01") != NULL);

    // 读 1 号槽（自动档在 0）→ 回到保存的那一句
    short_press(app, BSP_BTN_DOWN);
    short_press(app, BSP_BTN_OK);
    CHECK_EQ_INT(app->page, PQ_PAGE_READER);
    CHECK_EQ_INT(app->game.scene, scene_saved);
    CHECK_EQ_INT(app->game.text_op, op_saved);
    CHECK_EQ_INT(app->page_index, page_saved);
    CHECK(app->notice_ms == 0);

    // 坏档读取：CRC/长度校验失败时只提示，不得改动当前 live 场景/句子。
    const uint16_t live_scene = app->game.scene;
    const uint32_t live_op = app->game.text_op;
    host_nvs_corrupt("pqsave", "s01", 7);
    app->load_sel = 1;
    gal_app_test_set_page(app, PQ_PAGE_LOAD);
    short_press(app, BSP_BTN_OK);
    CHECK_EQ_INT(app->page, PQ_PAGE_LOAD);
    CHECK_EQ_INT(app->game.scene, live_scene);
    CHECK_EQ_INT(app->game.text_op, live_op);
    CHECK(strstr(app->notice, "没有可用存档") != NULL);
}

static void test_choice_save_restore(gal_app_t *app)
{
    CHECK(gal_app_test_goto_choice(app));
    CHECK_EQ_INT(app->page, PQ_PAGE_CHOICE);
    CHECK(lv_stub_find_text("叫醒茨木") != NULL);
    CHECK(lv_stub_find_text("叫醒城门") != NULL);
    CHECK_EQ_INT(app->game.scene, 98);

    // 先把光标移到第 2 项（「叫醒城门」），存档要保存这个非零光标
    short_press(app, BSP_BTN_DOWN);
    CHECK_EQ_INT(app->game.choice_sel, 1);
    // 长按 OK 打开菜单（待选状态保持）
    long_press(app, BSP_BTN_OK);
    CHECK_EQ_INT(app->page, PQ_PAGE_MENU);
    short_press(app, BSP_BTN_DOWN);   // 保存进度
    short_press(app, BSP_BTN_OK);
    CHECK_EQ_INT(app->page, PQ_PAGE_SAVE);
    press_n(app, BSP_BTN_DOWN, 2);    // 存档 03
    short_press(app, BSP_BTN_OK);
    CHECK(pq_game_slot_exists(2));

    // 返回待选页：光标与非零选择保持
    long_press(app, BSP_BTN_OK);      // → 菜单
    CHECK_EQ_INT(app->page, PQ_PAGE_MENU);
    long_press(app, BSP_BTN_OK);      // → 待选
    CHECK_EQ_INT(app->page, PQ_PAGE_CHOICE);
    CHECK_EQ_INT(app->game.choice_sel, 1);
    short_press(app, BSP_BTN_OK);     // 确认「叫醒城门」
    CHECK_EQ_INT(app->page, PQ_PAGE_READER);
    CHECK_EQ_INT(app->game.scene, 104);

    // 读取待选存档：场景/待选/光标恢复（正文页短 OK 已经改成下一句，这里用长 OK）
    long_press(app, BSP_BTN_OK);      // 菜单
    short_press(app, BSP_BTN_DOWN);
    short_press(app, BSP_BTN_DOWN);   // 读取存档
    short_press(app, BSP_BTN_OK);
    CHECK_EQ_INT(app->page, PQ_PAGE_LOAD);
    press_n(app, BSP_BTN_DOWN, 3);    // 自动档 → 存档 03
    short_press(app, BSP_BTN_OK);
    CHECK_EQ_INT(app->page, PQ_PAGE_CHOICE);
    CHECK_EQ_INT(app->game.scene, 98);
    CHECK_EQ_INT(app->game.mode, 1);
    CHECK_EQ_INT(app->game.choice_sel, 1);
    CHECK(lv_stub_find_text("叫醒城门") != NULL);
}

static void test_slow_typing(gal_app_t *app)
{
    // 50ms 输入节拍 + 最慢 55ms/字：不能停住，必须按累计时间推进并保留余数
    host_nvs_reset();
    lv_stub_reset();
    gal_app_test_goto_title(app);
    app->title_sel = 0;
    short_press(app, BSP_BTN_OK);          // START → 前传 scene 3
    CHECK_EQ_INT(app->page, PQ_PAGE_READER);
    CHECK(app->typing);
    app->settings.speed = 0;               // 慢速：55ms/字
    app->typed = 0;
    app->type_accum_ms = 0;
    app->typing = true;
    for (int i = 0; i < 20; i++) {
        tick_ms(app, 50);                  // 1000ms → 18 个码点（中文 54 字节）
    }
    CHECK_EQ_INT(utf8_chars(app->lbl_text->text), 18);
    CHECK_EQ_INT(app->typed, 54);

    // 不规则短 tick：10×7ms + 10×3ms = 100ms → 1 字，余 45ms；再 10ms → 第 2 字
    app->typed = 0;
    app->type_accum_ms = 0;
    app->typing = true;
    for (int i = 0; i < 10; i++) {
        tick_ms(app, 7);
    }
    for (int i = 0; i < 10; i++) {
        tick_ms(app, 3);
    }
    CHECK_EQ_INT(utf8_chars(app->lbl_text->text), 1);
    tick_ms(app, 10);
    CHECK_EQ_INT(utf8_chars(app->lbl_text->text), 2);

    // 继续按 50ms 节拍到整页显示完
    for (int i = 0; i < 60; i++) {
        tick_ms(app, 50);
    }
    CHECK_EQ_INT(app->typing, false);
    CHECK_EQ_INT((int)strlen(app->lbl_text->text), app->typed);
    // 显示完整后 UP 能切到下一页/下一句
    uint32_t op = app->shown_op;
    int page = app->page_index;
    short_press(app, BSP_BTN_UP);
    CHECK(app->shown_op != op || app->page_index > page);
    app->settings.speed = 1;   // 还原，避免影响后续设置页测试
}

static void test_sleep_checkpoint(gal_app_t *app)
{
    host_nvs_reset();
    lv_stub_reset();
    gal_app_test_goto_title(app);
    app->title_sel = 0;
    short_press(app, BSP_BTN_OK);          // START
    for (int i = 0; i < 5; i++) {
        short_press(app, BSP_BTN_UP);
        if (app->page != PQ_PAGE_READER) {
            break;
        }
    }
    CHECK_EQ_INT(app->page, PQ_PAGE_READER);
    uint16_t scene = app->game.scene;
    uint32_t op = app->game.text_op;
    uint8_t page = (uint8_t)app->page_index;
    char text[256];
    snprintf(text, sizeof(text), "%s", app->text);

    // 在菜单页 flush（模拟睡前）；正文页短 OK 现在是“下一句”，进菜单用长 OK
    long_press(app, BSP_BTN_OK);
    CHECK_EQ_INT(app->page, PQ_PAGE_MENU);
    gal_app_flush(app);
    // 改动当前状态，再从菜单读自动档
    pq_game_start(&app->game, app->game.pack.flow, true);
    gal_app_test_set_page(app, PQ_PAGE_MENU);
    app->menu_sel = 2;
    short_press(app, BSP_BTN_OK);          // 读取存档
    CHECK_EQ_INT(app->page, PQ_PAGE_LOAD);
    app->load_sel = 0;                     // 自动档
    short_press(app, BSP_BTN_OK);
    CHECK_EQ_INT(app->page, PQ_PAGE_READER);
    CHECK_EQ_INT(app->game.scene, scene);
    CHECK_EQ_INT(app->game.text_op, op);
    CHECK_EQ_INT(app->page_index, page);
    CHECK(strcmp(app->text, text) == 0);

    // 历史页 flush 同样保存同一句
    short_press(app, BSP_BTN_DOWN);
    CHECK_EQ_INT(app->page, PQ_PAGE_HISTORY);
    gal_app_flush(app);
    pq_game_start(&app->game, app->game.pack.flow, true);
    gal_app_test_set_page(app, PQ_PAGE_LOAD);
    app->load_sel = 0;
    short_press(app, BSP_BTN_OK);
    CHECK_EQ_INT(app->game.scene, scene);
    CHECK_EQ_INT(app->game.text_op, op);

    // 设置页 flush
    long_press(app, BSP_BTN_OK);           // 菜单
    app->menu_sel = 6;
    short_press(app, BSP_BTN_OK);          // 设置
    CHECK_EQ_INT(app->page, PQ_PAGE_SETTINGS);
    gal_app_flush(app);
    pq_game_start(&app->game, app->game.pack.flow, true);
    gal_app_test_set_page(app, PQ_PAGE_LOAD);
    app->load_sel = 0;
    short_press(app, BSP_BTN_OK);
    CHECK_EQ_INT(app->game.scene, scene);
    CHECK_EQ_INT(app->game.text_op, op);

    // 菜单「返回标题」也保存自动档
    long_press(app, BSP_BTN_OK);           // 菜单
    app->menu_sel = 7;
    short_press(app, BSP_BTN_OK);          // 返回标题
    CHECK_EQ_INT(app->page, PQ_PAGE_TITLE);
    pq_game_start(&app->game, app->game.pack.flow, true);
    gal_app_test_set_page(app, PQ_PAGE_TITLE);
    app->title_sel = 3;                    // LOAD
    short_press(app, BSP_BTN_OK);
    CHECK_EQ_INT(app->page, PQ_PAGE_LOAD);
    app->load_sel = 0;
    short_press(app, BSP_BTN_OK);
    CHECK_EQ_INT(app->game.scene, scene);
    CHECK_EQ_INT(app->game.text_op, op);

    // 写失败：旧档保持，恢复后 2s 退避重试成功
    pq_game_start(&app->game, app->game.pack.flow, true);
    gal_app_test_set_page(app, PQ_PAGE_READER);
    short_press(app, BSP_BTN_UP);
    uint32_t newer = app->game.text_op;
    host_nvs_set_fail(HOST_NVS_FAIL_WRITE);
    gal_app_flush(app);
    CHECK(app->auto_dirty);
    host_nvs_set_fail(HOST_NVS_OK);
    tick_ms(app, 2100);
    CHECK(!app->auto_dirty);
    pq_game_start(&app->game, app->game.pack.flow, true);
    gal_app_test_set_page(app, PQ_PAGE_LOAD);
    app->load_sel = 0;
    short_press(app, BSP_BTN_OK);
    CHECK_EQ_INT(app->game.text_op, newer);
}

static void test_choice_save_visual(gal_app_t *app)
{
    host_nvs_reset();
    lv_stub_reset();
    CHECK(gal_app_test_goto_choice(app));   // AFTER STORY 97→98
    CHECK_EQ_INT(app->page, PQ_PAGE_CHOICE);
    CHECK_EQ_INT(app->game.scene, 98);
    CHECK(app->game.current_visual < app->game.pack.visual_count);
    pq_visual_t expected = app->game.pack.visuals[app->game.current_visual];
    CHECK_EQ_INT(app->bg, expected.bg);
    CHECK_EQ_INT(app->bodies[0], expected.body[0]);

    // 选第二项，存到 8 号槽
    short_press(app, BSP_BTN_DOWN);
    CHECK_EQ_INT(app->game.choice_sel, 1);
    long_press(app, BSP_BTN_OK);
    CHECK_EQ_INT(app->page, PQ_PAGE_MENU);
    short_press(app, BSP_BTN_DOWN);        // 保存进度
    short_press(app, BSP_BTN_OK);
    CHECK_EQ_INT(app->page, PQ_PAGE_SAVE);
    press_n(app, BSP_BTN_DOWN, 7);         // 存档 08
    short_press(app, BSP_BTN_OK);
    CHECK(pq_game_slot_exists(7));

    // 返回待选并确认 → scene 104
    long_press(app, BSP_BTN_OK);           // 菜单
    long_press(app, BSP_BTN_OK);           // 待选
    short_press(app, BSP_BTN_OK);
    CHECK_EQ_INT(app->game.scene, 104);

    // 再从前传入口进入实际正文 scene 3，画面与待选场景完全不同
    CHECK(gal_app_test_advance_to_scene(app, app->game.pack.side, 3));
    CHECK(app->bg != expected.bg || app->bodies[0] != expected.body[0]);

    // 真实菜单 → 读取 → 存档 08（正文页短 OK 已改为下一句，进菜单用长 OK）
    long_press(app, BSP_BTN_OK);
    short_press(app, BSP_BTN_DOWN);
    short_press(app, BSP_BTN_DOWN);        // 读取存档
    short_press(app, BSP_BTN_OK);
    CHECK_EQ_INT(app->page, PQ_PAGE_LOAD);
    press_n(app, BSP_BTN_DOWN, 8);         // 自动档 → 存档 08
    short_press(app, BSP_BTN_OK);

    // 待选与它自己的画面一起恢复
    CHECK_EQ_INT(app->page, PQ_PAGE_CHOICE);
    CHECK_EQ_INT(app->game.scene, 98);
    CHECK_EQ_INT(app->game.choice_sel, 1);
    CHECK_EQ_INT(app->bg, expected.bg);
    CHECK_EQ_INT(app->bodies[0], expected.body[0]);
    CHECK_EQ_INT(app->bodies[1], expected.body[1]);
    CHECK_EQ_INT(app->bodies[2], expected.body[2]);
    CHECK_EQ_INT(app->cutin, expected.cutin);
    CHECK_EQ_INT(app->view, expected.view);
    CHECK_EQ_INT(app->cutin_opacity, expected.opacity);
    CHECK(app->draw_error == false);
}

static void test_key_guard(gal_app_t *app)
{
    // 从待选确认进入正文
    short_press(app, BSP_BTN_OK);
    CHECK_EQ_INT(app->page, PQ_PAGE_READER);
    uint32_t before = app->shown_op;
    int before_page = app->page_index;
    // 打字中：先把这一句完全显示，便于观察"是否推进"
    short_press(app, BSP_BTN_UP);
    CHECK(app->typing == false);
    before = app->shown_op;
    before_page = app->page_index;
    // 长按结束后驱动补发的 CLICK 必须被吞掉
    send_key(app, BSP_BTN_UP, BSP_BTN_PRESS);
    send_key(app, BSP_BTN_UP, BSP_BTN_LONG);
    send_key(app, BSP_BTN_UP, BSP_BTN_RELEASE);
    send_key(app, BSP_BTN_UP, BSP_BTN_CLICK);
    CHECK_EQ_INT(app->shown_op, before);
    CHECK_EQ_INT(app->page_index, before_page);
    // 下一次独立短按不受影响
    short_press(app, BSP_BTN_UP);
    CHECK(app->shown_op != before || app->page_index > before_page);
}

static void test_gallery(gal_app_t *app)
{
    // 用真实剧本里的 CG 背景渲染一张 → 解锁该组
    uint16_t asset = pq_gallery_member(&app->game.pack, 0, 0);
    CHECK(pq_asset_image(&app->game.pack, asset) != PQ_NO_IMAGE);
    CHECK(gal_app_test_render_scene(app, asset, PQ_NO_IMAGE, PQ_NO_IMAGE, PQ_NO_IMAGE,
                                    PQ_NO_IMAGE));
    CHECK(pq_game_cg_unlocked(&app->game, 0));

    gal_app_test_goto_title(app);
    CHECK_EQ_INT(app->page, PQ_PAGE_TITLE);
    CHECK(lv_stub_find_text("GALLERY") != NULL);
    // 未通关时标题项：START/SIDE STORY/CHAPTER/LOAD/SETTINGS/GALLERY
    app->title_sel = 5;
    short_press(app, BSP_BTN_OK);
    CHECK_EQ_INT(app->page, PQ_PAGE_GALLERY);
    CHECK(lv_stub_find_containing("◆") != NULL);
    CHECK(lv_stub_find_containing("◇") != NULL);
    app->gallery_sel = 0;
    short_press(app, BSP_BTN_OK);
    CHECK_EQ_INT(app->page, PQ_PAGE_CG);
    CHECK(app->draw_error == false);
    // 双带绘制：两条 320x120 的图像控件分别贴在 y=0 / y=120，
    // 各自指向独立的 76,800 B 条带缓冲，绘制后两条都刷新。
    CHECK(app->img_art != NULL && app->img_art2 != NULL);
    if (app->img_art && app->img_art2) {
        const lv_image_dsc_t *d0 = (const lv_image_dsc_t *)app->img_art->src;
        const lv_image_dsc_t *d1 = (const lv_image_dsc_t *)app->img_art2->src;
        CHECK(d0 != NULL && d1 != NULL);
        CHECK_EQ_INT(app->img_art->y, 0);
        CHECK_EQ_INT(app->img_art2->y, PQ_BAND_H);
        if (d0 && d1) {
            CHECK_EQ_INT(d0->header.h, 120);
            CHECK_EQ_INT(d1->header.h, 120);
            CHECK_EQ_INT(d0->data_size, 76800u);
            CHECK_EQ_INT(d1->data_size, 76800u);
            CHECK(d0->data != d1->data);          // 两条独立缓冲
        }
        CHECK(app->img_art->invalidated > 0 && app->img_art2->invalidated > 0);
    }
    long_press(app, BSP_BTN_OK);
    CHECK_EQ_INT(app->page, PQ_PAGE_GALLERY);
    if (!pq_game_cg_unlocked(&app->game, 1)) {
        app->gallery_sel = 1;
        short_press(app, BSP_BTN_OK);
        CHECK_EQ_INT(app->page, PQ_PAGE_GALLERY);
        CHECK(strstr(app->notice, "尚未解锁") != NULL);
    }
}

static void test_another_view(gal_app_t *app)
{
    // 源的 another 视角：画面区左上角显示 ANOTHER VIEW 横幅
    app->bg = PQ_NO_IMAGE;
    app->bodies[0] = app->bodies[1] = app->bodies[2] = PQ_NO_IMAGE;
    app->cutin = PQ_NO_IMAGE;
    app->view = PQ_VIEW_ANOTHER;
    app->tag = 0;
    gal_app_test_set_page(app, PQ_PAGE_READER);
    CHECK(lv_stub_find_text("ANOTHER VIEW") != NULL);
    app->view = PQ_VIEW_NORMAL;
    gal_app_test_set_page(app, PQ_PAGE_READER);
    CHECK(lv_stub_find_text("ANOTHER VIEW") == NULL);
}

static void test_settings(gal_app_t *app)
{
    gal_app_test_goto_title(app);
    app->title_sel = 4;   // SETTINGS
    short_press(app, BSP_BTN_OK);
    CHECK_EQ_INT(app->page, PQ_PAGE_SETTINGS);
    CHECK(lv_stub_find_containing("文字速度") != NULL);
    CHECK(lv_stub_find_containing("自动速度") != NULL);
    CHECK(lv_stub_find_containing("标题背景") != NULL);
    // 字号切换：正文用 20px 字体重排，设置持久化
    press_n(app, BSP_BTN_DOWN, 2);
    short_press(app, BSP_BTN_OK);
    CHECK_EQ_INT(app->settings.font, 1);
    CHECK(strstr(app->notice, "字号") != NULL);
    // 未解锁的后日谈背景不能选中（第 5 项 = 梨乃）
    press_n(app, BSP_BTN_DOWN, 2);
    short_press(app, BSP_BTN_OK);
    CHECK_EQ_INT(app->settings.title_bg, 0);
    CHECK(strstr(app->notice, "解锁") != NULL);
    // 切换文字速度并持久化
    press_n(app, BSP_BTN_UP, 4);
    short_press(app, BSP_BTN_OK);
    CHECK(app->settings.speed != 1);
    pq_settings_t st;
    CHECK(pq_settings_load(&st));
    CHECK_EQ_INT(st.speed, app->settings.speed);
    long_press(app, BSP_BTN_OK);
    CHECK_EQ_INT(app->page, PQ_PAGE_TITLE);
    // 进入正文确认 20px 字体已应用到正文标签（标题光标回到 START）
    app->title_sel = 0;
    short_press(app, BSP_BTN_OK);   // START
    CHECK_EQ_INT(app->page, PQ_PAGE_READER);
    extern const lv_font_t gal_cjk_20;
    CHECK(app->lbl_text != NULL && app->lbl_text->font == &gal_cjk_20);
    gal_app_test_goto_title(app);
}


// 反复重建所有普通页面：clear_root 必须释放上一页的整棵对象树。用真实生产
// gal_app.c 驱动 LVGL 薄桩；薄桩只证明对象生命周期/数量，不证明真实 LVGL pool 足够。
static void test_page_lifecycle(gal_app_t *app)
{
    gal_app_test_goto_title(app);
    const int baseline = lv_stub_count();
    int max_objects = baseline;
    const pq_page_t pages[] = {
        PQ_PAGE_TITLE, PQ_PAGE_READER, PQ_PAGE_MENU, PQ_PAGE_SAVE, PQ_PAGE_LOAD,
        PQ_PAGE_HISTORY, PQ_PAGE_SETTINGS, PQ_PAGE_GALLERY, PQ_PAGE_ROUTES,
        PQ_PAGE_CHAPTERS, PQ_PAGE_ENDING, PQ_PAGE_ABOUT, PQ_PAGE_OVERWRITE,
        PQ_PAGE_ERROR, PQ_PAGE_TITLE,
    };
    for (int round = 0; round < 12; round++) {
        for (unsigned i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
            gal_app_test_set_page(app, pages[i]);
            const int now = lv_stub_count();
            if (now > max_objects) max_objects = now;
            CHECK(now > 0);
            CHECK(now <= 40);   // 本作普通页面对象数上界（不是 LVGL 内存证明）
        }
    }
    CHECK_EQ_INT(lv_stub_count(), baseline);
    printf("page lifecycle: baseline=%d max=%d final=%d", baseline, max_objects,
           lv_stub_count());
    putchar(10);

    // 空提示不能盖住页脚/提示；没有 notice 时必须回落到正常文案。
    app->notice_ms = 0;
    app->notice[0] = 0;
    gal_app_test_set_page(app, PQ_PAGE_TITLE);
    CHECK(lv_stub_find_text("UP/DOWN · OK") != NULL);
}

// 导航回归：标题页长 OK 必须能打开“关于/按键说明”，单击 OK 回标题。
// 修复前 PQ_PAGE_ABOUT 有构建/按键分支但全文件没有任何入口。
static void test_about_entry(gal_app_t *app)
{
    gal_app_test_goto_title(app);
    long_press(app, BSP_BTN_OK);
    CHECK_EQ_INT(app->page, PQ_PAGE_ABOUT);
    CHECK(lv_stub_find_text("关于") != NULL);
    short_press(app, BSP_BTN_OK);
    CHECK_EQ_INT(app->page, PQ_PAGE_TITLE);
}

// 导航回归：0 路线（合法但裁剪/损坏的剧本包）下按 UP/DOWN 不得整数除零。
// 修复前 ROUTES 分支直接对 route_count 取模，count==0 时 host 上 SIGFPE。
static void test_empty_routes_guard(gal_app_t *app)
{
    gal_app_test_goto_title(app);
    const uint16_t saved_routes = app->game.pack.route_count;
    app->game.pack.route_count = 0;
    gal_app_test_set_page(app, PQ_PAGE_ROUTES);
    CHECK_EQ_INT(app->page, PQ_PAGE_ROUTES);
    short_press(app, BSP_BTN_UP);
    CHECK_EQ_INT(app->page, PQ_PAGE_ROUTES);
    short_press(app, BSP_BTN_DOWN);
    CHECK_EQ_INT(app->page, PQ_PAGE_ROUTES);
    long_press(app, BSP_BTN_OK);      // 空数据仍必须可退出
    CHECK_EQ_INT(app->page, PQ_PAGE_TITLE);
    app->game.pack.route_count = saved_routes;
}

static void test_after_unlock_title(void)
{
    // 直接跑完正篇（引擎层），写全局记录；重开应用必须显示 AFTER STORY
    host_nvs_reset();
    pq_game_t seed;
    CHECK(pq_game_init(&seed));
    pq_game_start(&seed, seed.pack.flow, true);
    for (int i = 0; i < 30000; i++) {
        pq_step_t step;
        if (!pq_game_step(&seed, &step)) {
            break;
        }
        if (step.kind == PQ_STEP_END) {
            break;
        }
        if (step.kind == PQ_STEP_CHOICE) {
            pq_game_choose(&seed, 0);
        }
    }
    CHECK(pq_game_save_meta(&seed));

    lv_stub_reset();
    host_lvgl_reset_violations();
    gal_app_t *fresh = malloc(sizeof(gal_app_t));
    CHECK(gal_app_init(fresh));
    CHECK_EQ_INT(fresh->page, PQ_PAGE_TITLE);
    CHECK(lv_stub_find_text("AFTER STORY") != NULL);
    short_press(fresh, BSP_BTN_OK);   // 第一项 = AFTER STORY
    CHECK_EQ_INT(fresh->page, PQ_PAGE_READER);
    CHECK(fresh->game.scene >= fresh->game.pack.after);   // 93→94→95 是纯跳转
    free(fresh);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);   // host 回归失败（含除零崩溃）也要保留已跑用例输出
    if (argc < 3) {
        printf("usage: %s <parquet_img.bin> <parquet_scn.bin>\n", argv[0]);
        return 2;
    }
    size_t img_size = 0, scn_size = 0;
    uint8_t *img = read_file(argv[1], &img_size);
    uint8_t *scn = read_file(argv[2], &scn_size);
    CHECK(pq_host_set_pack(img, img_size, scn, scn_size));

    host_nvs_reset();
    lv_stub_reset();
    host_lvgl_reset_violations();
    gal_app_t *app = malloc(sizeof(gal_app_t));
    CHECK(gal_app_init(app));
    CHECK(host_landscape());
    test_title_and_reader(app);
    test_menu_save_load(app);
    check_portrait_bottom(app);
    check_fast_forward_no_redecode(app);
    check_auto_next_chapter(app);
    test_choice_save_restore(app);
    test_slow_typing(app);
    test_sleep_checkpoint(app);
    test_choice_save_visual(app);
    test_key_guard(app);
    test_gallery(app);
    test_another_view(app);
    test_settings(app);
    test_page_lifecycle(app);
    test_about_entry(app);
    test_empty_routes_guard(app);
    free(app);
    test_after_unlock_title();
    CHECK_EQ_INT(host_lvgl_violations(), 0);
    CHECK_EQ_INT(host_lvgl_depth(), 0);

    printf("\n%d checks, %d failures\n", g_checks, g_fail);
    free(img);
    free(scn);
    return g_fail ? 1 : 0;
}
