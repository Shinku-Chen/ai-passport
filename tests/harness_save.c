// tests/harness_save.c —— NVS 失败注入回归（真实 nn_save + nn_app + nn_ui）。
//
// 覆盖审查报告 P2-3 与“写失败保留旧档”：
//   F1 nvs_set_blob 失败：写入返回失败、旧档内容不变、界面看到失败提示、阅读状态不变
//   F2 nvs_commit 失败 ：同一组断言（未提交 = 未落盘）
//   F3 NVS 初始化失败   ：降级为 BROKEN、界面显示“存档不可用”、仍能继续阅读
//   F4 读坏档/空槽      ：提示“该槽位没有存档”，当前剧情位置不变
//   F5 写成功           ：提示“已保存”可见，槽位存在
//   F6 全程不得调用 nvs_flash_erase()（动态断言，配合静态契约测试）
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nn_app.h"
#include "nn_pack.h"
#include "nn_save.h"
#include "nn_ui.h"

#include "nvs.h"
#include "nvs_flash.h"

// ---------------------------------------------------------------- 内存 NVS（可注入失败）
#define NVS_MAX 64
typedef struct {
    char key[16];
    uint8_t *blob;
    size_t len;
} ent_t;
static ent_t g_db[NVS_MAX];
static int g_db_n;
static ent_t g_pending[NVS_MAX];
static int g_pending_n;
static int g_fail_set_blob, g_fail_commit, g_fail_get_blob;
static esp_err_t g_init_rc = ESP_OK;
static int g_erase_calls;

static ent_t *db_find(const char *k)
{
    for (int i = 0; i < g_db_n; ++i) if (!strcmp(g_db[i].key, k)) return &g_db[i];
    return NULL;
}
static ent_t *db_slot(const char *k)
{
    ent_t *e = db_find(k);
    if (e) return e;
    if (g_db_n >= NVS_MAX) return NULL;
    e = &g_db[g_db_n++];
    snprintf(e->key, sizeof(e->key), "%s", k);
    return e;
}
static ent_t *pending_slot(const char *k)
{
    for (int i = 0; i < g_pending_n; ++i) if (!strcmp(g_pending[i].key, k)) return &g_pending[i];
    if (g_pending_n >= NVS_MAX) return NULL;
    ent_t *e = &g_pending[g_pending_n++];
    snprintf(e->key, sizeof(e->key), "%s", k);
    return e;
}

esp_err_t nvs_open(const char *ns, nvs_open_mode_t m, nvs_handle_t *h) { (void)ns; (void)m; *h = 1; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t len)
{
    (void)h;
    if (g_fail_set_blob) return ESP_FAIL;
    ent_t *e = pending_slot(k);
    if (!e) return ESP_FAIL;
    free(e->blob);
    e->blob = malloc(len);
    memcpy(e->blob, v, len);
    e->len = len;
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *out, size_t *len)
{
    (void)h;
    if (g_fail_get_blob) return ESP_FAIL;
    ent_t *e = db_find(k);
    if (!e || !e->len) return ESP_ERR_NVS_NOT_FOUND;
    if (*len < e->len) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out, e->blob, e->len);
    *len = e->len;
    return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t h, const char *k, uint8_t v)
{
    (void)h;
    if (g_fail_set_blob) return ESP_FAIL;
    ent_t *e = pending_slot(k);
    if (!e) return ESP_FAIL;
    free(e->blob);
    e->blob = malloc(1);
    ((uint8_t *)e->blob)[0] = v;
    e->len = 1;
    return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t h, const char *k, uint8_t *v)
{
    (void)h;
    ent_t *e = db_find(k);
    if (!e || !e->len) return ESP_ERR_NVS_NOT_FOUND;
    *v = ((uint8_t *)e->blob)[0];
    return ESP_OK;
}
esp_err_t nvs_set_u16(nvs_handle_t h, const char *k, uint16_t v)
{
    (void)h;
    if (g_fail_set_blob) return ESP_FAIL;
    ent_t *e = pending_slot(k);
    if (!e) return ESP_FAIL;
    free(e->blob);
    e->blob = malloc(2);
    memcpy(e->blob, &v, 2);
    e->len = 2;
    return ESP_OK;
}
esp_err_t nvs_get_u16(nvs_handle_t h, const char *k, uint16_t *v)
{
    (void)h;
    ent_t *e = db_find(k);
    if (!e || e->len < 2) return ESP_ERR_NVS_NOT_FOUND;
    memcpy(v, e->blob, 2);
    return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t h, const char *k)
{
    (void)h;
    ent_t *e = db_find(k);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    free(e->blob);
    e->blob = NULL;
    e->len = 0;
    e->key[0] = '\0';
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h)
{
    (void)h;
    if (g_fail_commit) {
        for (int i = 0; i < g_pending_n; ++i) {
            free(g_pending[i].blob);
            g_pending[i].blob = NULL;
            g_pending[i].len = 0;
        }
        g_pending_n = 0;
        return ESP_FAIL;
    }
    for (int i = 0; i < g_pending_n; ++i) {
        ent_t *d = db_slot(g_pending[i].key);
        if (!d) continue;
        free(d->blob);
        d->blob = g_pending[i].blob;
        d->len = g_pending[i].len;
        g_pending[i].blob = NULL;
        g_pending[i].len = 0;
    }
    g_pending_n = 0;
    return ESP_OK;
}
esp_err_t nvs_flash_init(void) { return g_init_rc; }
esp_err_t nvs_flash_erase(void) { g_erase_calls++; return ESP_OK; }
esp_err_t nvs_flash_deinit(void) { return ESP_OK; }
const char *esp_err_to_name(esp_err_t e) { return e == 0 ? "ESP_OK" : "ERR"; }

// ---------------------------------------------------------------- 薄桩
const lv_font_t gal_cjk_16 = { .line_height = 19 };
const lv_font_t gal_cjk_20 = { .line_height = 23 };
// 画布桩挪到 tests/harness_stubs/nn_canvas_stub.c（分带接口），这里不再定义。
int bsp_battery_soc(void) { return 50; }

// ---------------------------------------------------------------- 驱动
static nn_app_t app;
static nn_pack_t pack;
static uint8_t *g_blob;
static int g_fails;

static void check(int cond, const char *what, const char *detail)
{
    printf("  [%s] %-48s %s\n", cond ? "PASS" : "FAIL", what, detail ? detail : "");
    if (!cond) g_fails++;
}
static void click(int btn)
{
    // 真实驱动：PRESS -> CLICK（PRESS 用来解除上一次长按留下的“吞 CLICK”标志）。
    nn_key_t p = { (uint8_t)btn, 0 };
    nn_app_key(&app, &p);
    nn_key_t k = { (uint8_t)btn, 1 };
    nn_app_key(&app, &k);
}
static void hold(int btn)
{
    nn_key_t k = { (uint8_t)btn, 3 };
    nn_app_key(&app, &k);
}
static const char *foot(void) { return app.ui.ov_foot ? app.ui.ov_foot->text : ""; }
static void fresh_app(void)
{
    memset(&app, 0, sizeof(app));
    (void)nn_store_init();
    if (!nn_app_init(&app, &pack)) {
        printf("app init failed\n");
        exit(2);
    }
}
// 从标题进入正文并读到第 5 句，返回后处于 GAME
static void enter_reading(void)
{
    fresh_app();
    for (int i = 0; i < 8; ++i) click(0);   // 标题光标归零
    click(2);
    for (int i = 0; i < 4; ++i) click(0);
}
static void open_menu(void)
{
    // 短按 OK 已统一为“下一句”，进菜单必须长按 OK。
    nn_key_t p = { (uint8_t)2, 0 }; nn_app_key(&app, &p);   // PRESS
    p.ev = 3; nn_app_key(&app, &p);                          // LONG
    p.ev = 4; nn_app_key(&app, &p);                          // RELEASE
}

static void open_save_page(void)
{
    // 短按 OK 已统一为“下一句”，进菜单必须长按 OK（btn 2）；光标显式赋值，
    // 不再依赖行序（1 = 保存进度）。
    open_menu();
    app.menu_sel = 1;
    click(2);
}
static void write_slot(int slot)
{
    app.slot_sel = slot;   // 直接置光标：不依赖“按几次 DOWN”的旧行序假设
    click(2);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "assets/nn/nn_pack.bin";
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cannot read %s\n", path); return 2; }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_blob = malloc((size_t)n);
    if (!g_blob || fread(g_blob, 1, (size_t)n, f) != (size_t)n) return 2;
    fclose(f);
    if (!nn_pack_init(&pack, g_blob, (size_t)n)) { printf("pack init failed\n"); return 2; }

    // 先成功写一个基准存档到槽 2
    enter_reading();
    open_save_page();
    write_slot(2);
    check(nn_save_exists(2), "baseline save written", NULL);
    nn_save_t base;
    check(nn_save_read(2, &base), "baseline save readable", NULL);
    const uint16_t base_scene = base.scene, base_op = base.op;
    hold(2);   // 离开保存页

    // ---------------------------------------------------------------- F1
    printf("== F1 nvs_set_blob 失败\n");
    g_fail_set_blob = 1;
    enter_reading();
    const uint16_t live_scene = app.line_scene, live_op = app.line_op;
    const char *live_text = app.line_text;
    const uint16_t live_len = app.line_len;
    open_save_page();
    write_slot(4);
    check(app.page == NN_PAGE_SLOTS, "stays on save page after failure", NULL);
    check(strstr(foot(), "保存失败") != NULL, "failure notice visible", foot());
    check(!nn_save_exists(4), "failed write did not create slot 4", NULL);
    nn_save_t after;
    check(nn_save_read(2, &after) && after.scene == base_scene && after.op == base_op,
          "existing slot 2 unchanged", NULL);
    check(app.line_scene == live_scene && app.line_op == live_op &&
              app.line_len == live_len && app.line_text == live_text,
          "reading state unchanged by failed save", NULL);
    g_fail_set_blob = 0;

    // ---------------------------------------------------------------- F2
    printf("== F2 nvs_commit 失败\n");
    g_fail_commit = 1;
    enter_reading();
    open_save_page();
    write_slot(1);
    check(strstr(foot(), "保存失败") != NULL, "failure notice visible", foot());
    check(!nn_save_exists(1), "uncommitted slot 1 does not exist", NULL);
    check(nn_save_read(2, &after) && after.scene == base_scene && after.op == base_op,
          "slot 2 unchanged after commit failure", NULL);
    g_fail_commit = 0;

    // ---------------------------------------------------------------- F5
    printf("== F5 写成功（提示必须可见）\n");
    enter_reading();
    open_save_page();
    write_slot(3);
    check(strstr(foot(), "已保存") != NULL, "success notice visible", foot());
    check(nn_save_exists(3), "slot 3 written", NULL);
    hold(2);
    check(app.page != NN_PAGE_SLOTS, "long OK leaves the save page", NULL);

    // ---------------------------------------------------------------- F4
    printf("== F4 空槽读取\n");
    enter_reading();
    const uint16_t keep_scene = app.line_scene, keep_op = app.line_op;
    const char *keep_text = app.line_text;
    const uint16_t keep_len = app.line_len;
    open_menu();
    app.menu_sel = 2;               // 2 = 读取进度（不依赖行序假定的 DOWN 次数）
    click(2);
    check(app.page == NN_PAGE_SLOTS, "opened load page", NULL);
    write_slot(4);                  // 槽 4 从未成功写入，应为空
    check(strstr(foot(), "没有存档") != NULL, "empty slot notice visible", foot());
    check(app.page == NN_PAGE_SLOTS, "empty slot keeps the load page", NULL);
    check(app.line_scene == keep_scene && app.line_op == keep_op &&
              app.line_len == keep_len && app.line_text == keep_text,
          "empty slot leaves the reading position alone", NULL);
    hold(2);                        // 回到来源页（菜单）
    check(app.page == NN_PAGE_MENU, "long OK returns to the menu", NULL);

    // ---------------------------------------------------------------- F3
    printf("== F3 NVS 初始化失败（不擦除、降级）\n");
    g_init_rc = ESP_ERR_NVS_NO_FREE_PAGES;
    fresh_app();
    check(nn_store_state() == NN_STORE_BROKEN, "store state is BROKEN", nn_store_reason());
    check(!nn_store_available(), "store unavailable", NULL);
    for (int i = 0; i < 8; ++i) click(0);
    click(2);
    check(app.page == NN_PAGE_GAME, "can still read after NVS failure", NULL);
    for (int i = 0; i < 4; ++i) click(0);
    open_menu();
    app.menu_sel = 1;    // 保存进度
    click(2);
    check(app.page == NN_PAGE_SLOTS, "save page still reachable", NULL);
    check(strstr(foot(), "存档不可用") != NULL, "unavailable notice visible", foot());
    write_slot(1);
    check(strstr(foot(), "保存失败") != NULL, "write failure visible when store broken", foot());
    g_init_rc = ESP_OK;

    // ---------------------------------------------------------------- F6
    printf("== F6 全程不得调用 nvs_flash_erase()\n");
    check(g_erase_calls == 0, "nvs_flash_erase never called", NULL);

    printf("\n%s\n", g_fails ? "harness_save: FAIL" : "harness_save: PASS");
    return g_fails ? 1 : 0;
}
