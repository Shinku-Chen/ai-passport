// tests/test_stella_app_regressions.c —— 标题/菜单来源、真实按键序列、NVS 事务、
// CG 失败与解锁重试的补充回归。
#include "stella_app.h"
#include "stella_model.h"
#include "stella_pack.h"

#include "bsp_button.h"
#include "nvs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef STELLA_PACK_PATH
#define STELLA_PACK_PATH "assets/stella/stella_pack.bin"
#endif

extern void host_nvs_reset(void);
extern int host_nvs_commit_fail;
extern int host_nvs_set_fail;
extern int host_nvs_open_fail;
extern int host_page;
extern char host_rows[STELLA_LIST_ROWS][72];
extern int host_notice_visible;
extern int host_cg_fail;
extern int host_scene_fail;
extern int host_validate_fail;

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
    CHECK(nvs_set_blob(h, key, raw, sizeof(raw)) == ESP_OK, "nvs set");
    CHECK(nvs_commit(h) == ESP_OK, "nvs commit");
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

static uint16_t first_choice_op(uint16_t scene_id)
{
    uint16_t idx, n = 0;
    stella_scene_t sc;
    if (!stella_pack_find_scene(&s_app.pack, scene_id, &idx) ||
        !stella_pack_scene(&s_app.pack, idx, &sc)) return 0xFFFF;
    const uint8_t *ops = stella_pack_ops(&s_app.pack, &sc, &n);
    for (uint16_t i = 0; i < n; ++i) {
        stella_op_t op;
        if (stella_pack_op(ops, n, i, &op) && op.type == STELLA_OP_CHOICE) return i;
    }
    return 0xFFFF;
}

static uint16_t last_end_op(uint16_t scene_id)
{
    uint16_t idx, n = 0;
    stella_scene_t sc;
    if (!stella_pack_find_scene(&s_app.pack, scene_id, &idx) ||
        !stella_pack_scene(&s_app.pack, idx, &sc)) return 0;
    const uint8_t *ops = stella_pack_ops(&s_app.pack, &sc, &n);
    for (uint16_t i = n; i > 0; --i) {
        stella_op_t op;
        if (stella_pack_op(ops, n, (uint16_t)(i - 1), &op) && op.type == STELLA_OP_END)
            return (uint16_t)(i - 1);
    }
    return 0;
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

    // 0) UI 统一：短 OK 推进、长 OK 进菜单且吞补发 CLICK；快进瞬时且无延迟。
    {
        host_nvs_reset();
        boot();
        s_app.title_sel = 0;
        click(BSP_BTN_OK);                       // 开始游戏
        CHECK(s_app.page == STELLA_PAGE_GAME, "enter game (%d)", (int)s_app.page);
        for (int i = 0; i < 200 && s_app.typing; ++i) stella_app_tick(&s_app, 50);
        char before[64];
        snprintf(before, sizeof(before), "%s", s_app.page_text);
        click(BSP_BTN_OK);                       // 短 OK = 下一句
        CHECK(s_app.page == STELLA_PAGE_GAME, "short OK stays in game (page=%d)", (int)s_app.page);
        CHECK(strcmp(before, s_app.page_text) != 0 || s_app.typing,
              "short OK must advance");
        // 长 OK 进菜单；补发的 CLICK 只能关菜单/不推进
        longp(BSP_BTN_OK);
        CHECK(s_app.page == STELLA_PAGE_MENU, "long OK -> menu (%d)", (int)s_app.page);
        click(BSP_BTN_OK);                       // 菜单第 0 行 = 返回剧情
        CHECK(s_app.page == STELLA_PAGE_GAME, "menu row0 -> game");
        // 快进：一进就整句瞬显，且每 tick 都推进
        key(BSP_BTN_UP, BSP_BTN_PRESS);
        key(BSP_BTN_UP, BSP_BTN_LONG);
        CHECK(s_app.fast_forward, "long UP enters fast forward");
        CHECK(!s_app.typing, "fast forward must show the whole line");
        const uint16_t op0 = s_app.op;
        stella_app_tick(&s_app, 50);
        CHECK(s_app.op != op0 || s_app.page_no != 0, "fast forward advances every tick");
        key(BSP_BTN_UP, BSP_BTN_RELEASE);
    }

    // 1) catalog 驱动起点；标题/设置/读档/鉴赏长 OK 回标题。
    host_nvs_reset();
    boot();
    CHECK(stella_pack_meta_int(&s_app.pack, "initial", -1) == 1, "initial meta");
    CHECK(stella_pack_meta_int(&s_app.pack, "after_initial", -1) == 101, "after initial meta");
    CHECK(stella_pack_meta_int(&s_app.pack, "flow_scene", -1) == 1, "flow scene meta");
    s_app.title_sel = 6;
    click(BSP_BTN_OK);
    CHECK(s_app.page == STELLA_PAGE_SETTINGS, "title settings");
    longp(BSP_BTN_OK);
    CHECK(s_app.page == STELLA_PAGE_TITLE, "settings long OK -> title");
    s_app.title_sel = 4;
    click(BSP_BTN_OK);
    CHECK(s_app.page == STELLA_PAGE_SLOTS, "title load");
    longp(BSP_BTN_OK);
    CHECK(s_app.page == STELLA_PAGE_TITLE, "load long OK -> title");
    s_app.title_sel = 5;
    click(BSP_BTN_OK);
    CHECK(s_app.page == STELLA_PAGE_GALLERY, "title gallery");
    longp(BSP_BTN_OK);
    CHECK(s_app.page == STELLA_PAGE_TITLE, "gallery long OK -> title");

    // 2) 菜单来源：设置/读档长 OK 回菜单，历史回菜单；正文历史回正文。
    host_nvs_reset();
    boot();
    s_app.title_sel = 0;
    click(BSP_BTN_OK);
    longp(BSP_BTN_OK);
    CHECK(s_app.page == STELLA_PAGE_MENU, "menu");
    s_app.menu_sel = 4;
    click(BSP_BTN_OK);
    CHECK(s_app.page == STELLA_PAGE_SETTINGS, "menu settings");
    longp(BSP_BTN_OK);
    CHECK(s_app.page == STELLA_PAGE_MENU, "settings long OK -> menu");
    s_app.menu_sel = 2;
    click(BSP_BTN_OK);
    CHECK(s_app.page == STELLA_PAGE_SLOTS, "menu load");
    longp(BSP_BTN_OK);
    CHECK(s_app.page == STELLA_PAGE_MENU, "load long OK -> menu");
    s_app.menu_sel = 3;
    click(BSP_BTN_OK);
    CHECK(s_app.page == STELLA_PAGE_HISTORY, "menu history");
    click(BSP_BTN_OK);
    CHECK(s_app.page == STELLA_PAGE_MENU, "history returns menu");
    s_app.menu_sel = 0;
    click(BSP_BTN_OK);
    CHECK(s_app.page == STELLA_PAGE_GAME, "resume game");
    click(BSP_BTN_DOWN);
    CHECK(s_app.page == STELLA_PAGE_HISTORY, "game history");
    click(BSP_BTN_OK);
    CHECK(s_app.page == STELLA_PAGE_GAME, "history returns game");

    // 3) 真实短按 PRESS->RELEASE->CLICK 只推进一次；长按后的 CLICK 被吞。
    host_nvs_reset();
    boot();
    s_app.title_sel = 0;
    click(BSP_BTN_OK);
    click(BSP_BTN_UP);   // complete first line
    const uint16_t before_op = s_app.op;
    const uint16_t before_page = s_app.page_no;
    key(BSP_BTN_UP, BSP_BTN_PRESS);
    key(BSP_BTN_UP, BSP_BTN_RELEASE);
    key(BSP_BTN_UP, BSP_BTN_CLICK);
    const bool one_step = (s_app.op == before_op && s_app.page_no == before_page + 1) ||
                          (s_app.op == before_op + 1 && s_app.page_no == 0);
    CHECK(one_step, "short sequence advanced %u/%u -> %u/%u", (unsigned)before_op,
          (unsigned)before_page, (unsigned)s_app.op, (unsigned)s_app.page_no);
    const uint16_t long_op = s_app.op;
    key(BSP_BTN_UP, BSP_BTN_PRESS);
    key(BSP_BTN_UP, BSP_BTN_LONG);
    key(BSP_BTN_UP, BSP_BTN_RELEASE);
    key(BSP_BTN_UP, BSP_BTN_CLICK);
    CHECK(s_app.op == long_op, "long sequence extra click advanced");
    key(BSP_BTN_UP, BSP_BTN_PRESS);
    key(BSP_BTN_UP, BSP_BTN_LONG);
    CHECK(s_app.fast_forward, "long UP sets ff");
    click(BSP_BTN_DOWN);
    CHECK(!s_app.fast_forward && s_app.page == STELLA_PAGE_HISTORY, "other key cancels ff");

    // 4) 自动存档独立 namespace；commit 失败保留 RAM/NVS 旧档。
    host_nvs_reset();
    stella_save_t old_main = { .scene = 1, .op = 0, .page = 0, .flags = 0, .mode = STELLA_MODE_STORY };
    stella_save_t old_after = { .scene = 101, .op = 0, .page = 0, .flags = STELLA_FLAG_MAIN_CLEAR, .mode = STELLA_MODE_STORY };
    put_save("s0", &old_main);
    put_save("s1", &old_after);
    boot();
    s_app.title_sel = 0;
    click(BSP_BTN_OK);
    stella_app_flush(&s_app);
    CHECK(s_app.have_auto && s_app.autosave.scene == 1, "auto scene 1");
    s_app.page = STELLA_PAGE_TITLE;
    s_app.title_sel = 4;
    click(BSP_BTN_OK);
    s_app.slot_sel = 2;
    click(BSP_BTN_OK);
    CHECK(s_app.scene.id == 101, "loaded 101 for auto failure");
    host_nvs_commit_fail = 1;
    stella_app_flush(&s_app);
    CHECK(s_app.autosave.scene == 1, "auto overwritten on commit failure");
    longp(BSP_BTN_OK);
    s_app.menu_sel = 1;
    click(BSP_BTN_OK);
    CHECK(s_app.page == STELLA_PAGE_SLOTS, "save slots");
    s_app.slot_sel = 1;
    click(BSP_BTN_OK);  // 覆盖确认
    CHECK(s_app.page == STELLA_PAGE_CONFIRM, "overwrite confirm");
    click(BSP_BTN_OK);  // 确认覆盖 -> commit 失败
    CHECK(s_app.slots[0].scene == 1 && s_app.slot_valid[0], "manual slot overwritten on failure");
    CHECK(host_notice_visible == 1, "save failure notice");
    host_nvs_commit_fail = 0;
    host_nvs_set_fail = 1;
    s_app.page = STELLA_PAGE_SLOTS;
    s_app.slot_action = 0;
    s_app.slot_sel = 3;  // empty slot 3
    click(BSP_BTN_OK);
    CHECK(!s_app.slot_valid[2], "set_blob failure marked slot valid");
    CHECK(host_notice_visible == 1, "set_blob failure notice");
    host_nvs_set_fail = 0;
    boot();
    s_app.title_sel = 4;
    click(BSP_BTN_OK);
    s_app.slot_sel = 1;
    click(BSP_BTN_OK);
    CHECK(s_app.scene.id == 1, "NVS manual old preserved");

    // 4b) 三条结局的跨周目标志与 catalog 对齐。
    host_nvs_reset();
    stella_save_t end109 = { .scene = 109, .op = (uint16_t)(last_end_op(109) - 1), .page = 0,
                             .flags = STELLA_FLAG_MAIN_CLEAR | STELLA_FLAG_AFTER_CLEAR,
                             .mode = STELLA_MODE_STORY };
    put_save("s0", &end109);
    boot();
    s_app.title_sel = 4;
    click(BSP_BTN_OK);
    s_app.slot_sel = 1;
    click(BSP_BTN_OK);
    for (int i = 0; i < 8 && s_app.page != STELLA_PAGE_ENDING; ++i) click(BSP_BTN_UP);
    CHECK(stella_unlock_has_flag(&s_app.unlock, STELLA_FLAG_EXTRA_END), "extra end flag");
    host_nvs_reset();
    stella_save_t end108 = { .scene = 108, .op = (uint16_t)(last_end_op(108) - 1), .page = 0,
                             .flags = STELLA_FLAG_MAIN_CLEAR,
                             .mode = STELLA_MODE_STORY };
    put_save("s0", &end108);
    boot();
    s_app.title_sel = 4;
    click(BSP_BTN_OK);
    s_app.slot_sel = 1;
    click(BSP_BTN_OK);
    for (int i = 0; i < 8 && s_app.page != STELLA_PAGE_ENDING; ++i) click(BSP_BTN_UP);
    CHECK(stella_unlock_has_flag(&s_app.unlock, STELLA_FLAG_MAIN_END), "legacy main end flag");
    CHECK(!stella_unlock_has_flag(&s_app.unlock, STELLA_FLAG_EXTRA_END), "legacy extra flag");

    // 4b2) 停在历史页睡前 flush 仍保存最后成功阅读 checkpoint，而不是历史页。
    host_nvs_reset();
    boot();
    s_app.title_sel = 1;
    click(BSP_BTN_OK);
    CHECK(s_app.checkpoint_valid && s_app.checkpoint.scene == 1, "reading checkpoint");
    click(BSP_BTN_DOWN);
    CHECK(s_app.page == STELLA_PAGE_HISTORY, "history before flush");
    stella_app_flush(&s_app);
    CHECK(s_app.have_auto && s_app.autosave.scene == 1 && s_app.autosave.op == 0,
          "history flush wrote a non-reading checkpoint");

    // 4c) 真实章节入口是 chapter 模式；读完 21 章不污染主线 flag。
    host_nvs_reset();
    boot();
    s_app.title_sel = 3;
    click(BSP_BTN_OK);
    s_app.chapter_page = 3;
    s_app.chapter_sel = 2;
    click(BSP_BTN_OK);
    CHECK(s_app.mode == STELLA_MODE_CHAPTER && s_app.scene.id == 21, "chapter mode entry");
    for (int i = 0; i < 20000 && s_app.page == STELLA_PAGE_GAME; ++i) click(BSP_BTN_UP);
    CHECK(s_app.page == STELLA_PAGE_ENDING, "chapter 21 ending");
    CHECK(!stella_unlock_has_flag(&s_app.unlock, STELLA_FLAG_MAIN_CLEAR),
          "chapter read polluted main clear");

    // 4d) 存档模式显式 round-trip；chapter 档恢复后读完仍不授予主线 flag。
    host_nvs_reset();
    stella_save_t chapter_save = { .scene = 21, .op = 155, .page = 0, .flags = 0,
                                   .mode = STELLA_MODE_CHAPTER };
    put_save("s0", &chapter_save);
    boot();
    s_app.title_sel = 4;
    click(BSP_BTN_OK);
    s_app.slot_sel = 1;
    click(BSP_BTN_OK);
    CHECK(s_app.mode == STELLA_MODE_CHAPTER, "chapter save mode restored");
    for (int i = 0; i < 8 && s_app.page != STELLA_PAGE_ENDING; ++i) click(BSP_BTN_UP);
    CHECK(!stella_unlock_has_flag(&s_app.unlock, STELLA_FLAG_MAIN_CLEAR),
          "chapter save ending polluted main clear");

    // 4e) 正文坏图：可见提示、不标记已读/checkpoint，恢复后正常标记。
    host_nvs_reset();
    boot();
    host_scene_fail = 1;
    s_app.title_sel = 1;
    click(BSP_BTN_OK);
    CHECK(s_app.page == STELLA_PAGE_GAME && host_notice_visible, "scene image failure notice");
    CHECK(!stella_unlock_has_scene(&s_app.unlock, 1), "failed image marked scene read");
    CHECK(!s_app.checkpoint_valid, "failed image made checkpoint");
    host_scene_fail = 0;
    host_nvs_reset();
    boot();
    s_app.title_sel = 1;
    click(BSP_BTN_OK);
    CHECK(stella_unlock_has_scene(&s_app.unlock, 1) && s_app.checkpoint_valid,
          "good image did not mark/checkpoint");

    // 4f) 选项档预检失败不得破坏当前正文进度。
    const uint16_t choice = first_choice_op(107);
    CHECK(choice != 0xFFFF, "choice op for preflight");
    host_nvs_reset();
    stella_save_t choice_save2 = { .scene = 107, .op = choice, .page = 0, .flags = 0,
                                   .mode = STELLA_MODE_STORY };
    put_save("s0", &choice_save2);
    boot();
    s_app.title_sel = 1;  // start scene 1
    click(BSP_BTN_OK);
    CHECK(s_app.scene.id == 1 && !s_app.at_choice, "current story before bad load");
    key(BSP_BTN_OK, BSP_BTN_LONG);
    s_app.menu_sel = 2;
    click(BSP_BTN_OK);
    host_validate_fail = 1;
    s_app.slot_sel = 1;
    click(BSP_BTN_OK);
    CHECK(s_app.scene.id == 1 && !s_app.at_choice, "bad choice image replaced current progress");
    CHECK(host_notice_visible, "bad choice image notice");
    host_validate_fail = 0;

    // 4g) 待选时长 OK 进菜单、保存、返回均保留同一待选和 op。
    host_nvs_reset();
    put_save("s0", &choice_save2);
    boot();
    s_app.title_sel = 4;
    click(BSP_BTN_OK);
    s_app.slot_sel = 1;
    click(BSP_BTN_OK);
    CHECK(s_app.at_choice && s_app.op == choice, "choice fixture for menu");
    key(BSP_BTN_OK, BSP_BTN_LONG);
    CHECK(s_app.page == STELLA_PAGE_MENU && s_app.at_choice && s_app.op == choice,
          "choice lost on menu open");
    s_app.menu_sel = 1;
    click(BSP_BTN_OK);
    s_app.slot_sel = 2;  // empty manual slot
    click(BSP_BTN_OK);
    CHECK(s_app.page == STELLA_PAGE_SLOTS && s_app.at_choice && s_app.op == choice,
          "choice lost on save page");
    key(BSP_BTN_OK, BSP_BTN_LONG);
    CHECK(s_app.page == STELLA_PAGE_MENU, "save long OK back to menu");
    s_app.menu_sel = 0;
    click(BSP_BTN_OK);
    CHECK(s_app.page == STELLA_PAGE_GAME && s_app.at_choice && s_app.op == choice,
          "choice changed after menu resume");

    // 5) 图片失败不解锁 CG；解锁写失败 dirty 后重试。
    host_nvs_reset();
    boot();
    s_app.title_sel = 5;
    click(BSP_BTN_OK);
    CHECK(s_app.page == STELLA_PAGE_GALLERY, "gallery for cg failure");
    host_cg_fail = 1;
    s_app.gallery_sel = 1;
    click(BSP_BTN_OK);
    CHECK(s_app.page == STELLA_PAGE_GALLERY, "failed cg stays gallery");
    CHECK(!stella_unlock_has_gallery(&s_app.unlock, 1), "failed cg unlocked");
    CHECK(s_app.cg_decode_failed, "cg failure flag");
    host_cg_fail = 0;
    click(BSP_BTN_OK);
    CHECK(s_app.page == STELLA_PAGE_CG && stella_unlock_has_gallery(&s_app.unlock, 1),
          "cg retry unlock");
    host_nvs_commit_fail = 1;
    s_app.page = STELLA_PAGE_GALLERY;
    s_app.gallery_sel = 2;
    click(BSP_BTN_OK);
    CHECK(s_app.page == STELLA_PAGE_CG && stella_unlock_has_gallery(&s_app.unlock, 2),
          "cg ram unlock");
    CHECK(s_app.unlock_dirty, "unlock dirty on commit failure");
    host_nvs_commit_fail = 0;
    stella_app_tick(&s_app, 2100);
    CHECK(!s_app.unlock_dirty, "unlock retry cleared dirty");
    boot();
    CHECK(stella_unlock_has_gallery(&s_app.unlock, 2), "unlock persisted after retry");

    free(s_pack);
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("PASS: app regressions (origins/key sequence/NVS transaction/CG unlock)\n");
    return 0;
}
