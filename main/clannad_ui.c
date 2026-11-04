// main/clannad_ui.c —— 作品专属横屏界面实现。
#include "clannad_ui.h"

#include "lvgl.h"

#include <stdio.h>
#include <string.h>

#define COL_BG 0x0C1814u
#define COL_PANEL 0x08120Fu
#define COL_TEXT 0xEAF3ECu
#define COL_DIM 0x8FAA9Cu
#define COL_ACCENT 0xBFE6C8u
#define COL_SEL 0x1E4A3Au
#define COL_WARN 0xF2B8A0u

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    lv_label_set_text(label, "");
    return label;
}

static lv_obj_t *mk_rect(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color,
                         lv_opa_t opa)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, opa, LV_PART_MAIN);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static lv_obj_t *mk_page(lv_obj_t *parent, uint32_t color, lv_opa_t opa)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, CLAN_SCREEN_W, CLAN_SCREEN_H);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, opa, LV_PART_MAIN);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    return obj;
}

static void row_hide(lv_obj_t *row, bool hidden)
{
    if (!row) return;
    if (hidden) lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(row, LV_OBJ_FLAG_HIDDEN);
}

bool clannad_ui_create(clannad_ui_t *ui, lv_obj_t *parent, uint16_t *canvas_pixels,
                       const lv_font_t *font16)
{
    if (!ui || !parent || !canvas_pixels || !font16) return false;
    memset(ui, 0, sizeof(*ui));
    ui->font = font16;
    ui->page = CLAN_UI_PAGE_COUNT;
    if (!clannad_image_init(&ui->image, parent, canvas_pixels)) return false;

    // ------------------------------------------------------------- title
    lv_obj_t *title = mk_page(parent, 0x000000, LV_OPA_TRANSP);
    ui->pages[CLAN_UI_TITLE] = title;
    (void)mk_rect(title, 0, CLAN_ART_H, CLAN_SCREEN_W, CLAN_SCREEN_H - CLAN_ART_H,
                  COL_PANEL, LV_OPA_COVER);
    ui->title_panel = mk_rect(title, 190, 74, 126, 162, 0x08120F, LV_OPA_80);
    lv_obj_set_style_radius(ui->title_panel, 6, LV_PART_MAIN);
    ui->title_sel = mk_rect(title, 193, 78, 120, 20, COL_SEL, LV_OPA_COVER);
    lv_obj_set_style_radius(ui->title_sel, 4, LV_PART_MAIN);
    for (int i = 0; i < CLAN_UI_TITLE_ROWS; ++i) {
        lv_obj_t *label = mk_label(title, font16, COL_TEXT);
        lv_obj_set_pos(label, 200, 80 + i * 22);
        lv_obj_set_size(label, 108, 18);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
        ui->title_rows[i] = label;
    }
    ui->title_progress = mk_label(title, font16, COL_ACCENT);
    lv_obj_set_pos(ui->title_progress, 8, 216);
    lv_obj_set_size(ui->title_progress, 150, 18);
    ui->title_hint = mk_label(title, font16, COL_DIM);
    lv_obj_set_pos(ui->title_hint, 166, 216);
    lv_obj_set_size(ui->title_hint, 148, 18);
    lv_obj_set_style_text_align(ui->title_hint, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

    // -------------------------------------------------------------- game
    lv_obj_t *game = mk_page(parent, 0x000000, LV_OPA_TRANSP);
    ui->pages[CLAN_UI_GAME] = game;
    // 文本框：半透明深色底 @170(≈67%)。透明底+白字在亮背景(天空/雪地)上不可读，
    // 此轮以可读优先（与用户先前“完全透明”要求冲突，待用户确认）。
    ui->game_panel = mk_rect(game, 0, CLAN_PANEL_Y, CLAN_IMG_W, CLAN_PANEL_H,
                             COL_PANEL, 170);
    ui->speaker = mk_label(game, font16, COL_ACCENT);
    lv_obj_set_pos(ui->speaker, 8, CLAN_NAME_Y);
    // 名字条只包住文字本身（内容宽度 + 左右 6px），不再固定 164 拉满。
    lv_obj_set_size(ui->speaker, LV_SIZE_CONTENT, 16);
    lv_obj_set_style_pad_hor(ui->speaker, 6, LV_PART_MAIN);
    // 名字在框外（压在画面上）：底条只包文字 + 描边保证对比。
    lv_obj_set_style_bg_color(ui->speaker, lv_color_hex(COL_PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->speaker, 180, LV_PART_MAIN);
    lv_obj_set_style_text_outline_stroke_width(ui->speaker, 2, LV_PART_MAIN);
    lv_obj_set_style_text_outline_stroke_color(ui->speaker, lv_color_hex(0x000000),
                                               LV_PART_MAIN);
    // 章节/日期（“共通线 4月…”）贴顶：y=4..22，半透明底条保证可读，不压立绘脸部。
    ui->chapter = mk_label(game, font16, COL_DIM);
    lv_obj_set_pos(ui->chapter, 8, 4);
    // 底条只包文字：宽度严格 LV_SIZE_CONTENT，不继承父容器/padding。
    lv_obj_set_width(ui->chapter, LV_SIZE_CONTENT);
    lv_obj_set_height(ui->chapter, 18);
    lv_obj_set_style_text_align(ui->chapter, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui->chapter, lv_color_hex(COL_PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->chapter, 180, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(ui->chapter, 6, LV_PART_MAIN);
    ui->page_mark = mk_label(game, font16, COL_DIM);
    lv_obj_set_style_text_outline_stroke_width(ui->page_mark, 2, LV_PART_MAIN);
    lv_obj_set_style_text_outline_stroke_color(ui->page_mark, lv_color_hex(0x000000),
                                               LV_PART_MAIN);
    lv_obj_set_pos(ui->page_mark, 274, CLAN_PANEL_Y + 2);
    lv_obj_set_size(ui->page_mark, 40, 16);
    lv_obj_set_style_text_align(ui->page_mark, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    for (int i = 0; i < CLAN_TEXT_LINES; ++i) {
        lv_obj_t *line = mk_label(game, font16, COL_TEXT);
        lv_obj_set_pos(line, 8, CLAN_LINES_TOP + i * 18);
        lv_obj_set_size(line, 304, 18);
        lv_label_set_long_mode(line, LV_LABEL_LONG_CLIP);
        // 透明文本框：正文纯白 + 1px 黑描边（16px 字用 2px 会把字吃成黑块），并置顶。
        lv_obj_set_style_text_color(line, lv_color_hex(0xFFFFFFu), LV_PART_MAIN);
        lv_obj_set_style_text_outline_stroke_width(line, 1, LV_PART_MAIN);
        lv_obj_set_style_text_outline_stroke_color(line, lv_color_hex(0x000000u),
                                                   LV_PART_MAIN);
        lv_obj_move_foreground(line);
        ui->lines[i] = line;
    }

    // ------------------------------------------------------------ choice
    lv_obj_t *choice = mk_page(parent, 0x050D0A, LV_OPA_90);
    ui->pages[CLAN_UI_CHOICE] = choice;
    ui->choice_title = mk_label(choice, font16, COL_ACCENT);
    lv_obj_set_pos(ui->choice_title, 8, 6);
    lv_obj_set_size(ui->choice_title, 304, 18);
    ui->choice_sel = mk_rect(choice, 6, 34, 308, 38, COL_SEL, LV_OPA_COVER);
    lv_obj_set_style_radius(ui->choice_sel, 4, LV_PART_MAIN);
    for (int i = 0; i < CLAN_UI_CHOICE_ROWS; ++i) {
        lv_obj_t *row = mk_label(choice, font16, COL_TEXT);
        lv_obj_set_pos(row, 14, 38 + i * 42);
        lv_obj_set_size(row, 292, 34);
        lv_label_set_long_mode(row, LV_LABEL_LONG_WRAP);
        ui->choice_rows[i] = row;
    }
    ui->choice_hint = mk_label(choice, font16, COL_DIM);
    lv_obj_set_pos(ui->choice_hint, 8, 214);
    lv_obj_set_size(ui->choice_hint, 304, 18);
    lv_obj_set_style_text_align(ui->choice_hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    // -------------------------------------------------------------- list
    lv_obj_t *list = mk_page(parent, COL_BG, LV_OPA_COVER);
    ui->pages[CLAN_UI_LIST] = list;
    ui->list_title = mk_label(list, font16, COL_ACCENT);
    lv_obj_set_pos(ui->list_title, 8, 4);
    lv_obj_set_size(ui->list_title, 304, 20);
    ui->list_sel = mk_rect(list, 4, 30, 312, 36, COL_SEL, LV_OPA_COVER);
    lv_obj_set_style_radius(ui->list_sel, 4, LV_PART_MAIN);
    for (int i = 0; i < CLAN_UI_LIST_ROWS; ++i) {
        lv_obj_t *main_label = mk_label(list, font16, COL_TEXT);
        lv_obj_set_pos(main_label, 10, 32 + i * 38);
        lv_obj_set_size(main_label, 236, 18);
        lv_label_set_long_mode(main_label, LV_LABEL_LONG_CLIP);
        lv_obj_t *value = mk_label(list, font16, COL_DIM);
        lv_obj_set_pos(value, 244, 32 + i * 38);
        lv_obj_set_size(value, 68, 18);
        lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
        lv_obj_t *sub = mk_label(list, font16, COL_DIM);
        lv_obj_set_pos(sub, 10, 50 + i * 38);
        lv_obj_set_size(sub, 300, 16);
        lv_label_set_long_mode(sub, LV_LABEL_LONG_CLIP);
        ui->list_rows[i][0] = main_label;
        ui->list_rows[i][1] = value;
        ui->list_rows[i][2] = sub;
    }
    ui->list_hint = mk_label(list, font16, COL_DIM);
    lv_obj_set_pos(ui->list_hint, 8, 216);
    lv_obj_set_size(ui->list_hint, 304, 18);
    lv_obj_set_style_text_align(ui->list_hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    // ---------------------------------------------------------------- cg
    lv_obj_t *cg = mk_page(parent, 0x000000, LV_OPA_TRANSP);
    ui->pages[CLAN_UI_CG] = cg;
    lv_obj_t *bar = mk_rect(cg, 0, CLAN_ART_H, CLAN_SCREEN_W,
                            CLAN_SCREEN_H - CLAN_ART_H, COL_PANEL, LV_OPA_COVER);
    (void)bar;
    ui->cg_caption = mk_label(cg, font16, COL_TEXT);
    lv_obj_set_pos(ui->cg_caption, 8, CLAN_ART_H + 10);
    lv_obj_set_size(ui->cg_caption, 304, 18);
    ui->cg_hint = mk_label(cg, font16, COL_DIM);
    lv_obj_set_pos(ui->cg_hint, 8, CLAN_ART_H + 34);
    lv_obj_set_size(ui->cg_hint, 304, 14);
    lv_obj_set_style_text_align(ui->cg_hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    // ---------------------------------------------------------- message
    lv_obj_t *msg = mk_page(parent, 0x0A1510, LV_OPA_COVER);
    ui->pages[CLAN_UI_MESSAGE] = msg;
    ui->msg_title = mk_label(msg, font16, COL_WARN);
    lv_obj_set_pos(ui->msg_title, 12, 36);
    lv_obj_set_size(ui->msg_title, 296, 20);
    lv_obj_set_style_text_align(ui->msg_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    ui->msg_body = mk_label(msg, font16, COL_TEXT);
    lv_obj_set_pos(ui->msg_body, 12, 70);
    lv_obj_set_size(ui->msg_body, 296, 120);
    lv_label_set_long_mode(ui->msg_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(ui->msg_body, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    ui->msg_hint = mk_label(msg, font16, COL_DIM);
    lv_obj_set_pos(ui->msg_hint, 12, 210);
    lv_obj_set_size(ui->msg_hint, 296, 18);
    lv_obj_set_style_text_align(ui->msg_hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    // ----------------------------------------------------------- notice
    ui->notice = mk_label(parent, font16, 0xFFF4E6);
    lv_obj_set_pos(ui->notice, 40, 150);
    lv_obj_set_size(ui->notice, 240, 18);
    lv_obj_set_style_text_align(ui->notice, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui->notice, lv_color_hex(0x14342A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->notice, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ui->notice, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(ui->notice, 4, LV_PART_MAIN);
    lv_obj_add_flag(ui->notice, LV_OBJ_FLAG_HIDDEN);

    clannad_ui_show(ui, CLAN_UI_TITLE);
    return true;
}

void clannad_ui_show(clannad_ui_t *ui, clannad_ui_page_t page)
{
    if (!ui || page >= CLAN_UI_PAGE_COUNT) return;
    for (int i = 0; i < CLAN_UI_PAGE_COUNT; ++i) {
        row_hide(ui->pages[i], i != (int)page);
    }
    ui->page = page;
}

void clannad_ui_title(clannad_ui_t *ui, const char *const *labels, int count,
                      int selected, const char *progress, const char *hint)
{
    if (!ui) return;
    if (count > CLAN_UI_TITLE_ROWS) count = CLAN_UI_TITLE_ROWS;
    memset(ui->title_main, 0, sizeof(ui->title_main));
    for (int i = 0; i < CLAN_UI_TITLE_ROWS; ++i) {
        const bool visible = i < count;
        row_hide(ui->title_rows[i], !visible);
        if (visible) {
            snprintf(ui->title_main[i], sizeof(ui->title_main[i]), "%s",
                     labels[i] ? labels[i] : "");
            lv_label_set_text(ui->title_rows[i], ui->title_main[i]);
        }
    }
    if (count <= 0) count = 1;
    if (selected < 0) selected = 0;
    if (selected >= count) selected = count - 1;
    lv_obj_set_pos(ui->title_sel, 193, 78 + selected * 22);
    lv_label_set_text(ui->title_progress, progress ? progress : "");
    lv_label_set_text(ui->title_hint, hint ? hint : "");
}

void clannad_ui_game(clannad_ui_t *ui, const char *speaker, const char *line0,
                     const char *line1, const char *line2, const char *chapter,
                     const char *page_mark)
{
    if (!ui) return;
    lv_label_set_text(ui->speaker, speaker ? speaker : "");
    lv_label_set_text(ui->chapter, chapter ? chapter : "");
    lv_label_set_text(ui->page_mark, page_mark ? page_mark : "");
    lv_label_set_text(ui->lines[0], line0 ? line0 : "");
    lv_label_set_text(ui->lines[1], line1 ? line1 : "");
    lv_label_set_text(ui->lines[2], line2 ? line2 : "");
}

void clannad_ui_choice(clannad_ui_t *ui, const char *title, const char *const *rows,
                       int count, int selected, int top, const char *hint)
{
    if (!ui) return;
    if (count > CLAN_UI_CHOICE_ROWS) count = CLAN_UI_CHOICE_ROWS;
    lv_label_set_text(ui->choice_title, title ? title : "");
    for (int i = 0; i < CLAN_UI_CHOICE_ROWS; ++i) {
        const bool visible = i < count;
        row_hide(ui->choice_rows[i], !visible);
        if (visible) lv_label_set_text(ui->choice_rows[i], rows[i] ? rows[i] : "");
    }
    if (top < 0) top = 0;
    if (selected < top) selected = top;
    if (selected >= top + CLAN_UI_CHOICE_ROWS) {
        selected = top + CLAN_UI_CHOICE_ROWS - 1;
    }
    const int relative = selected - top;
    lv_obj_set_pos(ui->choice_sel, 6, 34 + relative * 42);
    lv_label_set_text(ui->choice_hint, hint ? hint : "");
}

void clannad_ui_list(clannad_ui_t *ui, const char *title, const clannad_ui_row_t *rows,
                     int count, int selected, int top, const char *hint)
{
    if (!ui) return;
    if (count > CLAN_UI_LIST_ROWS) count = CLAN_UI_LIST_ROWS;
    lv_label_set_text(ui->list_title, title ? title : "");
    memset(ui->row_main, 0, sizeof(ui->row_main));
    memset(ui->row_sub, 0, sizeof(ui->row_sub));
    for (int i = 0; i < CLAN_UI_LIST_ROWS; ++i) {
        const bool visible = i < count;
        for (int k = 0; k < 3; ++k) row_hide(ui->list_rows[i][k], !visible);
        if (!visible) continue;
        snprintf(ui->row_main[i], sizeof(ui->row_main[i]), "%s", rows[i].main);
        snprintf(ui->row_sub[i], sizeof(ui->row_sub[i]), "%s", rows[i].sub);
        lv_label_set_text(ui->list_rows[i][0], ui->row_main[i]);
        lv_label_set_text(ui->list_rows[i][1], rows[i].value);
        lv_label_set_text(ui->list_rows[i][2], ui->row_sub[i]);
    }
    // 行整体垂直居中：top = (240 - rows*行高)/2，与 anemoi 一致。
    const int row_h = 38;
    const int y0 = (CLAN_SCREEN_H - (count > 0 ? count : 1) * row_h) / 2;
    for (int i = 0; i < count; ++i) {
        lv_obj_set_pos(ui->list_rows[i][0], 10, y0 + i * row_h);
        lv_obj_set_pos(ui->list_rows[i][1], 244, y0 + i * row_h);
        lv_obj_set_pos(ui->list_rows[i][2], 10, y0 + 18 + i * row_h);
    }
    if (top < 0) top = 0;
    if (selected < top) selected = top;
    if (selected >= top + CLAN_UI_LIST_ROWS) selected = top + CLAN_UI_LIST_ROWS - 1;
    const int relative = selected - top;
    row_hide(ui->list_sel, count <= 0);
    lv_obj_set_pos(ui->list_sel, 4, y0 - 2 + relative * row_h);
    lv_label_set_text(ui->list_hint, hint ? hint : "");
}

void clannad_ui_cg(clannad_ui_t *ui, const char *caption, const char *hint)
{
    if (!ui) return;
    lv_label_set_text(ui->cg_caption, caption ? caption : "");
    lv_label_set_text(ui->cg_hint, hint ? hint : "");
}

void clannad_ui_message(clannad_ui_t *ui, const char *title, const char *body,
                        const char *hint)
{
    if (!ui) return;
    lv_label_set_text(ui->msg_title, title ? title : "");
    lv_label_set_text(ui->msg_body, body ? body : "");
    lv_label_set_text(ui->msg_hint, hint ? hint : "");
}

void clannad_ui_notice(clannad_ui_t *ui, const char *text)
{
    if (!ui) return;
    if (text && text[0]) {
        lv_label_set_text(ui->notice, text);
        lv_obj_remove_flag(ui->notice, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(ui->notice);
    } else {
        lv_label_set_text(ui->notice, "");
        lv_obj_add_flag(ui->notice, LV_OBJ_FLAG_HIDDEN);
    }
}

clannad_ui_page_t clannad_ui_page(const clannad_ui_t *ui)
{
    return ui ? ui->page : CLAN_UI_PAGE_COUNT;
}

const char *clannad_ui_row_main(const clannad_ui_t *ui, int row)
{
    if (!ui || row < 0 || row >= CLAN_UI_LIST_ROWS) return "";
    return ui->row_main[row];
}

const char *clannad_ui_row_sub(const clannad_ui_t *ui, int row)
{
    if (!ui || row < 0 || row >= CLAN_UI_LIST_ROWS) return "";
    return ui->row_sub[row];
}
