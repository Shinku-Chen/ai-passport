#include "gal_ui.h"
#include "nine_ui_stub.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

char nine_ui_text[2048];
char nine_ui_speaker[128];
char nine_ui_notice[128];
char nine_ui_ending[3][512];
int nine_ui_page;
int nine_ui_choice_count;
char nine_ui_choice[16][128];

struct _lv_obj_t { int dummy; };
static lv_obj_t s_obj;
static lv_font_t s_font;

lv_obj_t *lv_screen_active(void) { return &s_obj; }
void lv_obj_move_background(lv_obj_t *obj) { (void)obj; }
void lv_obj_clean(lv_obj_t *obj) { (void)obj; }
void lv_obj_set_style_bg_color(lv_obj_t *obj, lv_color_t color, uint32_t part) { (void)obj; (void)color; (void)part; }
void lv_obj_set_style_bg_opa(lv_obj_t *obj, uint8_t opa, uint32_t part) { (void)obj; (void)opa; (void)part; }
void lv_obj_set_style_text_font(lv_obj_t *obj, const lv_font_t *font, uint32_t part) { (void)obj; (void)font; (void)part; }
void lv_obj_set_style_text_color(lv_obj_t *obj, lv_color_t color, uint32_t part) { (void)obj; (void)color; (void)part; }
void lv_obj_set_width(lv_obj_t *obj, int32_t w) { (void)obj; (void)w; }
void lv_obj_set_pos(lv_obj_t *obj, int32_t x, int32_t y) { (void)obj; (void)x; (void)y; }
void lv_label_set_long_mode(lv_obj_t *obj, int32_t mode) { (void)obj; (void)mode; }
lv_obj_t *lv_label_create(lv_obj_t *parent) { (void)parent; return &s_obj; }
void lv_label_set_text(lv_obj_t *obj, const char *text) { (void)obj; (void)text; }
void lv_refr_now(void *disp) { (void)disp; }
lv_color_t lv_color_black(void) { lv_color_t c = {0,0,0}; return c; }
lv_color_t lv_color_white(void) { lv_color_t c = {255,255,255}; return c; }

void nine_ui_stub_reset(void)
{
    memset(nine_ui_text, 0, sizeof(nine_ui_text));
    memset(nine_ui_speaker, 0, sizeof(nine_ui_speaker));
    memset(nine_ui_notice, 0, sizeof(nine_ui_notice));
    memset(nine_ui_ending, 0, sizeof(nine_ui_ending));
    nine_ui_page = -1;
    nine_ui_choice_count = 0;
}

bool gal_ui_create(gal_ui_t *ui, const lv_font_t *a, const lv_font_t *b)
{
    if (!ui) return false;
    ui->font_small = a ? a : &s_font;
    ui->font_large = b ? b : &s_font;
    return true;
}
void gal_ui_show_page(gal_ui_t *ui, gal_page_t page) { (void)ui; nine_ui_page = (int)page; }
void gal_ui_set_brand(gal_ui_t *ui, const char *a, const char *b) { (void)ui; (void)a; (void)b; }
void gal_ui_set_text(gal_ui_t *ui, const char *speaker, const char *text, uint32_t ms,
                     const void *image, const void *body)
{
    (void)image; (void)body;
    snprintf(nine_ui_speaker, sizeof(nine_ui_speaker), "%s", speaker ? speaker : "");
    snprintf(nine_ui_text, sizeof(nine_ui_text), "%s", text ? text : "");
    if (ui) {
        ui->typing_total = (uint32_t)strlen(text ? text : "");
        ui->typing_index = 0;
        ui->typing_active = ms > 0 && text && text[0];
    }
}
void gal_ui_show_full_text(gal_ui_t *ui) { if (ui) { ui->typing_active = false; ui->typing_index = ui->typing_total; } }
bool gal_ui_typing(const gal_ui_t *ui) { return ui && ui->typing_active; }
void gal_ui_set_choice(gal_ui_t *ui, const char *prompt, const gal_option_t *options,
                       int count, int selected)
{
    (void)prompt; (void)selected;
    nine_ui_choice_count = count;
    if (ui) ui->lists[GAL_LIST_CHOICE].count = count;
    for (int i = 0; i < count && i < 16; ++i)
        snprintf(nine_ui_choice[i], sizeof(nine_ui_choice[i]), "%s", options[i].label ? options[i].label : "");
}
void gal_ui_set_mode(gal_ui_t *ui, bool auto_play) { (void)ui; (void)auto_play; }
void gal_ui_set_chapter(gal_ui_t *ui, const char *text) { (void)ui; (void)text; }
void gal_ui_set_battery(gal_ui_t *ui, int percent) { (void)ui; (void)percent; }
void gal_ui_set_notice(gal_ui_t *ui, const char *text)
{
    (void)ui;
    snprintf(nine_ui_notice, sizeof(nine_ui_notice), "%s", text ? text : "");
}
void gal_ui_set_font_size(gal_ui_t *ui, bool large) { (void)ui; (void)large; }
void gal_ui_set_panel_transparency(gal_ui_t *ui, uint8_t percent) { (void)ui; (void)percent; }
void gal_ui_set_rows(gal_ui_t *ui, gal_list_t id, const char *const *labels,
                     const char *const *values, int count, int selected)
{
    (void)labels; (void)values; (void)selected;
    if (ui && id < GAL_LIST_COUNT) ui->lists[id].count = count;
}
void gal_ui_set_row_value(gal_ui_t *ui, gal_list_t id, int row, const char *value) { (void)ui; (void)id; (void)row; (void)value; }
void gal_ui_select_row(gal_ui_t *ui, gal_list_t id, int selected) { (void)ui; (void)id; (void)selected; }
void gal_ui_set_row_enabled(gal_ui_t *ui, gal_list_t id, int row, bool enabled) { (void)ui; (void)id; (void)row; (void)enabled; }
void gal_ui_set_page_title(gal_ui_t *ui, gal_page_t page, const char *title) { (void)ui; (void)page; (void)title; }
void gal_ui_set_save_preview(gal_ui_t *ui, const char *text) { (void)ui; (void)text; }
void gal_ui_set_footer(gal_ui_t *ui, gal_page_t page, const char *text) { (void)ui; (void)page; (void)text; }
void gal_ui_set_chapter_info(gal_ui_t *ui, const char *text) { (void)ui; (void)text; }
void gal_ui_set_action_hint(gal_ui_t *ui, const char *text) { (void)ui; (void)text; }
void gal_ui_set_gallery(gal_ui_t *ui, const gal_cg_t *items, int count, int selected) { (void)ui; (void)items; (void)count; (void)selected; }
void gal_ui_set_cg(gal_ui_t *ui, const char *index_text, const char *name,
                   const char *caption, const void *image)
{
    (void)ui; (void)image;
    snprintf(nine_ui_ending[0], sizeof(nine_ui_ending[0]), "%s", index_text ? index_text : "");
    snprintf(nine_ui_ending[1], sizeof(nine_ui_ending[1]), "%s", name ? name : "");
    snprintf(nine_ui_ending[2], sizeof(nine_ui_ending[2]), "%s", caption ? caption : "");
}
void gal_ui_set_title_image(gal_ui_t *ui, const void *image) { (void)ui; (void)image; }
void gal_ui_set_transition(gal_ui_t *ui, const char *a, const char *b, const char *c) { (void)ui; (void)a; (void)b; (void)c; }
void gal_ui_set_ending(gal_ui_t *ui, const char *eyebrow, const char *title, const char *body)
{
    (void)ui;
    snprintf(nine_ui_ending[0], sizeof(nine_ui_ending[0]), "%s", eyebrow ? eyebrow : "");
    snprintf(nine_ui_ending[1], sizeof(nine_ui_ending[1]), "%s", title ? title : "");
    snprintf(nine_ui_ending[2], sizeof(nine_ui_ending[2]), "%s", body ? body : "");
}
void gal_ui_set_about(gal_ui_t *ui, const char *a, const char *b, const char *c) { (void)ui; (void)a; (void)b; (void)c; }
void gal_ui_scroll(gal_ui_t *ui, gal_page_t page, int dy) { (void)ui; (void)page; (void)dy; }
void gal_ui_show_modal(gal_ui_t *ui, gal_modal_t kind, const char *title, const char *body,
                       const char *const *actions, int count, int selected)
{ (void)ui; (void)kind; (void)title; (void)body; (void)actions; (void)count; (void)selected; }
void gal_ui_hide_modal(gal_ui_t *ui) { (void)ui; }
void lv_mem_monitor(lv_mem_monitor_t *mon) { if (mon) { mon->total_size = 28672; mon->free_size = 20000; mon->used_pct = 30; } }
