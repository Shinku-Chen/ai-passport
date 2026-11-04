#include "stella_ui.h"
#include "stella_image.h"
#include "lvgl.h"
#include "nvs.h"

#include <stdio.h>
#include <string.h>

// ---- LVGL / fonts ----
const lv_font_t stella_cjk_16 = { 0 };
const lv_font_t stella_cjk_20 = { 0 };
lv_obj_t *lv_screen_active(void) { return NULL; }

// ---- NVS in-memory ----
#define HOST_NVS_SLOTS 64
typedef struct { char key[40]; uint8_t data[512]; size_t len; int used; } host_nvs_entry_t;
static host_nvs_entry_t s_nvs[HOST_NVS_SLOTS];
static host_nvs_entry_t s_pending[HOST_NVS_SLOTS];
static char s_ns[24];
int host_nvs_commit_fail;
int host_nvs_open_fail;
int host_nvs_set_fail;

void host_nvs_reset(void)
{
    memset(s_nvs, 0, sizeof(s_nvs));
    memset(s_pending, 0, sizeof(s_pending));
    s_ns[0] = 0;
    host_nvs_commit_fail = 0;
    host_nvs_open_fail = 0;
    host_nvs_set_fail = 0;
}

static void make_key(char *out, size_t cap, const char *key)
{
    snprintf(out, cap, "%s:%s", s_ns, key);
}

static host_nvs_entry_t *find_entry(host_nvs_entry_t *table, const char *key)
{
    for (int i = 0; i < HOST_NVS_SLOTS; ++i) {
        if (table[i].used && strcmp(table[i].key, key) == 0) return &table[i];
    }
    return NULL;
}

static host_nvs_entry_t *free_entry(host_nvs_entry_t *table)
{
    for (int i = 0; i < HOST_NVS_SLOTS; ++i) if (!table[i].used) return &table[i];
    return NULL;
}

esp_err_t nvs_open(const char *name, int open_mode, nvs_handle_t *out_handle)
{
    (void)open_mode;
    if (host_nvs_open_fail) return ESP_FAIL;
    snprintf(s_ns, sizeof(s_ns), "%s", name ? name : "");
    if (out_handle) *out_handle = 1;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle) { (void)handle; }

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length)
{
    (void)handle;
    if (host_nvs_set_fail) return ESP_FAIL;
    if (!key || !value || length > 512) return ESP_ERR_INVALID_ARG;
    char full[40];
    make_key(full, sizeof(full), key);
    host_nvs_entry_t *e = find_entry(s_pending, full);
    if (!e) e = free_entry(s_pending);
    if (!e) return ESP_FAIL;
    memset(e, 0, sizeof(*e));
    snprintf(e->key, sizeof(e->key), "%s", full);
    memcpy(e->data, value, length);
    e->len = length;
    e->used = 1;
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value, size_t *length)
{
    (void)handle;
    char full[40];
    make_key(full, sizeof(full), key);
    host_nvs_entry_t *e = find_entry(s_nvs, full);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    if (!length || !out_value || *length < e->len) return ESP_ERR_INVALID_ARG;
    memcpy(out_value, e->data, e->len);
    *length = e->len;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    if (host_nvs_commit_fail) {
        // 真实 NVS 提交失败时，本次 set 不应变成可见状态。
        memset(s_pending, 0, sizeof(s_pending));
        return ESP_FAIL;
    }
    for (int i = 0; i < HOST_NVS_SLOTS; ++i) {
        if (!s_pending[i].used) continue;
        host_nvs_entry_t *dst = find_entry(s_nvs, s_pending[i].key);
        if (!dst) dst = free_entry(s_nvs);
        if (dst) *dst = s_pending[i];
    }
    memset(s_pending, 0, sizeof(s_pending));
    return ESP_OK;
}

// ---- battery ----
int host_battery_soc = 87;
int bsp_battery_soc(void) { return host_battery_soc; }

// ---- UI capture ----
int host_page = -1;
char host_ui_text[STELLA_TEXT_BYTES];
char host_ui_name[64];
char host_ui_chapter[64];
char host_ui_ending[512];
char host_ui_prompt[STELLA_TEXT_BYTES];
char host_ui_option[STELLA_TEXT_BYTES];
int host_has_choice;
int host_list_count;
int host_list_selected;
char host_rows[STELLA_LIST_ROWS][72];
char host_values[STELLA_LIST_ROWS][48];
int host_notice_visible;
int host_last_cg_asset = -1;
int host_cg_fail;
int host_scene_fail;
int host_validate_fail;
int host_scene_calls;

void host_ui_reset(void)
{
    host_page = -1;
    host_ui_text[0] = 0;
    host_ui_name[0] = 0;
    host_ui_chapter[0] = 0;
    host_ui_ending[0] = 0;
    host_ui_prompt[0] = 0;
    host_ui_option[0] = 0;
    host_has_choice = 0;
    host_list_count = 0;
    host_list_selected = 0;
    host_notice_visible = 0;
    host_last_cg_asset = -1;
    host_scene_calls = 0;
    host_cg_fail = 0;
    host_scene_fail = 0;
    host_validate_fail = 0;
    memset(host_rows, 0, sizeof(host_rows));
    memset(host_values, 0, sizeof(host_values));
}

bool stella_ui_create(stella_ui_t *ui, lv_obj_t *root, uint16_t *canvas_pixels,
                      const lv_font_t *font16, const lv_font_t *font20)
{
    (void)root; (void)canvas_pixels; (void)font16; (void)font20;
    if (!ui) return false;
    memset(ui, 0, sizeof(*ui));
    return true;
}

void stella_ui_show(stella_ui_t *ui, stella_page_t page) { (void)ui; host_page = (int)page; }

static void copy_row(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src ? src : "");
}

void stella_ui_set_title_rows(stella_ui_t *ui, const char *const *rows, int count, int selected)
{
    (void)ui;
    host_list_count = count;
    host_list_selected = selected;
    for (int i = 0; i < STELLA_LIST_ROWS; ++i) copy_row(host_rows[i], sizeof(host_rows[i]),
                                                       i < count ? rows[i] : "");
}

void stella_ui_set_list(stella_ui_t *ui, const char *title, const char *const *rows,
                        const char *const *values, int count, int selected)
{
    (void)ui; (void)title;
    host_list_count = count;
    host_list_selected = selected;
    for (int i = 0; i < STELLA_LIST_ROWS; ++i) {
        copy_row(host_rows[i], sizeof(host_rows[i]), i < count ? rows[i] : "");
        copy_row(host_values[i], sizeof(host_values[i]), i < count && values ? values[i] : "");
    }
}

void stella_ui_list_select(stella_ui_t *ui, int selected) { (void)ui; host_list_selected = selected; }

void stella_ui_set_text(stella_ui_t *ui, const char *text)
{
    (void)ui;
    copy_row(host_ui_text, sizeof(host_ui_text), text);
}

void stella_ui_set_page(stella_ui_t *ui, int page, int pages)
{ (void)ui; (void)page; (void)pages; }

void stella_ui_set_game(stella_ui_t *ui, const char *name, const char *text, bool large,
                        const char *chapter, int battery)
{
    (void)ui; (void)large; (void)battery;
    copy_row(host_ui_name, sizeof(host_ui_name), name);
    copy_row(host_ui_chapter, sizeof(host_ui_chapter), chapter);
    if (text && text[0]) copy_row(host_ui_text, sizeof(host_ui_text), text);
}

void stella_ui_show_choices(stella_ui_t *ui, const char *prompt, const char *option)
{
    (void)ui;
    copy_row(host_ui_prompt, sizeof(host_ui_prompt), prompt);
    copy_row(host_ui_option, sizeof(host_ui_option), option);
    host_has_choice = 1;
}

void stella_ui_hide_choices(stella_ui_t *ui) { (void)ui; host_has_choice = 0; }
void stella_ui_set_battery(stella_ui_t *ui, int battery) { (void)ui; (void)battery; }
void stella_ui_set_cg_caption(stella_ui_t *ui, const char *caption) { (void)ui; (void)caption; }
void stella_ui_set_history(stella_ui_t *ui, const char *text) { (void)ui; (void)text; }
void stella_ui_history_scroll(stella_ui_t *ui, int dy) { (void)ui; (void)dy; }

void stella_ui_set_ending(stella_ui_t *ui, const char *text)
{
    (void)ui;
    copy_row(host_ui_ending, sizeof(host_ui_ending), text);
}

void stella_ui_set_confirm(stella_ui_t *ui, const char *prompt, const char *yes,
                           const char *no, int selected)
{
    (void)ui; (void)prompt; (void)yes; (void)no; (void)selected;
}

void stella_ui_confirm_select(stella_ui_t *ui, int selected) { (void)ui; (void)selected; }

void stella_ui_notice(stella_ui_t *ui, const char *text) { (void)ui; (void)text; host_notice_visible = 1; }
void stella_ui_notice_hide(stella_ui_t *ui) { (void)ui; host_notice_visible = 0; }

// ---- image layer ----
bool stella_image_init(stella_image_t *img, lv_obj_t *parent, uint16_t *pixels)
{
    (void)parent; (void)pixels;
    if (!img) return false;
    memset(img, 0, sizeof(*img));
    return true;
}
void stella_image_clear(stella_image_t *img, uint16_t top, uint16_t bottom)
{ (void)img; (void)top; (void)bottom; }
bool stella_image_show_scene(stella_image_t *img, const stella_pack_t *pack, uint16_t bg,
                             const uint16_t bodies[3], uint8_t body_count)
{ (void)img; (void)pack; (void)bg; (void)bodies; (void)body_count; host_scene_calls++; return !host_scene_fail; }
bool stella_image_show_title(stella_image_t *img, const stella_pack_t *pack, uint16_t asset)
{ (void)img; (void)pack; (void)asset; return true; }
bool stella_image_show_cg(stella_image_t *img, const stella_pack_t *pack, uint16_t asset)
{ (void)img; (void)pack; if (host_cg_fail) return false; host_last_cg_asset = asset; return true; }
bool stella_image_validate_asset(const stella_pack_t *pack, uint16_t asset)
{ (void)pack; (void)asset; return !host_cg_fail && !host_validate_fail; }
bool stella_image_validate_scene(const stella_pack_t *pack, uint16_t bg,
                                 const uint16_t bodies[3], uint8_t body_count)
{ (void)pack; (void)bg; (void)bodies; (void)body_count; return !host_cg_fail && !host_validate_fail; }
void stella_image_portrait_box(uint8_t count, uint8_t index, const stella_asset_t *asset,
                               int *dx, int *dy, int *dw, int *dh)
{ (void)count; (void)index; (void)asset; if(dx)*dx=0; if(dy)*dy=0; if(dw)*dw=0; if(dh)*dh=0; }
