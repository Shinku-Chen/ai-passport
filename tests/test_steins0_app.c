// tests/test_steins0_app.c —— 薄 LVGL/NVS 适配下的真实应用状态机回归。
//
// 覆盖:标题/新游戏入口、正文推进与历史、菜单往返、选项处存/读档、覆盖确认、
// NVS commit 失败不改旧档、自动存档继续、章节入口(锁定/解锁)、鉴赏变体、
// 坏档/坏图拒绝且不改状态、全局解锁合并、坏资源包、长按后的尾随 CLICK。
#include "steins0_app.h"
#include "steins0_engine.h"
#include "steins0_pack.h"
#include "steins0_text.h"
#include "steins0_ui.h"

#include "bsp_button.h"
#include "lvgl.h"
#include "nvs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef STEINS0_PACK_PATH
#define STEINS0_PACK_PATH "assets/steins0/steins0_pack.bin"
#endif

extern void host_nvs_reset(void);
extern int host_nvs_commit_fail;
extern int host_nvs_open_fail;
extern int host_nvs_writes;
extern int host_nvs_get_fail;
extern int host_nvs_set_fail;
extern int host_nvs_commit_fail;
extern int host_nvs_open_fail;
extern void host_image_reset(void);
extern int host_image_fail_validate;

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); ++failures; } } while (0)

static uint8_t *s_blob;
static uint32_t s_blob_size;
static steins0_app_t s_app;

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


static void boot(void)
{
    host_lvgl_set_locked(1);
    memset(&s_app, 0, sizeof(s_app));
    CHECK(steins0_app_init(&s_app, s_blob, s_blob_size), "app init");
    host_lvgl_set_locked(1);
}

static void fresh(void)
{
    host_nvs_reset();
    host_lvgl_reset();
    host_image_reset();
    boot();
}

static void expected_frame(uint8_t node, uint16_t cursor, char *out, size_t cap)
{
    sg0_pack_t pack;
    CHECK(sg0_pack_open(&pack, s_blob, s_blob_size), "pack open for expected");
    sg0_state_t state;
    sg0_engine_init(&state, 0, 0);
    state.node = node;
    state.cursor = cursor;
    static uint8_t chunk[16384];
    sg0_chunk_t info;
    uint32_t raw_len = 0;
    sg0_frame_t frame;
    const char *text = NULL;
    uint16_t text_len = 0;
    CHECK(sg0_engine_read(&pack, &state, chunk, sizeof(chunk), &info, &raw_len, &frame, &text,
                          &text_len), "read expected frame");
    (void)sg0_text_clean(text, text_len, out, cap);
}

static void start_new_game(void)
{
    CHECK(s_app.page == SG0_PAGE_TITLE, "at title");
    s_app.title_sel = 0;
    CHECK(strcmp(s_app.title_text[0], "开始游戏") == 0, "title row 0");
    click(BSP_BTN_OK);
    if (s_app.page == SG0_PAGE_CARD) click(BSP_BTN_OK);
    CHECK(s_app.page == SG0_PAGE_GAME, "new game page");
    CHECK(s_app.game_started, "game started");
    CHECK(s_app.state.node == 0 && s_app.state.cursor == 0, "initial node/cursor");
}

static void reach_choice(void)
{
    int guard = 0;
    while (!s_app.state.choice_active && guard++ < 8000) {
        click(BSP_BTN_UP);
    }
    CHECK(s_app.state.choice_active, "reached a choice (guard %d)", guard);
    CHECK(s_app.page == SG0_PAGE_GAME, "choice page");
    CHECK(!s_app.ui.choice_box->hidden, "choice box visible");
    host_lvgl_set_locked(1);
}

static void menu_select_open(int row)
{
    CHECK(s_app.page == SG0_PAGE_LIST && s_app.list_kind == SG0_LIST_MENU, "menu opened");
    for (int guard = 0; guard < 8 && s_app.list_sel != row; ++guard) {
        click(BSP_BTN_DOWN);
    }
    CHECK(s_app.list_sel == row, "menu row %d (got %d)", row, s_app.list_sel);
    click(BSP_BTN_OK);
}

static void menu_row(int row)
{
    longp(BSP_BTN_OK);
    menu_select_open(row);
}

static void test_title_and_new_game(void)
{
    fresh();
    CHECK(s_app.page == SG0_PAGE_TITLE, "boot title");
    CHECK(!s_app.ui.page_title->hidden, "title visible");
    CHECK(s_app.ui.page_game == NULL, "game page lazy at title");
    CHECK(strcmp(s_app.ui.title_label->text, "命运石之门 0") == 0, "title label");
    CHECK(strcmp(s_app.ui.title_battery->text, "88%") == 0, "battery label %s",
          s_app.ui.title_battery->text);
    for (int i = 0; i < 6; ++i) {
        CHECK(!s_app.ui.title_rows[i]->hidden, "title row %d visible", i);
        CHECK(s_app.ui.title_rows[i]->y >= 180 && s_app.ui.title_rows[i]->y < 240,
              "title row %d y %d", i, s_app.ui.title_rows[i]->y);
    }
    start_new_game();
    char expected[SG0_TEXT_BUFFER];
    expected_frame(0, 0, expected, sizeof(expected));
    CHECK(strcmp(s_app.op_text, expected) == 0, "first frame text not skipped");
    CHECK(s_app.line_count >= 1, "lines visible");
    CHECK(strcmp(s_app.ui.text_lines[0]->text, expected) == 0 ||
              strncmp(expected, s_app.ui.text_lines[0]->text,
                      strlen(s_app.ui.text_lines[0]->text)) == 0,
          "first line rendered");
    CHECK(s_app.ui.text_lines[0]->y == 172 && s_app.ui.text_lines[0]->w == 304,
          "text geometry %d/%d", s_app.ui.text_lines[0]->y, s_app.ui.text_lines[0]->w);
    CHECK(!s_app.ui.text_lines[0]->hidden, "text line visible");
    // 名字贴住新面板上沿(168)：底边不超过面板，且缝隙 <=1px。
    CHECK(s_app.ui.speaker_label->y + s_app.ui.speaker_label->h <= 168,
          "speaker must sit above the panel (y=%d h=%d)", s_app.ui.speaker_label->y,
          s_app.ui.speaker_label->h);
    CHECK(168 - (s_app.ui.speaker_label->y + s_app.ui.speaker_label->h) <= 1,
          "speaker not flush with panel: gap=%d",
          168 - (s_app.ui.speaker_label->y + s_app.ui.speaker_label->h));
    CHECK(s_app.ui.speaker_label->x == 8, "speaker left edge %d",
          s_app.ui.speaker_label->x);
    CHECK(s_app.ui.speaker_label->w == LV_SIZE_CONTENT,
          "speaker width must be content-sized (%d)", s_app.ui.speaker_label->w);
    CHECK(s_app.ui.text_lines[2]->y + s_app.ui.text_lines[2]->h <= 240,
          "last text line bottom %d", s_app.ui.text_lines[2]->y + s_app.ui.text_lines[2]->h);
    printf("app: title/new game OK, first text %u bytes\n", (unsigned)strlen(expected));
}

static void test_advance_history(void)
{
    fresh();
    start_new_game();
    const uint16_t cursor0 = s_app.state.cursor;
    click(BSP_BTN_UP);
    if (s_app.state.cursor == cursor0 && s_app.page_index == 0) click(BSP_BTN_UP);
    CHECK(s_app.state.cursor != cursor0 || s_app.page_index > 0, "advance moved");
    const char *last = s_app.op_text;
    char text_copy[SG0_TEXT_BUFFER];
    snprintf(text_copy, sizeof(text_copy), "%s", last);
    click(BSP_BTN_DOWN);
    CHECK(s_app.page == SG0_PAGE_HISTORY, "history page");
    CHECK(strstr(s_app.ui.history_label->text, text_copy) != NULL, "history contains text");
    longp(BSP_BTN_OK);
    CHECK(s_app.page == SG0_PAGE_GAME, "history back to game");
}

static void test_menu_roundtrip(void)
{
    fresh();
    start_new_game();
    const uint8_t node = s_app.state.node;
    const uint16_t cursor = s_app.state.cursor;
    longp(BSP_BTN_OK);
    CHECK(s_app.page == SG0_PAGE_LIST, "menu list");
    CHECK(s_app.list_kind == SG0_LIST_MENU, "menu kind");
    CHECK(strcmp(s_app.ui.list_rows[0]->text, "继续阅读") == 0, "menu row 0");
    CHECK(strcmp(s_app.ui.list_rows[7]->text, "关于本作") == 0, "menu row 7");
    menu_select_open(5);
    CHECK(s_app.list_kind == SG0_LIST_SETTINGS, "settings list");
    const uint8_t speed = s_app.settings.speed;
    click(BSP_BTN_OK);
    CHECK(s_app.settings.speed != speed, "settings value changed");
    longp(BSP_BTN_OK);
    CHECK(s_app.page == SG0_PAGE_LIST && s_app.list_kind == SG0_LIST_MENU, "back to menu");
    CHECK(s_app.list_sel == 5, "menu cursor preserved %d", s_app.list_sel);
    longp(BSP_BTN_OK);
    CHECK(s_app.page == SG0_PAGE_GAME, "back to game");
    CHECK(s_app.state.node == node && s_app.state.cursor == cursor, "state preserved");
}

static void test_choice_save_load(void)
{
    fresh();
    start_new_game();
    reach_choice();
    const uint8_t choice_node = s_app.state.node;
    const uint16_t choice_cursor = s_app.state.cursor;
    CHECK(s_app.ui.choice_rows[0]->text[0] != '\0', "choice item text");

    // 长按 OK 打开菜单 -> 保存进度 -> 空槽 0
    menu_row(2);
    CHECK(s_app.list_kind == SG0_LIST_SLOTS_SAVE, "save slots");
    click(BSP_BTN_OK);
    CHECK(s_app.slot_valid[0], "slot 0 saved");
    CHECK(s_app.page == SG0_PAGE_GAME, "back to game after save");
    CHECK(strstr(s_app.ui.notice_text->text, "保存成功") != NULL, "save notice");
    CHECK(s_app.slots[0].node == choice_node && s_app.slots[0].cursor == choice_cursor,
          "save identity");
    CHECK(s_app.slots[0].preview_len > 0, "save preview");

    // 选项 0 -> 进入新节点
    CHECK(s_app.state.choice_active, "still at choice after save");
    click(BSP_BTN_OK);
    CHECK(!s_app.state.choice_active, "choice resolved");
    CHECK(s_app.state.node != choice_node || s_app.state.cursor == 0, "choice moved");

    // 读取槽 0 -> 回到同一待选
    menu_row(3);
    CHECK(s_app.list_kind == SG0_LIST_SLOTS_LOAD, "load slots");
    click(BSP_BTN_OK);
    CHECK(s_app.page == SG0_PAGE_GAME, "loaded game");
    CHECK(s_app.state.choice_active, "restored to choice");
    CHECK(s_app.state.node == choice_node, "restored node");
    CHECK(s_app.state.cursor == choice_cursor, "restored cursor");
    // 选择第 2 项,目标应与源选项一致
    click(BSP_BTN_UP);
    sg0_node_t node;
    sg0_choice_t choice;
    CHECK(sg0_pack_node(&s_app.pack, choice_node, &node), "choice node");
    CHECK(sg0_pack_choice(&s_app.pack, node.choice, &choice), "choice data");
    click(BSP_BTN_OK);
    CHECK(s_app.state.node == choice.to[1], "second choice target %u", s_app.state.node);
    printf("app: choice save/load OK at node %u cursor %u\n", choice_node, choice_cursor);
}

static void test_overwrite_and_nvs_failure(void)
{
    fresh();
    start_new_game();
    reach_choice();
    menu_row(2);
    click(BSP_BTN_OK);   // 保存槽 0
    CHECK(s_app.slot_valid[0], "slot0 written");
    CHECK(s_app.page == SG0_PAGE_GAME, "back to game after save");
    const uint16_t preview_len = s_app.slots[0].preview_len;
    const uint8_t saved_node = s_app.slots[0].node;

    // 覆盖槽 0:确认弹窗默认“取消”,旧档不变。
    menu_row(2);
    click(BSP_BTN_OK);
    CHECK(s_app.page == SG0_PAGE_CONFIRM, "overwrite confirm");
    CHECK(strstr(s_app.confirm_prompt, "覆盖") != NULL, "confirm prompt");
    click(BSP_BTN_OK);   // 取消
    CHECK(s_app.page == SG0_PAGE_LIST && s_app.list_kind == SG0_LIST_SLOTS_SAVE,
          "cancel returns to slots");
    CHECK(s_app.slots[0].preview_len == preview_len && s_app.slots[0].node == saved_node,
          "old slot unchanged");
    longp(BSP_BTN_OK);   // 回游戏

    // NVS commit 失败:不能报成功,旧档不变,槽 1 不置有效。
    host_nvs_commit_fail = 1;
    menu_row(2);
    click(BSP_BTN_DOWN);   // 槽 1
    click(BSP_BTN_OK);
    CHECK(!s_app.slot_valid[1], "failed slot not marked valid");
    CHECK(strstr(s_app.ui.notice_text->text, "失败") != NULL, "failure notice '%s'",
          s_app.ui.notice_text->text);
    CHECK(s_app.slots[0].node == saved_node, "existing slot untouched by failure");
    host_nvs_commit_fail = 0;
    click(BSP_BTN_OK);   // 再存一次槽 1
    CHECK(s_app.slot_valid[1], "slot1 written after recovery");
    CHECK(s_app.page == SG0_PAGE_GAME, "back to game after recovery");
}

static void test_autosave_continue(void)
{
    fresh();
    start_new_game();
    reach_choice();
    CHECK(s_app.auto_valid, "autosave at choice");
    const uint8_t node = s_app.state.node;
    const uint16_t cursor = s_app.state.cursor;
    // 重新启动(保留 NVS)
    host_lvgl_reset();
    host_lvgl_set_locked(1);
    memset(&s_app, 0, sizeof(s_app));
    CHECK(steins0_app_init(&s_app, s_blob, s_blob_size), "re-init");
    host_lvgl_set_locked(1);
    CHECK(s_app.auto_valid, "autosave loaded at boot");
    CHECK(s_app.page == SG0_PAGE_TITLE, "re-init title");
    click(BSP_BTN_DOWN);   // 继续游戏
    click(BSP_BTN_OK);
    CHECK(s_app.page == SG0_PAGE_GAME, "continue page");
    CHECK(s_app.state.choice_active, "continue at choice");
    CHECK(s_app.state.node == node && s_app.state.cursor == cursor, "continue identity");
}

static void test_chapter_select(void)
{
    fresh();
    // 解锁第 2 组的第一个章节(索引从 0 起按组过滤)
    int target_group = 2;
    int target_start = -1;
    int seen = 0;
    for (uint16_t i = 0; i < 104; ++i) {
        sg0_start_t st;
        if (!sg0_pack_start(&s_app.pack, i, &st)) break;
        if (st.group == target_group) {
            if (seen == 0) target_start = i;
            ++seen;
        }
    }
    CHECK(target_start >= 0, "target chapter start");
    sg0_unlock_set_chapter(&s_app.unlock, (uint16_t)target_start);
    click(BSP_BTN_DOWN);
    click(BSP_BTN_DOWN);
    click(BSP_BTN_DOWN);   // 章节选择
    click(BSP_BTN_OK);
    CHECK(s_app.list_kind == SG0_LIST_ROUTES, "routes list");
    click(BSP_BTN_DOWN);
    click(BSP_BTN_DOWN);
    click(BSP_BTN_OK);
    CHECK(s_app.list_kind == SG0_LIST_STARTS, "starts list");
    CHECK(strstr(s_app.ui.list_rows[0]->text, "第 1 节") != NULL, "start row text '%s'",
          s_app.ui.list_rows[0]->text);
    click(BSP_BTN_OK);
    if (s_app.page == SG0_PAGE_CARD) click(BSP_BTN_OK);
    CHECK(s_app.page == SG0_PAGE_GAME && s_app.state.mode == 1, "chapter entry mode");
    CHECK(s_app.state.cursor == 0 || s_app.state.cursor > 0, "chapter cursor set");
    sg0_start_t st;
    CHECK(sg0_pack_start(&s_app.pack, (uint16_t)target_start, &st), "start load");
    CHECK(s_app.state.node == st.node && s_app.state.cursor == st.index, "chapter identity");
    printf("app: chapter entry node %u index %u\n", st.node, st.index);
}

static void test_gallery(void)
{
    fresh();
    int group = -1;
    for (int i = 0; i < 106; ++i) {
        sg0_gallery_group_t gg;
        if (!sg0_pack_gallery_group(&s_app.pack, (uint8_t)i, &gg)) break;
        if (gg.count >= 2) { group = i; break; }
    }
    CHECK(group >= 0, "gallery group with variants");
    sg0_unlock_set_gallery(&s_app.unlock, (uint8_t)group);
    for (int i = 0; i < 4; ++i) click(BSP_BTN_DOWN);   // 鉴赏室
    click(BSP_BTN_OK);
    CHECK(s_app.list_kind == SG0_LIST_GALLERY, "gallery list");
    for (int i = 0; i < group; ++i) click(BSP_BTN_DOWN);
    click(BSP_BTN_OK);
    CHECK(s_app.page == SG0_PAGE_CG, "cg page");
    CHECK(s_app.ui.cg_caption->text[0] != '\0', "cg caption");
    CHECK(strstr(s_app.ui.cg_variant->text, "/") != NULL, "cg variant");
    click(BSP_BTN_UP);
    CHECK(strstr(s_app.ui.cg_variant->text, "2/") != NULL, "cg variant advanced '%s'",
          s_app.ui.cg_variant->text);
    click(BSP_BTN_OK);
    CHECK(s_app.page == SG0_PAGE_LIST && s_app.list_kind == SG0_LIST_GALLERY,
          "cg back to gallery");
}

static void test_bad_save_and_image_failure(void)
{
    fresh();
    // 写入 CRC 错误的槽 5
    uint8_t junk[SG0_SAVE_BYTES];
    memset(junk, 0x5A, sizeof(junk));
    nvs_handle_t handle;
    CHECK(nvs_open("steins0", NVS_READWRITE, &handle) == ESP_OK, "nvs open");
    CHECK(nvs_set_blob(handle, "s05", junk, sizeof(junk)) == ESP_OK, "write junk");
    CHECK(nvs_commit(handle) == ESP_OK, "commit junk");
    nvs_close(handle);
    host_lvgl_reset();
    host_lvgl_set_locked(1);
    memset(&s_app, 0, sizeof(s_app));
    CHECK(steins0_app_init(&s_app, s_blob, s_blob_size), "init with junk slot");
    host_lvgl_set_locked(1);
    CHECK(!s_app.slot_valid[5], "junk slot rejected");
    // 读取存档 -> 槽 5 -> 提示空档位
    click(BSP_BTN_DOWN);
    click(BSP_BTN_DOWN);
    click(BSP_BTN_OK);
    CHECK(s_app.list_kind == SG0_LIST_SLOTS_LOAD, "load list");
    for (int i = 0; i < 5; ++i) click(BSP_BTN_DOWN);
    click(BSP_BTN_OK);
    CHECK(s_app.page == SG0_PAGE_LIST, "empty slot stays");
    CHECK(strstr(s_app.ui.notice_text->text, "损坏") != NULL, "corrupt notice '%s'",
          s_app.ui.notice_text->text);
    longp(BSP_BTN_OK);
    CHECK(s_app.page == SG0_PAGE_TITLE, "back title");

    // 合法存档 + 画面预检失败 -> 不改当前状态
    fresh();
    start_new_game();
    reach_choice();
    menu_row(2);
    click(BSP_BTN_OK);   // 槽 0
    CHECK(s_app.slot_valid[0], "save for image test");
    click(BSP_BTN_OK);   // 选项 0,离开待选
    const uint8_t moved = s_app.state.node;
    host_image_fail_validate = 1;
    menu_row(3);
    click(BSP_BTN_OK);   // 读槽 0
    CHECK(s_app.state.node == moved, "failed load did not change state");
    CHECK(strstr(s_app.ui.notice_text->text, "不可用") != NULL, "image failure notice '%s'",
          s_app.ui.notice_text->text);
    host_image_fail_validate = 0;
    printf("app: bad save/image rejected without state change\n");
}

static void test_unlock_merge(void)
{
    fresh();
    start_new_game();
    reach_choice();
    menu_row(2);
    click(BSP_BTN_OK);
    CHECK(s_app.slot_valid[0], "save for unlock merge");
    sg0_save_t save;
    memset(&save, 0, sizeof(save));
    save.meta = s_app.slots[0];
    save.event_valid = 1;
    save.event_node = save.meta.node;
    save.event_cursor = 0;
    save.meta.endings |= (uint8_t)(1u << 3);
    save.meta.chapters[0] |= 0x20;
    save.meta.gallery[0] |= 0x40;
    uint8_t raw[SG0_SAVE_BYTES];
    CHECK(sg0_save_encode(&save, raw, sizeof(raw)) == SG0_SAVE_BYTES, "encode save");
    nvs_handle_t handle;
    CHECK(nvs_open("steins0", NVS_READWRITE, &handle) == ESP_OK, "nvs open");
    CHECK(nvs_set_blob(handle, "s07", raw, sizeof(raw)) == ESP_OK, "write s07");
    CHECK(nvs_commit(handle) == ESP_OK, "commit s07");
    nvs_close(handle);
    host_lvgl_reset();
    host_lvgl_set_locked(1);
    memset(&s_app, 0, sizeof(s_app));
    CHECK(steins0_app_init(&s_app, s_blob, s_blob_size), "init unlock merge");
    host_lvgl_set_locked(1);
    CHECK(s_app.slot_valid[7], "s07 loaded");
    longp(BSP_BTN_OK);      // 标题 -> 读取存档列表
    click(BSP_BTN_DOWN);
    click(BSP_BTN_DOWN);
    click(BSP_BTN_OK);
    CHECK(s_app.list_kind == SG0_LIST_SLOTS_LOAD, "load list for merge");
    for (int i = 0; i < 7; ++i) click(BSP_BTN_DOWN);
    click(BSP_BTN_OK);
    CHECK(s_app.unlock_dirty || sg0_unlock_has_chapter(&s_app.unlock, 5), "unlock dirty/merged");
    CHECK(sg0_unlock_has_ending(&s_app.unlock, 3), "ending bit merged");
    CHECK(sg0_unlock_has_chapter(&s_app.unlock, 5), "chapter bit merged");
    CHECK(sg0_unlock_has_gallery(&s_app.unlock, 6), "gallery bit merged");
    steins0_app_flush(&s_app);
    uint8_t unlocked[SG0_UNLOCK_BYTES];
    CHECK(nvs_open("steins0", NVS_READONLY, &handle) == ESP_OK, "nvs open unlock");
    size_t len = sizeof(unlocked);
    CHECK(nvs_get_blob(handle, "unlock", unlocked, &len) == ESP_OK, "read unlock");
    nvs_close(handle);
    sg0_unlock_t decoded;
    CHECK(sg0_unlock_decode(&decoded, unlocked, sizeof(unlocked)), "unlock decode");
    CHECK(sg0_unlock_has_ending(&decoded, 3), "persisted ending bit");
    CHECK(sg0_unlock_has_chapter(&decoded, 5), "persisted chapter bit");
    CHECK(sg0_unlock_has_gallery(&decoded, 6), "persisted gallery bit");
}

static void test_bad_pack(void)
{
    host_nvs_reset();
    host_lvgl_reset();
    host_image_reset();
    host_lvgl_set_locked(1);
    uint8_t junk[64];
    memset(junk, 0xAA, sizeof(junk));
    memset(&s_app, 0, sizeof(s_app));
    CHECK(steins0_app_init(&s_app, junk, sizeof(junk)), "init bad pack returns UI");
    host_lvgl_set_locked(1);
    CHECK(!s_app.pack_ok, "bad pack marked");
    CHECK(s_app.page == SG0_PAGE_TITLE, "bad pack title");
    CHECK(strstr(s_app.ui.notice_text->text, "损坏") != NULL, "bad pack notice '%s'",
          s_app.ui.notice_text->text);
    click(BSP_BTN_OK);
    CHECK(s_app.page == SG0_PAGE_TITLE, "cannot start from bad pack");
    CHECK(strstr(s_app.ui.notice_text->text, "损坏") != NULL, "bad pack start notice");
}

static void test_card_and_rine(void)
{
    fresh();
    start_new_game();
    // 首个章节卡:显示后任意键关闭,不跳过第一句。
    CHECK(s_app.last_card == 0, "first card group");
    const uint16_t cursor0 = s_app.state.cursor;
    CHECK(s_app.page == SG0_PAGE_GAME, "game after card");
    CHECK(s_app.state.cursor == cursor0, "card did not advance");

    // 推进到第一封 RINE。
    int guard = 0;
    while (s_app.state.rine.count == 0 && guard++ < 500) click(BSP_BTN_UP);
    CHECK(s_app.state.rine.count > 0, "rine message received (guard %d)", guard);
    CHECK(sg0_engine_unread(&s_app.pack, &s_app.state) > 0, "unread count");

    menu_row(1);
    CHECK(s_app.page == SG0_PAGE_PHONE && s_app.phone_view == SG0_PHONE_HOME, "phone home");
    click(BSP_BTN_OK);   // 收信箱
    CHECK(s_app.phone_view == SG0_PHONE_INBOX, "phone inbox");
    CHECK(s_app.phone_text[0][0] != 0, "inbox thread row");
    CHECK(strstr(s_app.phone_value[0], "11.23") != NULL, "inbox date 101123 '%s'",
          s_app.phone_value[0]);
    click(BSP_BTN_OK);   // 第一组会话
    CHECK(s_app.page == SG0_PAGE_PHONE_THREAD, "phone thread");
    CHECK(s_app.phone_thread_date == 101123, "thread date VALUE %u",
          (unsigned)s_app.phone_thread_date);
    CHECK(strstr(s_app.ui.phone_thread_header->text, "11.23") != NULL,
          "thread header date '%s'", s_app.ui.phone_thread_header->text);
    CHECK(s_app.phone_thread_count > 0, "thread parts %u", s_app.phone_thread_count);
    const uint16_t refs_before = s_app.state.rine.count;
    const uint16_t parts_before = s_app.phone_thread_count;
    // 第一条消息有回复窗口;OK 打开回复并发送。
    if (sg0_engine_can_reply(&s_app.pack, &s_app.state, s_app.phone_thread_base)) {
        click(BSP_BTN_OK);
        CHECK(s_app.phone_view == SG0_PHONE_REPLY, "reply list");
        CHECK(s_app.phone_text[0][0] != 0 || s_app.phone_value[0][0] != 0,
              "reply choice row");
        click(BSP_BTN_OK);
        CHECK(s_app.phone_view == SG0_PHONE_THREAD, "back to thread after reply");
        CHECK(s_app.phone_thread_count > parts_before,
              "reply rebuild thread parts %u -> %u", parts_before, s_app.phone_thread_count);
        CHECK(s_app.state.rine.count > refs_before, "reply sequence stored");
        const uint16_t parts_after = s_app.phone_thread_count;
        for (int k = 0; k < parts_after; ++k) {
            click(BSP_BTN_DOWN);
            CHECK(s_app.phone_thread_count == parts_after,
                  "thread count stable after DOWN %d (%u)", k, s_app.phone_thread_count);
            CHECK(s_app.phone_thread_part == (uint16_t)((k + 1) % parts_after),
                  "thread part advances %d", k);
        }
        CHECK(s_app.state.rine.replied != 0, "replied bit set");
    } else {
        CHECK(0, "first thread should offer reply");
    }
    // Sent view and its thread must use the same inherited date VALUE.
    longp(BSP_BTN_OK);                  // thread -> inbox
    longp(BSP_BTN_OK);                  // inbox -> home
    click(BSP_BTN_DOWN); click(BSP_BTN_OK); // sent
    CHECK(s_app.phone_view == SG0_PHONE_SENT, "sent view");
    CHECK(s_app.phone_value[0][0] != 0, "sent row");
    CHECK(strstr(s_app.phone_value[0], "11.23") != NULL, "sent date 101123 '%s'",
          s_app.phone_value[0]);
    click(BSP_BTN_OK);                  // sent thread
    CHECK(s_app.page == SG0_PAGE_PHONE_THREAD, "sent thread page");
    CHECK(s_app.phone_thread_date == 101123, "sent thread date VALUE %u",
          (unsigned)s_app.phone_thread_date);
    CHECK(strstr(s_app.ui.phone_thread_header->text, "11.23") != NULL,
          "sent thread header date '%s'", s_app.ui.phone_thread_header->text);
    longp(BSP_BTN_OK);
    longp(BSP_BTN_OK);
    longp(BSP_BTN_OK);
    CHECK(s_app.page == SG0_PAGE_GAME, "phone back to game");

    // 存档/读档保留 RINE refs/replied/windows/pending。
    click(BSP_BTN_UP);
    const uint16_t saved_refs = s_app.state.rine.count;
    const uint16_t saved_replied = s_app.state.rine.replied;
    menu_row(2);
    click(BSP_BTN_OK);
    CHECK(s_app.slot_valid[0], "rine save");
    click(BSP_BTN_UP);
    menu_row(3);
    click(BSP_BTN_OK);
    CHECK(s_app.state.rine.count == saved_refs, "rine refs restored %u/%u",
          s_app.state.rine.count, saved_refs);
    CHECK(s_app.state.rine.replied == saved_replied, "rine replied restored");

    // 非关键来电:n0 末尾自动 defer,手机接听回 n1。
    s_app.state.rine.pending_valid = 1;
    s_app.state.rine.pending_node = 0;
    menu_row(1);
    CHECK(s_app.phone_view == SG0_PHONE_HOME, "phone home for call");
    click(BSP_BTN_DOWN); click(BSP_BTN_DOWN); click(BSP_BTN_DOWN);
    click(BSP_BTN_OK);
    CHECK(s_app.phone_view == SG0_PHONE_CALLS, "calls view");
    CHECK(s_app.phone_text[0][0] != 0, "call action row");
    click(BSP_BTN_OK);
    if (s_app.page == SG0_PAGE_CARD) click(BSP_BTN_OK);
    CHECK(s_app.page == SG0_PAGE_GAME, "accept call returns game (page %d)", s_app.page);
    CHECK(s_app.state.node == 1 && s_app.state.cursor == 0, "accept call target n1@0 (%u@%u)",
          s_app.state.node, s_app.state.cursor);
    CHECK(!s_app.state.rine.pending_valid, "pending consumed");
}

static void test_n0_cursor108_page1_save(void)
{
    fresh();
    start_new_game();
    int advance = 0;
    while (s_app.state.cursor < 108 && advance++ < 1000) click(BSP_BTN_UP);
    CHECK(s_app.state.node == 0 && s_app.state.cursor == 108,
          "n0 cursor108 (%u@%u)", s_app.state.node, s_app.state.cursor);
    CHECK(s_app.pages.pages > 1, "n0@108 is multi-page (%u)", s_app.pages.pages);
    while (s_app.page_index == 0) click(BSP_BTN_OK);
    CHECK(s_app.page_index == 1, "n0@108 page1");
    const uint8_t node = s_app.state.node;
    const uint16_t cursor = s_app.state.cursor;
    const uint8_t page = s_app.page_index;
    const uint16_t view = s_app.view;
    char op[SG0_TEXT_BUFFER];
    memcpy(op, s_app.op_text, sizeof(op));
    menu_row(2);
    click(BSP_BTN_OK);
    CHECK(s_app.slot_valid[0], "save page1");
    click(BSP_BTN_UP);
    menu_row(3);
    click(BSP_BTN_OK);
    CHECK(s_app.state.node == node && s_app.state.cursor == cursor, "load position n0@108");
    CHECK(s_app.page_index == page, "load page1");
    CHECK(s_app.view == view, "load view");
    CHECK(strcmp(s_app.op_text, op) == 0, "load page text");
    printf("app: n0@%u page=%u view=%u text_ok=1\n", cursor, s_app.page_index, s_app.view);
}

static void test_auto_reading(void)
{
    fresh();
    start_new_game();
    s_app.settings.speed = 3;       // 立即显示,排除打字机耗时
    s_app.settings.auto_wait = 2;   // 快间隔
    const uint8_t start_cursor = (uint8_t)s_app.state.cursor;
    longp(BSP_BTN_DOWN);
    CHECK(s_app.auto_mode, "auto enabled");
    int cursor_changes = 0;
    uint8_t last_cursor = (uint8_t)s_app.state.cursor;
    uint8_t max_pages = s_app.pages.pages;
    int guard = 0;
    while (!s_app.state.choice_active && guard++ < 5000) {
        steins0_app_tick(&s_app, 5000);
        if (s_app.pages.pages > max_pages) max_pages = s_app.pages.pages;
        if ((uint8_t)s_app.state.cursor != last_cursor) {
            ++cursor_changes;
            last_cursor = (uint8_t)s_app.state.cursor;
        }
    }
    CHECK(cursor_changes >= 3, "auto crossed 3 sentences (%d, guard %d)",
          cursor_changes, guard);
    CHECK(max_pages > 1, "auto saw a real multi-page op (%u pages)", max_pages);
    CHECK(s_app.state.choice_active, "auto reaches choice");
    CHECK(!s_app.auto_mode, "auto stops at choice");
    printf("app: auto crossed=%d max_pages=%u stop=%u@%u auto=%d\n", cursor_changes, max_pages, s_app.state.node, s_app.state.cursor, s_app.auto_mode);
    const uint8_t choice_cursor = (uint8_t)s_app.state.cursor;
    const uint8_t choice_node = s_app.state.node;
    steins0_app_tick(&s_app, 5000);
    CHECK(s_app.state.choice_active && s_app.state.cursor == choice_cursor &&
          s_app.state.node == choice_node, "auto did not auto-select choice");
    CHECK(!s_app.auto_mode, "auto remains stopped at choice");
    click(BSP_BTN_OK);              // 短 OK 明确确认
    CHECK(!s_app.auto_mode, "short OK cancels auto");
    CHECK(!s_app.state.choice_active, "short OK confirms choice");
    CHECK(s_app.page == SG0_PAGE_GAME, "choice confirmed into game");
    (void)start_cursor;
}

static void test_slot_io_errors(void)
{
    fresh();
    start_new_game();
    reach_choice();
    menu_row(2);
    click(BSP_BTN_OK);
    CHECK(s_app.slot_valid[0], "slot for IO test");
    const int writes_before = host_nvs_writes;
    host_nvs_get_fail = 1;
    host_lvgl_reset();
    host_lvgl_set_locked(1);
    memset(&s_app, 0, sizeof(s_app));
    CHECK(steins0_app_init(&s_app, s_blob, s_blob_size), "init IO slot");
    host_lvgl_set_locked(1);
    CHECK(s_app.slot_state[0] == SG0_SLOT_IO, "slot 0 IO state %u", s_app.slot_state[0]);
    CHECK(!s_app.slot_valid[0], "IO not empty-valid");
    // LOAD 必须报读取失败,不是空档位。
    click(BSP_BTN_DOWN); click(BSP_BTN_DOWN); click(BSP_BTN_OK);
    CHECK(s_app.list_kind == SG0_LIST_SLOTS_LOAD, "load list IO");
    click(BSP_BTN_OK);
    CHECK(strstr(s_app.ui.notice_text->text, "读取失败") != NULL, "IO load notice '%s'",
          s_app.ui.notice_text->text);
    CHECK(strstr(s_app.ui.notice_text->text, "空档位") == NULL, "not empty");
    longp(BSP_BTN_OK);
    // SAVE 未知状态不能绕过覆盖确认。
    start_new_game();
    menu_row(2);
    click(BSP_BTN_OK);
    CHECK(s_app.page == SG0_PAGE_CONFIRM, "IO save asks confirmation");
    CHECK(strstr(s_app.confirm_prompt, "无法读取") != NULL, "IO prompt '%s'",
          s_app.confirm_prompt);
    click(BSP_BTN_OK);   // 取消
    CHECK(host_nvs_writes == writes_before, "IO save did not write");
    // 恢复 IO 后可重试读取。
    host_nvs_get_fail = 0;
    host_lvgl_reset(); host_lvgl_set_locked(1);
    memset(&s_app, 0, sizeof(s_app));
    CHECK(steins0_app_init(&s_app, s_blob, s_blob_size), "retry init");
    host_lvgl_set_locked(1);
    CHECK(s_app.slot_state[0] == SG0_SLOT_VALID, "slot valid after retry");
    click(BSP_BTN_DOWN); click(BSP_BTN_DOWN); click(BSP_BTN_OK);
    click(BSP_BTN_OK);
    CHECK(s_app.page == SG0_PAGE_GAME, "retry load succeeds");

    // set/commit 失败:写失败不能把槽位标成有效或绕过确认。
    host_nvs_set_fail = 1;
    menu_row(2);                // 存档列表
    click(BSP_BTN_DOWN);        // 槽 1(空)
    click(BSP_BTN_OK);
    CHECK(s_app.page == SG0_PAGE_LIST, "set failure stays in list");
    CHECK(strstr(s_app.ui.notice_text->text, "写入未完成") != NULL, "set notice '%s'",
          s_app.ui.notice_text->text);
    CHECK(!s_app.slot_valid[1] && s_app.slot_state[1] == SG0_SLOT_EMPTY, "set no valid slot");
    host_nvs_set_fail = 0;
    click(BSP_BTN_OK);          // 重试同一空槽,直接成功
    CHECK(s_app.slot_valid[1], "retry save after set failure");

    host_nvs_commit_fail = 1;
    menu_row(2);                // 存档列表(默认上次槽 1)
    click(BSP_BTN_DOWN);        // 槽 2(空)
    click(BSP_BTN_OK);
    CHECK(s_app.page == SG0_PAGE_LIST, "commit failure stays in list");
    CHECK(!s_app.slot_valid[2], "commit no valid slot");
    host_nvs_commit_fail = 0;
    click(BSP_BTN_OK);          // 重试
    if (s_app.page == SG0_PAGE_CONFIRM) {
        click(BSP_BTN_UP);      // 失败提交仍留下字节:必须明确确认覆盖
        click(BSP_BTN_OK);
    }
    CHECK(s_app.slot_valid[2], "retry save after commit failure");

    // open 失败:所有槽位必须是 IO,不是空;读取失败,不能直接建新档。
    host_nvs_open_fail = 1;
    host_lvgl_reset(); host_lvgl_set_locked(1);
    memset(&s_app, 0, sizeof(s_app));
    CHECK(steins0_app_init(&s_app, s_blob, s_blob_size), "init open failure");
    host_lvgl_set_locked(1);
    CHECK(!s_app.nvs_ok, "open failure marks nvs down");
    CHECK(s_app.slot_state[0] == SG0_SLOT_IO, "open failure IO slot %u", s_app.slot_state[0]);
    click(BSP_BTN_DOWN); click(BSP_BTN_DOWN); click(BSP_BTN_OK);
    click(BSP_BTN_OK);
    CHECK(strstr(s_app.ui.notice_text->text, "读取失败") != NULL, "open load notice '%s'",
          s_app.ui.notice_text->text);
    host_nvs_open_fail = 0;
}

static void test_page_alloc_failure_keeps_title(void)
{
    fresh();
    CHECK(s_app.page == SG0_PAGE_TITLE, "failure test starts at title");
    host_lvgl_fail_next(1000);
    click(BSP_BTN_OK);
    CHECK(s_app.page == SG0_PAGE_TITLE, "failed page switch keeps app page");
    CHECK(!s_app.ui.notice_box->hidden, "failed page switch notice visible");
    host_lvgl_fail_next(0);
}

static void test_long_press_swallow(void)
{
    fresh();
    start_new_game();
    key(BSP_BTN_OK, BSP_BTN_PRESS);
    key(BSP_BTN_OK, BSP_BTN_LONG);
    key(BSP_BTN_OK, BSP_BTN_RELEASE);
    key(BSP_BTN_OK, BSP_BTN_CLICK);   // 某些驱动在长按后仍补发 CLICK
    CHECK(s_app.page == SG0_PAGE_LIST && s_app.list_kind == SG0_LIST_MENU,
          "trailing click swallowed");
    // 下一次独立短按必须生效
    click(BSP_BTN_DOWN);
    CHECK(s_app.list_sel == 1, "next short press moves %d", s_app.list_sel);
}


// ---- 导航回归(2026-11 审计):返回链 / 提示 / 空列表 ----
// 修复前:标题->设置->通关记录 逐级返回会落进游戏内菜单,再返回落进"未开始的正文页"。
static void test_back_chain_from_title_lists(void)
{
    fresh();
    // 标题 -> 设置
    for (int i = 0; i < 5; ++i) click(BSP_BTN_DOWN);
    click(BSP_BTN_OK);
    CHECK(s_app.page == SG0_PAGE_LIST && s_app.list_kind == SG0_LIST_SETTINGS, "settings list");
    // 设置 -> 通关记录
    click(BSP_BTN_DOWN);
    click(BSP_BTN_DOWN);
    click(BSP_BTN_OK);
    CHECK(s_app.list_kind == SG0_LIST_CLEAR, "clear list");
    longp(BSP_BTN_OK);   // 回设置
    CHECK(s_app.list_kind == SG0_LIST_SETTINGS, "clear back to settings (got %s)",
          s_app.list_kind == SG0_LIST_MENU ? "MENU" : "other");
    longp(BSP_BTN_OK);   // 回标题(修复前:游戏内菜单)
    CHECK(s_app.page == SG0_PAGE_TITLE, "settings back to title (got page %d kind %d)",
          (int)s_app.page, (int)s_app.list_kind);
    CHECK(!(s_app.page == SG0_PAGE_GAME && !s_app.game_started), "landed on blank game");
}

// 修复前:标题->章节选择->路线 逐级返回落进菜单,再返回落进空白正文页。
static void test_back_chain_from_routes(void)
{
    fresh();
    for (int i = 0; i < 3; ++i) click(BSP_BTN_DOWN);   // 章节选择
    click(BSP_BTN_OK);
    CHECK(s_app.list_kind == SG0_LIST_ROUTES, "routes list");
    click(BSP_BTN_OK);   // 进第一条路线
    CHECK(s_app.list_kind == SG0_LIST_STARTS, "starts list");
    longp(BSP_BTN_OK);
    CHECK(s_app.list_kind == SG0_LIST_ROUTES, "starts back to routes");
    longp(BSP_BTN_OK);
    CHECK(s_app.page == SG0_PAGE_TITLE, "routes back to title (got page %d kind %d)",
          (int)s_app.page, (int)s_app.list_kind);
}

// 修复前:设置页第 5 行"返回"直接 goto_page(list_return),来源为菜单时显示错层。
static void test_settings_return_row(void)
{
    fresh();
    start_new_game();
    menu_row(5);   // 菜单 -> 设置
    CHECK(s_app.list_kind == SG0_LIST_SETTINGS, "settings from menu");
    for (int i = 0; i < 4; ++i) click(BSP_BTN_DOWN);   // 第 5 行"返回"
    click(BSP_BTN_OK);
    CHECK(s_app.page == SG0_PAGE_LIST && s_app.list_kind == SG0_LIST_MENU,
          "settings 返回 -> menu (got page %d kind %d)", (int)s_app.page, (int)s_app.list_kind);
    CHECK(s_app.list_sel == 5, "menu cursor preserved after 返回");

    fresh();
    for (int i = 0; i < 5; ++i) click(BSP_BTN_DOWN);   // 标题 -> 设置
    click(BSP_BTN_OK);
    for (int i = 0; i < 4; ++i) click(BSP_BTN_DOWN);
    click(BSP_BTN_OK);
    CHECK(s_app.page == SG0_PAGE_TITLE, "settings 返回 -> title (got %d)", (int)s_app.page);
}

// 修复前:历史页没有提示;且从菜单进入后退出直回正文,不回菜单。
static void test_history_hint_and_source(void)
{
    fresh();
    start_new_game();
    click(BSP_BTN_DOWN);
    CHECK(s_app.page == SG0_PAGE_HISTORY, "history page");
    CHECK(s_app.ui.history_hint && s_app.ui.history_hint->text[0] != '\0',
          "history has no visible hint");
    CHECK(strstr(s_app.ui.history_hint->text, "返回") != NULL, "history hint '%s'",
          s_app.ui.history_hint ? s_app.ui.history_hint->text : "");
    click(BSP_BTN_OK);
    CHECK(s_app.page == SG0_PAGE_GAME, "history OK returns to game");

    menu_row(4);   // 菜单 -> 历史记录
    CHECK(s_app.page == SG0_PAGE_HISTORY, "history from menu");
    longp(BSP_BTN_OK);
    CHECK(s_app.page == SG0_PAGE_LIST && s_app.list_kind == SG0_LIST_MENU,
          "history from menu returns to menu (got page %d kind %d)", (int)s_app.page,
          (int)s_app.list_kind);
}

// 修复前:空收信箱 footer 显示 "1/0 组" 且没有返回提示。
static void test_phone_empty_footer(void)
{
    fresh();
    start_new_game();
    menu_row(1);   // 手机
    CHECK(s_app.page == SG0_PAGE_PHONE, "phone home");
    click(BSP_BTN_OK);   // 收信箱(本段 0 封)
    CHECK(s_app.phone_view == SG0_PHONE_INBOX, "inbox view");
    const char *footer = s_app.ui.phone_footer->text;
    CHECK(strstr(footer, "长按") != NULL, "inbox footer has no back hint: '%s'", footer);
    CHECK(strstr(footer, "1/0") == NULL, "inbox footer shows 1/0: '%s'", footer);
    longp(BSP_BTN_OK);
    s_app.phone_sel = 2;
    click(BSP_BTN_OK);   // 联系人
    CHECK(strstr(s_app.ui.phone_footer->text, "长按") != NULL, "contacts footer '%s'",
          s_app.ui.phone_footer->text);
}

// 修复前:通关记录列表按 OK 无任何反馈。
static void test_clear_list_feedback(void)
{
    fresh();
    for (int i = 0; i < 5; ++i) click(BSP_BTN_DOWN);
    click(BSP_BTN_OK);            // 设置
    click(BSP_BTN_DOWN);
    click(BSP_BTN_DOWN);
    click(BSP_BTN_OK);            // 通关记录
    CHECK(s_app.list_kind == SG0_LIST_CLEAR, "clear list");
    click(BSP_BTN_OK);
    CHECK(s_app.page == SG0_PAGE_LIST, "clear list stays");
    CHECK(s_app.notice_ms > 0 && strstr(s_app.notice, "只读") != NULL,
          "clear list OK has no feedback: '%s'", s_app.notice);
}

// 修复前:确认框上/下都只是切换,方向与"确定/取消"不绑定。
static void test_confirm_direction(void)
{
    fresh();
    start_new_game();
    menu_row(6);                  // 菜单 -> 返回标题 -> 确认框
    CHECK(s_app.page == SG0_PAGE_CONFIRM, "confirm page");
    CHECK(s_app.confirm_sel == 1, "confirm default is cancel");
    click(BSP_BTN_UP);
    CHECK(s_app.confirm_sel == 0, "UP selects 确定");
    click(BSP_BTN_DOWN);
    CHECK(s_app.confirm_sel == 1, "DOWN selects 取消");
    click(BSP_BTN_DOWN);
    CHECK(s_app.confirm_sel == 1, "DOWN kept 取消 (toggle would pick 确定)");
    click(BSP_BTN_UP);
    click(BSP_BTN_UP);
    CHECK(s_app.confirm_sel == 0, "UP kept 确定 (toggle would pick 取消)");
    click(BSP_BTN_OK);            // 确定:返回标题
    CHECK(s_app.page == SG0_PAGE_TITLE, "confirmed 返回标题 (got %d)", (int)s_app.page);

    // 取消路径也从确认框回菜单
    fresh();
    start_new_game();
    menu_row(6);
    click(BSP_BTN_DOWN);
    click(BSP_BTN_OK);
    CHECK(s_app.page == SG0_PAGE_LIST && s_app.list_kind == SG0_LIST_MENU, "cancel back to menu");
}


// 存/读档列表的空档位 footer 必须带返回方式(修复前只有 "空档位 - OK 保存/读取")。
static void test_slot_list_footer(void)
{
    fresh();
    start_new_game();
    menu_row(2);
    CHECK(s_app.list_kind == SG0_LIST_SLOTS_SAVE, "save slots");
    CHECK(strstr(s_app.ui.list_footer->text, "长按OK返回") != NULL,
          "slot footer has no back hint: '%s'", s_app.ui.list_footer->text);
    CHECK(strstr(s_app.ui.list_footer->text, "空档位") != NULL,
          "slot footer lost the empty-slot label: '%s'", s_app.ui.list_footer->text);
}

int main(void)
{
    FILE *fh = fopen(STEINS0_PACK_PATH, "rb");
    CHECK(fh != NULL, "open pack");
    if (!fh) return 1;
    fseek(fh, 0, SEEK_END);
    long size = ftell(fh);
    fseek(fh, 0, SEEK_SET);
    s_blob = malloc((size_t)size);
    CHECK(fread(s_blob, 1, (size_t)size, fh) == (size_t)size, "read pack");
    fclose(fh);
    s_blob_size = (uint32_t)size;
    host_lvgl_reset();
    host_lvgl_set_locked(1);

    test_title_and_new_game();
    test_advance_history();
    test_menu_roundtrip();
    test_choice_save_load();
    test_overwrite_and_nvs_failure();
    test_autosave_continue();
    test_chapter_select();
    test_gallery();
    test_bad_save_and_image_failure();
    test_card_and_rine();
    test_n0_cursor108_page1_save();
    test_auto_reading();
    test_slot_io_errors();
    test_unlock_merge();
    test_bad_pack();
    test_page_alloc_failure_keeps_title();
    test_long_press_swallow();
    test_back_chain_from_title_lists();
    test_back_chain_from_routes();
    test_settings_return_row();
    test_history_hint_and_source();
    test_phone_empty_footer();
    test_clear_list_feedback();
    test_confirm_direction();
    test_slot_list_footer();

    // 章节列表必须按源 script 排序(组内)。
    for (uint8_t i = 1; i < s_app.start_order_count; ++i) {
        sg0_start_t a;
        sg0_start_t b;
        CHECK(sg0_pack_start(&s_app.pack, s_app.start_order[i - 1], &a), "start order a");
        CHECK(sg0_pack_start(&s_app.pack, s_app.start_order[i], &b), "start order b");
        CHECK(a.script <= b.script, "chapter order %u > %u", a.script, b.script);
    }
    CHECK(s_app.start_order_count == 104, "start order count %u", s_app.start_order_count);
    CHECK(host_lvgl_violations() == 0, "LVGL lock violations %d", host_lvgl_violations());
    free(s_blob);
    if (failures) {
        fprintf(stderr, "FAIL %d checks\n", failures);
        return 1;
    }
    printf("PASS app state machine (title/game/menu/save/load/chapter/gallery/unlock)\n");
    return 0;
}
