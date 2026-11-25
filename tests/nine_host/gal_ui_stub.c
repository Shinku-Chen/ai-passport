// Host view of the real gal_ui_t. Behavior mirrors the production typewriter and
// records what the production nine_app asked the UI to show.
#include "gal_ui.h"
#include <stdio.h>
#include <string.h>

#include "gal_stub.h"
static gal_stub_t g;
gal_stub_t *gal_stub(void) { return &g; }
void gal_stub_reset(void) { memset(&g, 0, sizeof(g)); }

bool gal_ui_create(gal_ui_t *ui, const lv_font_t *a, const lv_font_t *b)
{ (void)ui; (void)a; (void)b; return true; }
void gal_ui_show_page(gal_ui_t *ui, gal_page_t page) { (void)ui; g.page = page; }
void gal_ui_set_brand(gal_ui_t *ui, const char *a, const char *b)
{ (void)ui; snprintf(g.brand[0], 96, "%s", a ? a : ""); snprintf(g.brand[1], 96, "%s", b ? b : ""); }
void gal_ui_set_text(gal_ui_t *ui, const char *speaker, const char *text, uint32_t ms,
                     const void *image, const void *body)
{
    (void)speaker; (void)image; (void)body;
    if (!ui) return;
    snprintf(ui->typing_target, sizeof(ui->typing_target), "%s", text ? text : "");
    ui->typing_total = (uint32_t)strlen(ui->typing_target);
    ui->typing_index = 0;
    ui->typing_shown[0] = '\0';
    if (ms == 0 || !text || !*text) {
        snprintf(ui->typing_shown, sizeof(ui->typing_shown), "%s", ui->typing_target);
        ui->typing_index = ui->typing_total;
        ui->typing_active = false;
    } else {
        ui->typing_active = true;
    }
}
void gal_ui_show_full_text(gal_ui_t *ui)
{
    if (!ui) return;
    snprintf(ui->typing_shown, sizeof(ui->typing_shown), "%s", ui->typing_target);
    ui->typing_index = ui->typing_total;
    ui->typing_active = false;
}
bool gal_ui_typing(const gal_ui_t *ui) { return ui && ui->typing_active; }
void gal_ui_set_choice(gal_ui_t *ui, const char *prompt, const gal_option_t *options, int count, int selected)
{
    (void)ui; (void)prompt; (void)selected;
    g.choice_count = count;
    for (int i = 0; i < count && i < GAL_LIST_MAX_ROWS; ++i)
        snprintf(g.choices[i], sizeof(g.choices[i]), "%s", options[i].label ? options[i].label : "");
}
void gal_ui_set_mode(gal_ui_t *ui, bool auto_play) { (void)ui; g.auto_mode = auto_play; }
void gal_ui_set_chapter(gal_ui_t *ui, const char *text) { (void)ui; snprintf(g.chapter, 64, "%s", text ? text : ""); }
void gal_ui_set_battery(gal_ui_t *ui, int percent) { (void)ui; (void)percent; }
void gal_ui_set_notice(gal_ui_t *ui, const char *text) { (void)ui; snprintf(g.notice, sizeof(g.notice), "%s", text ? text : ""); }
void gal_ui_set_font_size(gal_ui_t *ui, bool large) { (void)ui; g.large_font = large; }
void gal_ui_set_panel_transparency(gal_ui_t *ui, uint8_t percent) { (void)ui; (void)percent; }
void gal_ui_set_rows(gal_ui_t *ui, gal_list_t id, const char *const *labels,
                     const char *const *values, int count, int selected)
{
    (void)ui; (void)selected;
    int n = count > GAL_LIST_MAX_ROWS ? GAL_LIST_MAX_ROWS : count;
    g.rows_count[id] = n;
    for (int i = 0; i < n; ++i) {
        snprintf(g.rows[id][i], sizeof(g.rows[id][i]), "%s",
                 labels && labels[i] ? labels[i] : "");
        if (values && values[i] && values[i][0])
            snprintf(g.rows[id][i] + strlen(g.rows[id][i]),
                     sizeof(g.rows[id][i]) - strlen(g.rows[id][i]), " | %s", values[i]);
    }
}
void gal_ui_set_row_value(gal_ui_t *ui, gal_list_t id, int row, const char *value)
{ (void)ui; (void)id; (void)row; (void)value; }
void gal_ui_select_row(gal_ui_t *ui, gal_list_t id, int selected) { (void)ui; (void)id; (void)selected; }
void gal_ui_set_row_enabled(gal_ui_t *ui, gal_list_t id, int row, bool enabled)
{ (void)ui; if (row >= 0 && row < GAL_LIST_MAX_ROWS) g.row_enabled[id][row] = enabled; }
void gal_ui_set_page_title(gal_ui_t *ui, gal_page_t page, const char *text) { (void)ui; (void)page; (void)text; }
void gal_ui_set_save_preview(gal_ui_t *ui, const char *text) { (void)ui; (void)text; }
void gal_ui_set_footer(gal_ui_t *ui, gal_page_t page, const char *text) { (void)ui; (void)page; (void)text; }
void gal_ui_set_chapter_info(gal_ui_t *ui, const char *text) { (void)ui; (void)text; }
void gal_ui_set_action_hint(gal_ui_t *ui, const char *text) { (void)ui; (void)text; }
void gal_ui_set_gallery(gal_ui_t *ui, const gal_cg_t *items, int count, int selected)
{ (void)ui; (void)selected; g.rows_count[GAL_LIST_GALLERY] = count; (void)items; }
void gal_ui_set_cg(gal_ui_t *ui, const char *a, const char *b, const char *c, const void *d)
{ (void)ui; (void)d; snprintf(g.cg[0], 192, "%s", a ? a : ""); snprintf(g.cg[1], 192, "%s", b ? b : ""); snprintf(g.cg[2], 192, "%s", c ? c : ""); }
void gal_ui_set_title_image(gal_ui_t *ui, const void *d) { (void)ui; (void)d; }
void gal_ui_set_transition(gal_ui_t *ui, const char *a, const char *b, const char *c) { (void)ui; (void)a; (void)b; (void)c; }
void gal_ui_set_ending(gal_ui_t *ui, const char *a, const char *b, const char *c)
{ (void)ui; snprintf(g.ending[0], 192, "%s", a ? a : ""); snprintf(g.ending[1], 192, "%s", b ? b : ""); snprintf(g.ending[2], 192, "%s", c ? c : ""); }
void gal_ui_set_about(gal_ui_t *ui, const char *a, const char *b, const char *c) { (void)ui; (void)a; (void)b; (void)c; }
void gal_ui_scroll(gal_ui_t *ui, gal_page_t page, int dy) { (void)ui; (void)page; (void)dy; g.scrolls++; }
void gal_ui_show_modal(gal_ui_t *ui, gal_modal_t kind, const char *a, const char *b,
                       const char *const *actions, int count, int selected)
{ (void)ui; (void)kind; (void)a; (void)b; (void)actions; (void)count; g.modal = true; g.modal_sel = selected; }
void gal_ui_hide_modal(gal_ui_t *ui) { (void)ui; g.modal = false; }
int bsp_battery_soc(void) { return 77; }
static char s_host_screen_storage[1];
lv_obj_t *lv_screen_active(void) { return (lv_obj_t *)s_host_screen_storage; }
bool bsp_lvgl_lock(int timeout_ms) { (void)timeout_ms; return true; }
void bsp_lvgl_unlock(void) {}
const lv_font_t nine_cjk_16 = {0};
const lv_font_t nine_cjk_20 = {0};
