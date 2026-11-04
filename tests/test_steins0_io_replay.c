// tests/test_steins0_io_replay.c —— 旧 harness2 J 段的独立重放 + NVS 故障区分。
//
// 旧缺陷:槽 0 已有 64B 档,启动 get 失败后 slot_valid=0,存档同槽却直接
// page=GAME/保存成功/无 CONFIRM/changed=1,把已有 blob 当空覆盖。
// 本测试只读真实源码/包,不修改 review 原件。
#include "steins0_app.h"
#include "steins0_pack.h"
#include "bsp_button.h"
#include "lvgl.h"
#include "nvs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef STEINS0_PACK_PATH
#define STEINS0_PACK_PATH "assets/steins0/steins0_pack.bin"
#endif

extern int host_nvs_commit_fail;
extern int host_nvs_open_fail;
extern int host_nvs_set_fail;
extern int host_nvs_get_fail;
extern int host_nvs_writes;
extern void host_nvs_reset(void);
extern void host_lvgl_reset(void);
extern void host_lvgl_set_locked(int locked);
extern void host_image_reset(void);

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); ++failures; } else { \
    fprintf(stderr, "PASS %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); } } while (0)

static steins0_app_t s_app;
static uint8_t *s_blob;
static uint32_t s_blob_size;

static void key(uint8_t btn, uint8_t ev)
{
    steins0_key_t k = {.btn = btn, .ev = ev};
    steins0_app_key(&s_app, &k);
}
static void click(uint8_t btn)
{
    key(btn, BSP_BTN_PRESS); key(btn, BSP_BTN_RELEASE); key(btn, BSP_BTN_CLICK);
}
static void longp(uint8_t btn)
{
    key(btn, BSP_BTN_PRESS); key(btn, BSP_BTN_LONG); key(btn, BSP_BTN_RELEASE);
}

static void boot(void)
{
    host_lvgl_reset();
    host_lvgl_set_locked(1);
    memset(&s_app, 0, sizeof(s_app));
    CHECK(steins0_app_init(&s_app, s_blob, s_blob_size), "app init");
    host_lvgl_set_locked(1);
}

static void start_new_game(void)
{
    CHECK(s_app.page == SG0_PAGE_TITLE, "title");
    s_app.title_sel = 0;
    click(BSP_BTN_OK);
    if (s_app.page == SG0_PAGE_CARD) click(BSP_BTN_OK);
    CHECK(s_app.page == SG0_PAGE_GAME, "game");
}

static void write_raw_slot0(const uint8_t *raw, size_t len)
{
    nvs_handle_t h;
    CHECK(nvs_open("steins0", NVS_READWRITE, &h) == ESP_OK, "open nvs prewrite");
    CHECK(nvs_set_blob(h, "s00", raw, len) == ESP_OK, "set prewrite");
    CHECK(nvs_commit(h) == ESP_OK, "commit prewrite");
    nvs_close(h);
}

static size_t read_raw_slot0(uint8_t *out, size_t cap)
{
    nvs_handle_t h;
    if (nvs_open("steins0", NVS_READONLY, &h) != ESP_OK) return 0;
    size_t len = cap;
    const esp_err_t err = nvs_get_blob(h, "s00", out, &len);
    nvs_close(h);
    return err == ESP_OK ? len : 0;
}

static void open_save_slot0(void)
{
    longp(BSP_BTN_OK);                 // menu
    CHECK(s_app.page == SG0_PAGE_LIST && s_app.list_kind == SG0_LIST_MENU, "menu");
    for (int i = 0; i < 8 && s_app.list_sel != 2; ++i) click(BSP_BTN_DOWN);
    click(BSP_BTN_OK);                 // 保存进度
    CHECK(s_app.page == SG0_PAGE_LIST && s_app.list_kind == SG0_LIST_SLOTS_SAVE, "save list");
    for (int i = 0; i < SG0_SLOT_COUNT && s_app.list_sel != 0; ++i) click(BSP_BTN_UP);
    CHECK(s_app.list_sel == 0, "slot0 selected");
}

static void replay_get_failure_64b(void)
{
    fprintf(stderr, "--- J replay: existing 64B slot0, get failure at boot ---\n");
    host_nvs_reset();
    uint8_t old[64];
    memset(old, 0xA5, sizeof(old));
    old[0] = 'S'; old[1] = '0'; old[2] = 1;
    write_raw_slot0(old, sizeof(old));

    host_nvs_get_fail = 1;
    boot();
    CHECK(s_app.slot_state[0] == SG0_SLOT_IO, "slot0 IO, not EMPTY");
    CHECK(!s_app.slot_valid[0], "slot0 not valid under get failure");
    start_new_game();
    open_save_slot0();
    const int writes_before = host_nvs_writes;
    click(BSP_BTN_OK);                 // 同槽保存请求
    CHECK(s_app.page == SG0_PAGE_CONFIRM, "existing 64B slot asks CONFIRM, page=%d",
          s_app.page);
    CHECK(strstr(s_app.confirm_prompt, "无法读取") != NULL, "prompt distinguishes IO: '%s'",
          s_app.confirm_prompt);
    click(BSP_BTN_OK);                 // 默认取消
    CHECK(host_nvs_writes == writes_before, "cancelled IO save wrote nothing");
    uint8_t after[128];
    host_nvs_get_fail = 0;
    const size_t after_len = read_raw_slot0(after, sizeof(after));
    CHECK(after_len == sizeof(old) && memcmp(after, old, sizeof(old)) == 0,
          "existing blob unchanged (%u bytes)", (unsigned)after_len);

    // 恢复 IO 后,64B 旧档应被识别为损坏/版本不兼容,仍不能无确认覆盖。
    host_nvs_get_fail = 0;
    boot();
    CHECK(s_app.slot_state[0] == SG0_SLOT_CORRUPT, "64B junk is CORRUPT, not EMPTY");
    start_new_game();
    open_save_slot0();
    click(BSP_BTN_OK);
    CHECK(s_app.page == SG0_PAGE_CONFIRM, "CORRUPT slot asks CONFIRM");
    CHECK(strstr(s_app.confirm_prompt, "损坏") != NULL, "prompt distinguishes CORRUPT: '%s'",
          s_app.confirm_prompt);
    const int writes2 = host_nvs_writes;
    click(BSP_BTN_OK);                 // 取消
    CHECK(host_nvs_writes == writes2, "cancelled CORRUPT save wrote nothing");
}

static void matrix_write_and_open_failures(void)
{
    fprintf(stderr, "--- NVS open/get/set/commit fault matrix ---\n");
    host_nvs_reset();
    host_nvs_open_fail = 1;
    boot();
    CHECK(!s_app.nvs_ok, "open failure: nvs_ok false");
    CHECK(s_app.slot_state[0] == SG0_SLOT_IO, "open failure: slot0 IO");
    host_nvs_open_fail = 0;

    host_nvs_reset();
    boot();
    start_new_game();
    host_nvs_set_fail = 1;
    open_save_slot0();
    click(BSP_BTN_OK);
    CHECK(s_app.page == SG0_PAGE_LIST && !s_app.slot_valid[0],
          "set failure: no direct save/page");
    CHECK(strstr(s_app.ui.notice_text->text, "写入未完成") != NULL, "set failure notice");
    host_nvs_set_fail = 0;

    host_nvs_reset();
    boot();
    start_new_game();
    host_nvs_commit_fail = 1;
    open_save_slot0();
    click(BSP_BTN_OK);
    CHECK(s_app.page == SG0_PAGE_LIST && !s_app.slot_valid[0],
          "commit failure: no valid slot");
    host_nvs_commit_fail = 0;
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
    s_blob_size = (uint32_t)size;
    CHECK(fread(s_blob, 1, (size_t)size, fh) == (size_t)size, "read pack");
    fclose(fh);
    replay_get_failure_64b();
    matrix_write_and_open_failures();
    free(s_blob);
    if (failures) {
        fprintf(stderr, "FAIL %d checks\n", failures);
        return 1;
    }
    fprintf(stderr, "PASS io replay: get-failure 64B slot protected; open/get/set/commit distinguished\n");
    return 0;
}
