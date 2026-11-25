#include "gal_ui_stub.h"

#include <string.h>

gal_page_t host_page;
char host_text[2048];
char host_speaker[64];
bool host_typing;
bool host_auto;
char host_notice[64];
int host_choice_count;
char host_choice_labels[16][64];
int host_choice_sel;
int host_row_count[GAL_LIST_COUNT];
char host_rows[GAL_LIST_COUNT][32][64];
char host_values[GAL_LIST_COUNT][32][64];
bool host_row_enabled[GAL_LIST_COUNT][32];
bool host_modal;
char host_ending_eyebrow[32];
char host_ending_title[96];
int host_font_large;
uint8_t host_transparency = 30;
unsigned host_visuals;
unsigned host_assets;

static void copy(char *dst, size_t cap, const char *src)
{
    if (!cap) return;
    size_t n = src ? strlen(src) : 0;
    if (n >= cap) n = cap - 1;
    if (n) memcpy(dst, src, n);
    dst[n] = 0;
}

bool gal_ui_create(gal_ui_t *ui, const lv_font_t *a, const lv_font_t *b)
{
    memset(ui, 0, sizeof(*ui));
    ui->font_small = a;
    ui->font_large = b;
    return true;
}
void gal_ui_show_page(gal_ui_t *ui, gal_page_t page) { (void)ui; host_page = page; }
void gal_ui_set_brand(gal_ui_t *ui, const char *a, const char *b) { (void)ui; (void)a; (void)b; }
void gal_ui_set_text(gal_ui_t *ui, const char *speaker, const char *text, uint32_t ms,
                     const void *image, const void *body)
{
    (void)ui; (void)image; (void)body;
    copy(host_speaker, sizeof(host_speaker), speaker);
    copy(host_text, sizeof(host_text), text);
    host_typing = ms > 0 && text && text[0];
}
void gal_ui_show_full_text(gal_ui_t *ui) { (void)ui; host_typing = false; }
bool gal_ui_typing(const gal_ui_t *ui) { (void)ui; return host_typing; }
void gal_ui_set_choice(gal_ui_t *ui, const char *prompt, const gal_option_t *options,
                       int count, int selected)
{
    (void)ui;
    copy(host_text, sizeof(host_text), prompt);
    host_choice_count = count;
    host_choice_sel = selected;
    for (int i = 0; i < 16; ++i) {
        host_choice_labels[i][0] = 0;
        if (i < count && options && options[i].label) {
            copy(host_choice_labels[i], sizeof(host_choice_labels[i]), options[i].label);
        }
    }
}
void gal_ui_set_mode(gal_ui_t *ui, bool auto_play) { (void)ui; host_auto = auto_play; }
void gal_ui_set_chapter(gal_ui_t *ui, const char *text) { (void)ui; (void)text; }
void gal_ui_set_battery(gal_ui_t *ui, int percent) { (void)ui; (void)percent; }
void gal_ui_set_page_hud(gal_ui_t *ui, const char *text) { (void)ui; (void)text; }
void gal_ui_set_notice(gal_ui_t *ui, const char *text)
{
    (void)ui;
    copy(host_notice, sizeof(host_notice), text);
}
void gal_ui_set_font_size(gal_ui_t *ui, bool large) { (void)ui; host_font_large = large; }
void gal_ui_set_panel_transparency(gal_ui_t *ui, uint8_t percent)
{
    (void)ui;
    host_transparency = percent;
}
void gal_ui_set_rows(gal_ui_t *ui, gal_list_t list, const char *const *labels,
                     const char *const *values, int count, int selected)
{
    (void)ui; (void)selected;
    if (list < 0 || list >= GAL_LIST_COUNT) return;
    host_row_count[list] = count;
    for (int i = 0; i < 32; ++i) {
        host_rows[list][i][0] = 0;
        host_values[list][i][0] = 0;
        if (i < count && labels && labels[i]) copy(host_rows[list][i], 64, labels[i]);
        if (i < count && values && values[i]) copy(host_values[list][i], 64, values[i]);
    }
}
void gal_ui_set_row_value(gal_ui_t *ui, gal_list_t list, int row, const char *value)
{
    (void)ui; (void)list; (void)row; (void)value;
}
void gal_ui_select_row(gal_ui_t *ui, gal_list_t list, int selected)
{
    (void)ui; (void)list; (void)selected;
}
void gal_ui_set_row_enabled(gal_ui_t *ui, gal_list_t list, int row, bool enabled)
{
    (void)ui;
    if (list >= 0 && list < GAL_LIST_COUNT && row >= 0 && row < 32)
        host_row_enabled[list][row] = enabled;
}
void gal_ui_set_page_title(gal_ui_t *ui, gal_page_t page, const char *title)
{
    (void)ui; (void)page; (void)title;
}
void gal_ui_set_save_preview(gal_ui_t *ui, const char *text) { (void)ui; (void)text; }
void gal_ui_set_footer(gal_ui_t *ui, gal_page_t page, const char *text)
{
    (void)ui; (void)page; (void)text;
}
void gal_ui_set_chapter_info(gal_ui_t *ui, const char *text) { (void)ui; (void)text; }
void gal_ui_set_action_hint(gal_ui_t *ui, const char *text) { (void)ui; (void)text; }
void gal_ui_set_gallery(gal_ui_t *ui, const gal_cg_t *items, int count, int selected)
{
    (void)ui; (void)items; (void)count; (void)selected;
}
void gal_ui_set_cg(gal_ui_t *ui, const char *index_text, const char *name,
                   const char *caption, const void *image)
{
    (void)ui; (void)index_text; (void)name; (void)caption; (void)image;
}
void gal_ui_set_title_image(gal_ui_t *ui, const void *image) { (void)ui; (void)image; }
void gal_ui_set_transition(gal_ui_t *ui, const char *a, const char *b, const char *c)
{
    (void)ui; (void)a; (void)b; (void)c;
}
void gal_ui_set_ending(gal_ui_t *ui, const char *a, const char *b, const char *c)
{
    (void)ui; (void)c;
    copy(host_ending_eyebrow, sizeof(host_ending_eyebrow), a);
    copy(host_ending_title, sizeof(host_ending_title), b);
}
void gal_ui_set_about(gal_ui_t *ui, const char *a, const char *b, const char *c)
{
    (void)ui; (void)a; (void)b; (void)c;
}
void gal_ui_scroll(gal_ui_t *ui, gal_page_t page, int dy) { (void)ui; (void)page; (void)dy; }
void gal_ui_show_modal(gal_ui_t *ui, gal_modal_t kind, const char *title, const char *body,
                       const char *const *actions, int count, int selected)
{
    (void)ui; (void)kind; (void)title; (void)body; (void)actions; (void)count; (void)selected;
    host_modal = true;
}
void gal_ui_hide_modal(gal_ui_t *ui) { (void)ui; host_modal = false; }
void lv_obj_add_flag(lv_obj_t *obj, uint32_t flag) { (void)obj; (void)flag; }
lv_obj_t *lv_screen_active(void) { return NULL; }
void lv_label_set_text(lv_obj_t *obj, const char *text) { (void)obj; (void)text; }
void lv_refr_now(void *disp) { (void)disp; }
