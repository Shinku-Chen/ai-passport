// main/gal_ui.c —— 作品专属横屏阅读界面实现。
#include "gal_ui.h"

#include "lvgl.h"

#include <stdio.h>
#include <string.h>

extern const lv_font_t manosaba_16;
#define UI_FONT (&manosaba_16)
#define UI_LINE 19

#define COL_BG      0x1A1319
#define COL_PANEL   0x2A1D27
#define COL_ROW     0x3A2733
#define COL_ROW_SEL 0x6B3B4E
#define COL_FRAME   0x8A5A6B
#define COL_TEXT    0xF2E7E2
#define COL_DIM     0xBCA7AD
#define COL_ACCENT  0xF3C9C9
#define COL_WARN    0xFFB4A2

static lv_obj_t *mk_box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, int opa)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, (lv_opa_t)opa, LV_PART_MAIN);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static void set_hidden(lv_obj_t *o, bool hidden);

static lv_obj_t *mk_label(lv_obj_t *parent, int x, int y, int w, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_pos(l, x, y);
    if (w > 0) lv_obj_set_width(l, w);
    lv_obj_set_style_text_font(l, UI_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_line_space(l, 0, LV_PART_MAIN);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    if (w > 0) lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(l, "");
    return l;
}

static void style_row(lv_obj_t *row, bool selected, bool dim)
{
    lv_obj_set_style_bg_color(row, lv_color_hex(selected ? COL_ROW_SEL : COL_ROW), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, selected ? LV_OPA_COVER : LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 3, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, lv_color_hex(selected ? COL_ACCENT :
                                     (dim ? COL_ROW : COL_FRAME)), LV_PART_MAIN);
}

bool gal_ui_init(gal_ui_t *ui, struct _lv_obj_t *root)
{
    if (!ui || !root) return false;
    memset(ui, 0, sizeof(*ui));
    ui->root = root;
    ui->page = GAL_UI_TITLE;
    // The default LVGL screen is light grey. Paint the application backdrop so
    // an uncovered canvas pixel can never flash white on this dark reader UI.
    lv_obj_set_style_bg_color(root, lv_color_hex(COL_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);

    // 标题页:右侧竖排菜单,左侧留给标题画布
    ui->title_box = mk_box(root, 192, 0, 128, 240, COL_PANEL, 224);
    lv_obj_set_style_border_width(ui->title_box, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(ui->title_box, LV_BORDER_SIDE_LEFT, LV_PART_MAIN);
    lv_obj_set_style_border_color(ui->title_box, lv_color_hex(COL_FRAME), LV_PART_MAIN);
    ui->title_name = mk_label(ui->title_box, 8, 6, 112, COL_TEXT);
    lv_obj_set_style_text_align(ui->title_name, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    ui->title_ver = mk_label(ui->title_box, 8, 44, 112, COL_DIM);
    lv_obj_set_style_text_align(ui->title_ver, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    // 正文页:不透明对白带压在 176..239
    ui->panel = mk_box(root, 0, GAL_UI_PANEL_Y, GAL_UI_W, GAL_UI_PANEL_H, COL_PANEL, 180);   // 与 33750153 相同的半透值
    lv_obj_set_style_border_width(ui->panel, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(ui->panel, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_set_style_border_color(ui->panel, lv_color_hex(COL_FRAME), LV_PART_MAIN);
    ui->speaker = mk_label(root, GAL_UI_NAME_X, GAL_UI_NAME_Y, 0, COL_ACCENT);
    // 名字框在画面上、面板外；宽度随文字变化，仅保留 3px 左右内边距。
    lv_obj_set_width(ui->speaker, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_left(ui->speaker, GAL_UI_NAME_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_right(ui->speaker, GAL_UI_NAME_PAD, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui->speaker, lv_color_hex(COL_PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->speaker, 180, LV_PART_MAIN);
    lv_obj_set_style_radius(ui->speaker, 2, LV_PART_MAIN);

    // HP 与页码放在同一顶栏；hint 仅保留隐藏对象，阅读页不再显示。
    ui->hint = mk_label(ui->panel, 160, 2, 78, COL_ACCENT);
    lv_obj_set_style_text_align(ui->hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    set_hidden(ui->hint, true);
    ui->hp = mk_label(root, GAL_UI_TOP_HP_X, 2, 0, COL_DIM);
    lv_obj_set_width(ui->hp, LV_SIZE_CONTENT);
    ui->page_label = mk_label(root, GAL_UI_TOP_PAGE_X, 2, 0, COL_DIM);
    lv_obj_set_width(ui->page_label, LV_SIZE_CONTENT);
    ui->text = mk_label(ui->panel, GAL_UI_TEXT_X, GAL_UI_TEXT_Y, GAL_UI_TEXT_W, COL_TEXT);
    lv_obj_set_height(ui->text, GAL_UI_TEXT_H);

    // 列表页:全屏不透明
    ui->list_page = mk_box(root, 0, 0, GAL_UI_W, GAL_UI_H, COL_BG, 255);
    ui->list_title = mk_label(ui->list_page, 8, 2, 230, COL_TEXT);
    ui->list_sub = mk_label(ui->list_page, 8, 20, 304, COL_DIM);
    for (int i = 0; i < GAL_UI_LIST_ROWS; ++i) {
        lv_obj_t *row = mk_box(ui->list_page, 6, 42 + i * 40, 308, 38, COL_ROW, LV_OPA_70);
        ui->list_rows[i] = row;
        ui->list_labels[i] = mk_label(row, 5, 3, 296, COL_TEXT);
    }
    ui->list_footer = mk_label(ui->list_page, 8, 218, 304, COL_DIM);

    // 纯文本页(关于/结局/错误/手册详情)
    ui->text_page = mk_box(root, 0, 0, GAL_UI_W, GAL_UI_H, COL_BG, 255);
    ui->text_title = mk_label(ui->text_page, 8, 4, 304, COL_ACCENT);
    ui->text_body = mk_label(ui->text_page, 8, 28, 304, COL_TEXT);
    lv_obj_set_height(ui->text_body, 180);
    ui->text_footer = mk_label(ui->text_page, 8, 216, 304, COL_DIM);

    // CG 全屏:只留底部字幕条
    ui->cg_bar = mk_box(root, 0, GAL_UI_CG_BAR_Y, GAL_UI_W, GAL_UI_CG_BAR_H, COL_PANEL, 230);
    ui->cg_caption = mk_label(ui->cg_bar, 6, 3, 176, COL_TEXT);
    ui->cg_hint = mk_label(ui->cg_bar, 186, 3, 130, COL_DIM);
    lv_obj_set_style_text_align(ui->cg_hint, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_label_set_text(ui->cg_hint, "OK差分 · 长OK返回");

    // 电量：文字本身就是背板，宽度随 “98%” 内容变化。
    ui->battery_label = mk_label(root, GAL_UI_BATTERY_X, 2, 0, COL_DIM);
    ui->battery = ui->battery_label;
    lv_obj_set_width(ui->battery_label, LV_SIZE_CONTENT);
    lv_obj_set_style_text_align(ui->battery_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui->battery_label, lv_color_hex(COL_PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->battery_label, 190, LV_PART_MAIN);
    lv_obj_set_style_radius(ui->battery_label, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_left(ui->battery_label, 3, LV_PART_MAIN);
    lv_obj_set_style_pad_right(ui->battery_label, 3, LV_PART_MAIN);

    // 背景底边171与面板176之间留4px底色，禁止背景纹理/立绘进入。
    // 提示条(后创建,永远在最上层)
    ui->notice_bg = mk_box(root, 20, 74, 280, 34, COL_ROW_SEL, 240);
    lv_obj_set_style_radius(ui->notice_bg, 4, LV_PART_MAIN);
    ui->notice = mk_label(ui->notice_bg, 5, 4, 270, COL_TEXT);
    lv_obj_set_style_text_align(ui->notice, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    // An empty toast must not be visible at boot. It used to sit above the
    // title menu forever because notice_until_ms starts at zero and tick()
    // therefore returned before hiding it.
    set_hidden(ui->notice_bg, true);

    gal_ui_show(ui, GAL_UI_TITLE);
    return true;
}

static void set_hidden(lv_obj_t *o, bool hidden)
{
    if (!o) return;
    if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}

void gal_ui_show(gal_ui_t *ui, gal_ui_page_t page)
{
    if (!ui) return;
    ui->page = page;
    set_hidden(ui->title_box, page != GAL_UI_TITLE);
    set_hidden(ui->panel, page != GAL_UI_GAME);
    set_hidden(ui->list_page, page != GAL_UI_LIST);
    set_hidden(ui->text_page, page != GAL_UI_TEXT);
    set_hidden(ui->cg_bar, page != GAL_UI_CG);
    set_hidden(ui->speaker, true);
    set_hidden(ui->hp, true);
    set_hidden(ui->page_label, true);
    // Do not hide the toast here: callers set a result notice before
    // render_*() (for example a successful manual save), and page navigation
    // must not swallow it. gal_ui_tick() owns its timeout lifecycle.
    // 电量:标题放左上,阅读页放右上。
    lv_obj_set_pos(ui->battery, page == GAL_UI_TITLE ? 6 : GAL_UI_BATTERY_X, 2);
}

void gal_ui_title(gal_ui_t *ui, const char *name, const char *sub,
                  const char *const *items, int count, int sel, const char *battery)
{
    if (!ui) return;
    lv_label_set_text(ui->title_name, name ? name : "");
    lv_label_set_text(ui->title_ver, sub ? sub : "");
    if (count > 8) count = 8;
    ui->title_count = count;
    for (int i = 0; i < 8; ++i) {
        if (i >= count || !items || !items[i]) {
            set_hidden(ui->title_rows[i], true);
            continue;
        }
        if (!ui->title_rows[i]) {
            ui->title_rows[i] = mk_box(ui->title_box, 6, 68 + i * 24, 116, 22, COL_ROW, LV_OPA_70);
            ui->title_labels[i] = mk_label(ui->title_rows[i], 4, 1, 108, COL_TEXT);
        }
        set_hidden(ui->title_rows[i], false);
        style_row(ui->title_rows[i], i == sel, false);
        char buf[96];
        snprintf(buf, sizeof(buf), "%s%s", i == sel ? "› " : "  ", items[i]);
        lv_label_set_text(ui->title_labels[i], buf);
    }
    lv_label_set_text(ui->battery_label, battery ? battery : "");
    set_hidden(ui->battery, !battery || !battery[0]);
    set_hidden(ui->speaker, true);
    set_hidden(ui->hp, true);
    set_hidden(ui->page_label, true);
}

static void set_speaker(gal_ui_t *ui, const char *speaker, int x, int y)
{
    if (!ui) return;
    lv_label_set_text(ui->speaker, speaker ? speaker : "");
    lv_obj_set_pos(ui->speaker, x, y);
    set_hidden(ui->speaker, !speaker || !speaker[0]);
}

void gal_ui_choice_name(gal_ui_t *ui, const char *speaker)
{
    set_speaker(ui, speaker, GAL_UI_CHOICE_NAME_X, 0);
}

void gal_ui_game(gal_ui_t *ui, const char *speaker, const char *text,
                 const char *hp, const char *page, const char *hint,
                 const char *battery)
{
    if (!ui) return;
    set_speaker(ui, speaker, GAL_UI_NAME_X, GAL_UI_NAME_Y);
    lv_label_set_text(ui->text, text ? text : "");
    lv_label_set_text(ui->battery_label, battery ? battery : "");
    set_hidden(ui->battery, !battery || !battery[0]);
    lv_obj_set_pos(ui->battery, GAL_UI_BATTERY_X, 2);
    lv_label_set_text(ui->hp, hp ? hp : "");
    lv_obj_set_pos(ui->hp, GAL_UI_TOP_HP_X, 2);
    set_hidden(ui->hp, !hp || !hp[0]);
    lv_label_set_text(ui->page_label, page ? page : "");
    lv_obj_set_pos(ui->page_label, GAL_UI_TOP_PAGE_X, 2);
    set_hidden(ui->page_label, !page || !page[0]);
    set_hidden(lv_obj_get_user_data(ui->root), false);
    lv_label_set_text(ui->hint, hint ? hint : "");
    set_hidden(ui->hint, true); // 阅读页不再显示按键提示；保留隐藏对象供旧测试/API 兼容
}

void gal_ui_list(gal_ui_t *ui, const char *title, const char *sub,
                 const gal_ui_row_t *rows, int count, int sel, const char *footer)
{
    if (!ui) return;
    lv_label_set_text(ui->list_title, title ? title : "");
    lv_label_set_text(ui->list_sub, sub ? sub : "");
    set_hidden(ui->list_sub, !sub || !sub[0]);
    for (int i = 0; i < GAL_UI_LIST_ROWS; ++i) {
        if (i >= count || !rows || !rows[i].text) {
            set_hidden(ui->list_rows[i], true);
            continue;
        }
        set_hidden(ui->list_rows[i], false);
        style_row(ui->list_rows[i], i == sel, rows[i].dim != 0);
        lv_label_set_text(ui->list_labels[i], rows[i].text);
    }
    lv_label_set_text(ui->list_footer, footer ? footer : "");
    lv_label_set_text(ui->battery_label, "");
    set_hidden(ui->battery, true);
    set_hidden(ui->speaker, true);
    set_hidden(ui->hp, true);
    set_hidden(ui->page_label, true);
}

void gal_ui_text(gal_ui_t *ui, const char *title, const char *body, const char *footer)
{
    if (!ui) return;
    lv_label_set_text(ui->text_title, title ? title : "");
    lv_label_set_text(ui->text_body, body ? body : "");
    lv_label_set_text(ui->text_footer, footer ? footer : "");
    lv_label_set_text(ui->battery_label, "");
    set_hidden(ui->battery, true);
    set_hidden(ui->speaker, true);
    set_hidden(ui->hp, true);
    set_hidden(ui->page_label, true);
}

void gal_ui_cg(gal_ui_t *ui, const char *caption)
{
    if (!ui) return;
    lv_label_set_text(ui->cg_caption, caption ? caption : "");
    lv_label_set_text(ui->battery_label, "");
    set_hidden(ui->battery, true);
    set_hidden(ui->speaker, true);
    set_hidden(ui->hp, true);
    set_hidden(ui->page_label, true);
}

void gal_ui_notice(gal_ui_t *ui, const char *text, int now_ms, int duration_ms)
{
    if (!ui) return;
    if (!text || !text[0]) {
        set_hidden(ui->notice_bg, true);
        ui->notice_until_ms = 0;
        return;
    }
    lv_label_set_text(ui->notice, text);
    set_hidden(ui->notice_bg, false);
    lv_obj_move_foreground(ui->notice_bg);
    ui->notice_until_ms = now_ms + (duration_ms > 0 ? duration_ms : 2000);
}

void gal_ui_tick(gal_ui_t *ui, int now_ms)
{
    if (!ui || ui->notice_until_ms == 0) return;
    if (now_ms >= ui->notice_until_ms) {
        set_hidden(ui->notice_bg, true);
        ui->notice_until_ms = 0;
    }
}
