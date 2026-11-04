// main/summer_ui.c —— LVGL 页面层实现。
#include "summer_ui.h"

#include <stdio.h>
#include <string.h>

#define COL_BG 0x070D14u
#define COL_PANEL 0x102C42u
#define COL_TEXT 0xF4F7FFu
#define COL_ACCENT 0x9BDAEDu
#define COL_DIM 0x6F8496u
#define COL_SELECT 0x1E4A66u
#define COL_TITLE_BG 0x0A111Fu
#define COL_ROW_BG 0x0D1C2Au

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color,
                     uint8_t opa, int radius)
{
    lv_obj_t *obj = lv_obj_create(parent);
    if (!obj) return NULL;
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, opa, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    return obj;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                       uint32_t color, int x, int y, int w, int h,
                       lv_text_align_t align)
{
    lv_obj_t *obj = lv_label_create(parent);
    if (!obj) return NULL;
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_CLIP);
    lv_label_set_text(obj, text ? text : "");
    if (font) lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(obj, align, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    return obj;
}

static void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (!obj) return;
    if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void set_text(lv_obj_t *obj, const char *text)
{
    if (obj) lv_label_set_text(obj, text ? text : "");
}

static int style_visible_rows(summer_list_style_t style)
{
    switch (style) {
    case SUMMER_LIST_COMPACT: return 7;
    case SUMMER_LIST_CARD: return 3;
    case SUMMER_LIST_HISTORY: return 3;
    case SUMMER_LIST_TITLE: return 8;
    default: return 7;
    }
}

static void position_rows(summer_ui_t *ui, summer_list_style_t style)
{
    const bool title = style == SUMMER_LIST_TITLE;
    const int x = title ? 4 : 8;
    const int start_y = title ? 4 : 44;
    const int w = title ? 100 : 304;
    const int row_h = title ? 20 : (style == SUMMER_LIST_COMPACT ? 24 : 54);
    for (int i = 0; i < SUMMER_UI_ROWS; ++i) {
        if (!ui->rows[i]) continue;
        lv_obj_set_pos(ui->rows[i], x, start_y + i * row_h);
        lv_obj_set_size(ui->rows[i], w, row_h - 2);
        lv_obj_set_pos(ui->row_text[i], 4, 2);
        lv_obj_set_size(ui->row_text[i], title ? w - 8 : (style == SUMMER_LIST_COMPACT ? 186 : w - 8), 18);
        lv_obj_set_pos(ui->row_meta[i], style == SUMMER_LIST_COMPACT ? 194 : 4,
                       style == SUMMER_LIST_COMPACT ? 2 : 20);
        lv_obj_set_size(ui->row_meta[i], style == SUMMER_LIST_COMPACT ? 104 : w - 8, 16);
        lv_obj_set_pos(ui->row_preview[i], 4, 34);
        lv_obj_set_size(ui->row_preview[i], w - 8, 16);
        lv_obj_set_style_text_align(ui->row_meta[i],
                                    style == SUMMER_LIST_COMPACT ? LV_TEXT_ALIGN_RIGHT
                                                                 : LV_TEXT_ALIGN_LEFT, 0);
    }
}

bool summer_ui_create(summer_ui_t *ui, uint16_t *pixels, const lv_font_t *font)
{
    if (!ui || !pixels) return false;
    memset(ui, 0, sizeof(*ui));
    ui->font = font;
    lv_obj_t *screen = lv_screen_active();
    if (screen) {
        lv_obj_set_style_bg_color(screen, lv_color_hex(COL_BG), 0);
        lv_obj_set_style_bg_opa(screen, 255, 0);
        lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    }
    ui->root = box(screen, 0, 0, 320, 240, COL_BG, 255, 0);
    if (!ui->root) return false;
    if (!summer_image_init(&ui->canvas, ui->root, pixels)) return false;

    // 对话框 176..240（64 高，原 150..240 / 90 高）：画面可见区从 150 提到 176，
    // 正文 3 行 × 19px = 57 从面板内 y=2 起 → 178..235，末行不出 240。
    ui->panel = box(ui->root, 0, SUMMER_PANEL_Y, 320, SUMMER_PANEL_READ_H, COL_PANEL, 180, 0);
    // 名字在框外（压在画面上），底条只包文字：宽度 LV_SIZE_CONTENT + 左右 6px。
    ui->speaker_box = box(ui->root, 6, SUMMER_PANEL_Y - 22, 0, 22, 0x0A1824, 200, 6);
    lv_obj_set_width(ui->speaker_box, LV_SIZE_CONTENT);
    lv_obj_set_height(ui->speaker_box, 22);
    lv_obj_set_style_pad_hor(ui->speaker_box, 6, 0);
    ui->speaker_label = label(ui->speaker_box, "", font, COL_ACCENT, 0, 2, 0, 18,
                              LV_TEXT_ALIGN_LEFT);
    lv_obj_set_width(ui->speaker_label, LV_SIZE_CONTENT);
    ui->text_label = label(ui->panel, "", font, COL_TEXT, 8, 2, 304, 57, LV_TEXT_ALIGN_LEFT);
    // 页码移出面板（157..175，贴面板上沿）：面板只剩正文，才能压到 64 高。
    ui->page_label = label(ui->root, "", font, COL_DIM, 258, SUMMER_PANEL_Y - 19, 54, 19,
                           LV_TEXT_ALIGN_RIGHT);
    ui->cg_caption = label(ui->panel, "", font, COL_TEXT, 8, 2, 304, 20, LV_TEXT_ALIGN_LEFT);
    ui->cg_hint = label(ui->panel, "", font, COL_DIM, 8, 24, 304, 18, LV_TEXT_ALIGN_LEFT);

    ui->chapter_label = label(ui->root, "", font, COL_ACCENT, 6, 4, 200, 20, LV_TEXT_ALIGN_LEFT);
    ui->battery_label = label(ui->root, "", font, COL_ACCENT, 250, 4, 64, 20, LV_TEXT_ALIGN_RIGHT);
    ui->auto_label = label(ui->root, "", font, COL_ACCENT, 232, 24, 82, 18, LV_TEXT_ALIGN_RIGHT);
    ui->title_progress_label = label(ui->root, "", font, COL_DIM, 8, 68, 190, 18,
                                     LV_TEXT_ALIGN_LEFT);

    ui->notice_label = box(ui->root, 30, 4, 260, 24, 0x132433, 240, 6);
    ui->notice_text = label(ui->notice_label, "", font, COL_TEXT, 6, 2, 248, 20,
                            LV_TEXT_ALIGN_CENTER);
    set_hidden(ui->notice_label, true);

    ui->overlay = box(ui->root, 0, 0, 320, 240, COL_TITLE_BG, 255, 0);
    ui->overlay_title = label(ui->overlay, "", font, COL_TEXT, 8, 4, 304, 22, LV_TEXT_ALIGN_CENTER);
    ui->overlay_sub = label(ui->overlay, "", font, COL_DIM, 8, 26, 304, 16, LV_TEXT_ALIGN_CENTER);
    ui->overlay_footer = label(ui->overlay, "", font, COL_DIM, 8, 222, 304, 16,
                               LV_TEXT_ALIGN_CENTER);
    for (int i = 0; i < SUMMER_UI_ROWS; ++i) {
        ui->rows[i] = box(ui->overlay, 8, 44, 304, 22, COL_ROW_BG, 255, 5);
        ui->row_text[i] = label(ui->rows[i], "", font, COL_TEXT, 4, 2, 288, 18,
                                LV_TEXT_ALIGN_LEFT);
        ui->row_meta[i] = label(ui->rows[i], "", font, COL_DIM, 4, 20, 288, 16,
                                LV_TEXT_ALIGN_LEFT);
        ui->row_preview[i] = label(ui->rows[i], "", font, COL_DIM, 4, 34, 288, 16,
                                   LV_TEXT_ALIGN_LEFT);
        set_hidden(ui->rows[i], true);
    }

    ui->help_view = box(ui->overlay, 8, 44, 304, 176, COL_TITLE_BG, 255, 4);
    lv_obj_add_flag(ui->help_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(ui->help_view, LV_DIR_VER);
    ui->help_label = label(ui->help_view, "", font, COL_TEXT, 4, 4, 292, 400,
                           LV_TEXT_ALIGN_LEFT);
    lv_label_set_long_mode(ui->help_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(ui->help_label, 2, 0);
    set_hidden(ui->help_view, true);

    ui->ending_title = label(ui->overlay, "", font, COL_TEXT, 8, 76, 304, 30, LV_TEXT_ALIGN_CENTER);
    ui->ending_body = label(ui->overlay, "", font, COL_DIM, 20, 112, 280, 70, LV_TEXT_ALIGN_CENTER);
    lv_label_set_long_mode(ui->ending_body, LV_LABEL_LONG_WRAP);
    ui->error_text = label(ui->overlay, "", font, 0xEAB7B7u, 16, 70, 288, 100,
                           LV_TEXT_ALIGN_CENTER);
    lv_label_set_long_mode(ui->error_text, LV_LABEL_LONG_WRAP);

    summer_ui_show_page(ui, SUMMER_PAGE_TITLE);
    return true;
}

void summer_ui_show_page(summer_ui_t *ui, summer_page_t page)
{
    if (!ui) return;
    ui->page = page;
    const bool title = page == SUMMER_PAGE_TITLE;
    const bool game = page == SUMMER_PAGE_GAME;
    const bool choice = page == SUMMER_PAGE_CHOICE;
    const bool list = page == SUMMER_PAGE_MENU || page == SUMMER_PAGE_LIST;
    const bool cg = page == SUMMER_PAGE_CG;
    const bool ending = page == SUMMER_PAGE_ENDING;
    const bool error = page == SUMMER_PAGE_ERROR;

    set_hidden(ui->panel, !(game || cg));
    // CG 页只留一条字幕带（46 高，194..240），把 CG 可见区放到最大；
    // 阅读页回到 64 高（176..240）。
    lv_obj_set_pos(ui->panel, 0, cg ? SUMMER_PANEL_CG_Y : SUMMER_PANEL_Y);
    lv_obj_set_size(ui->panel, 320, cg ? SUMMER_PANEL_CG_H : SUMMER_PANEL_READ_H);
    set_hidden(ui->speaker_box, !game);
    set_hidden(ui->text_label, !game);
    set_hidden(ui->page_label, !game);
    set_hidden(ui->cg_caption, !cg);
    set_hidden(ui->cg_hint, !cg);
    set_hidden(ui->chapter_label, !(game || cg));
    set_hidden(ui->auto_label, !game);
    set_hidden(ui->title_progress_label, !title);
    set_hidden(ui->ending_title, !ending);
    set_hidden(ui->ending_body, !ending);
    set_hidden(ui->error_text, !error);
    set_hidden(ui->help_view, true);
    set_hidden(ui->overlay, !(title || choice || list || ending || error));
    set_hidden(ui->overlay_title, !(choice || list));
    set_hidden(ui->overlay_sub, !list);
    set_hidden(ui->overlay_footer, !(choice || list));
    for (int i = 0; i < SUMMER_UI_ROWS; ++i) {
        set_hidden(ui->rows[i], !(title || choice || list));
    }

    if (title) {
        lv_obj_set_pos(ui->overlay, 206, 56);
        lv_obj_set_size(ui->overlay, 108, 180);
        lv_obj_set_style_bg_opa(ui->overlay, 170, 0);
        lv_obj_set_style_radius(ui->overlay, 6, 0);
        position_rows(ui, SUMMER_LIST_TITLE);
    } else {
        lv_obj_set_pos(ui->overlay, 0, 0);
        lv_obj_set_size(ui->overlay, 320, 240);
        lv_obj_set_style_bg_opa(ui->overlay, choice ? 238 : 252, 0);
        lv_obj_set_style_radius(ui->overlay, 0, 0);
    }
}

void summer_ui_set_chapter(summer_ui_t *ui, const char *text)
{
    if (ui) set_text(ui->chapter_label, text);
}

void summer_ui_set_battery(summer_ui_t *ui, int percent)
{
    if (!ui) return;
    char buf[24];
    // 只留数值/图标位：屏幕上“电量”两字没有信息量。
    if (percent < 0) snprintf(buf, sizeof(buf), "--");
    else snprintf(buf, sizeof(buf), "%d%%", percent);
    lv_label_set_text(ui->battery_label, buf);
}

void summer_ui_set_auto(summer_ui_t *ui, bool auto_mode, bool fast_forward)
{
    if (!ui) return;
    if (fast_forward) lv_label_set_text(ui->auto_label, "快进");
    else if (auto_mode) lv_label_set_text(ui->auto_label, "自动");
    else lv_label_set_text(ui->auto_label, "");
}

void summer_ui_set_panel_opacity(summer_ui_t *ui, int level)
{
    if (!ui || !ui->panel) return;
    const int v = level < 0 ? 0 : level > 4 ? 4 : level;
    const int opa = 180 + v * 15;
    lv_obj_set_style_bg_opa(ui->panel, (uint8_t)(opa > 255 ? 255 : opa), 0);
}

void summer_ui_set_game(summer_ui_t *ui, const char *speaker, const char *text,
                        int page, int pages)
{
    if (!ui) return;
    summer_ui_set_speaker(ui, speaker);
    summer_ui_set_text(ui, text);
    summer_ui_set_page(ui, page, pages);
}

void summer_ui_set_text(summer_ui_t *ui, const char *text)
{
    if (ui) set_text(ui->text_label, text);
}

void summer_ui_set_speaker(summer_ui_t *ui, const char *speaker)
{
    if (!ui) return;
    set_text(ui->speaker_label, speaker);
    set_hidden(ui->speaker_box, !(speaker && speaker[0]));
}

void summer_ui_set_page(summer_ui_t *ui, int page, int pages)
{
    if (!ui) return;
    if (pages > 1) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d/%d", page, pages);
        lv_label_set_text(ui->page_label, buf);
        set_hidden(ui->page_label, false);
    } else {
        lv_label_set_text(ui->page_label, "");
        set_hidden(ui->page_label, true);
    }
}

static void apply_window(summer_ui_t *ui, summer_list_style_t style, const char *title,
                         const char *subtitle, const char *footer,
                         const summer_ui_row_t *rows, int visible, int first, int total,
                         int selected)
{
    ui->row_style = style;
    ui->row_selected = selected;
    ui->row_top = first;
    ui->row_count = total;
    if (style != SUMMER_LIST_TITLE) {
        set_text(ui->overlay_title, title);
        set_text(ui->overlay_sub, subtitle);
        set_text(ui->overlay_footer, footer);
        set_hidden(ui->overlay_title, !(title && title[0]));
        set_hidden(ui->overlay_sub, !(subtitle && subtitle[0]));
        set_hidden(ui->overlay_footer, !(footer && footer[0]));
    }
    position_rows(ui, style);
    for (int i = 0; i < SUMMER_UI_ROWS; ++i) {
        if (i >= visible || i >= total) {
            set_hidden(ui->rows[i], true);
            continue;
        }
        const summer_ui_row_t *row = &rows[i];
        set_hidden(ui->rows[i], false);
        set_text(ui->row_text[i], row->text);
        set_text(ui->row_meta[i], row->meta);
        set_text(ui->row_preview[i], row->preview);
        set_hidden(ui->row_meta[i], !(row->meta && row->meta[0]));
        set_hidden(ui->row_preview[i], !(row->preview && row->preview[0]));
        const bool sel = (first + i) == selected;
        lv_obj_set_style_bg_color(ui->rows[i], lv_color_hex(sel ? COL_SELECT : COL_ROW_BG), 0);
        lv_obj_set_style_bg_opa(ui->rows[i], sel ? 255 : 210, 0);
        const uint32_t color = row->disabled ? COL_DIM : (sel ? COL_TEXT : 0xD5E5F2u);
        lv_obj_set_style_text_color(ui->row_text[i], lv_color_hex(color), 0);
        lv_obj_set_style_text_color(ui->row_meta[i], lv_color_hex(COL_DIM), 0);
        lv_obj_set_style_text_color(ui->row_preview[i], lv_color_hex(COL_DIM), 0);
    }
}

void summer_ui_set_window(summer_ui_t *ui, summer_list_style_t style, const char *title,
                          const char *subtitle, const char *footer, const summer_ui_row_t *rows,
                          int visible, int first, int total, int selected)
{
    if (!ui || !rows) return;
    if (visible < 0) visible = 0;
    if (visible > style_visible_rows(style)) visible = style_visible_rows(style);
    // Do not pad total to visible: a short list must leave its unused rows
    // hidden instead of showing blank, apparently selectable entries.
    if (total < 0) total = 0;
    if (first < 0) first = 0;
    if (first > total - visible) first = total - visible;
    if (first < 0) first = 0;
    if (selected < 0) selected = 0;
    if (selected >= total && total > 0) selected = total - 1;
    apply_window(ui, style, title, subtitle, footer, rows, visible, first, total, selected);
}

void summer_ui_set_rows(summer_ui_t *ui, summer_list_style_t style, const char *title,
                        const char *subtitle, const char *footer, const summer_ui_row_t *rows,
                        int count, int selected)
{
    if (!ui) return;
    const int visible = style_visible_rows(style);
    if (selected < 0) selected = 0;
    if (selected >= count && count > 0) selected = count - 1;
    int top = selected - visible / 2;
    if (top < 0) top = 0;
    if (top > count - visible) top = count - visible;
    if (top < 0) top = 0;
    apply_window(ui, style, title, subtitle, footer, rows + top, visible, top, count, selected);
}

void summer_ui_set_choice_rows(summer_ui_t *ui, const summer_ui_row_t *rows, int count,
                               int selected)
{
    summer_ui_set_rows(ui, SUMMER_LIST_COMPACT, "选择下一步", NULL, "长按 OK 打开菜单", rows,
                       count, selected);
}

void summer_ui_set_title_rows(summer_ui_t *ui, const summer_ui_row_t *rows, int count,
                              int selected, const char *progress)
{
    if (!ui) return;
    summer_ui_set_rows(ui, SUMMER_LIST_TITLE, NULL, NULL, NULL, rows, count, selected);
    set_text(ui->title_progress_label, progress);
}

void summer_ui_set_cg(summer_ui_t *ui, const char *caption, const char *hint)
{
    if (!ui) return;
    set_text(ui->cg_caption, caption);
    set_text(ui->cg_hint, hint);
}

void summer_ui_set_ending(summer_ui_t *ui, const char *title, const char *body)
{
    if (!ui) return;
    set_text(ui->ending_title, title);
    set_text(ui->ending_body, body);
}

void summer_ui_set_error(summer_ui_t *ui, const char *text)
{
    if (ui) set_text(ui->error_text, text);
}

void summer_ui_set_notice(summer_ui_t *ui, const char *text)
{
    if (!ui) return;
    if (!text || !text[0]) {
        set_hidden(ui->notice_label, true);
        return;
    }
    set_text(ui->notice_text, text);
    set_hidden(ui->notice_label, false);
    lv_obj_move_foreground(ui->notice_label);
}

void summer_ui_hide_notice(summer_ui_t *ui)
{
    if (ui) set_hidden(ui->notice_label, true);
}

void summer_ui_set_help(summer_ui_t *ui, const char *text)
{
    if (!ui) return;
    set_text(ui->help_label, text);
    set_hidden(ui->help_view, false);
    set_hidden(ui->overlay_title, true);
    set_hidden(ui->overlay_sub, true);
    set_hidden(ui->overlay_footer, true);
    for (int i = 0; i < SUMMER_UI_ROWS; ++i) set_hidden(ui->rows[i], true);
    lv_obj_move_foreground(ui->help_view);
}

void summer_ui_scroll(summer_ui_t *ui, int dy)
{
    if (ui && ui->help_view) lv_obj_scroll_by(ui->help_view, 0, dy, LV_ANIM_OFF);
}

int summer_ui_row_count(const summer_ui_t *ui)
{
    return ui ? ui->row_count : 0;
}

int summer_ui_row_selected(const summer_ui_t *ui)
{
    return ui ? ui->row_selected : 0;
}
