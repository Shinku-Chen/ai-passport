#include "cafe_ui.h"
#include <stdio.h>
#include <string.h>
static void copy(char *d, size_t n, const char *s) { if (n) snprintf(d, n, "%s", s ? s : ""); }
bool cafe_ui_create(cafe_ui_t *ui) { (void)ui; return true; }
void cafe_ui_show_page(cafe_ui_t *ui, cafe_page_t page) { if (ui) ui->page = page; }
void cafe_ui_set_brand(cafe_ui_t *ui, const char *t, const char *s) { (void)ui; (void)t; (void)s; }
void cafe_ui_set_text(cafe_ui_t *ui, const char *speaker, const char *text, uint32_t ms)
{
    if (!ui) return;
    copy(ui->speaker, sizeof(ui->speaker), speaker);
    copy(ui->text, sizeof(ui->text), text);
    ui->typing = ms > 0;
}
void cafe_ui_show_full_text(cafe_ui_t *ui) { if (ui) ui->typing = false; }
bool cafe_ui_typing(const cafe_ui_t *ui) { return ui && ui->typing; }
void cafe_ui_set_chapter(cafe_ui_t *ui, const char *t) { (void)ui; (void)t; }
void cafe_ui_set_battery(cafe_ui_t *ui, int p) { (void)ui; (void)p; }
void cafe_ui_set_notice(cafe_ui_t *ui, const char *t) { if (ui) ui->notice_seq += t && t[0] ? 1 : 0; }
void cafe_ui_set_mode(cafe_ui_t *ui, bool auto_play) { if (ui) ui->auto_mode = auto_play; }
void cafe_ui_set_font_large(cafe_ui_t *ui, bool large) { (void)ui; (void)large; }
void cafe_ui_set_panel_transparency(cafe_ui_t *ui, uint8_t p) { (void)ui; (void)p; }
void cafe_ui_set_rows(cafe_ui_t *ui, cafe_list_t list, const char *const *labels,
                      const char *const *values, int count, int selected)
{ (void)list; (void)labels; (void)values; if (ui) { ui->rows_count = count; ui->rows_selected = selected; } }
void cafe_ui_set_row_enabled(cafe_ui_t *ui, cafe_list_t list, int row, bool enabled)
{ (void)ui; (void)list; (void)row; (void)enabled; }
void cafe_ui_select_row(cafe_ui_t *ui, cafe_list_t list, int selected)
{ (void)list; if (ui) ui->rows_selected = selected; }
void cafe_ui_set_page_title(cafe_ui_t *ui, cafe_page_t page, const char *t) { (void)ui; (void)page; (void)t; }
void cafe_ui_set_footer(cafe_ui_t *ui, cafe_page_t page, const char *t) { (void)ui; (void)page; (void)t; }
void cafe_ui_set_save_preview(cafe_ui_t *ui, const char *t) { (void)ui; (void)t; }
void cafe_ui_set_chapter_info(cafe_ui_t *ui, const char *t) { (void)ui; (void)t; }
void cafe_ui_set_choice(cafe_ui_t *ui, const char *const *labels, const bool *enabled,
                        int count, int selected)
{ (void)labels; (void)enabled; if (ui) { ui->choice_count = count; ui->choice_selected = selected; } }
void cafe_ui_set_gallery_tiles(cafe_ui_t *ui, const char *const *labels,
                               const uint16_t *image_ids, const bool *unlocked,
                               int count, int selected)
{ (void)labels; (void)image_ids; (void)unlocked; if (ui) { ui->rows_count = count; ui->rows_selected = selected; } }
void cafe_ui_set_cg(cafe_ui_t *ui, const char *a, const char *b, const char *c)
{ (void)ui; (void)a; (void)b; (void)c; }
void cafe_ui_set_transition(cafe_ui_t *ui, const char *a, const char *b, const char *c)
{ (void)ui; (void)a; (void)b; (void)c; }
void cafe_ui_set_transition_op(cafe_ui_t *ui, const char *a) { (void)ui; (void)a; }
void cafe_ui_set_ending(cafe_ui_t *ui, const char *a, const char *b, const char *c)
{ (void)ui; (void)a; (void)b; (void)c; }
void cafe_ui_set_about(cafe_ui_t *ui, const char *t) { (void)ui; (void)t; }
void cafe_ui_scroll_about(cafe_ui_t *ui, int dy) { (void)ui; (void)dy; }
void cafe_ui_show_modal(cafe_ui_t *ui, const char *a, const char *b, const char *const *c, int d, int e)
{ (void)ui; (void)a; (void)b; (void)c; (void)d; (void)e; }
void cafe_ui_hide_modal(cafe_ui_t *ui) { (void)ui; }
void cafe_ui_bind_art(cafe_ui_t *ui, cafe_bands_t *bands) { (void)ui; (void)bands; }
void cafe_ui_art_visible(cafe_ui_t *ui, bool visible) { if (ui) ui->art_visible = visible; }

void cafe_ui_art_invalidate(cafe_ui_t *ui) { (void)ui; }
