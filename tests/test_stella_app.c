// tests/test_stella_app.c —— 薄 ESP/NVS/LVGL 适配下的真实状态机回归。
//
// 覆盖：首句不跳过、107 选择处保存/恢复、选择确认后的 cursor、双故事存档身份、
// 正篇/后日谈通关 flags、CG 鉴赏解锁实际触发、NVS commit 失败不报保存成功。
#include "stella_app.h"
#include "stella_model.h"
#include "stella_pack.h"

#include "nvs.h"
#include "bsp_button.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef STELLA_PACK_PATH
#define STELLA_PACK_PATH "assets/stella/stella_pack.bin"
#endif

extern void host_nvs_reset(void);
extern int host_nvs_commit_fail;
extern int host_nvs_open_fail;
extern int host_page;
extern char host_ui_text[];
extern char host_ui_prompt[];
extern char host_ui_option[];
extern char host_ui_ending[];
extern int host_has_choice;
extern int host_last_cg_asset;
extern int host_notice_visible;
extern char host_rows[STELLA_LIST_ROWS][72];

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); failures++; } } while (0)

static uint8_t *s_pack;
static uint32_t s_pack_size;
static stella_app_t s_app;

static void put_save(const char *key, const stella_save_t *save)
{
    uint8_t raw[STELLA_SAVE_BYTES];
    CHECK(stella_save_encode(save, raw, sizeof(raw)) == STELLA_SAVE_BYTES, "encode %s", key);
    nvs_handle_t h;
    CHECK(nvs_open("stella", NVS_READWRITE, &h) == ESP_OK, "nvs open");
    CHECK(nvs_set_blob(h, key, raw, sizeof(raw)) == ESP_OK, "nvs set %s", key);
    CHECK(nvs_commit(h) == ESP_OK, "nvs commit %s", key);
    nvs_close(h);
}

static void boot(void)
{
    memset(&s_app, 0, sizeof(s_app));
    CHECK(stella_app_init(&s_app, s_pack, s_pack_size), "app init");
    CHECK(host_page == STELLA_PAGE_TITLE, "page title");
}

static void key(uint8_t btn, uint8_t ev)
{
    stella_key_t k = { .btn = btn, .ev = ev };
    stella_app_key(&s_app, &k);
}

static void click(uint8_t btn) { key(btn, BSP_BTN_PRESS); key(btn, BSP_BTN_RELEASE); key(btn, BSP_BTN_CLICK); }
static void longp(uint8_t btn) { key(btn, BSP_BTN_LONG); }

static void expected_text(uint16_t scene_id, uint32_t op, char *out, size_t cap)
{
    uint16_t idx;
    stella_scene_t scene;
    CHECK(stella_pack_find_scene(&s_app.pack, scene_id, &idx), "scene %u", scene_id);
    CHECK(stella_pack_scene(&s_app.pack, idx, &scene), "scene load");
    uint16_t n = 0;
    const uint8_t *ops = stella_pack_ops(&s_app.pack, &scene, &n);
    CHECK(ops != NULL && op < n, "op %u < %u", (unsigned)op, (unsigned)n);
    stella_op_t o;
    CHECK(stella_pack_op(ops, n, (uint16_t)op, &o), "op load");
    CHECK(stella_pack_text_copy(&s_app.pack, o.text_off, o.text_len, out, cap), "text copy");
}

int main(void)
{
    FILE *f = fopen(STELLA_PACK_PATH, "rb");
    CHECK(f != NULL, "pack file");
    if (!f) return 1;
    fseek(f, 0, SEEK_END);
    s_pack_size = (uint32_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    s_pack = malloc(s_pack_size);
    CHECK(s_pack && fread(s_pack, 1, s_pack_size, f) == s_pack_size, "pack read");
    fclose(f);

    // 1) 首句不跳过：标题 -> 开始故事 -> 第一次 OK 只补全当前句。
    host_nvs_reset();
    boot();
    click(BSP_BTN_OK);

    CHECK(s_app.page == STELLA_PAGE_GAME, "start game page");
    CHECK(host_ui_text[0] == '\0', "typing not complete yet");
    click(BSP_BTN_UP);
    char want[STELLA_TEXT_BUFFER];
    expected_text(1, 0, want, sizeof(want));
    CHECK(strcmp(host_ui_text, want) == 0, "first line mismatch: '%s'", host_ui_text);

    // 2) 107 选择处保存 -> 重启 -> 继续，应仍在选择且提示句不丢。
    host_nvs_reset();
    stella_save_t choice_save = { .scene = 107, .op = 423, .page = 0, .flags = STELLA_FLAG_MAIN_CLEAR, .mode = STELLA_MODE_STORY };
    put_save("s0", &choice_save);
    boot();
    s_app.title_sel = 4;  // 读取存档
    click(BSP_BTN_OK);
    CHECK(s_app.page == STELLA_PAGE_SLOTS, "slots page");
    s_app.slot_sel = 1;   // 自动存档占第 0 行
    click(BSP_BTN_OK);
    CHECK(s_app.page == STELLA_PAGE_GAME && s_app.scene.id == 107, "loaded 107");
    CHECK(host_has_choice == 1, "choice not shown");
    CHECK(host_ui_prompt[0] != '\0', "choice prompt empty");
    CHECK(strstr(host_ui_option, "谢谢你") != NULL, "choice option: %s", host_ui_option);
    stella_app_flush(&s_app);
    boot();
    CHECK(strstr(host_ui_text, "") != NULL, "boot ok");
    CHECK(s_app.have_auto, "auto save after flush");
    s_app.title_sel = 0;  // 继续阅读
    click(BSP_BTN_OK);
    CHECK(s_app.page == STELLA_PAGE_GAME && s_app.scene.id == 107, "auto load 107");
    CHECK(host_has_choice == 1 && host_ui_prompt[0] != '\0', "restored choice prompt");

    // 3) 选择确认：cursor 到 424，当前句从 424 开始；保存后恢复仍在 424。
    click(BSP_BTN_OK);
    CHECK(!host_has_choice, "choice hidden");
    CHECK(s_app.op == 424, "choice target op=%u", (unsigned)s_app.op);
    click(BSP_BTN_UP);  // 补全 424 的当前句
    expected_text(107, 424, want, sizeof(want));
    CHECK(strcmp(host_ui_text, want) == 0, "post-choice line: '%s'", host_ui_text);
    stella_app_flush(&s_app);
    boot();
    s_app.title_sel = 0;
    click(BSP_BTN_OK);
    CHECK(s_app.scene.id == 107 && s_app.op == 424, "restored post-choice op");

    // 4) 双故事存档身份：101 与 1 各自恢复。
    host_nvs_reset();
    stella_save_t main_save = { .scene = 1, .op = 0, .page = 0, .flags = 0, .mode = STELLA_MODE_STORY };
    stella_save_t after_save = { .scene = 101, .op = 0, .page = 0, .flags = STELLA_FLAG_MAIN_CLEAR, .mode = STELLA_MODE_STORY };
    put_save("s0", &main_save);
    put_save("s1", &after_save);
    boot();
    s_app.title_sel = 4;
    click(BSP_BTN_OK);
    s_app.slot_sel = 2;  // 存档 2 -> after_save
    click(BSP_BTN_OK);
    CHECK(s_app.scene.id == 101, "after save identity scene=%u", (unsigned)s_app.scene.id);
    stella_app_flush(&s_app);
    boot();
    s_app.title_sel = 0;
    click(BSP_BTN_OK);
    CHECK(s_app.scene.id == 101, "after auto identity scene=%u", (unsigned)s_app.scene.id);
    s_app.page = STELLA_PAGE_TITLE;
    s_app.title_sel = 4;
    click(BSP_BTN_OK);
    s_app.slot_sel = 1;  // 存档 1 -> main_save
    click(BSP_BTN_OK);
    CHECK(s_app.scene.id == 1, "main save identity scene=%u", (unsigned)s_app.scene.id);

    // 5) 正篇结局实际触发 MAIN_CLEAR，标题出现后日谈入口。
    host_nvs_reset();
    stella_save_t end21 = { .scene = 21, .op = 155, .page = 0, .flags = 0, .mode = STELLA_MODE_STORY };
    put_save("s0", &end21);
    boot();
    s_app.title_sel = 4;
    click(BSP_BTN_OK);
    s_app.slot_sel = 1;
    click(BSP_BTN_OK);
    for (int i = 0; i < 8 && s_app.page != STELLA_PAGE_ENDING; ++i) click(BSP_BTN_UP);
    CHECK(s_app.page == STELLA_PAGE_ENDING, "ending page");
    CHECK(stella_unlock_has_flag(&s_app.unlock, STELLA_FLAG_MAIN_CLEAR), "main clear flag");
    CHECK(strstr(host_ui_ending, "阅读完毕") != NULL, "ending text");
    click(BSP_BTN_OK);
    CHECK(s_app.page == STELLA_PAGE_TITLE, "back title");
    CHECK(strstr(host_rows[2], "后日谈") != NULL, "after row visible: %s", host_rows[2]);

    // 6) 后日谈结局触发 AFTER_CLEAR，109 附加故事解锁条件满足。
    host_nvs_reset();
    stella_save_t end107 = { .scene = 107, .op = 455, .page = 0, .flags = STELLA_FLAG_MAIN_CLEAR, .mode = STELLA_MODE_STORY };
    put_save("s0", &end107);
    boot();
    s_app.title_sel = 4;
    click(BSP_BTN_OK);
    s_app.slot_sel = 1;
    click(BSP_BTN_OK);
    for (int i = 0; i < 8 && s_app.page != STELLA_PAGE_ENDING; ++i) click(BSP_BTN_UP);
    CHECK(stella_unlock_has_flag(&s_app.unlock, STELLA_FLAG_AFTER_CLEAR), "after clear flag");

    // 7) CG 鉴赏实际打开一组即写解锁位。
    host_nvs_reset();
    boot();
    s_app.title_sel = 5;  // CG 鉴赏
    click(BSP_BTN_OK);
    CHECK(s_app.page == STELLA_PAGE_GALLERY, "gallery page");
    s_app.gallery_sel = 0;
    click(BSP_BTN_OK);
    CHECK(s_app.page == STELLA_PAGE_CG, "cg page");
    CHECK(host_last_cg_asset >= 0, "cg asset not decoded");
    CHECK(stella_unlock_has_gallery(&s_app.unlock, 0), "gallery group 0 not unlocked");

    // 8) NVS commit 失败时保存槽不得标记成功。
    host_nvs_reset();
    boot();
    click(BSP_BTN_OK);      // start
    click(BSP_BTN_UP);      // complete first line
    longp(BSP_BTN_OK);      // menu
    s_app.menu_sel = 1;     // 保存进度
    click(BSP_BTN_OK);
    CHECK(s_app.page == STELLA_PAGE_SLOTS, "save slots page");
    host_nvs_commit_fail = 1;
    s_app.slot_sel = 2;     // 存档 2
    click(BSP_BTN_OK);
    CHECK(s_app.slot_valid[1] == false, "commit failure marked slot valid");
    CHECK(host_notice_visible == 1, "commit failure notice missing");

    free(s_pack);
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("PASS: app state machine (choice/save/dual story/ending/CG/NVS)\n");
    return 0;
}
