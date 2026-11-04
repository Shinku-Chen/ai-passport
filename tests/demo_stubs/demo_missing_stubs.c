// tests/demo_stubs/demo_missing_stubs.c —— baseline demo 运行时测试用的空实现。
//
// 这些符号只在 demo_*.c 里被引用。ELF 链接器用 --gc-sections 时它们会被丢弃,
// 但 Windows/COFF 链接器(zig cc / lld-link)不做同样的裁剪,所以这里补一份
// 无副作用的实现,让 baseline 的 4 个 demo 测试在所有平台上都能真正链接运行。
#include "demo_test_stubs.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>

// ---- LVGL ----
uint32_t lv_color_hex(uint32_t color) { return color; }

lv_obj_t *lv_obj_create(lv_obj_t *parent) { (void)parent; return NULL; }
lv_obj_t *lv_label_create(lv_obj_t *parent) { (void)parent; return NULL; }
void lv_obj_set_size(lv_obj_t *o, int w, int h) { (void)o; (void)w; (void)h; }
void lv_obj_set_width(lv_obj_t *o, int w) { (void)o; (void)w; }
void lv_obj_set_style_radius(lv_obj_t *o, int r, int s) { (void)o; (void)r; (void)s; }
void lv_obj_set_style_bg_color(lv_obj_t *o, uint32_t c, int s) { (void)o; (void)c; (void)s; }
void lv_obj_set_style_border_width(lv_obj_t *o, int w, int s) { (void)o; (void)w; (void)s; }
void lv_obj_set_style_text_color(lv_obj_t *o, uint32_t c, int s) { (void)o; (void)c; (void)s; }
void lv_obj_set_style_text_align(lv_obj_t *o, int a, int s) { (void)o; (void)a; (void)s; }
void lv_obj_set_style_text_font(lv_obj_t *o, const lv_font_t *f, int s)
{
    (void)o; (void)f; (void)s;
}
void lv_obj_center(lv_obj_t *o) { (void)o; }
void lv_obj_align(lv_obj_t *o, int a, int x, int y) { (void)o; (void)a; (void)x; (void)y; }
void lv_obj_delete(lv_obj_t *o) { (void)o; }
void lv_label_set_text_fmt(lv_obj_t *o, const char *fmt, ...)
{
    (void)o; (void)fmt;
}
void lv_label_set_long_mode(lv_obj_t *o, int m) { (void)o; (void)m; }
void lv_screen_load(lv_obj_t *s) { (void)s; }
lv_timer_t *lv_timer_create(void (*cb)(lv_timer_t *), unsigned period, void *data)
{
    (void)cb; (void)period; (void)data;
    return NULL;
}
void lv_timer_delete(lv_timer_t *t) { (void)t; }

// ---- ui_pixel(首页吉祥物/面板)----
static int s_dummy_obj;

lv_obj_t *ui_pixel_screen_create(const char *title)
{
    (void)title;
    return (lv_obj_t *)&s_dummy_obj;
}

lv_obj_t *ui_pixel_panel_create(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    (void)parent; (void)x; (void)y; (void)w; (void)h; (void)color;
    return NULL;
}

lv_obj_t *ui_pixel_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color)
{
    (void)parent; (void)text; (void)font; (void)color;
    return NULL;
}

lv_obj_t *ui_pixel_mascot_create(lv_obj_t *parent, int x, int y)
{
    (void)parent; (void)x; (void)y;
    return NULL;
}


// ---- 深睡(只被 demo_low_power 引用)----
int esp_sleep_get_wakeup_cause(void) { return 0; }

// ---- Wi-Fi 扫描(只被 demo_wifi 引用)----
esp_err_t esp_wifi_scan_get_ap_num(uint16_t *count)
{
    if (count) *count = 0;
    return ESP_OK;
}

esp_err_t esp_wifi_scan_get_ap_records(uint16_t *count, wifi_ap_record_t *records)
{
    (void)records;
    if (count) *count = 0;
    return ESP_OK;
}
