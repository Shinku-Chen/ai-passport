#include "bsp_battery.h"
#include "maitetsu_media.h"
#include "bsp_display.h"
#include "gal_music.h"
#include "gal_ui.h"
#include "lvgl.h"

#include <stdio.h>
const lv_font_t gal_cjk_16 = {0};
const lv_font_t gal_cjk_20 = {0};

// —— 测试可控制的状态 ——
int g_media_init_result = 1;       // 0 = 媒体包损坏
int g_media_faulted;               // 1 = 解码中途失败（模拟条带损坏）
int g_lock_failures_left;          // >0 时接下来 N 次 bsp_lvgl_lock 返回失败
int g_typing_active;               // 1 = 正在逐字显示
int g_show_full_text_calls;
char g_last_asset_error[96];
int bsp_battery_soc(void) { return 100; }
bool bsp_lvgl_lock(int timeout_ms)
{
    (void)timeout_ms;
    if (g_lock_failures_left > 0) {
        g_lock_failures_left--;
        return false;
    }
    return true;
}
void bsp_lvgl_unlock(void) {}
bool gal_music_init(void) { return true; }
void gal_music_play(int track_index) { (void)track_index; }
void gal_music_stop(void) {}
int gal_music_current(void) { return -1; }
bool gal_ui_create(gal_ui_t *ui, const lv_font_t *a, const lv_font_t *b) { (void)ui; (void)a; (void)b; return true; }
void gal_ui_show_page(gal_ui_t *ui, gal_page_t page) { (void)ui; (void)page; }
void gal_ui_set_brand(gal_ui_t *ui, const char *a, const char *b) { (void)ui; (void)a; (void)b; }
void gal_ui_set_text(gal_ui_t *ui, const char *a, const char *b, uint32_t c, uint16_t scene) { (void)ui; (void)a; (void)b; (void)c; (void)scene; }
void gal_ui_set_scene(gal_ui_t *ui, uint16_t scene) { (void)ui; (void)scene; }
void gal_ui_show_asset_error(gal_ui_t *ui, const char *text)
{
    if (!ui) return;
    snprintf(g_last_asset_error, sizeof(g_last_asset_error), "%s", text ? text : "");
}
void gal_ui_show_full_text(gal_ui_t *ui)
{
    (void)ui;
    g_show_full_text_calls++;
    g_typing_active = 0;
}
bool gal_ui_typing(const gal_ui_t *ui) { (void)ui; return g_typing_active != 0; }
void gal_ui_set_choice(gal_ui_t *ui, const char *a, const gal_option_t *b, int c, int d)
{
    (void)a;
    (void)b;
    (void)d;
    if (!ui || c < 0) return;
    ui->lists[GAL_LIST_CHOICE].count = c > GAL_LIST_MAX_ROWS ? GAL_LIST_MAX_ROWS : c;
    for (int i = 0; i < GAL_LIST_MAX_ROWS; ++i) ui->lists[GAL_LIST_CHOICE].disabled[i] = false;
}
void gal_ui_set_mode(gal_ui_t *ui, bool a) { (void)ui; (void)a; }
void gal_ui_set_chapter(gal_ui_t *ui, const char *a) { (void)ui; (void)a; }
void gal_ui_set_battery(gal_ui_t *ui, int a) { (void)ui; (void)a; }
void gal_ui_set_notice(gal_ui_t *ui, const char *a) { (void)ui; (void)a; }
void gal_ui_set_font_size(gal_ui_t *ui, bool a) { (void)ui; (void)a; }
void gal_ui_set_panel_transparency(gal_ui_t *ui, uint8_t a) { (void)ui; (void)a; }
void gal_ui_set_rows(gal_ui_t *ui, gal_list_t a, const char *const *b, const char *const *c, int d, int e) { (void)ui; (void)a; (void)b; (void)c; (void)d; (void)e; }
void gal_ui_set_row_value(gal_ui_t *ui, gal_list_t a, int b, const char *c) { (void)ui; (void)a; (void)b; (void)c; }
void gal_ui_select_row(gal_ui_t *ui, gal_list_t a, int b) { (void)ui; (void)a; (void)b; }
void gal_ui_set_row_enabled(gal_ui_t *ui, gal_list_t a, int b, bool c)
{
    if (!ui || a < 0 || a >= GAL_LIST_COUNT || b < 0 || b >= GAL_LIST_MAX_ROWS) return;
    ui->lists[a].disabled[b] = !c;
}
void gal_ui_set_page_title(gal_ui_t *ui, gal_page_t a, const char *b) { (void)ui; (void)a; (void)b; }
void gal_ui_set_save_preview(gal_ui_t *ui, const char *a) { (void)ui; (void)a; }
void gal_ui_set_footer(gal_ui_t *ui, gal_page_t a, const char *b) { (void)ui; (void)a; (void)b; }
void gal_ui_set_chapter_info(gal_ui_t *ui, const char *a) { (void)ui; (void)a; }
void gal_ui_set_action_hint(gal_ui_t *ui, const char *a) { (void)ui; (void)a; }
void gal_ui_set_gallery(gal_ui_t *ui, const gal_cg_t *a, int b, int c) { (void)ui; (void)a; (void)b; (void)c; }
void gal_ui_set_cg(gal_ui_t *ui, const char *a, const char *b, const char *c, const void *d) { (void)ui; (void)a; (void)b; (void)c; (void)d; }
void gal_ui_set_title_image(gal_ui_t *ui, const void *a) { (void)ui; (void)a; }
void gal_ui_set_transition(gal_ui_t *ui, const char *a, const char *b, const char *c) { (void)ui; (void)a; (void)b; (void)c; }
void gal_ui_set_ending(gal_ui_t *ui, const char *a, const char *b, const char *c) { (void)ui; (void)a; (void)b; (void)c; }
void gal_ui_set_about(gal_ui_t *ui, const char *a, const char *b, const char *c) { (void)ui; (void)a; (void)b; (void)c; }
void gal_ui_scroll(gal_ui_t *ui, gal_page_t a, int b) { (void)ui; (void)a; (void)b; }
void gal_ui_show_modal(gal_ui_t *ui, gal_modal_t a, const char *b, const char *c, const char *const *d, int e, int f) { (void)ui; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; }
void gal_ui_hide_modal(gal_ui_t *ui) { (void)ui; }

// —— 媒体层桩：场景号在 0..674 内视为存在，但不产生像素 ——
static const maitetsu_scene_t s_stub_scene = { MAITETSU_SCENE_NONE, 0, 0 };
bool maitetsu_media_init(void) { return g_media_init_result != 0; }
bool maitetsu_media_ready(void) { return g_media_init_result != 0; }
const char *maitetsu_media_error(void) { return "stub media error"; }
bool maitetsu_media_faulted(void) { return g_media_faulted != 0; }
const char *maitetsu_media_fault_text(void) { return "stub strip failure"; }
bool maitetsu_media_init_from(const uint8_t *data, size_t size)
{
    (void)data;
    (void)size;
    return g_media_init_result != 0;
}
uint16_t maitetsu_media_image_count(void) { return 759; }
uint16_t maitetsu_media_scene_count(void) { return 675; }
const lv_image_dsc_t *maitetsu_media_image(uint16_t index) { (void)index; return NULL; }
const maitetsu_scene_t *maitetsu_media_scene(uint16_t scene)
{
    return scene < 675 ? &s_stub_scene : NULL;
}
const lv_image_dsc_t *maitetsu_media_scene_bg(uint16_t scene) { (void)scene; return NULL; }
const lv_image_dsc_t *maitetsu_media_scene_patch(uint16_t scene, uint8_t patch)
{
    (void)scene;
    (void)patch;
    return NULL;
}
const lv_image_dsc_t *maitetsu_media_cg(uint16_t cg) { (void)cg; return NULL; }
const lv_image_dsc_t *maitetsu_media_cg_thumb(uint16_t cg) { (void)cg; return NULL; }
const lv_image_dsc_t *maitetsu_media_title(void) { return NULL; }
