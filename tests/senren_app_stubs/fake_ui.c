#include "fake_ui.h"
#include <string.h>
#include <stdarg.h>
const lv_font_t lv_font_montserrat_14 = {0};
fake_ui_state_t g_fake_ui;
int fake_log(const char *fmt, ...) { (void)fmt; va_list ap; va_start(ap, fmt); va_end(ap); return 0; }
int bsp_battery_soc(void) { return 100; }
bool atri_ui_create(atri_ui_t *ui, uint16_t *art, const lv_font_t *font) { (void)ui; (void)art; (void)font; memset(&g_fake_ui, 0, sizeof(g_fake_ui)); return true; }
void atri_ui_show_page(atri_ui_t *ui, atri_page_t page) { (void)ui; g_fake_ui.page = page; }
void atri_ui_compose(atri_ui_t *ui, const senren_pack_t *pack, const char *bg, const char *sprite, const char *ev) { (void)ui; (void)pack; (void)bg; (void)sprite; (void)ev; }
void atri_ui_set_text(atri_ui_t *ui, const char *speaker, const char *text, uint32_t ms) { (void)ui; (void)speaker; (void)text; (void)ms; }
void atri_ui_finish_typing(atri_ui_t *ui) { (void)ui; }
bool atri_ui_typing(const atri_ui_t *ui) { (void)ui; return false; }
void atri_ui_set_progress(atri_ui_t *ui, const char *chapter, int page, int pages) { (void)ui; (void)chapter; (void)page; (void)pages; }
void atri_ui_set_auto(atri_ui_t *ui, bool on) { (void)ui; (void)on; }
void atri_ui_set_battery(atri_ui_t *ui, int percent) { (void)ui; (void)percent; }
void atri_ui_show_choices(atri_ui_t *ui, const char *const *texts, int count) { (void)ui; (void)texts; g_fake_ui.choice_count = count; g_fake_ui.choice_selected = 0; }
void atri_ui_hide_choices(atri_ui_t *ui) { (void)ui; g_fake_ui.choice_count = 0; }
void atri_ui_set_choice_selected(atri_ui_t *ui, int index) { (void)ui; if (g_fake_ui.choice_count <= 0) return; if (index < 0) index = 0; if (index >= g_fake_ui.choice_count) index = g_fake_ui.choice_count-1; g_fake_ui.choice_selected = index; }
int atri_ui_choice_selected(const atri_ui_t *ui) { (void)ui; return g_fake_ui.choice_selected; }
void atri_ui_set_list(atri_ui_t *ui, atri_list_id_t id, const char *title, const char *hint, const char *const *labels, const char *const *values, int count, int selected) { (void)ui; (void)title; (void)hint; (void)selected; if (id < 0 || id >= ATRI_LIST_COUNT || count > ATRI_UI_MAX_ROWS) return; g_fake_ui.list_count[id] = count; for (int i=0;i<count;i++) { strncpy(g_fake_ui.list_labels[id][i], labels && labels[i] ? labels[i] : "", 63); strncpy(g_fake_ui.list_values[id][i], values && values[i] ? values[i] : "", 63); } }
void atri_ui_set_list_selected(atri_ui_t *ui, atri_list_id_t id, int selected) { (void)ui; (void)id; (void)selected; }
void atri_ui_set_list_value(atri_ui_t *ui, atri_list_id_t id, int row, const char *value) { (void)ui; if (id < 0 || id >= ATRI_LIST_COUNT || row < 0 || row >= ATRI_UI_MAX_ROWS) return; strncpy(g_fake_ui.list_values[id][row], value ? value : "", 63); }
void atri_ui_set_ending(atri_ui_t *ui, const char *kicker, const char *name, const char *hint) { (void)ui; (void)kicker; (void)hint; strncpy(g_fake_ui.ending_name, name ? name : "", 63); }
void atri_ui_set_warning(atri_ui_t *ui, const char *body, const char *hint) { (void)ui; (void)body; (void)hint; }
void atri_ui_set_about(atri_ui_t *ui, const char *body) { (void)ui; (void)body; }
int atri_ui_set_about_scroll(atri_ui_t *ui, int y) { (void)ui; return y; }
void atri_ui_notice(atri_ui_t *ui, const char *text) { (void)ui; strncpy(g_fake_ui.notice, text ? text : "", 63); }
