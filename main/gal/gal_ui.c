// main/gal/gal_ui.c —— 框架骨架的 LVGL 界面实现(竖屏 240x320)。
//
// 每个逻辑屏一个页面根对象,创建时全部建好,切换时只改可见性。
// 列表行按需创建;行列表组件在标题菜单、选项、槽位、设置、历史、
// 章节与结局入口之间复用。
#include "gal_ui.h"

#include "esp_log.h"

#include "gal_settings.h"   // gal_panel_opa()：透明度百分比换算

#include <stdio.h>
#include <string.h>

static const char *TAG = "gal_ui";

// 骨架配色:深蓝紫渐变 + 青绿色强调(与原型 docs/design 一致)。
#define COL_BG_TOP    0x1C2A4A
#define COL_BG_BOT    0x2E2440
#define COL_PANEL     0x0B1020
#define COL_TEXT      0xEDF1EE
#define COL_DIM       0x87919A
#define COL_ACCENT    0x74D3A0
#define COL_NAME      0x9FE0BE
#define COL_ROW       0x1B2026
#define COL_ROW_SEL   0x2B5F45
#define COL_ROW_DIS   0x697386
#define COL_PAGE_BG   0x0A0C0F
#define COL_GOLD      0xE0B7AC
#define COL_BOX       0x0F1625
#define COL_FRAME     0x40506A
#define COL_TILE_A    0x526782
#define COL_TILE_B    0x8D708A
#define COL_TILE_C    0xD49B83

// 版面常量(竖屏 240x320)。
#define BAR_Y 6          // 页头标题
#define BODY_TOP 44      // 页头下方的正文起点
#define ROW_H 34         // 列表行高(含 4px 间距)
#define ROW_INSET 12     // 列表左右留白

// ---------------------------------------------------------------- 图片
// 有图就显示,没有就隐藏,露出程序绘制的渐变。
static void apply_image(lv_obj_t *img, const void *dsc)
{
    if (!img) return;
    if (dsc) {
        lv_image_set_src(img, dsc);
        lv_obj_remove_flag(img, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    }
}

// ---------------------------------------------------------------- 打字机
static void typing_stop(gal_ui_t *ui)
{
    ui->typing_active = false;
    if (ui->typewriter_timer) lv_timer_pause(ui->typewriter_timer);
}

static void typing_cb(lv_timer_t *timer)
{
    gal_ui_t *ui = (gal_ui_t *)lv_timer_get_user_data(timer);
    if (!ui || !ui->typing_active) return;
    // 每 tick 推进 2 个字符:把定时器中断与重绘开销摊薄。
    for (int n = 0; n < 2 && ui->typing_index < ui->typing_total; ++n) {
        const uint8_t *src = (const uint8_t *)ui->typing_target;
        size_t step = 1;
        const uint8_t lead = src[ui->typing_index];
        if ((lead & 0xE0u) == 0xC0u) step = 2;
        else if ((lead & 0xF0u) == 0xE0u) step = 3;
        else if ((lead & 0xF8u) == 0xF0u) step = 4;
        if (ui->typing_index + step > ui->typing_total) step = 1;
        memcpy(ui->typing_shown + ui->typing_index, src + ui->typing_index, step);
        ui->typing_index += (uint32_t)step;
    }
    ui->typing_shown[ui->typing_index] = '\0';
    lv_label_set_text(ui->text, ui->typing_shown);
    if (ui->typing_index >= ui->typing_total) typing_stop(ui);
}

void gal_ui_show_full_text(gal_ui_t *ui)
{
    if (!ui || !ui->typing_active) return;
    ui->typing_index = ui->typing_total;
    memcpy(ui->typing_shown, ui->typing_target, ui->typing_total);
    ui->typing_shown[ui->typing_total] = '\0';
    lv_label_set_text(ui->text, ui->typing_shown);
    typing_stop(ui);
}

bool gal_ui_typing(const gal_ui_t *ui)
{
    return ui && ui->typing_active;
}

#ifdef NINE_READER
// A label sized with LV_SIZE_CONTENT wraps an overlong name before the max
// width can apply. Measure the real glyph run instead and pin the pill to
// min(measured+padding, 160); names beyond it are manually shortened with
// a real font-metric prefix plus "...".
static void reader_fit_name(gal_ui_t *ui, const char *name)
{
    if (!ui || !ui->name || !name || !name[0]) return;
    lv_point_t sz;
    lv_text_get_size(&sz, name, ui->font_small, 0, 0, 10000, LV_TEXT_FLAG_NONE);
    int32_t w = sz.x + 16;
    char shown[256];
    if (w > GAL_NAME_MAX_W) {
        const lv_font_t *font = ui->font_small;
        const char *ell = "...";
        lv_point_t es;
        lv_text_get_size(&es, ell, font, 0, 0, 10000, LV_TEXT_FLAG_NONE);
        int32_t budget = GAL_NAME_MAX_W - 16 - es.x;
        size_t best = 0;
        for (size_t i = 1; name[i]; ++i) {
            if (((unsigned char)name[i] & 0xC0) == 0x80) continue; // UTF-8 boundary
            char tmp[256];
            size_t n = i < sizeof(tmp) - 1 ? i : sizeof(tmp) - 1;
            memcpy(tmp, name, n); tmp[n] = 0;
            lv_point_t ps;
            lv_text_get_size(&ps, tmp, font, 0, 0, 10000, LV_TEXT_FLAG_NONE);
            if (ps.x <= budget) best = i;
            else break;
        }
        if (best >= sizeof(shown) - 4) best = sizeof(shown) - 4;
        memcpy(shown, name, best);
        strcpy(shown + best, ell);
        w = GAL_NAME_MAX_W;
    } else {
        if (strlen(name) >= sizeof(shown)) return;
        strcpy(shown, name);
    }
    lv_obj_set_width(ui->name, w);
    lv_label_set_long_mode(ui->name, LV_LABEL_LONG_MODE_CLIP);
    lv_label_set_text(ui->name, shown);
    // One real font line + the 2px vertical padding.
    lv_obj_set_height(ui->name, lv_font_get_line_height(ui->font_small) + 4);
    lv_obj_align_to(ui->name, ui->panel, LV_ALIGN_OUT_TOP_LEFT, 8, -2);
}
#endif

void gal_ui_set_text(gal_ui_t *ui, const char *speaker, const char *page_text,
                     uint32_t ms_per_char, const void *image, const void *body)
{
    if (!ui) return;
    apply_image(ui->scene_img, image);
    apply_image(ui->body_img, body);
    lv_label_set_text(ui->name, speaker ? speaker : "");
    // 旁白不显示名牌,避免一个空胶囊浮在对话框上方。
    if (speaker && speaker[0] != '\0') {
        lv_obj_remove_flag(ui->name, LV_OBJ_FLAG_HIDDEN);
#ifdef NINE_READER
        reader_fit_name(ui, speaker);
#endif
    } else {
        lv_obj_add_flag(ui->name, LV_OBJ_FLAG_HIDDEN);
    }

    const char *src = page_text ? page_text : "";
    size_t len = strlen(src);
    if (len >= sizeof(ui->typing_target)) {
        ESP_LOGW(TAG, "文本超长被截断: %u", (unsigned)len);
        len = sizeof(ui->typing_target) - 1;
    }
    memcpy(ui->typing_target, src, len);
    ui->typing_target[len] = '\0';
    ui->typing_total = (uint32_t)len;
    ui->typing_index = 0;
    ui->typing_shown[0] = '\0';
    ui->typing_step_ms = ms_per_char;
    lv_label_set_text(ui->text, "");

    if (len == 0 || ms_per_char == 0 || !ui->typewriter_timer) {
        memcpy(ui->typing_shown, ui->typing_target, len);
        ui->typing_shown[len] = '\0';
        ui->typing_index = ui->typing_total;
        lv_label_set_text(ui->text, ui->typing_shown);
        typing_stop(ui);
        return;
    }

    ui->typing_active = true;
    lv_timer_set_period(ui->typewriter_timer, ms_per_char < 10 ? 10 : ms_per_char);
    lv_timer_reset(ui->typewriter_timer);
    lv_timer_resume(ui->typewriter_timer);
}

// ---------------------------------------------------------------- 列表
static void list_clear_rows(gal_list_ui_t *list)
{
    for (int i = 0; i < GAL_LIST_MAX_ROWS; ++i) {
        if (list->rows[i]) {
            lv_obj_t *row = lv_obj_get_parent(list->rows[i]);
            if (row) lv_obj_delete(row);
            list->rows[i] = NULL;
            list->values[i] = NULL;
        }
    }
    list->first = 0;
    list->visible = 0;
}

static int list_visible_capacity(const gal_list_ui_t *list)
{
    const int height = list->box ? lv_obj_get_height(list->box) : GAL_UI_H;
    const int usable = height - list->top - 4;
    int rows = usable / (list->row_h ? list->row_h : 1);
    if (rows < 1) rows = 1;
    if (rows > GAL_LIST_MAX_ROWS) rows = GAL_LIST_MAX_ROWS;
    return rows;
}

static lv_obj_t *list_create_row(gal_ui_t *ui, gal_list_t id, int absolute, int slot)
{
    gal_list_ui_t *list = &ui->lists[id];
    const int width = list->width - 2 * list->inset;
    lv_obj_t *row = lv_obj_create(list->page);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, width, list->row_h - 4);
    lv_obj_set_pos(row, list->inset, list->top + slot * list->row_h);
    lv_obj_set_style_radius(row, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row, lv_color_hex(COL_ROW), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, list->label_text[absolute] ? list->label_text[absolute] : "");
    lv_obj_set_style_text_color(label, lv_color_hex(
        list->disabled[absolute] ? COL_ROW_DIS : COL_TEXT), LV_PART_MAIN);
    if (list->wrap) {
        lv_obj_set_width(label, width - 16);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 8, 0);
    } else {
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 10, 0);
#ifdef NINE_READER
        lv_obj_set_width(label, width - 24);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
#endif
    }

    lv_obj_t *value = NULL;
    if (list->value_text[absolute]) {
        value = lv_label_create(row);
        lv_label_set_text(value, list->value_text[absolute]);
        lv_obj_set_style_text_color(value, lv_color_hex(
            list->disabled[absolute] ? COL_ROW_DIS : COL_ACCENT), LV_PART_MAIN);
        lv_obj_align(value, LV_ALIGN_RIGHT_MID, -10, 0);
    }

    if (list->font) {
        lv_obj_set_style_text_font(label, list->font, LV_PART_MAIN);
        if (value) lv_obj_set_style_text_font(value, list->font, LV_PART_MAIN);
    }

    list->rows[absolute] = label;
    list->values[absolute] = value;
    return row;
}

static void list_render_window(gal_ui_t *ui, gal_list_t id)
{
    gal_list_ui_t *list = &ui->lists[id];
    list_clear_rows(list);
    if (list->count <= 0) return;
    int capacity = list_visible_capacity(list);
    if (capacity < 1) capacity = 1;
    list->visible = list->count < capacity ? list->count : capacity;
    int first = list->selected - list->visible / 2;
    if (first < 0) first = 0;
    if (first > list->count - list->visible) first = list->count - list->visible;
    list->first = first;
    for (int slot = 0; slot < list->visible; ++slot) {
        lv_obj_t *row = list_create_row(ui, id, first + slot, slot);
        const bool on = first + slot == list->selected;
        lv_obj_set_style_bg_color(row, lv_color_hex(on ? COL_ROW_SEL : COL_ROW), LV_PART_MAIN);
        lv_obj_set_style_border_width(row, on ? 2 : 0, LV_PART_MAIN);
    }
}

void gal_ui_set_rows(gal_ui_t *ui, gal_list_t id, const char *const *labels,
                     const char *const *values, int count, int selected)
{
    if (!ui) return;
    gal_list_ui_t *list = &ui->lists[id];
    if (count > GAL_LIST_MAX_ROWS) count = GAL_LIST_MAX_ROWS;
    list->count = count;
    for (int i = 0; i < GAL_LIST_MAX_ROWS; ++i) {
        list->label_text[i] = labels && i < count ? labels[i] : NULL;
        list->value_text[i] = values && i < count ? values[i] : NULL;
        list->disabled[i] = false;
    }
    if (selected < 0) selected = 0;
    if (selected >= count) selected = count ? count - 1 : 0;
    list->selected = selected;
    list_render_window(ui, id);
    gal_ui_select_row(ui, id, selected);
}

void gal_ui_set_row_value(gal_ui_t *ui, gal_list_t id, int row, const char *value)
{
    if (!ui || row < 0 || row >= GAL_LIST_MAX_ROWS) return;
    gal_list_ui_t *list = &ui->lists[id];
    list->value_text[row] = value;
    if (!list->values[row]) return;
    lv_label_set_text(list->values[row], value ? value : "");
}

void gal_ui_set_row_enabled(gal_ui_t *ui, gal_list_t id, int row, bool enabled)
{
    if (!ui || row < 0 || row >= GAL_LIST_MAX_ROWS) return;
    gal_list_ui_t *list = &ui->lists[id];
    list->disabled[row] = !enabled;
    if (!list->rows[row]) return;
    lv_obj_t *label = list->rows[row];
    lv_obj_t *value = list->values[row];
    if (label)
        lv_obj_set_style_text_color(label, lv_color_hex(enabled ? COL_TEXT : COL_ROW_DIS),
                                    LV_PART_MAIN);
    if (value)
        lv_obj_set_style_text_color(value,
                                    lv_color_hex(enabled ? COL_ACCENT : COL_ROW_DIS),
                                    LV_PART_MAIN);
    if (row == list->selected) gal_ui_select_row(ui, id, row);
}

void gal_ui_select_row(gal_ui_t *ui, gal_list_t id, int selected)
{
    if (!ui) return;
    gal_list_ui_t *list = &ui->lists[id];
    if (list->count <= 0) return;
    if (selected < 0) selected = 0;
    if (selected >= list->count) selected = list->count - 1;
    list->selected = selected;
    if (list->visible <= 0 || selected < list->first ||
        selected >= list->first + list->visible) {
        list_render_window(ui, id);
    } else {
        for (int i = list->first; i < list->first + list->visible; ++i) {
            if (!list->rows[i]) continue;
            lv_obj_t *row = lv_obj_get_parent(list->rows[i]);
            const bool on = i == selected;
            lv_obj_set_style_bg_color(row, lv_color_hex(on ? COL_ROW_SEL : COL_ROW),
                                      LV_PART_MAIN);
            lv_obj_set_style_border_width(row, on ? 2 : 0, LV_PART_MAIN);
        }
    }
    lv_obj_t *sel_row = list->rows[selected] ? lv_obj_get_parent(list->rows[selected]) : NULL;
    if (sel_row) lv_obj_scroll_to_view(sel_row, LV_ANIM_OFF);
}

static gal_page_t list_page(gal_list_t id)
{
    switch (id) {
    case GAL_LIST_START: return GAL_PAGE_START;
    case GAL_LIST_TITLE: return GAL_PAGE_TITLE;
    case GAL_LIST_CHOICE: return GAL_PAGE_CHOICE;
    case GAL_LIST_MENU: return GAL_PAGE_MENU;
    case GAL_LIST_SAVE: return GAL_PAGE_SAVE;
    case GAL_LIST_LOAD: return GAL_PAGE_LOAD;
    case GAL_LIST_SETTINGS: return GAL_PAGE_SETTINGS;
    case GAL_LIST_HISTORY: return GAL_PAGE_HISTORY;
    case GAL_LIST_GALLERY: return GAL_PAGE_GALLERY;
    case GAL_LIST_RECALL: return GAL_PAGE_RECALL;
    case GAL_LIST_MUSIC: return GAL_PAGE_MUSIC;
    case GAL_LIST_CHAPTER: return GAL_PAGE_CHAPTER;
    case GAL_LIST_MODAL: return GAL_PAGE_MODAL;
    default: return GAL_PAGE_COUNT;
    }
}

void gal_ui_release_inactive_rows(gal_ui_t *ui, gal_page_t keep)
{
    if (!ui) return;
    for (int id = 0; id < GAL_LIST_COUNT; ++id) {
        if (list_page((gal_list_t)id) == keep) continue;
        gal_list_ui_t *list = &ui->lists[id];
        list_clear_rows(list);
        for (int i = 0; i < GAL_LIST_MAX_ROWS; ++i) {
            list->label_text[i] = NULL;
            list->value_text[i] = NULL;
            list->disabled[i] = false;
        }
        list->count = 0;
        list->selected = 0;
    }
}

static void list_create(gal_ui_t *ui, gal_list_t id, lv_obj_t *page, lv_obj_t *box,
                        const char *title, int row_h, int top, int inset, int width, bool wrap)
{
    gal_list_ui_t *list = &ui->lists[id];
    memset(list, 0, sizeof(*list));
    list->page = page;
    list->box = box;
    list->row_h = row_h;
    list->top = top;
    list->inset = inset;
    list->width = width;
    list->wrap = wrap;
    list->font = ui->font_small;   // 懒创建的行要跟当前字号档
    // 内容超出窗口时整页滚动,select_row 会把选中行带进可视区。
    lv_obj_add_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
    if (title && title[0] != '\0') {
        list->title = lv_label_create(page);
        lv_label_set_text(list->title, title);
        lv_obj_set_pos(list->title, 12, 6);
        lv_obj_set_style_text_color(list->title, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
    }
}

// ---------------------------------------------------------------- 建界面
static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    // LVGL 新建标签的默认文本是 “Text”:先清空,避免尚未赋值时露出占位文本。
    lv_label_set_text(label, "");
    return label;
}

static lv_obj_t *mk_page(gal_ui_t *ui, gal_page_t page, uint32_t bg, bool opaque)
{
    lv_obj_t *obj = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, GAL_UI_W, GAL_UI_H);
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    if (opaque) {
        lv_obj_set_style_bg_color(obj, lv_color_hex(bg), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    }
    ui->pages[page] = obj;
    return obj;
}

// 页头标题(全屏列表页共用)。
static void mk_page_title(gal_ui_t *ui, gal_page_t page, const char *text)
{
    ui->titles[page] = mk_label(ui->pages[page], ui->font_small, COL_ACCENT);
    lv_label_set_text(ui->titles[page], text ? text : "");
    lv_obj_set_pos(ui->titles[page], 12, BAR_Y);
}

// 全屏页的滚动正文容器。
static lv_obj_t *mk_scroll_view(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *view = lv_obj_create(parent);
    lv_obj_remove_style_all(view);
    lv_obj_set_pos(view, x, y);
    lv_obj_set_size(view, w, h);
    lv_obj_set_style_bg_opa(view, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(view, LV_SCROLLBAR_MODE_OFF);
    return view;
}

// 对话框(对话页与选项页共用同一块贴底面板)。
static void build_dialogue(gal_ui_t *ui, gal_page_t page)
{
    lv_obj_t *root = ui->pages[page];

    // 背景图与立绘:默认隐藏,由 gal_ui_set_text 按句切换。
    if (page == GAL_PAGE_GAME) {
        ui->scene_img = lv_image_create(root);
        lv_obj_align(ui->scene_img, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_add_flag(ui->scene_img, LV_OBJ_FLAG_HIDDEN);
        ui->body_img = lv_image_create(root);
        lv_obj_align(ui->body_img, LV_ALIGN_BOTTOM_MID, 0, -(GAL_PANEL_H + 6));
        lv_obj_add_flag(ui->body_img, LV_OBJ_FLAG_HIDDEN);
    }

    ui->panel = lv_obj_create(root);
    lv_obj_remove_style_all(ui->panel);
    lv_obj_set_size(ui->panel, GAL_UI_W, GAL_PANEL_H);
    lv_obj_set_pos(ui->panel, 0, GAL_PANEL_Y);
    lv_obj_set_style_bg_color(ui->panel, lv_color_hex(COL_PANEL), LV_PART_MAIN);
    // 默认 30% 透明(70% 不透明),初始化时按设置覆盖。
    lv_obj_set_style_bg_opa(ui->panel, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_color(ui->panel, lv_color_hex(COL_FRAME), LV_PART_MAIN);
    lv_obj_set_style_border_width(ui->panel, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(ui->panel, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_remove_flag(ui->panel, LV_OBJ_FLAG_SCROLLABLE);

    // 名牌挂在对话框上沿外侧、左对齐(不压住正文);父对象是本页,跟着页面一起隐藏。
    if (page == GAL_PAGE_GAME) {
        ui->name = mk_label(root, ui->font_small, COL_NAME);
        lv_obj_set_style_bg_color(ui->name, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(ui->name, LV_OPA_50, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(ui->name, 8, LV_PART_MAIN);
        lv_obj_set_style_pad_ver(ui->name, 2, LV_PART_MAIN);
        lv_obj_set_style_radius(ui->name, 6, LV_PART_MAIN);
#ifdef NINE_READER
        // Fit the pill to the real name and padding; long names ellipsize.
        lv_obj_set_style_max_width(ui->name, GAL_NAME_MAX_W, LV_PART_MAIN);
        lv_obj_set_size(ui->name, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_label_set_long_mode(ui->name, LV_LABEL_LONG_MODE_DOTS);
#endif
        lv_obj_align_to(ui->name, ui->panel, LV_ALIGN_OUT_TOP_LEFT, 8, -2);
        lv_obj_add_flag(ui->name, LV_OBJ_FLAG_HIDDEN);
    }

    ui->text = mk_label(ui->panel, ui->font_small, COL_TEXT);
    lv_obj_set_pos(ui->text, GAL_TEXT_X, GAL_TEXT_Y);
    lv_obj_set_width(ui->text, GAL_TEXT_W);
    lv_label_set_long_mode(ui->text, LV_LABEL_LONG_WRAP);
    // 字体行高为 19/23,补 1px 行距后正好 20/24(框架规范)。
    lv_obj_set_style_text_line_space(ui->text, 1, LV_PART_MAIN);

    // 选项面板:叠在对话页上的分支选择框(原型 choice-panel)。
    if (page == GAL_PAGE_GAME) {
        lv_obj_t *box = lv_obj_create(root);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, GAL_UI_W - 16, 118);
        lv_obj_set_pos(box, 8, 46);
        lv_obj_set_style_bg_color(box, lv_color_hex(COL_BOX), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(box, (lv_opa_t)242, LV_PART_MAIN);
        lv_obj_set_style_border_color(box, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
        lv_obj_set_style_border_width(box, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(box, 9, LV_PART_MAIN);
        lv_obj_set_style_pad_all(box, 0, LV_PART_MAIN);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        ui->choice_box = box;
        list_create(ui, GAL_LIST_CHOICE, box, box, "", ROW_H, 8, 4, GAL_UI_W - 16, false);
        lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
    }
}

static void build_start(gal_ui_t *ui)
{
    lv_obj_t *page = mk_page(ui, GAL_PAGE_START, 0, false);
    // 开机首屏:只有一个贴底的「开始游戏」按钮。
    list_create(ui, GAL_LIST_START, page, NULL, "", 40, GAL_UI_H - 72, 36, GAL_UI_W, false);
}

static void build_title(gal_ui_t *ui)
{
    lv_obj_t *page = mk_page(ui, GAL_PAGE_TITLE, 0, false);

    // 标题封面(默认隐藏;由 gal_ui_set_title_image 设置)。
    ui->title_img = lv_image_create(page);
    lv_obj_align(ui->title_img, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_add_flag(ui->title_img, LV_OBJ_FLAG_HIDDEN);
    // 作品名要压在封面之上。
    lv_obj_move_foreground(ui->brand_title);
    lv_obj_move_foreground(ui->brand_sub);

    // 标题菜单框(原型:宽度占满、深色半透明、圆角)。
    lv_obj_t *box = lv_obj_create(page);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, GAL_UI_W - 22, 204);
    lv_obj_set_pos(box, 11, 112);
    lv_obj_set_style_bg_color(box, lv_color_hex(COL_BOX), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(box, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_style_border_color(box, lv_color_hex(COL_FRAME), LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(box, 10, LV_PART_MAIN);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    // 列表挂在 box 里,坐标相对 box。
    list_create(ui, GAL_LIST_TITLE, box, box, "", 32, 8, 4, GAL_UI_W - 22, false);
}

static void build_menu(gal_ui_t *ui)
{
    lv_obj_t *page = mk_page(ui, GAL_PAGE_MENU, 0, false);

    // 背景压暗(游戏内菜单是覆盖在剧情上的弹窗)。
    ui->dim = lv_obj_create(page);
    lv_obj_remove_style_all(ui->dim);
    lv_obj_set_size(ui->dim, GAL_UI_W, GAL_UI_H);
    lv_obj_set_pos(ui->dim, 0, 0);
    lv_obj_set_style_bg_color(ui->dim, lv_color_hex(0x040810), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->dim, 170, LV_PART_MAIN);
    lv_obj_remove_flag(ui->dim, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *box = lv_obj_create(page);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 196, 244);
    lv_obj_set_pos(box, 22, 38);
    lv_obj_set_style_bg_color(box, lv_color_hex(COL_BOX), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(box, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(box, 10, LV_PART_MAIN);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    ui->menu_box = box;

    lv_obj_t *title = mk_label(box, ui->font_small, COL_TEXT);
    lv_label_set_text(title, "快捷菜单");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    list_create(ui, GAL_LIST_MENU, box, box, "", 32, 40, 10, 196, false);
}

static void build_save_load(gal_ui_t *ui, gal_page_t page, gal_list_t list_id,
                            const char *title)
{
    lv_obj_t *root = mk_page(ui, page, COL_PAGE_BG, true);
    mk_page_title(ui, page, title);

    // 槽位列表(3 行) + 槽位预览 + 页脚。
    list_create(ui, list_id, root, NULL, "", 52, BODY_TOP, ROW_INSET, GAL_UI_W, false);

    lv_obj_t *preview = lv_obj_create(root);
    lv_obj_remove_style_all(preview);
    lv_obj_set_pos(preview, ROW_INSET, 208);
    lv_obj_set_size(preview, GAL_UI_W - 2 * ROW_INSET, 46);
    lv_obj_set_style_bg_color(preview, lv_color_hex(COL_ROW), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(preview, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(preview, lv_color_hex(COL_FRAME), LV_PART_MAIN);
    lv_obj_set_style_border_width(preview, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(preview, 6, LV_PART_MAIN);
    lv_obj_remove_flag(preview, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *preview_text = mk_label(preview, ui->font_small, COL_DIM);
    lv_obj_set_pos(preview_text, 8, 6);
    lv_obj_set_width(preview_text, GAL_UI_W - 2 * ROW_INSET - 16);
    lv_label_set_long_mode(preview_text, LV_LABEL_LONG_WRAP);
    if (page == GAL_PAGE_SAVE) ui->save_preview = preview_text;

    ui->footers[page] = mk_label(root, ui->font_small, COL_DIM);
    lv_obj_align(ui->footers[page], LV_ALIGN_BOTTOM_MID, 0, -8);
}

// 全屏列表页(页头标题 + 列表 + 页脚):CG 回想与音乐鉴赏都用它。
static void build_simple_list(gal_ui_t *ui, gal_page_t page, gal_list_t list,
                              const char *title, int row_h)
{
    lv_obj_t *root = mk_page(ui, page, COL_PAGE_BG, true);
    mk_page_title(ui, page, title);
    list_create(ui, list, root, NULL, "", row_h, BODY_TOP, ROW_INSET, GAL_UI_W, false);
    ui->footers[page] = mk_label(root, ui->font_small, COL_DIM);
    lv_obj_align(ui->footers[page], LV_ALIGN_BOTTOM_MID, 0, -8);
}

static void build_settings(gal_ui_t *ui)
{
    lv_obj_t *root = mk_page(ui, GAL_PAGE_SETTINGS, COL_PAGE_BG, true);
    mk_page_title(ui, GAL_PAGE_SETTINGS, "设置");
    list_create(ui, GAL_LIST_SETTINGS, root, NULL, "", ROW_H, BODY_TOP, ROW_INSET, GAL_UI_W,
                false);
}

static void build_history(gal_ui_t *ui)
{
    lv_obj_t *root = mk_page(ui, GAL_PAGE_HISTORY, COL_PAGE_BG, true);
    mk_page_title(ui, GAL_PAGE_HISTORY, "历史文本");
    // 每条两行:说话人 + 正文,所以行高更大并允许换行。
    list_create(ui, GAL_LIST_HISTORY, root, NULL, "", 52, BODY_TOP, ROW_INSET, GAL_UI_W, true);
}

static void build_gallery(gal_ui_t *ui)
{
    lv_obj_t *root = mk_page(ui, GAL_PAGE_GALLERY, COL_PAGE_BG, true);
    mk_page_title(ui, GAL_PAGE_GALLERY, "鉴赏室");
    // 分类入口:CG 鉴赏 / 剧情鉴赏(右侧的数量在渲染时填)。
    list_create(ui, GAL_LIST_GALLERY, root, NULL, "", 44, 100, 20, GAL_UI_W, false);
}

static void build_cg_list(gal_ui_t *ui)
{
    lv_obj_t *root = mk_page(ui, GAL_PAGE_CG_LIST, COL_PAGE_BG, true);
    mk_page_title(ui, GAL_PAGE_CG_LIST, "CG 鉴赏");

    // 3 列 x 3 行缩略图(前 6 张已解锁 + 2 张锁定,末格留空)。
    const int gap = 6;
    const int left = 9;
    const int top = 45;
    const int bottom = 29;
    const int cols = 3;
    const int rows = (GAL_CG_TILES + cols - 1) / cols;
    const int tile_w = (GAL_UI_W - 2 * left - (cols - 1) * gap) / cols;
    const int tile_h = (GAL_UI_H - top - bottom - (rows - 1) * gap) / rows;

    for (int i = 0; i < GAL_CG_TILES; ++i) {
        const int col = i % cols;
        const int row = i / cols;
        lv_obj_t *tile = lv_obj_create(root);
        lv_obj_remove_style_all(tile);
        lv_obj_set_size(tile, tile_w, tile_h);
        lv_obj_set_pos(tile, left + col * (tile_w + gap), top + row * (tile_h + gap));
        lv_obj_set_style_radius(tile, 6, LV_PART_MAIN);
        lv_obj_set_style_bg_color(tile, lv_color_hex(COL_TILE_A), LV_PART_MAIN);
        lv_obj_set_style_bg_grad_color(tile, lv_color_hex(COL_TILE_C), LV_PART_MAIN);
        lv_obj_set_style_bg_grad_dir(tile, LV_GRAD_DIR_VER, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(tile, lv_color_hex(COL_FRAME), LV_PART_MAIN);
        lv_obj_set_style_border_width(tile, 1, LV_PART_MAIN);
        lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        ui->cg_tiles[i] = tile;

        // 缩略图:用同一张 CG 图缩到格子大小(scale 以 1/256 为单位)。
        lv_obj_t *thumb = lv_image_create(tile);
        lv_obj_align(thumb, LV_ALIGN_TOP_MID, 0, 2);
#ifdef NINE_READER
        // Decoded thumbnails already match the tile, no extra LVGL scaling.
        lv_image_set_scale(thumb, 256);
#else
        lv_image_set_scale(thumb, 70);
#endif
        lv_obj_add_flag(thumb, LV_OBJ_FLAG_HIDDEN);
        ui->tile_img[i] = thumb;

        lv_obj_t *label = mk_label(tile, ui->font_small, COL_TEXT);
        lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -6);
        ui->cg_tiles_label[i] = label;
    }

    ui->footers[GAL_PAGE_CG_LIST] = mk_label(root, ui->font_small, COL_DIM);
    lv_obj_align(ui->footers[GAL_PAGE_CG_LIST], LV_ALIGN_BOTTOM_MID, 0, -8);
}

static void build_chapter(gal_ui_t *ui)
{
    lv_obj_t *root = mk_page(ui, GAL_PAGE_CHAPTER, COL_PAGE_BG, true);
    mk_page_title(ui, GAL_PAGE_CHAPTER, "章节 / 路线");

    list_create(ui, GAL_LIST_CHAPTER, root, NULL, "", 38, BODY_TOP, ROW_INSET, GAL_UI_W, false);

    lv_obj_t *info = lv_obj_create(root);
    lv_obj_remove_style_all(info);
    lv_obj_set_pos(info, ROW_INSET, 166);
    lv_obj_set_size(info, GAL_UI_W - 2 * ROW_INSET, 66);
    lv_obj_set_style_bg_color(info, lv_color_hex(COL_ROW), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(info, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(info, lv_color_hex(COL_FRAME), LV_PART_MAIN);
    lv_obj_set_style_border_width(info, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(info, 6, LV_PART_MAIN);
    lv_obj_remove_flag(info, LV_OBJ_FLAG_SCROLLABLE);

    ui->chapter_info = mk_label(info, ui->font_small, COL_DIM);
    lv_obj_set_pos(ui->chapter_info, 8, 6);
    lv_obj_set_width(ui->chapter_info, GAL_UI_W - 2 * ROW_INSET - 16);
    lv_label_set_long_mode(ui->chapter_info, LV_LABEL_LONG_WRAP);

    ui->action_hint = mk_label(root, ui->font_small, COL_DIM);
    lv_obj_align(ui->action_hint, LV_ALIGN_BOTTOM_MID, 0, -8);
}

static void build_transition(gal_ui_t *ui)
{
    lv_obj_t *root = mk_page(ui, GAL_PAGE_TRANSITION, 0, false);
    ui->transition_eyebrow = mk_label(root, ui->font_small, COL_GOLD);
    lv_obj_align(ui->transition_eyebrow, LV_ALIGN_TOP_MID, 0, 108);
    ui->transition_title = mk_label(root, ui->font_large, COL_TEXT);
    lv_obj_align(ui->transition_title, LV_ALIGN_TOP_MID, 0, 138);
    ui->transition_sub = mk_label(root, ui->font_small, COL_GOLD);
    lv_obj_align(ui->transition_sub, LV_ALIGN_TOP_MID, 0, 178);
}

static void build_ending(gal_ui_t *ui)
{
    // 结局页只有结局标题与正文,底部不放后续入口按钮。
    lv_obj_t *root = mk_page(ui, GAL_PAGE_ENDING, 0, false);
    ui->ending_eyebrow = mk_label(root, ui->font_small, COL_GOLD);
    lv_obj_align(ui->ending_eyebrow, LV_ALIGN_TOP_MID, 0, 110);
    ui->ending_title = mk_label(root, ui->font_large, COL_TEXT);
    lv_obj_align(ui->ending_title, LV_ALIGN_TOP_MID, 0, 142);
    ui->ending_body = mk_label(root, ui->font_small, COL_DIM);
    lv_obj_set_width(ui->ending_body, GAL_UI_W - 40);
    lv_label_set_long_mode(ui->ending_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(ui->ending_body, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(ui->ending_body, LV_ALIGN_TOP_MID, 0, 182);
}

static void build_about(gal_ui_t *ui)
{
    lv_obj_t *root = mk_page(ui, GAL_PAGE_ABOUT, COL_PAGE_BG, true);
    mk_page_title(ui, GAL_PAGE_ABOUT, "关于");

    lv_obj_t *logo = lv_obj_create(root);
    lv_obj_remove_style_all(logo);
    lv_obj_set_pos(logo, 10, 40);
    lv_obj_set_size(logo, GAL_UI_W - 20, 52);
    lv_obj_set_style_bg_color(logo, lv_color_hex(0x263858), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(logo, lv_color_hex(0x43364F), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(logo, LV_GRAD_DIR_HOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(logo, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(logo, lv_color_hex(COL_FRAME), LV_PART_MAIN);
    lv_obj_set_style_border_width(logo, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(logo, 7, LV_PART_MAIN);
    lv_obj_remove_flag(logo, LV_OBJ_FLAG_SCROLLABLE);

    ui->about_logo = mk_label(logo, ui->font_small, COL_TEXT);
    lv_obj_align(ui->about_logo, LV_ALIGN_TOP_MID, 0, 8);
    ui->about_logo_sub = mk_label(logo, ui->font_small, COL_GOLD);
    lv_obj_align(ui->about_logo_sub, LV_ALIGN_TOP_MID, 0, 30);

    ui->about_view = mk_scroll_view(root, 10, 100, GAL_UI_W - 20, 200);
    ui->about_text = mk_label(ui->about_view, ui->font_small, COL_TEXT);
    lv_obj_set_width(ui->about_text, GAL_UI_W - 32);
    lv_label_set_long_mode(ui->about_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(ui->about_text, 1, LV_PART_MAIN);
}

static void build_cg(gal_ui_t *ui)
{
    lv_obj_t *root = mk_page(ui, GAL_PAGE_CG, 0, false);

#ifndef NINE_READER
    // 骨架占位画面:非阅读器构建保留原有渐变占位。
    lv_obj_t *image = lv_obj_create(root);
    lv_obj_remove_style_all(image);
    lv_obj_set_size(image, GAL_UI_W, GAL_UI_H);
    lv_obj_set_pos(image, 0, 0);
    lv_obj_set_style_bg_color(image, lv_color_hex(0x253857), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(image, lv_color_hex(COL_TILE_C), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(image, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(image, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(image, LV_OBJ_FLAG_SCROLLABLE);
#else
    // The reader paints the real 240x135 art bands on the root canvas below
    // every page. This page must stay transparent; a full-page opaque
    // placeholder here hid every decoded CG (the old blank-CG defect).
#endif

    // CG 图对象仅用于其它构建/兼容 API; NINE_READER 的实际像素在根 canvas
    // 的五条 band 上,本对象保持隐藏。
    ui->cg_image = lv_image_create(root);
    lv_obj_align(ui->cg_image, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_add_flag(ui->cg_image, LV_OBJ_FLAG_HIDDEN);

    ui->cg_index = mk_label(root, ui->font_small, COL_TEXT);
    lv_obj_set_style_bg_color(ui->cg_index, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->cg_index, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(ui->cg_index, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(ui->cg_index, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(ui->cg_index, 6, LV_PART_MAIN);
    lv_obj_set_pos(ui->cg_index, 8, 8);

    lv_obj_t *bottom = lv_obj_create(root);
    lv_obj_remove_style_all(bottom);
    lv_obj_set_pos(bottom, 8, 254);
    lv_obj_set_size(bottom, GAL_UI_W - 16, 58);
    lv_obj_set_style_bg_color(bottom, lv_color_hex(COL_PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bottom, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_border_color(bottom, lv_color_hex(COL_FRAME), LV_PART_MAIN);
    lv_obj_set_style_border_width(bottom, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(bottom, 7, LV_PART_MAIN);
    lv_obj_remove_flag(bottom, LV_OBJ_FLAG_SCROLLABLE);

    ui->cg_name = mk_label(bottom, ui->font_small, COL_TEXT);
    lv_obj_set_pos(ui->cg_name, 8, 8);
    lv_obj_set_width(ui->cg_name, GAL_UI_W - 32);
    ui->cg_caption = mk_label(bottom, ui->font_small, COL_DIM);
    lv_obj_set_pos(ui->cg_caption, 8, 32);
    lv_obj_set_width(ui->cg_caption, GAL_UI_W - 32);
}

static void build_modal(gal_ui_t *ui)
{
    lv_obj_t *root = mk_page(ui, GAL_PAGE_MODAL, 0, false);   // 弹窗层:不参与页面枚举

    // 弹窗自己的压暗层(与游戏内菜单的 dim 是不同对象)。
    lv_obj_t *dim = lv_obj_create(root);
    lv_obj_remove_style_all(dim);
    lv_obj_set_size(dim, GAL_UI_W, GAL_UI_H);
    lv_obj_set_pos(dim, 0, 0);
    lv_obj_set_style_bg_color(dim, lv_color_hex(0x040810), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dim, 170, LV_PART_MAIN);
    lv_obj_remove_flag(dim, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *box = lv_obj_create(root);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 206, 190);
    lv_obj_set_pos(box, 17, 65);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x121A2A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(box, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(box, 9, LV_PART_MAIN);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    ui->modal_box = box;

    lv_obj_t *mark = mk_label(box, ui->font_large, COL_GOLD);
    lv_label_set_text(mark, "···");
    lv_obj_align(mark, LV_ALIGN_TOP_MID, 0, 10);
    ui->modal_mark = mark;

    ui->modal_title = mk_label(box, ui->font_small, COL_TEXT);
    lv_obj_align(ui->modal_title, LV_ALIGN_TOP_MID, 0, 14);

    ui->modal_body = mk_label(box, ui->font_small, COL_DIM);
    lv_obj_set_width(ui->modal_body, 182);
    lv_label_set_long_mode(ui->modal_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(ui->modal_body, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(ui->modal_body, LV_ALIGN_TOP_MID, 0, 46);

    // 弹窗按钮:居中、可左右排列(这里纵向排列,三键操作更直观)。
    list_create(ui, GAL_LIST_MODAL, box, box, "", 36, 106, 12, 206, false);
}

bool gal_ui_create(gal_ui_t *ui, const lv_font_t *font_small, const lv_font_t *font_large)
{
    if (!ui || !font_small || !font_large) return false;
    memset(ui, 0, sizeof(*ui));
    ui->font_small = font_small;
    ui->font_large = font_large;

    lv_obj_t *root = lv_screen_active();
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(root, lv_color_hex(COL_PAGE_BG), LV_PART_MAIN);

    // 程序绘制的渐变背景:骨架不携带任何美术资源。
    ui->bg = lv_obj_create(root);
    lv_obj_remove_style_all(ui->bg);
    lv_obj_set_size(ui->bg, GAL_UI_W, GAL_UI_H);
    lv_obj_set_pos(ui->bg, 0, 0);
    lv_obj_set_style_bg_color(ui->bg, lv_color_hex(COL_BG_TOP), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(ui->bg, lv_color_hex(COL_BG_BOT), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(ui->bg, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->bg, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(ui->bg, LV_OBJ_FLAG_SCROLLABLE);

    // 启动页与标题页的作品名(标题页显示,启动页隐藏)。
    ui->brand_title = mk_label(root, font_large, COL_TEXT);
    lv_obj_align(ui->brand_title, LV_ALIGN_TOP_MID, 0, 56);
    ui->brand_sub = mk_label(root, font_small, COL_GOLD);
    lv_obj_align(ui->brand_sub, LV_ALIGN_TOP_MID, 0, 88);

    // 左上角章节、右上角电量、模式指示与提示。
    ui->chapter = mk_label(root, font_small, COL_DIM);
    lv_obj_set_pos(ui->chapter, 9, 4);

    ui->battery = mk_label(root, font_small, COL_DIM);
    lv_label_set_text(ui->battery, "--%");
    lv_obj_align(ui->battery, LV_ALIGN_TOP_RIGHT, -9, 4);

    // 自动阅读指示:只有文字,贴在对话框上沿外侧的右上角(位置在对话框建好后算)。
    ui->mode_auto = mk_label(root, font_small, COL_ACCENT);
    lv_label_set_text(ui->mode_auto, "自动");
    lv_obj_add_flag(ui->mode_auto, LV_OBJ_FLAG_HIDDEN);

    ui->notice = mk_label(root, font_small, COL_ACCENT);
    lv_obj_align(ui->notice, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_add_flag(ui->notice, LV_OBJ_FLAG_HIDDEN);

    // 各逻辑屏(顺序与原型一致)。
    build_start(ui);
    build_title(ui);
    build_menu(ui);
    build_save_load(ui, GAL_PAGE_SAVE, GAL_LIST_SAVE, "保存进度");
    build_save_load(ui, GAL_PAGE_LOAD, GAL_LIST_LOAD, "读取存档");
    build_settings(ui);
    build_history(ui);
    build_gallery(ui);
    build_cg_list(ui);
    build_simple_list(ui, GAL_PAGE_RECALL, GAL_LIST_RECALL, "CG 回想", ROW_H);
    build_simple_list(ui, GAL_PAGE_MUSIC, GAL_LIST_MUSIC, "音乐鉴赏", ROW_H);
    build_chapter(ui);
    build_transition(ui);
    build_ending(ui);
    build_about(ui);
    build_cg(ui);
    build_modal(ui);

    // 对话页最后建:名牌要与对话框同层,且在其它页面之上。
    mk_page(ui, GAL_PAGE_GAME, 0, false);
    build_dialogue(ui, GAL_PAGE_GAME);

    // 快进不显示文字;自动阅读指示贴在对话框上沿外侧的右上角。
#ifdef NINE_READER
    lv_obj_set_parent(ui->mode_auto, ui->pages[GAL_PAGE_GAME]);
    lv_obj_align(ui->mode_auto, LV_ALIGN_TOP_RIGHT, -8, GAL_NAME_Y);
    lv_obj_set_parent(ui->notice, ui->pages[GAL_PAGE_GAME]);
    lv_obj_set_pos(ui->notice, 176, GAL_NAME_Y);
    lv_obj_set_width(ui->notice, 100);
    lv_label_set_long_mode(ui->notice, LV_LABEL_LONG_MODE_DOTS);
#else
    lv_obj_align_to(ui->mode_auto, ui->panel, LV_ALIGN_OUT_TOP_RIGHT, -8, -2);
#endif

    // 对话页是最后创建的,会盖住先建的对象;叠加层与顶部文字再提到最前:
    // 弹窗 → 游戏内菜单 → 自动指示 → 提示。
    lv_obj_move_foreground(ui->pages[GAL_PAGE_MENU]);
    lv_obj_move_foreground(ui->pages[GAL_PAGE_MODAL]);
    lv_obj_move_foreground(ui->mode_auto);
    lv_obj_move_foreground(ui->notice);

    ui->typewriter_timer = lv_timer_create(typing_cb, 40, ui);
    if (!ui->typewriter_timer) {
        ESP_LOGE(TAG, "打字机定时器创建失败");
        return false;
    }
    lv_timer_pause(ui->typewriter_timer);

#ifdef NINE_READER
    // Reflow the framework's pages for the 320x240 landscape reader. Rows are
    // windowed by nine_app, so object count never scales with a title.
    lv_obj_align(ui->brand_title, LV_ALIGN_TOP_MID, 0, 5);
    lv_obj_align(ui->brand_sub, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_t *title_box = ui->lists[GAL_LIST_TITLE].box;
    lv_obj_set_pos(title_box, 12, 58);
    lv_obj_set_size(title_box, 296, 174);
    ui->lists[GAL_LIST_TITLE].width = 296;
    ui->lists[GAL_LIST_TITLE].row_h = 29;
    lv_obj_set_pos(ui->menu_box, 34, 16);
    lv_obj_set_size(ui->menu_box, 252, 216);
    ui->lists[GAL_LIST_MENU].width = 252;
    ui->lists[GAL_LIST_MENU].row_h = 28;
    ui->lists[GAL_LIST_MENU].top = 36;
    lv_obj_set_pos(ui->choice_box, 8, 22);
    lv_obj_set_size(ui->choice_box, 304, 124);
    ui->lists[GAL_LIST_CHOICE].row_h = 28;
    for (int id = GAL_LIST_SAVE; id <= GAL_LIST_LOAD; ++id) {
        ui->lists[id].row_h = 34;
        ui->lists[id].top = 36;
    }
    lv_obj_t *preview_box = lv_obj_get_parent(ui->save_preview);
    lv_obj_set_pos(preview_box, 12, 176);
    lv_obj_set_height(preview_box, 36);
    lv_obj_t *load_root = ui->pages[GAL_PAGE_LOAD];
    if (lv_obj_get_child_count(load_root) > 1) {
        lv_obj_t *load_preview = lv_obj_get_child(load_root, 1);
        lv_obj_set_pos(load_preview, 12, 176);
        lv_obj_set_height(load_preview, 36);
    }
    ui->lists[GAL_LIST_GALLERY].top = 44;
    ui->lists[GAL_LIST_GALLERY].row_h = 36;
    ui->lists[GAL_LIST_CHAPTER].row_h = 30;
    lv_obj_t *chapter_box = lv_obj_get_parent(ui->chapter_info);
    lv_obj_set_pos(chapter_box, 12, 184);
    lv_obj_set_height(chapter_box, 28);
    lv_obj_align(ui->transition_eyebrow, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_align(ui->transition_title, LV_ALIGN_TOP_MID, 0, 90);
    lv_obj_align(ui->transition_sub, LV_ALIGN_TOP_MID, 0, 128);
    lv_obj_align(ui->ending_eyebrow, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_align(ui->ending_title, LV_ALIGN_TOP_MID, 0, 66);
    lv_obj_align(ui->ending_body, LV_ALIGN_TOP_MID, 0, 102);
    lv_obj_t *about_view = lv_obj_get_parent(ui->about_text);
    lv_obj_set_height(about_view, 132);
    lv_obj_t *cg_bottom = lv_obj_get_parent(ui->cg_name);
    lv_obj_set_pos(cg_bottom, 8, 198);
    lv_obj_set_height(cg_bottom, 36);
    lv_obj_set_pos(ui->cg_name, 8, 0);
    lv_obj_set_pos(ui->cg_caption, 8, 18);
    lv_obj_set_pos(ui->modal_box, 28, 22);
    lv_obj_set_size(ui->modal_box, 264, 190);
    ui->lists[GAL_LIST_MODAL].width = 264;
#endif

    gal_ui_set_font_size(ui, false);
    gal_ui_show_page(ui, GAL_PAGE_START);
    return true;
}

// ---------------------------------------------------------------- 页面与状态
void gal_ui_show_page(gal_ui_t *ui, gal_page_t page)
{
    if (!ui || page >= GAL_PAGE_COUNT) return;

    // 选项与游戏内菜单是叠在对话页上的层:底页仍是对话页。
    const gal_page_t base = (page == GAL_PAGE_CHOICE || page == GAL_PAGE_MENU)
                                ? GAL_PAGE_GAME : page;

    for (int i = 0; i < GAL_PAGE_COUNT; ++i) {
        if (!ui->pages[i]) continue;
        // 底页与叠加层一起显示(游戏内菜单是对话页上的一层)。
        if (i == (int)base || i == (int)page) {
            lv_obj_remove_flag(ui->pages[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ui->pages[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    // 弹窗层不属于页面枚举,单独处理。
    if (ui->pages[GAL_PAGE_MODAL]) {
        lv_obj_add_flag(ui->pages[GAL_PAGE_MODAL], LV_OBJ_FLAG_HIDDEN);
    }
    if (ui->choice_box) {
        if (page == GAL_PAGE_CHOICE) lv_obj_remove_flag(ui->choice_box, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(ui->choice_box, LV_OBJ_FLAG_HIDDEN);
    }

    // 作品名只在标题页出现。
    const bool brand = base == GAL_PAGE_TITLE;
    if (brand) lv_obj_remove_flag(ui->brand_title, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(ui->brand_title, LV_OBJ_FLAG_HIDDEN);
    if (brand) lv_obj_remove_flag(ui->brand_sub, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(ui->brand_sub, LV_OBJ_FLAG_HIDDEN);

    // 分数与电量只在阅读类页面显示。
    const bool hud = base == GAL_PAGE_GAME;
    if (hud) lv_obj_remove_flag(ui->chapter, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(ui->chapter, LV_OBJ_FLAG_HIDDEN);
    if (hud) lv_obj_remove_flag(ui->battery, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(ui->battery, LV_OBJ_FLAG_HIDDEN);
    if (hud) lv_obj_remove_flag(ui->notice, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(ui->notice, LV_OBJ_FLAG_HIDDEN);

    // 自动阅读指示跟随页面;章节过场也保持(避免看起来像被关掉)。
    const bool auto_hud = base == GAL_PAGE_GAME || base == GAL_PAGE_TRANSITION;
    if (!auto_hud) lv_obj_add_flag(ui->mode_auto, LV_OBJ_FLAG_HIDDEN);

    if (base != GAL_PAGE_GAME) typing_stop(ui);
}

void gal_ui_set_brand(gal_ui_t *ui, const char *title, const char *subtitle)
{
    if (!ui) return;
    lv_label_set_text(ui->brand_title, title ? title : "");
    lv_label_set_text(ui->brand_sub, subtitle ? subtitle : "");
}

void gal_ui_set_choice(gal_ui_t *ui, const char *prompt, const gal_option_t *options,
                       int count, int selected)
{
    if (!ui) return;
    // 选项页把分支前的最后一句台词留在底部对话框里。
    gal_ui_set_text(ui, NULL, prompt, 0, NULL, NULL);
    if (count > GAL_LIST_MAX_ROWS) count = GAL_LIST_MAX_ROWS;
    static const char *labels[GAL_LIST_MAX_ROWS];
    for (int i = 0; i < count; ++i) {
        labels[i] = (options && options[i].label) ? options[i].label : "";
    }
    // 可用性(剧情锁)由 gal_app 判定后用 gal_ui_set_row_enabled 设置。
    gal_ui_set_rows(ui, GAL_LIST_CHOICE, labels, NULL, count, selected);
}

void gal_ui_set_mode(gal_ui_t *ui, bool auto_play)
{
    if (!ui) return;
    if (auto_play) lv_obj_remove_flag(ui->mode_auto, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(ui->mode_auto, LV_OBJ_FLAG_HIDDEN);
}

void gal_ui_set_chapter(gal_ui_t *ui, const char *text)
{
    if (!ui) return;
    lv_label_set_text(ui->chapter, text ? text : "");
}

void gal_ui_set_battery(gal_ui_t *ui, int percent)
{
    if (!ui) return;
    if (percent < 0) {
        lv_label_set_text(ui->battery, "--%");
        lv_obj_set_style_text_color(ui->battery, lv_color_hex(COL_DIM), LV_PART_MAIN);
        return;
    }
    lv_label_set_text_fmt(ui->battery, "%d%%", percent);
    lv_obj_set_style_text_color(ui->battery,
                                lv_color_hex(percent < 20 ? 0xFF8A7A : COL_DIM), LV_PART_MAIN);
}

void gal_ui_set_notice(gal_ui_t *ui, const char *text)
{
    if (!ui) return;
    lv_label_set_text(ui->notice, text ? text : "");
    if (text && text[0] != '\0') {
        lv_obj_remove_flag(ui->notice, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui->notice, LV_OBJ_FLAG_HIDDEN);
    }
}

void gal_ui_set_page_title(gal_ui_t *ui, gal_page_t page, const char *title)
{
    if (!ui || page >= GAL_PAGE_COUNT) return;
    if (!ui->titles[page]) return;
    lv_label_set_text(ui->titles[page], title ? title : "");
}

void gal_ui_set_save_preview(gal_ui_t *ui, const char *text)
{
    if (!ui || !ui->save_preview) return;
    lv_label_set_text(ui->save_preview, text ? text : "");
}

void gal_ui_set_footer(gal_ui_t *ui, gal_page_t page, const char *text)
{
    if (!ui || page >= GAL_PAGE_COUNT) return;
    lv_obj_t *footer = ui->footers[page];
    if (!footer) return;
    lv_label_set_text(footer, text ? text : "");
}

void gal_ui_set_chapter_info(gal_ui_t *ui, const char *text)
{
    if (!ui || !ui->chapter_info) return;
    lv_label_set_text(ui->chapter_info, text ? text : "");
}

void gal_ui_set_action_hint(gal_ui_t *ui, const char *text)
{
    if (!ui || !ui->action_hint) return;
    lv_label_set_text(ui->action_hint, text ? text : "");
}

void gal_ui_set_gallery(gal_ui_t *ui, const gal_cg_t *items, int count, int selected)
{
    if (!ui) return;
    for (int i = 0; i < GAL_CG_TILES; ++i) {
        const bool filled = items && i < count;
        const bool unlocked = filled && items[i].unlocked;
        lv_obj_t *tile = ui->cg_tiles[i];
        lv_obj_t *label = ui->cg_tiles_label[i];

        if (unlocked) {
            lv_label_set_text(label, items[i].label);
            lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -6);
            lv_obj_set_style_text_color(label, lv_color_hex(COL_TEXT), LV_PART_MAIN);
            lv_obj_set_style_bg_color(tile, lv_color_hex(COL_TILE_A), LV_PART_MAIN);
            lv_obj_set_style_bg_grad_color(tile, lv_color_hex(COL_TILE_C), LV_PART_MAIN);
        } else {
            // 未解锁:只摆一个问号,不透露是哪一张 CG。
            lv_label_set_text(label, filled ? "?" : "");
            lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
            lv_obj_set_style_text_color(label, lv_color_hex(COL_DIM), LV_PART_MAIN);
            lv_obj_set_style_bg_color(tile, lv_color_hex(COL_ROW), LV_PART_MAIN);
            lv_obj_set_style_bg_grad_color(tile, lv_color_hex(COL_ROW), LV_PART_MAIN);
        }
        // 缩略图:解锁且有图才显示。
        apply_image(ui->tile_img[i], (unlocked && items[i].image) ? items[i].image : NULL);
        const bool on = filled && i == selected;
        lv_obj_set_style_border_color(tile, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
        lv_obj_set_style_border_width(tile, on ? 2 : 1, LV_PART_MAIN);
    }
}

void gal_ui_set_cg(gal_ui_t *ui, const char *index_text, const char *name,
                   const char *caption, const void *image)
{
    if (!ui) return;
    lv_label_set_text(ui->cg_index, index_text ? index_text : "");
    lv_label_set_text(ui->cg_name, name ? name : "");
    lv_label_set_text(ui->cg_caption, caption ? caption : "");
    apply_image(ui->cg_image, image);
}

void gal_ui_set_title_image(gal_ui_t *ui, const void *image)
{
    if (!ui) return;
    apply_image(ui->title_img, image);
}

void gal_ui_set_transition(gal_ui_t *ui, const char *eyebrow, const char *title,
                           const char *subtitle)
{
    if (!ui) return;
    lv_label_set_text(ui->transition_eyebrow, eyebrow ? eyebrow : "");
    lv_label_set_text(ui->transition_title, title ? title : "");
    lv_label_set_text(ui->transition_sub, subtitle ? subtitle : "");
}

void gal_ui_set_ending(gal_ui_t *ui, const char *eyebrow, const char *title, const char *body)
{
    if (!ui) return;
    lv_label_set_text(ui->ending_eyebrow, eyebrow ? eyebrow : "");
    lv_label_set_text(ui->ending_title, title ? title : "");
    lv_label_set_text(ui->ending_body, body ? body : "");
}

void gal_ui_set_about(gal_ui_t *ui, const char *logo, const char *logo_sub, const char *text)
{
    if (!ui) return;
    lv_label_set_text(ui->about_logo, logo ? logo : "");
    lv_label_set_text(ui->about_logo_sub, logo_sub ? logo_sub : "");
    lv_label_set_text(ui->about_text, text ? text : "");
    if (ui->about_view) lv_obj_scroll_to_y(ui->about_view, 0, LV_ANIM_OFF);
}

void gal_ui_scroll(gal_ui_t *ui, gal_page_t page, int dy)
{
    if (!ui || dy == 0) return;
    lv_obj_t *view = (page == GAL_PAGE_ABOUT) ? ui->about_view : NULL;
    if (!view) return;
    // LVGL 的 scroll 值为负:界面层 dy > 0 表示往下看,传负值。
    lv_obj_scroll_by(view, 0, -dy, LV_ANIM_OFF);
}

void gal_ui_show_modal(gal_ui_t *ui, gal_modal_t kind, const char *title, const char *body,
                       const char *const *actions, int count, int selected)
{
    if (!ui || kind == GAL_MODAL_NONE) return;
    lv_label_set_text(ui->modal_title, title ? title : "");
    lv_label_set_text(ui->modal_body, body ? body : "");

    const bool empty = kind == GAL_MODAL_EMPTY;
    if (empty) {
        lv_obj_remove_flag(ui->modal_mark, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(ui->modal_title, LV_ALIGN_TOP_MID, 0, 68);
        lv_obj_align(ui->modal_body, LV_ALIGN_TOP_MID, 0, 98);
        ui->lists[GAL_LIST_MODAL].top = 140;
    } else {
        lv_obj_add_flag(ui->modal_mark, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(ui->modal_title, LV_ALIGN_TOP_MID, 0, 14);
        lv_obj_align(ui->modal_body, LV_ALIGN_TOP_MID, 0, 46);
        ui->lists[GAL_LIST_MODAL].top = 106;
    }
    // 行位置在创建时确定:清掉旧行,让新位置的按钮重新创建。
    gal_list_ui_t *list = &ui->lists[GAL_LIST_MODAL];
    for (int i = 0; i < GAL_LIST_MAX_ROWS; ++i) {
        if (list->rows[i]) {
            lv_obj_del(lv_obj_get_parent(list->rows[i]));
            list->rows[i] = NULL;
            list->values[i] = NULL;
        }
    }
    gal_ui_set_rows(ui, GAL_LIST_MODAL, actions, NULL, count, selected);
    if (!ui->pages[GAL_PAGE_MODAL]) return;
    lv_obj_remove_flag(ui->pages[GAL_PAGE_MODAL], LV_OBJ_FLAG_HIDDEN);
}

void gal_ui_hide_modal(gal_ui_t *ui)
{
    if (!ui || !ui->pages[GAL_PAGE_MODAL]) return;
    lv_obj_add_flag(ui->pages[GAL_PAGE_MODAL], LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------- 字体与透明度
static void apply_font(lv_obj_t *obj, const lv_font_t *font)
{
    if (!obj) return;
    if (lv_obj_check_type(obj, &lv_label_class)) {
        lv_obj_set_style_text_font(obj, font, LV_PART_MAIN);
    }
    const uint32_t children = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < children; ++i) {
        apply_font(lv_obj_get_child(obj, (int32_t)i), font);
    }
}

void gal_ui_set_font_size(gal_ui_t *ui, bool large)
{
    if (!ui) return;
    const lv_font_t *font = large ? ui->font_large : ui->font_small;
    // 屏内所有文字统一用对话字号档位(框架规范:不设 10-12px 小字)。
    apply_font(lv_screen_active(), font);
    // 列表行是按需创建的,不在上面的遍历范围里:记住字号,供新行使用。
    for (int i = 0; i < GAL_LIST_COUNT; ++i) {
        ui->lists[i].font = font;
        for (int r = 0; r < GAL_LIST_MAX_ROWS; ++r) {
            if (!ui->lists[i].rows[r]) continue;
            lv_obj_set_style_text_font(ui->lists[i].rows[r], font, LV_PART_MAIN);
            lv_obj_set_style_text_font(ui->lists[i].values[r], font, LV_PART_MAIN);
        }
        if (ui->lists[i].title) lv_obj_set_style_text_font(ui->lists[i].title, font, LV_PART_MAIN);
    }
    // 作品名始终用大档,标题层级高于正文。
    lv_obj_set_style_text_font(ui->brand_title, ui->font_large, LV_PART_MAIN);
    // Real font metrics: 3 small / 2 large complete body lines.
    if (ui->text) {
        lv_obj_set_style_text_font(ui->text, font, LV_PART_MAIN);
        const int lines = large ? GAL_TEXT_LINES_LARGE : GAL_TEXT_LINES_SMALL;
        const int line_h = large ? GAL_LINE_H_LARGE : GAL_LINE_H_SMALL;
        lv_obj_set_height(ui->text, line_h * lines - 1);
    }
    if (ui->name) {
        lv_obj_set_style_text_font(ui->name, ui->font_small, LV_PART_MAIN);
#ifdef NINE_READER
        if (!lv_obj_has_flag(ui->name, LV_OBJ_FLAG_HIDDEN))
            reader_fit_name(ui, lv_label_get_text(ui->name));
#endif
    }
    if (ui->mode_auto) lv_obj_set_style_text_font(ui->mode_auto, ui->font_small, LV_PART_MAIN);
    if (ui->notice) lv_obj_set_style_text_font(ui->notice, ui->font_small, LV_PART_MAIN);
}

void gal_ui_set_panel_transparency(gal_ui_t *ui, uint8_t percent)
{
    if (!ui) return;
    lv_obj_set_style_bg_opa(ui->panel, (lv_opa_t)gal_panel_opa(percent), LV_PART_MAIN);
}
