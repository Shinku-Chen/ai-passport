// tests/test_steins0_all_open.c —— 空进度下从真实标题 UI 进入全部 104 章节 / 106 CG 组。
//
// 源 index.ux: openRoutes/openChapters 列全部 starts,startChapter 无解锁门禁;
// galleryList 列全部 106 组,viewCg 只检查范围。这里用真实 app 按键链重放,
// 不把 unlock/endings memset 成全 1,也不进入每章全文。
#include "steins0_app.h"
#include "steins0_pack.h"
#include "bsp_button.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef STEINS0_PACK_PATH
#define STEINS0_PACK_PATH "assets/steins0/steins0_pack.bin"
#endif

extern int host_image_fail_validate;
extern void host_nvs_reset(void);
extern void host_lvgl_reset(void);
extern void host_lvgl_set_locked(int locked);
extern void host_image_reset(void);

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); ++failures; } } while (0)

static steins0_app_t s_app;
static sg0_pack_t s_pack;

static void key(uint8_t btn, uint8_t ev)
{
    steins0_key_t k = {.btn = btn, .ev = ev};
    steins0_app_key(&s_app, &k);
}

static void click(uint8_t btn)
{
    key(btn, BSP_BTN_PRESS);
    key(btn, BSP_BTN_RELEASE);
    key(btn, BSP_BTN_CLICK);
}

static void longp(uint8_t btn)
{
    key(btn, BSP_BTN_PRESS);
    key(btn, BSP_BTN_LONG);
    key(btn, BSP_BTN_RELEASE);
}

static void open_title_row(int row)
{
    s_app.title_sel = row;
    click(BSP_BTN_OK);
}

static void return_title_from_game(void)
{
    if (s_app.page == SG0_PAGE_CARD) click(BSP_BTN_OK);
    if (s_app.page == SG0_PAGE_ENDING) {
        click(BSP_BTN_OK);
        return;
    }
    CHECK(s_app.page == SG0_PAGE_GAME || s_app.page == SG0_PAGE_LIST, "chapter page %d",
          s_app.page);
    if (s_app.page == SG0_PAGE_GAME) {
        longp(BSP_BTN_OK);   // 菜单
    }
    CHECK(s_app.page == SG0_PAGE_LIST && s_app.list_kind == SG0_LIST_MENU, "chapter menu");
    for (int i = 0; i < 6 && s_app.list_sel != 6; ++i) click(BSP_BTN_DOWN);
    click(BSP_BTN_OK);       // 返回标题确认
    CHECK(s_app.page == SG0_PAGE_CONFIRM, "title confirm");
    click(BSP_BTN_UP);       // 默认“取消”,切到“返回”
    click(BSP_BTN_OK);
    CHECK(s_app.page == SG0_PAGE_TITLE, "back title");
}

static void test_all_chapters(uint32_t *starts_seen)
{
    host_nvs_reset();
    host_lvgl_reset();
    host_lvgl_set_locked(1);
    memset(&s_app, 0, sizeof(s_app));
    CHECK(steins0_app_init(&s_app, s_pack.data, s_pack.size), "init empty progress");
    host_lvgl_set_locked(1);
    CHECK(s_app.unlock.endings == 0, "empty endings %u", s_app.unlock.endings);

    open_title_row(3);       // 章节选择
    CHECK(s_app.page == SG0_PAGE_LIST && s_app.list_kind == SG0_LIST_ROUTES,
          "chapter routes page");
    CHECK(s_app.pack.sec[SG0_SEC_START].count == 104, "104 starts");
    uint32_t starts = 0;
    for (int group = 0; group < 14; ++group) {
        CHECK(s_app.list_kind == SG0_LIST_ROUTES, "routes group %d", group);
        for (int i = 0; i < group && s_app.list_sel != group; ++i) click(BSP_BTN_DOWN);
        CHECK(s_app.list_sel == group, "select route %d", group);
        click(BSP_BTN_OK);
        CHECK(s_app.page == SG0_PAGE_LIST && s_app.list_kind == SG0_LIST_STARTS,
              "starts group %d", group);
        int count = 0;
        for (uint16_t si = 0; si < SG0_MAX_STARTS; ++si) {
            sg0_start_t st;
            if (!sg0_pack_start(&s_pack, si, &st)) break;
            if (st.group == group) ++count;
        }
        CHECK(count >= 0, "start count %d", group);
        for (int i = 0; i < count; ++i) {
            for (int guard = 0; guard < 16 && s_app.list_sel != i; ++guard) {
                click(BSP_BTN_DOWN);
            }
            CHECK(s_app.list_sel == i, "start selection group %d i %d", group, i);
            click(BSP_BTN_OK);
            if (s_app.page == SG0_PAGE_CARD) click(BSP_BTN_OK);
            CHECK(s_app.game_started && s_app.state.mode == 1,
                  "start chapter group %d i %d page %d", group, i, s_app.page);
            ++starts;
            return_title_from_game();
            CHECK(s_app.page == SG0_PAGE_TITLE, "title after group %d i %d", group, i);
            // 真实标题→章节列表→该组 starts;不碰 unlock 内容。
            open_title_row(3);
            CHECK(s_app.list_kind == SG0_LIST_ROUTES, "routes reopen %d/%d", group, i);
            for (int r = 0; r < group && s_app.list_sel != group; ++r) click(BSP_BTN_DOWN);
            click(BSP_BTN_OK);
            CHECK(s_app.list_kind == SG0_LIST_STARTS, "starts reopen %d/%d", group, i);
        }
        // 下一组:starts 长按返回 routes。
        longp(BSP_BTN_OK);
        CHECK(s_app.page == SG0_PAGE_LIST && s_app.list_kind == SG0_LIST_ROUTES,
              "back routes after group %d", group);
        if (group + 1 < 14) {
            for (int r = group; r < 14 && s_app.list_sel != group + 1; ++r) {
                click(BSP_BTN_DOWN);
            }
            CHECK(s_app.list_sel == group + 1, "next route %d", group + 1);
        }
    }
    CHECK(starts == 104, "all starts entered %u", starts);
    *starts_seen = starts;
}

static void test_all_gallery(uint32_t *variants_seen)
{
    host_nvs_reset();
    host_lvgl_reset();
    host_lvgl_set_locked(1);
    memset(&s_app, 0, sizeof(s_app));
    CHECK(steins0_app_init(&s_app, s_pack.data, s_pack.size), "init empty gallery");
    host_lvgl_set_locked(1);
    open_title_row(4);       // 鉴赏室
    CHECK(s_app.page == SG0_PAGE_LIST && s_app.list_kind == SG0_LIST_GALLERY,
          "gallery page");
    const int groups = (int)sg0_pack_gallery_count(&s_pack);
    CHECK(groups == 106, "106 gallery groups (got %d)", groups);
    uint32_t variants = 0;
    for (int g = 0; g < groups; ++g) {
        CHECK(s_app.list_sel == g, "gallery select %d (got %d)", g, s_app.list_sel);
        for (int i = 0; i < SG0_LIST_ROWS; ++i) {
            if (i < groups - s_app.list_win) {
                CHECK(strstr(s_app.row_text[i], "未解锁") == NULL, "gallery row %d hidden", i);
            }
        }
        click(BSP_BTN_OK);
        CHECK(s_app.page == SG0_PAGE_CG && s_app.cg_group == g, "gallery open %d", g);
        CHECK(s_app.cg_caption[0] != 0, "gallery caption %d", g);
        sg0_gallery_group_t group;
        CHECK(sg0_pack_gallery_group(&s_pack, (uint8_t)g, &group), "group %d", g);
        for (int v = 1; v < group.count; ++v) {
            click(BSP_BTN_UP);
            CHECK(s_app.cg_variant == v, "variant group %d %d/%u", g, v,
                  (unsigned)group.count);
            ++variants;
        }
        ++variants;          // 首张
        click(BSP_BTN_OK);   // 返回 gallery list
        CHECK(s_app.page == SG0_PAGE_LIST && s_app.list_kind == SG0_LIST_GALLERY,
              "gallery back %d", g);
        if (g + 1 < groups) click(BSP_BTN_DOWN);
    }
    CHECK(variants == 198, "all 198 variants seen %u", variants);
    *variants_seen = variants;
}

static void test_chapter_end_writes_ending(void)
{
    host_nvs_reset();
    host_lvgl_reset();
    host_lvgl_set_locked(1);
    memset(&s_app, 0, sizeof(s_app));
    CHECK(steins0_app_init(&s_app, s_pack.data, s_pack.size), "init chapter end");
    host_lvgl_set_locked(1);
    CHECK(s_app.unlock.endings == 0, "empty endings");
    // 章节模式到达 n20 END:源 index.ux finishGame 写 _progress;引擎 bit5→bit3。
    const uint8_t node_index = 20;
    sg0_node_t node;
    CHECK(sg0_pack_node(&s_pack, node_index, &node), "node 20");
    uint32_t total = 0;
    for (uint8_t i = 0; i < node.chunk_count; ++i) {
        sg0_chunk_t chunk;
        CHECK(sg0_pack_chunk_at(&s_pack, (uint16_t)(node.chunk_first + i), &chunk), "chunk");
        total += chunk.frames;
    }
    CHECK(total > 0, "node 20 frames");
    s_app.state.node = node_index;
    s_app.state.cursor = total - 1;
    s_app.state.mode = 1;
    s_app.state.event_node = 0xFFu;
    s_app.game_started = true;
    s_app.page = SG0_PAGE_GAME;
    s_app.unlock_dirty = false;
    steins0_ui_show(&s_app.ui, SG0_PAGE_GAME);
    click(BSP_BTN_UP);
    CHECK(s_app.page == SG0_PAGE_ENDING, "chapter END page %d", s_app.page);
    CHECK(s_app.state.endings & (1u << 5), "state bit5");
    CHECK(s_app.state.endings & (1u << 3), "state bit3 normalized");
    CHECK(s_app.unlock.endings & (1u << 5), "unlock bit5 written");
    CHECK(s_app.unlock.endings & (1u << 3), "unlock bit3 written");
    click(BSP_BTN_OK);
    CHECK(s_app.page == SG0_PAGE_TITLE, "ending back title");
}

int main(void)
{
    FILE *fh = fopen(STEINS0_PACK_PATH, "rb");
    CHECK(fh != NULL, "open pack");
    if (!fh) return 1;
    fseek(fh, 0, SEEK_END);
    long size = ftell(fh);
    fseek(fh, 0, SEEK_SET);
    uint8_t *blob = malloc((size_t)size);
    CHECK(fread(blob, 1, (size_t)size, fh) == (size_t)size, "read pack");
    fclose(fh);
    CHECK(sg0_pack_open(&s_pack, blob, (uint32_t)size), "pack open");
    uint32_t starts = 0, variants = 0;
    test_all_chapters(&starts);
    test_all_gallery(&variants);
    test_chapter_end_writes_ending();
    free(blob);
    if (failures) {
        fprintf(stderr, "FAIL %d checks\n", failures);
        return 1;
    }
    printf("PASS all-open chapters 104 / gallery 106 (198 variants) / chapter END progress\n");
    return 0;
}
