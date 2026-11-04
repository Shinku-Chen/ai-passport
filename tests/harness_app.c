// tests/harness_app.c —— 页面/按键状态机回归（真实 nn_app + nn_ui + nn_pack + nn_save）。
//
// 这个 harness 用**真实**的应用与界面源码跑在宿主上：LVGL 用薄桩记录对象属性
// （tests/harness_stubs/lvgl.h），NVS 用内存实现（可注入失败），画布/BSP 用薄桩。
// 它覆盖独立审查报告里发现的 4 个 P0/P1：
//   P0-1 保存页无退出路径 + 长按 OK 删档
//   P1-1 历史页空白（浮层被隐藏）
//   P1-2 设置「关于」行无效
//   P1-3/P2-1 返回来源靠 hist_count 猜、存档页覆盖 return_page
// 另外保留：源页矩阵（标题/菜单/正文各自返回）、同句存档恢复、解锁持久化。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nn_app.h"
#include "nn_pack.h"
#include "nn_save.h"
#include "nn_save_codec.h"
#include "nn_ui.h"
#include "nn_canvas.h"
#include "nn_canvas_stub.h"

#include "nvs.h"
#include "nvs_flash.h"

// ---------------------------------------------------------------- 内存 NVS
#define NVS_MAX 96
typedef struct {
    char key[16];
    uint8_t *blob;
    size_t len;
} nvs_ent_t;
static nvs_ent_t g_db[NVS_MAX];
static int g_db_n;
static nvs_ent_t g_pending[NVS_MAX];
static int g_pending_n;
static int g_fail_set_blob, g_fail_commit, g_fail_get_blob;
static esp_err_t g_init_rc = ESP_OK;

static nvs_ent_t *db_find(const char *k)
{
    for (int i = 0; i < g_db_n; ++i) {
        if (!strcmp(g_db[i].key, k)) return &g_db[i];
    }
    return NULL;
}
static nvs_ent_t *db_slot(const char *k)
{
    nvs_ent_t *e = db_find(k);
    if (e) return e;
    if (g_db_n >= NVS_MAX) return NULL;
    e = &g_db[g_db_n++];
    snprintf(e->key, sizeof(e->key), "%s", k);
    return e;
}
static nvs_ent_t *pending_slot(const char *k)
{
    for (int i = 0; i < g_pending_n; ++i) {
        if (!strcmp(g_pending[i].key, k)) return &g_pending[i];
    }
    if (g_pending_n >= NVS_MAX) return NULL;
    nvs_ent_t *e = &g_pending[g_pending_n++];
    snprintf(e->key, sizeof(e->key), "%s", k);
    return e;
}

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out)
{
    (void)name; (void)mode;
    *out = 1;
    return ESP_OK;
}
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t len)
{
    (void)h;
    if (g_fail_set_blob) return ESP_FAIL;
    nvs_ent_t *e = pending_slot(k);
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
    nvs_ent_t *e = db_find(k);
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
    nvs_ent_t *e = pending_slot(k);
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
    nvs_ent_t *e = db_find(k);
    if (!e || !e->len) return ESP_ERR_NVS_NOT_FOUND;
    *v = ((uint8_t *)e->blob)[0];
    return ESP_OK;
}
esp_err_t nvs_set_u16(nvs_handle_t h, const char *k, uint16_t v)
{
    (void)h;
    if (g_fail_set_blob) return ESP_FAIL;
    nvs_ent_t *e = pending_slot(k);
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
    nvs_ent_t *e = db_find(k);
    if (!e || e->len < 2) return ESP_ERR_NVS_NOT_FOUND;
    memcpy(v, e->blob, 2);
    return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t h, const char *k)
{
    (void)h;
    nvs_ent_t *e = db_find(k);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    free(e->blob);
    e->blob = NULL;
    e->len = 0;
    e->key[0] = '\0';
    return ESP_OK;
}
// commit 只在成功时把 pending 落盘：失败 = 未提交 = 旧档保持
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
        nvs_ent_t *d = db_slot(g_pending[i].key);
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
esp_err_t nvs_flash_erase(void) { return ESP_OK; }
esp_err_t nvs_flash_deinit(void) { return ESP_OK; }
const char *esp_err_to_name(esp_err_t e) { return e == 0 ? "ESP_OK" : "ERR"; }

// ---------------------------------------------------------------- 画布 / 字体 / BSP 薄桩
const lv_font_t gal_cjk_16 = { .line_height = 19 };
const lv_font_t gal_cjk_20 = { .line_height = 23 };
// 画布桩挪到 tests/harness_stubs/nn_canvas_stub.c（分带接口），这里不再定义。
int bsp_battery_soc(void) { return 88; }

// ---------------------------------------------------------------- 驱动
static nn_app_t app;
static nn_pack_t pack;
static uint8_t *g_blob;
static int g_fails;

static const char *page_name(nn_page_t p)
{
    switch (p) {
    case NN_PAGE_TITLE: return "TITLE";
    case NN_PAGE_GAME: return "GAME";
    case NN_PAGE_CHOICE: return "CHOICE";
    case NN_PAGE_MENU: return "MENU";
    case NN_PAGE_SLOTS: return "SLOTS";
    case NN_PAGE_SETTINGS: return "SETTINGS";
    case NN_PAGE_HISTORY: return "HISTORY";
    case NN_PAGE_GALLERY: return "GALLERY";
    case NN_PAGE_CGVIEW: return "CGVIEW";
    case NN_PAGE_ENDING: return "ENDING";
    case NN_PAGE_ABOUT: return "ABOUT";
    default: return "?";
    }
}

#define BTN_UP NN_BTN_UP
#define BTN_DOWN NN_BTN_DOWN
#define BTN_OK NN_BTN_OK

static void raw_key(int btn, int ev)
{
    nn_key_t k = { (uint8_t)btn, (uint8_t)ev };
    nn_app_key(&app, &k);
}

// 真实按键组件的回调序列：一次短按 = PRESS -> RELEASE -> CLICK。
// 状态机必须在 PRESS/RELEASE 上“什么都不做”，否则同一次按键会跨两次页
// （v3 审查的 N1：历史页按一下先回正文、CLICK 再弹出菜单）。
static void press(int btn) { raw_key(btn, NN_EV_PRESS); }
static void release(int btn) { raw_key(btn, NN_EV_RELEASE); }

static void click(int btn)
{
    press(btn);
    release(btn);
    raw_key(btn, NN_EV_CLICK);
}
// 长按 = PRESS -> LONG（按住期间生效）-> RELEASE
static void hold(int btn)
{
    press(btn);
    raw_key(btn, NN_EV_LONG);
    release(btn);
}

static void check(int cond, const char *what, const char *detail)
{
    printf("  [%s] %-52s %s\n", cond ? "PASS" : "FAIL", what, detail ? detail : "");
    if (!cond) g_fails++;
}

static void expect_page(nn_page_t want, const char *what)
{
    char detail[64];
    snprintf(detail, sizeof(detail), "got=%s want=%s", page_name(app.page), page_name(want));
    check(app.page == want, what, detail);
}

static bool obj_hidden(const lv_obj_t *o) { return o && (o->flags & LV_OBJ_FLAG_HIDDEN); }
static bool obj_visible(const lv_obj_t *o) { return o && !(o->flags & LV_OBJ_FLAG_HIDDEN); }
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
static void to_title(void) { fresh_app(); expect_page(NN_PAGE_TITLE, "boot -> title"); }

// 画布只有 150 高（画面区）：标题 6 项菜单必须全部落在 150..240 的黑带里，
// 不能压在画面上；标题文字仍在画面上方 (10,8)。
static void check_title_menu_band(void)
{
    int rows = 0;
    for (int i = 0; i < NN_UI_MAX_ROWS; ++i) {
        if (obj_visible(app.ui.ov_rows[i])) rows++;
    }
    check(rows == 6, "title menu rows visible", "6");
    for (int i = 0; i < rows; ++i) {
        const lv_obj_t *r = app.ui.ov_rows[i];
        char detail[64];
        snprintf(detail, sizeof(detail), "y=%d", (int)r->y);
        check(r->y >= NN_PANEL_TOP, "title row below the art area", detail);
        snprintf(detail, sizeof(detail), "y+h=%d", (int)(r->y + r->h));
        check(r->y + r->h <= NN_SCREEN_H, "title row inside screen", detail);
    }
    char detail[64];
    snprintf(detail, sizeof(detail), "y=%d", (int)app.ui.ov_title->y);
    check(app.ui.ov_title->y < 150, "overlay title stays on the art", detail);
}

// 标题页光标从 0 开始；有自动存档时默认停在“继续阅读”，先归零。
static void title_sel(int index)
{
    // 按上键把光标推到 0（到顶不动），再按 index 次下键
    for (int i = 0; i < 8; ++i) click(BTN_UP);
    for (int i = 0; i < index; ++i) click(BTN_DOWN);
}
enum { T_NEW = 0, T_CONTINUE, T_LOAD, T_SETTINGS, T_GALLERY, T_ABOUT };
enum { M_RESUME = 0, M_SAVE, M_LOAD, M_HISTORY, M_SETTINGS, M_TITLE };


// 分带绘制：三条带各刷一次，图像控件分别贴在 0/80/160，缓冲 51,200 B。
static void check_band_paint(void)
{
    const int bands = nn_canvas_band_h();
    const int count = nn_canvas_band_count();
    char detail[80];
    snprintf(detail, sizeof(detail), "%d 带 x %d px", count, bands);
    check(count == NN_BAND_COUNT && bands == NN_BAND_H, "band layout", detail);
    // 三条带都必须被刷黑并画过图（背景/标题/CG）
    for (int b = 0; b < count; ++b) {
        snprintf(detail, sizeof(detail), "band%d black=%d draw=%d", b,
                 stub_canvas_black_calls[b], stub_canvas_draw_calls[b]);
        check(stub_canvas_black_calls[b] > 0 && stub_canvas_draw_calls[b] > 0,
              "every band is painted", detail);
    }
    // 图像控件：y = 0/80/160，且每带都被 invalidate（分带 flush）
    for (int b = 0; b < count; ++b) {
        const lv_obj_t *img = app.ui.art_bands[b];
        snprintf(detail, sizeof(detail), "y=%d", img ? (int)img->y : -1);
        check(img != NULL && img->y == b * bands, "band image position", detail);
        snprintf(detail, sizeof(detail), "inv=%d", img ? img->invalidated : -1);
        check(img && img->invalidated > 0, "band image invalidated", detail);
    }
    if (count < 4) {
        check(app.ui.art_bands[count] == NULL, "no extra band image", "");
    }
    // 独立缓冲后：整帧只 flush 一次（三带填完统一刷），不再逐带 flush。
    check(lv_stub_flush_count() >= 1, "one flush per frame", "");
    // 三带 dsc 的数据指针必须互不相同（同一块缓冲就会渲染成同一张图）。
    for (int b = 1; b < count; ++b) {
        const lv_image_dsc_t *d0 = (const lv_image_dsc_t *)app.ui.art_bands[0]->src;
        const lv_image_dsc_t *db = (const lv_image_dsc_t *)app.ui.art_bands[b]->src;
        snprintf(detail, sizeof(detail), "band%d data=%p vs %p", b, db ? (void *)db->data : NULL,
                 d0 ? (void *)d0->data : NULL);
        check(d0 && db && d0->data != db->data, "bands must not share one buffer", detail);
    }
}

// 回退：带高 60 / 四条带时，控件竖着排 0/60/120/180。
static void check_band_fallback(void)
{
    stub_canvas_set_bands(60, 4);
    fresh_app();
    char detail[64];
    for (int b = 0; b < 4; ++b) {
        const lv_obj_t *img = app.ui.art_bands[b];
        snprintf(detail, sizeof(detail), "y=%d", img ? (int)img->y : -1);
        check(img != NULL && img->y == b * 60, "fallback band image", detail);
    }
    stub_canvas_set_bands(NN_BAND_H, NN_BAND_COUNT);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "assets/nn/nn_pack.bin";
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("cannot read %s\n", path);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_blob = malloc((size_t)n);
    if (!g_blob || fread(g_blob, 1, (size_t)n, f) != (size_t)n) return 2;
    fclose(f);
    if (!nn_pack_init(&pack, g_blob, (size_t)n)) {
        printf("pack init failed\n");
        return 2;
    }

    // ---------------------------------------------------------------- S1
    printf("== S1 标题 -> 设置 -> 长按 OK 返回标题\n");
    to_title();
    check_title_menu_band();
    check_band_paint();
    title_sel(T_SETTINGS);
    click(BTN_OK);
    expect_page(NN_PAGE_SETTINGS, "title -> settings");
    hold(BTN_OK);
    expect_page(NN_PAGE_TITLE, "settings back -> title");

    // ---------------------------------------------------------------- S1b
    printf("== S1b 回退：320x60 四条带\n");
    check_band_fallback();

    // ---------------------------------------------------------------- S2
    printf("== S2 设置 -> 关于（真实按键路径）\n");
    to_title();
    title_sel(T_SETTINGS);
    click(BTN_OK);                    // 标题页短按 OK = 进入设置
    for (int i = 0; i < 5; ++i) click(BTN_DOWN);
    click(BTN_OK);
    expect_page(NN_PAGE_ABOUT, "OK on 关于");
    check(obj_visible(app.ui.about_body), "about body visible", NULL);
    check(obj_hidden(app.ui.overlay), "overlay hidden on about", NULL);
    click(BTN_OK);
    expect_page(NN_PAGE_SETTINGS, "about back -> settings (source page)");

    // ---------------------------------------------------------------- S3
    printf("== S3 读过剧情后：标题 -> 设置 -> 长按 OK 仍回标题\n");
    to_title();
    title_sel(T_NEW);
    click(BTN_OK);
    expect_page(NN_PAGE_GAME, "title -> new game");
    // 阅读页四硬指标：名字在面板外、页码独占面板内右上且不压正文、末行不出面板。
    check(app.ui.name->y + 19 <= app.ui.panel->y, "name above panel", NULL);
    // 竖屏：页码在面板外右上（贴面板顶），正文贴面板顶 +4。
    check(app.ui.page_label != NULL &&
              app.ui.page_label->y + 18 <= app.ui.panel->y,
          "page label above panel", NULL);
    check(app.ui.body->y == app.ui.panel->y + 4, "body at panel_top+4", NULL);
    check(app.ui.body->y + app.ui.body->h <= app.ui.panel->y + app.ui.panel->h,
          "body inside panel", NULL);
    check(app.settings.panel_opa >= 60, "panel opacity", NULL);
    for (int i = 0; i < 5; ++i) click(BTN_UP);
    for (int i = 0; i < 8; ++i) click(BTN_UP);
    hold(BTN_OK);
    expect_page(NN_PAGE_MENU, "long OK -> menu");
    for (int i = 0; i < 5; ++i) click(BTN_DOWN);
    check(app.menu_sel == 5, "menu cursor after 5 DOWN", NULL);
    click(BTN_OK);
    expect_page(NN_PAGE_TITLE, "menu -> back to title");
    title_sel(T_SETTINGS);
    click(BTN_OK);
    expect_page(NN_PAGE_SETTINGS, "title -> settings after reading");
    hold(BTN_OK);
    expect_page(NN_PAGE_TITLE, "settings back -> title (not menu)");

    // ---------------------------------------------------------------- S4
    printf("== S4 正文 -> 菜单 -> 设置 -> 长按 OK 回菜单\n");
    to_title();
    title_sel(T_NEW);
    click(BTN_OK);
    hold(BTN_OK);
    expect_page(NN_PAGE_MENU, "long OK -> menu");
    for (int i = 0; i < 4; ++i) click(BTN_DOWN);
    click(BTN_OK);
    expect_page(NN_PAGE_SETTINGS, "menu -> settings");
    hold(BTN_OK);
    expect_page(NN_PAGE_MENU, "settings back -> menu");

    // ---------------------------------------------------------------- S5
    printf("== S5 菜单 -> 读取存档 -> 长按 OK 回菜单\n");
    to_title();
    title_sel(T_NEW);
    click(BTN_OK);
    hold(BTN_OK);
    for (int i = 0; i < 2; ++i) click(BTN_DOWN);
    click(BTN_OK);
    expect_page(NN_PAGE_SLOTS, "menu -> slots(load)");
    hold(BTN_OK);
    expect_page(NN_PAGE_MENU, "slots back -> menu");

    // ---------------------------------------------------------------- S6 (P0-1)
    printf("== S6 正文 -> 菜单 -> 保存 -> 选槽 -> 保存 -> 长按 OK 返回且不删档\n");
    to_title();
    title_sel(T_NEW);
    click(BTN_OK);
    for (int i = 0; i < 12; ++i) click(BTN_UP);
    const char *saved_text = app.line_text;
    const uint16_t saved_len = app.line_len;
    const uint16_t saved_scene = app.line_scene, saved_op = app.line_op;
    hold(BTN_OK);
    click(BTN_DOWN);
    click(BTN_OK);
    expect_page(NN_PAGE_SLOTS, "menu -> save");
    check(app.slots_saving, "slot page in save mode", NULL);
    for (int i = 0; i < 3; ++i) click(BTN_DOWN);
    click(BTN_OK);   // 保存到槽 3
    expect_page(NN_PAGE_SLOTS, "still on slots after save");
    check(strstr(foot(), "已保存") != NULL, "success notice visible", foot());
    check(nn_save_exists(3), "slot 3 exists after save", NULL);
    hold(BTN_OK);
    expect_page(NN_PAGE_MENU, "long OK exits save page -> menu");
    check(nn_save_exists(3), "slot 3 still exists after long OK", NULL);
    // 连续长按不得删档：再进保存页长按两次
    click(BTN_OK);   // menu -> resume
    expect_page(NN_PAGE_GAME, "menu -> resume");
    click(BTN_OK);
    click(BTN_DOWN);
    click(BTN_OK);
    hold(BTN_OK);    // 第一次：离开保存页 -> 菜单
    hold(BTN_OK);    // 第二次：菜单长按 = 回正文（不应有任何删除副作用）
    check(nn_save_exists(3), "repeated long OK never deletes a save", NULL);
    expect_page(NN_PAGE_GAME, "menu long OK -> reading");

    // ---------------------------------------------------------------- S7 (P1-1)
    printf("== S7 正文短按 DOWN -> 历史（真实可见性）\n");
    to_title();
    title_sel(T_NEW);
    click(BTN_OK);
    for (int i = 0; i < 3; ++i) click(BTN_UP);
    click(BTN_DOWN);
    expect_page(NN_PAGE_HISTORY, "game DOWN -> history");
    check(obj_visible(app.ui.overlay), "history overlay visible", NULL);
    check(obj_hidden(app.ui.about_body), "history does not reuse about body", NULL);
    check(app.ui.ov_rows[0] && app.ui.ov_rows[0]->text[0] != '\0', "history rows populated",
          app.ui.ov_rows[0] ? app.ui.ov_rows[0]->text : "");
    click(BTN_OK);
    expect_page(NN_PAGE_GAME, "history OK -> game");

    // ---------------------------------------------------------------- S8
    printf("== S8 菜单 -> 历史 -> 长按 OK 回菜单\n");
    hold(BTN_OK);
    for (int i = 0; i < 3; ++i) click(BTN_DOWN);
    click(BTN_OK);
    expect_page(NN_PAGE_HISTORY, "menu -> history");
    check(obj_visible(app.ui.overlay), "history overlay visible (menu entry)", NULL);
    hold(BTN_OK);
    expect_page(NN_PAGE_MENU, "history back -> menu");

    // ---------------------------------------------------------------- S9
    printf("== S9 存/读档同句恢复\n");
    to_title();
    title_sel(T_NEW);
    click(BTN_OK);
    for (int i = 0; i < 29; ++i) click(BTN_UP);
    const char *t0 = app.line_text;
    const uint16_t l0 = app.line_len;
    const uint16_t sc0 = app.line_scene, op0 = app.line_op;
    // 菜单入口 = 长按 OK（短按 OK 已统一为“下一句”）；光标用显式赋值，
    // 不再依赖“按几次 DOWN 落在哪一行”的旧行序假设。
    hold(BTN_OK);
    expect_page(NN_PAGE_MENU, "S9 long OK -> menu");
    app.menu_sel = 1;                    // 1 = 保存进度
    click(BTN_OK);
    expect_page(NN_PAGE_SLOTS, "S9 menu -> save slots");
    app.slot_sel = 2;                    // 显式选槽 2（0 起）
    click(BTN_OK);
    check(nn_save_exists(2), "saved to slot 2", NULL);
    hold(BTN_OK);                        // 槽位页长按 OK 回菜单
    expect_page(NN_PAGE_MENU, "S9 slots back -> menu");
    app.menu_sel = 2;                    // 2 = 读取进度
    click(BTN_OK);
    expect_page(NN_PAGE_SLOTS, "S9 menu -> load slots");
    app.slot_sel = 2;
    click(BTN_OK);
    expect_page(NN_PAGE_GAME, "load -> game");
    check(app.line_scene == sc0 && app.line_op == op0 && app.line_len == l0 &&
              t0 && app.line_text && memcmp(t0, app.line_text, l0) == 0,
          "restored the same line", NULL);
    (void)saved_text; (void)saved_len; (void)saved_scene; (void)saved_op;

    // ---------------------------------------------------------------- S10
    printf("== S10 标题 -> 鉴赏室 -> 长按 OK 回标题；关于 -> OK 回标题\n");
    to_title();
    title_sel(T_GALLERY);
    click(BTN_OK);
    expect_page(NN_PAGE_GALLERY, "title -> gallery");
    hold(BTN_OK);
    expect_page(NN_PAGE_TITLE, "gallery back -> title");
    title_sel(T_ABOUT);
    click(BTN_OK);
    expect_page(NN_PAGE_ABOUT, "title -> about");
    check(obj_visible(app.ui.about_body), "about body visible (title entry)", NULL);
    click(BTN_OK);
    expect_page(NN_PAGE_TITLE, "about back -> title");

    // ---------------------------------------------------------------- S11
    printf("== S11 CG 解锁跨重启保持\n");
    to_title();
    title_sel(T_NEW);
    click(BTN_OK);
    for (int i = 0; i < 400 && app.page == NN_PAGE_GAME && app.rt.view.cg == NN_NONE16; ++i) {
        click(BTN_UP);
    }
    const int bits_before = __builtin_popcount((unsigned)nn_unlock_cg_mask());
    fresh_app();      // 模拟重启，内存 NVS 仍在
    const int bits_after = __builtin_popcount((unsigned)nn_unlock_cg_mask());
    check(bits_before > 0, "a CG got unlocked while reading", NULL);
    check(bits_after >= bits_before, "CG unlock survived a restart", NULL);


    // ---------------------------------------------------------------- S12 (N1)
    printf("== S12 真实事件序列：一次按键只跨一次页（PRESS/RELEASE 不得导航）\n");

    // 历史页：一次短按只回正文，不能顺手弹出菜单；下一次按键仍有效
    to_title();
    title_sel(T_NEW);
    click(BTN_OK);
    for (int i = 0; i < 3; ++i) click(BTN_UP);
    click(BTN_DOWN);
    expect_page(NN_PAGE_HISTORY, "history opened");
    press(BTN_OK);
    expect_page(NN_PAGE_HISTORY, "history: PRESS does nothing");
    release(BTN_OK);
    expect_page(NN_PAGE_HISTORY, "history: RELEASE does nothing");
    raw_key(BTN_OK, NN_EV_CLICK);
    expect_page(NN_PAGE_GAME, "history: CLICK returns to reading (no menu)");
    hold(BTN_OK);
    expect_page(NN_PAGE_MENU, "next long OK still opens the menu");

    // 结局页：一路走到某个结局（选项点选第一项 = bad end）
    to_title();
    title_sel(T_NEW);
    click(BTN_OK);
    int guard = 0;
    while (app.page != NN_PAGE_ENDING && guard++ < 8000) {
        if (app.page == NN_PAGE_CHOICE) click(BTN_OK);
        else if (app.page == NN_PAGE_GAME) click(BTN_UP);
        else break;
    }
    expect_page(NN_PAGE_ENDING, "reached an ending");
    press(BTN_OK);
    expect_page(NN_PAGE_ENDING, "ending: PRESS does nothing");
    raw_key(BTN_OK, NN_EV_CLICK);
    expect_page(NN_PAGE_TITLE, "ending: CLICK returns to title only");
    click(BTN_UP);   // 标题页按一下只移动光标，不应开始游戏
    expect_page(NN_PAGE_TITLE, "title stays after a stray key");

    // 关于页：一次短按只回来源页
    to_title();
    title_sel(T_ABOUT);
    click(BTN_OK);
    expect_page(NN_PAGE_ABOUT, "about opened");
    press(BTN_OK);
    expect_page(NN_PAGE_ABOUT, "about: PRESS does nothing");
    raw_key(BTN_OK, NN_EV_CLICK);
    expect_page(NN_PAGE_TITLE, "about: CLICK returns to title only");

    // CG 浏览：一次短按回鉴赏室，且不会被同一按键再次打开
    nn_unlock_cg(0);
    to_title();
    title_sel(T_GALLERY);
    click(BTN_OK);
    expect_page(NN_PAGE_GALLERY, "gallery opened");
    click(BTN_OK);
    if (app.page == NN_PAGE_CGVIEW) {
        // CGVIEW：CG 必须走 contain（整幅可见），不能走游戏内 cover。
        check(stub_canvas_cg_full_calls > 0, "cgview asks for contain (cg_full)", NULL);
        press(BTN_OK);
        expect_page(NN_PAGE_CGVIEW, "cgview: PRESS does nothing");
        release(BTN_OK);
        expect_page(NN_PAGE_CGVIEW, "cgview: RELEASE does nothing");
        raw_key(BTN_OK, NN_EV_CLICK);
        expect_page(NN_PAGE_GALLERY, "cgview: CLICK returns to the grid");
        release(BTN_OK);
        expect_page(NN_PAGE_GALLERY, "cgview: stray RELEASE keeps the grid");
    } else {
        check(0, "reached the CG viewer for the N1 check", page_name(app.page));
    }

    // ---------------------------------------------------------------- S13 (S1-1)
    // 设置 -> 关于 -> 返回 落回设置；再返回必须离开设置（回到设置的来源页）。
    // 修复前 return_page 被「进入关于」改写成 SETTINGS，第二次返回会重画设置页。
    printf("== S13 设置 -> 关于 -> 返回 -> 再返回（不得停在设置页）\n");
    to_title();
    title_sel(T_SETTINGS);
    click(BTN_OK);
    expect_page(NN_PAGE_SETTINGS, "title -> settings");
    for (int i = 0; i < 5; ++i) click(BTN_DOWN);
    click(BTN_OK);
    expect_page(NN_PAGE_ABOUT, "settings -> about");
    click(BTN_OK);
    expect_page(NN_PAGE_SETTINGS, "about -> settings (one level back)");
    hold(BTN_OK);
    expect_page(NN_PAGE_TITLE, "settings -> title (second level back)");
    // 同一路径从菜单进入时，第二次返回必须回菜单而不是标题
    to_title();
    title_sel(T_NEW);
    click(BTN_OK);
    hold(BTN_OK);
    expect_page(NN_PAGE_MENU, "long OK -> menu");
    for (int i = 0; i < 4; ++i) click(BTN_DOWN);
    click(BTN_OK);
    expect_page(NN_PAGE_SETTINGS, "menu -> settings");
    for (int i = 0; i < 5; ++i) click(BTN_DOWN);
    click(BTN_OK);
    expect_page(NN_PAGE_ABOUT, "settings -> about (from menu path)");
    click(BTN_OK);
    expect_page(NN_PAGE_SETTINGS, "about -> settings (from menu path)");
    hold(BTN_OK);
    expect_page(NN_PAGE_MENU, "settings -> menu (from menu path)");

    // ---------------------------------------------------------------- S14 (S2-1)
    // 剧本异常后 error_shown 必须能被「回标题重开」清掉，否则 UP 永远不推进。
    printf("== S14 error_shown 可在回到标题重开后复位\n");
    to_title();
    title_sel(T_NEW);
    click(BTN_OK);
    expect_page(NN_PAGE_GAME, "reading page for the error scenario");
    app.error_shown = true;          // 模拟一次 NN_STEP_BROKEN（阅读页停在错误文案上）
    click(BTN_OK);
    expect_page(NN_PAGE_MENU, "error page: OK opens the menu");
    for (int i = 0; i < 5; ++i) click(BTN_DOWN);
    click(BTN_OK);
    expect_page(NN_PAGE_TITLE, "menu -> title after a script error");
    check(!app.error_shown, "error flag cleared by returning to title", NULL);
    title_sel(T_NEW);
    click(BTN_OK);
    expect_page(NN_PAGE_GAME, "title -> new game after a script error");
    check(!app.error_shown, "error flag stays clear in the new game", NULL);
    const uint16_t op_before = app.rt.op;
    const int page_before = app.page_index;
    const bool typing_before = app.typing;
    for (int i = 0; i < 6; ++i) click(BTN_UP);
    check(app.rt.op != op_before || app.page_index != page_before || app.typing != typing_before,
          "UP advances again after recovery", NULL);

    // ---------------------------------------------------------------- S15 (S2-2)
    // 选项页长按 OK 只应「暂时离开」：再按 UP 必须回到同一个未选项，不得跳过分支。
    printf("== S15 选项页长按 OK 离开后不得静默跳过选项\n");
    to_title();
    title_sel(T_NEW);
    click(BTN_OK);
    int cguard = 0;
    while (app.page != NN_PAGE_CHOICE && cguard++ < 4000) {
        if (app.page == NN_PAGE_ENDING) break;
        click(BTN_UP);
    }
    expect_page(NN_PAGE_CHOICE, "reached a choice");
    const uint8_t choice_count = app.choice_count;
    const uint16_t choice_scene = app.rt.scene, choice_op = app.rt.op;
    hold(BTN_OK);
    expect_page(NN_PAGE_GAME, "choice: long OK leaves the overlay");
    click(BTN_UP);
    expect_page(NN_PAGE_CHOICE, "UP re-opens the pending choice (no silent skip)");
    check(app.choice_sel == 0, "re-entered choice keeps no selection", NULL);
    check(app.choice_count == choice_count, "choice count preserved", NULL);
    check(app.rt.scene == choice_scene && app.rt.op == choice_op,
          "script position preserved while the choice is pending", NULL);
    // 自动/快进也不得代选或跳过
    app.settings.auto_play = 1;
    app.auto_mode = true;
    hold(BTN_OK);
    expect_page(NN_PAGE_GAME, "choice: long OK leaves (auto on)");
    for (int i = 0; i < 3; ++i) nn_app_tick(&app, 5000);
    expect_page(NN_PAGE_GAME, "auto tick does not skip a pending choice");
    check(app.rt.scene == choice_scene && app.rt.op == choice_op,
          "auto tick keeps the script position", NULL);
    app.auto_mode = false;
    app.settings.auto_play = 0;
    click(BTN_UP);
    expect_page(NN_PAGE_CHOICE, "UP still re-opens the pending choice");
    click(BTN_OK);
    expect_page(NN_PAGE_GAME, "OK confirms the choice and continues");
    check(app.rt.choice_count == 0, "choice consumed after confirming", NULL);

    printf("\n%s\n", g_fails ? "harness_app: FAIL" : "harness_app: PASS");
    return g_fails ? 1 : 0;
}
