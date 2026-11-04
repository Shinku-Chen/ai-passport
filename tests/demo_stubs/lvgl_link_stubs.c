// Link-time stubs for the baseline demo host tests. The demo pages are not
// part of the Stella application, but validate.sh still compiles their small
// runtime tests; Zig's Windows linker does not drop the unused UI entry points
// the way GNU ld --gc-sections does, so provide no-op definitions here.
#include "demo_test_stubs.h"
#include "ui_pixel.h"

uint32_t lv_color_hex(uint32_t color) { return color; }
lv_obj_t *lv_obj_create(lv_obj_t *parent) { (void)parent; static lv_obj_t object; return &object; }
lv_obj_t *lv_label_create(lv_obj_t *parent) { (void)parent; static lv_obj_t object; return &object; }
void lv_obj_set_size(lv_obj_t *object, int width, int height) { (void)object; (void)width; (void)height; }
void lv_obj_set_width(lv_obj_t *object, int width) { (void)object; (void)width; }
void lv_obj_set_style_radius(lv_obj_t *object, int radius, int selector) { (void)object; (void)radius; (void)selector; }
void lv_obj_set_style_bg_color(lv_obj_t *object, uint32_t color, int selector) { (void)object; (void)color; (void)selector; }
void lv_obj_set_style_border_width(lv_obj_t *object, int width, int selector) { (void)object; (void)width; (void)selector; }
void lv_obj_set_style_text_color(lv_obj_t *object, uint32_t color, int selector) { (void)object; (void)color; (void)selector; }
void lv_obj_set_style_text_align(lv_obj_t *object, int align, int selector) { (void)object; (void)align; (void)selector; }
void lv_obj_center(lv_obj_t *object) { (void)object; }
void lv_obj_align(lv_obj_t *object, int align, int x, int y) { (void)object; (void)align; (void)x; (void)y; }
void lv_obj_delete(lv_obj_t *object) { (void)object; }
void lv_label_set_long_mode(lv_obj_t *object, int mode) { (void)object; (void)mode; }
void lv_screen_load(lv_obj_t *screen) { (void)screen; }
lv_obj_t *ui_pixel_screen_create(const char *title) { (void)title; return NULL; }
lv_obj_t *ui_pixel_panel_create(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color) {
    (void)parent; (void)x; (void)y; (void)w; (void)h; (void)color; return NULL;
}
lv_obj_t *ui_pixel_mascot_create(lv_obj_t *parent, int x, int y) {
    (void)parent; (void)x; (void)y; return NULL;
}
int esp_sleep_get_wakeup_cause(void) { return 0; }
void lv_label_set_text_fmt(lv_obj_t *object, const char *format, ...) { (void)object; (void)format; }
void lv_obj_set_style_text_font(lv_obj_t *object, const lv_font_t *font, int selector) { (void)object; (void)font; (void)selector; }
lv_timer_t *lv_timer_create(void (*callback)(lv_timer_t *), unsigned period, void *data) {
    (void)callback; (void)period; (void)data; return NULL;
}
void lv_timer_delete(lv_timer_t *timer) { (void)timer; }
esp_err_t esp_wifi_scan_get_ap_num(uint16_t *count) { if (count) *count = 0; return ESP_OK; }
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *count, wifi_ap_record_t *records) {
    (void)records; if (count) *count = 0; return ESP_OK;
}
