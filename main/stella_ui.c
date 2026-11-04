// main/stella_ui.c —— 横屏 320x240 的界面实现。
#include "stella_ui.h"


#include <stdio.h>
#include <string.h>

#define C_BG 0x08110F
#define C_PANEL 0x0B2A22
#define C_PANEL_DARK 0x06100D
#define C_TEXT 0xF2F7F3
#define C_DIM 0x9FB7AE
#define C_SELECT 0xF2C14E
#define C_ACCENT 0x3E8E72

static void hide_all(stella_ui_t *ui)
{
    lv_obj_add_flag(ui->page_title, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui->page_game, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui->page_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui->page_cg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui->page_history, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui->page_ending, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui->page_confirm, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *panel(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, int opa)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, (lv_opa_t)opa, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

static lv_obj_t *label(lv_obj_t *parent, int x, int y, int w, int h, const char *text,
                       const lv_font_t *font, uint32_t color)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    // 默认单行 CLIP（列表行/正文按行排版），需要折行的控件自己再设回 WRAP。
    lv_label_set_long_mode(obj, LV_LABEL_LONG_MODE_CLIP);
    lv_label_set_text(obj, text ? text : "");
    return obj;
}

// 高亮条：固定行宽的独立底色矩形（不跟文本宽度走），圆角 6、无边框；
// 只在选中行置 70，其余强制 0，且只标脏该行 -> 不会出现旧框残留/边缘竖线。
static void row_style(lv_obj_t *row, int selected, bool value)
{
    if (!row) return;
    lv_obj_set_style_text_color(row, lv_color_hex(selected ? C_SELECT : (value ? C_DIM : C_TEXT)), 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_bg_opa(row, (lv_opa_t)(selected ? 70 : 0), 0);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_invalidate(row);   // 只重绘本行（切选中时只有旧/新两行被标脏）
}

static void set_row(lv_obj_t *row, const char *text)
{
    lv_label_set_text(row, text ? text : "");
}

bool stella_ui_create(stella_ui_t *ui, lv_obj_t *root, uint16_t *canvas_pixels,
                      const lv_font_t *font16, const lv_font_t *font20)
{
    if (!ui || !root || !canvas_pixels || !font16) return false;
    memset(ui, 0, sizeof(*ui));
    ui->root = root;
    ui->font16 = font16;
    ui->font20 = font20 ? font20 : font16;
    if (!stella_image_init(&ui->image, root, canvas_pixels)) return false;

    // 标题页
    ui->page_title = lv_obj_create(root);
    lv_obj_remove_style_all(ui->page_title);
    lv_obj_set_size(ui->page_title, STELLA_SCREEN_W, STELLA_SCREEN_H);
    lv_obj_remove_flag(ui->page_title, LV_OBJ_FLAG_SCROLLABLE);
    ui->title_count = 0;
    panel(ui->page_title, 0, 150, STELLA_SCREEN_W, 90, C_PANEL, 180);
    // 标题字压在图上（图是明亮天空）：给一层 0xB4 底条，否则白字不可读。
    panel(ui->page_title, 6, 8, STELLA_SCREEN_W - 12, 62, C_PANEL, 180);
    label(ui->page_title, 12, 14, 296, 30, "星之终途", ui->font20, C_TEXT);
    label(ui->page_title, 12, 44, 296, 22, "STELLA OF THE END / AFTER STORY", ui->font16, C_DIM);
    for (int i = 0; i < 4; ++i) {
        // 单列 4 行窗口：154/172/190/208，末行盒底 226，选中项跟随滚动。
        ui->title_rows[i] = label(ui->page_title, 8, 154 + i * 20, 300, 18, "",
                                  ui->font16, C_TEXT);
    }

    // 正文页
    ui->page_game = lv_obj_create(root);
    lv_obj_remove_style_all(ui->page_game);
    lv_obj_set_size(ui->page_game, STELLA_SCREEN_W, STELLA_SCREEN_H);
    lv_obj_remove_flag(ui->page_game, LV_OBJ_FLAG_SCROLLABLE);
    // 对话框 156..240（84 高，原 142..240/98 高：减 14px，给画面多留一条）。
    panel(ui->page_game, 0, 156, STELLA_SCREEN_W, 84, C_PANEL, 180);   // 0xB4
    // 名字在框外（压在画面上）：底条只包文字（内容宽 + 左右 6px），不再拉满整行。
    ui->name_box = label(ui->page_game, 8, 134, 0, 22, "", ui->font16, C_TEXT);
    // CLIP + LV_SIZE_CONTENT 在 LVGL9 里算不出内容宽 → 宽度退化为 0、文字全被裁掉
    //（实机“名字消失”）。姓名条必须用“单行 + 末尾省略”模式才能量出内容宽。
    lv_label_set_long_mode(ui->name_box, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(ui->name_box, LV_SIZE_CONTENT);
    lv_obj_set_height(ui->name_box, 22);
    lv_obj_set_style_pad_hor(ui->name_box, 6, 0);
    lv_obj_set_style_bg_color(ui->name_box, lv_color_hex(C_PANEL_DARK), 0);
    lv_obj_set_style_bg_opa(ui->name_box, 170, 0);
    // 页码挪到面板外的右上（138..156），正文才能贴到面板顶 +4。
    ui->page_label = label(ui->page_game, 262, 138, 50, 18, "", ui->font16, C_DIM);
    lv_obj_set_style_text_align(ui->page_label, LV_TEXT_ALIGN_RIGHT, 0);
    // 正文贴面板顶：首行 y = 156 + 4 = 160；4 行 x19 = 76 → 160..236。
    ui->text_label = label(ui->page_game, 8, 160, 304, 76, "", ui->font16, C_TEXT);
    lv_label_set_long_mode(ui->text_label, LV_LABEL_LONG_WRAP);
    ui->chapter_label = label(ui->page_game, 6, 2, 210, 22, "", ui->font16, C_DIM);
    ui->battery_label = label(ui->page_game, 258, 2, 56, 22, "", ui->font16, C_DIM);
    lv_obj_set_style_text_align(ui->battery_label, LV_TEXT_ALIGN_RIGHT, 0);

    ui->choice_box = panel(ui->page_game, 0, 110, STELLA_SCREEN_W, 130, C_PANEL_DARK, 235);
    ui->choice_prompt = label(ui->choice_box, 10, 8, 300, 64, "", ui->font16, C_TEXT);
    ui->choice_row = label(ui->choice_box, 10, 78, 300, 40, "", ui->font16, C_SELECT);
    lv_obj_set_style_text_align(ui->choice_row, LV_TEXT_ALIGN_CENTER, 0);
    ui->choice_label = ui->choice_row;
    lv_obj_add_flag(ui->choice_box, LV_OBJ_FLAG_HIDDEN);

    // 通用列表页（菜单/存档/设置/章节/鉴赏）
    ui->page_list = lv_obj_create(root);
    lv_obj_remove_style_all(ui->page_list);
    lv_obj_set_size(ui->page_list, STELLA_SCREEN_W, STELLA_SCREEN_H);
    lv_obj_set_style_bg_color(ui->page_list, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(ui->page_list, LV_OPA_COVER, 0);
    lv_obj_remove_flag(ui->page_list, LV_OBJ_FLAG_SCROLLABLE);
    ui->list_title = label(ui->page_list, 10, 4, 300, 26, "", ui->font20, C_TEXT);
    for (int i = 0; i < STELLA_LIST_ROWS; ++i) {
        ui->list_rows[i] = label(ui->page_list, 8, 34 + i * 26, 304, 24, "", ui->font16, C_TEXT);
        ui->list_values[i] = label(ui->page_list, 226, 34 + i * 26, 84, 24, "", ui->font16, C_DIM);
        lv_obj_set_style_text_align(ui->list_values[i], LV_TEXT_ALIGN_RIGHT, 0);
    }

    // CG 页
    ui->page_cg = lv_obj_create(root);
    lv_obj_remove_style_all(ui->page_cg);
    lv_obj_set_size(ui->page_cg, STELLA_SCREEN_W, STELLA_SCREEN_H);
    lv_obj_remove_flag(ui->page_cg, LV_OBJ_FLAG_SCROLLABLE);
    panel(ui->page_cg, 0, 212, STELLA_SCREEN_W, 28, C_PANEL_DARK, 190);
    ui->cg_caption = label(ui->page_cg, 8, 214, 304, 24, "", ui->font16, C_TEXT);

    // 历史页
    ui->page_history = lv_obj_create(root);
    lv_obj_remove_style_all(ui->page_history);
    lv_obj_set_size(ui->page_history, STELLA_SCREEN_W, STELLA_SCREEN_H);
    lv_obj_set_style_bg_color(ui->page_history, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(ui->page_history, LV_OPA_COVER, 0);
    lv_obj_remove_flag(ui->page_history, LV_OBJ_FLAG_SCROLLABLE);
    label(ui->page_history, 10, 4, 300, 26, "历史记录", ui->font20, C_TEXT);
    ui->history_view = lv_obj_create(ui->page_history);
    lv_obj_remove_style_all(ui->history_view);
    lv_obj_set_pos(ui->history_view, 6, 32);
    lv_obj_set_size(ui->history_view, 308, 200);
    lv_obj_set_style_bg_color(ui->history_view, lv_color_hex(C_PANEL_DARK), 0);
    lv_obj_set_style_bg_opa(ui->history_view, 160, 0);
    lv_obj_set_scroll_dir(ui->history_view, LV_DIR_VER);
    ui->history_label = label(ui->history_view, 4, 4, 292, 900, "", ui->font16, C_TEXT);

    // 结局页
    ui->page_ending = lv_obj_create(root);
    lv_obj_remove_style_all(ui->page_ending);
    lv_obj_set_size(ui->page_ending, STELLA_SCREEN_W, STELLA_SCREEN_H);
    lv_obj_set_style_bg_color(ui->page_ending, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(ui->page_ending, LV_OPA_COVER, 0);
    lv_obj_remove_flag(ui->page_ending, LV_OBJ_FLAG_SCROLLABLE);
    ui->ending_label = label(ui->page_ending, 16, 70, 288, 110, "", ui->font20, C_TEXT);
    lv_obj_set_style_text_align(ui->ending_label, LV_TEXT_ALIGN_CENTER, 0);

    // 确认弹窗
    ui->page_confirm = panel(root, 20, 62, 280, 116, C_PANEL, 245);
    ui->confirm_prompt = label(ui->page_confirm, 12, 8, 256, 48, "", ui->font16, C_TEXT);
    ui->confirm_rows[0] = label(ui->page_confirm, 28, 62, 100, 28, "确定", ui->font16, C_TEXT);
    ui->confirm_rows[1] = label(ui->page_confirm, 168, 62, 100, 28, "取消", ui->font16, C_TEXT);
    lv_obj_set_style_text_align(ui->confirm_rows[0], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(ui->confirm_rows[1], LV_TEXT_ALIGN_CENTER, 0);

    ui->notice_label = panel(root, 16, 198, 288, 34, C_PANEL_DARK, 235);
    ui->notice_text = label(ui->notice_label, 2, 5, 284, 24, "", ui->font16, C_SELECT);
    lv_obj_add_flag(ui->notice_label, LV_OBJ_FLAG_HIDDEN);

    stella_ui_show(ui, STELLA_PAGE_TITLE);
    return true;
}

void stella_ui_show(stella_ui_t *ui, stella_page_t page)
{
    if (!ui) return;
    hide_all(ui);
    switch (page) {
    case STELLA_PAGE_TITLE: lv_obj_remove_flag(ui->page_title, LV_OBJ_FLAG_HIDDEN); break;
    case STELLA_PAGE_GAME: lv_obj_remove_flag(ui->page_game, LV_OBJ_FLAG_HIDDEN); break;
    case STELLA_PAGE_MENU:
    case STELLA_PAGE_SLOTS:
    case STELLA_PAGE_SETTINGS:
    case STELLA_PAGE_CHAPTERS:
    case STELLA_PAGE_GALLERY:
        lv_obj_remove_flag(ui->page_list, LV_OBJ_FLAG_HIDDEN);
        break;
    case STELLA_PAGE_CG: lv_obj_remove_flag(ui->page_cg, LV_OBJ_FLAG_HIDDEN); break;
    case STELLA_PAGE_HISTORY: lv_obj_remove_flag(ui->page_history, LV_OBJ_FLAG_HIDDEN); break;
    case STELLA_PAGE_ENDING: lv_obj_remove_flag(ui->page_ending, LV_OBJ_FLAG_HIDDEN); break;
    case STELLA_PAGE_CONFIRM: lv_obj_remove_flag(ui->page_confirm, LV_OBJ_FLAG_HIDDEN); break;
    }
}

void stella_ui_set_title_rows(stella_ui_t *ui, const char *const *rows, int count, int selected)
{
    if (!ui) return;
    ui->title_count = count > 8 ? 8 : count;
    ui->title_selected = selected;
    // 单列窗口：first = clamp(sel-1, 0, count-VIS)，让选中项尽量居中。
    int first = selected - 1;
    if (first > ui->title_count - 4) first = ui->title_count - 4;
    if (first < 0) first = 0;
    ui->title_first = first;
    for (int i = 0; i < 4; ++i) {
        const int index = first + i;
        if (index < ui->title_count) {
            set_row(ui->title_rows[i], rows[index]);
            row_style(ui->title_rows[i], index == selected, false);
            lv_obj_remove_flag(ui->title_rows[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            set_row(ui->title_rows[i], "");
            lv_obj_add_flag(ui->title_rows[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void stella_ui_set_list(stella_ui_t *ui, const char *title, const char *const *rows,
                        const char *const *values, int count, int selected)
{
    if (!ui) return;
    lv_label_set_text(ui->list_title, title ? title : "");
    ui->list_count = count > STELLA_LIST_ROWS ? STELLA_LIST_ROWS : count;
    ui->list_selected = selected;
    for (int i = 0; i < STELLA_LIST_ROWS; ++i) {
        if (i < ui->list_count) {
            set_row(ui->list_rows[i], rows[i]);
            set_row(ui->list_values[i], values ? values[i] : "");
            row_style(ui->list_rows[i], i == selected, false);
            row_style(ui->list_values[i], false, true);   // 数值列不参与高亮
            lv_obj_remove_flag(ui->list_rows[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(ui->list_values[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ui->list_rows[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(ui->list_values[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void stella_ui_list_select(stella_ui_t *ui, int selected)
{
    if (!ui) return;
    ui->list_selected = selected;
    for (int i = 0; i < ui->list_count; ++i) {
        row_style(ui->list_rows[i], i == selected, false);
        row_style(ui->list_values[i], false, true);
    }
}

void stella_ui_set_text(stella_ui_t *ui, const char *text)
{
    if (!ui) return;
    lv_label_set_text(ui->text_label, text ? text : "");
}

void stella_ui_set_page(stella_ui_t *ui, int page, int pages)
{
    if (!ui || !ui->page_label) return;
    if (pages > 1) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d/%d", page + 1, pages);
        lv_label_set_text(ui->page_label, buf);
        lv_obj_remove_flag(ui->page_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(ui->page_label, "");
        lv_obj_add_flag(ui->page_label, LV_OBJ_FLAG_HIDDEN);
    }
}

void stella_ui_set_game(stella_ui_t *ui, const char *name, const char *text, bool large,
                        const char *chapter, int battery)
{
    if (!ui) return;
    ui->large = large;
    const lv_font_t *font = large ? ui->font20 : ui->font16;
    lv_obj_set_style_text_font(ui->text_label, font, 0);
    lv_obj_set_style_text_font(ui->name_box, font, 0);
    lv_label_set_text(ui->name_box, name ? name : "");
    lv_label_set_text(ui->chapter_label, chapter ? chapter : "");
    if (text) stella_ui_set_text(ui, text);
    stella_ui_set_battery(ui, battery);
}

void stella_ui_show_choices(stella_ui_t *ui, const char *prompt, const char *option)
{
    if (!ui) return;
    lv_label_set_text(ui->choice_prompt, prompt ? prompt : "");
    lv_label_set_text(ui->choice_label, option ? option : "");
    lv_obj_remove_flag(ui->choice_box, LV_OBJ_FLAG_HIDDEN);
}

void stella_ui_hide_choices(stella_ui_t *ui)
{
    if (ui) lv_obj_add_flag(ui->choice_box, LV_OBJ_FLAG_HIDDEN);
}

void stella_ui_set_battery(stella_ui_t *ui, int battery)
{
    if (!ui) return;
    char buf[32];
    if (battery < 0) snprintf(buf, sizeof(buf), "--");
    else snprintf(buf, sizeof(buf), "%d%%", battery);
    lv_label_set_text(ui->battery_label, buf);
}

void stella_ui_set_cg_caption(stella_ui_t *ui, const char *caption)
{
    if (ui) lv_label_set_text(ui->cg_caption, caption ? caption : "");
}

void stella_ui_set_history(stella_ui_t *ui, const char *text)
{
    if (!ui) return;
    lv_label_set_text(ui->history_label, text ? text : "");
    lv_obj_scroll_to_y(ui->history_view, 0, LV_ANIM_OFF);
}

void stella_ui_history_scroll(stella_ui_t *ui, int dy)
{
    if (!ui) return;
    lv_obj_scroll_by(ui->history_view, 0, dy, LV_ANIM_OFF);
}

void stella_ui_set_ending(stella_ui_t *ui, const char *text)
{
    if (ui) lv_label_set_text(ui->ending_label, text ? text : "");
}

void stella_ui_set_confirm(stella_ui_t *ui, const char *prompt, const char *yes,
                           const char *no, int selected)
{
    if (!ui) return;
    lv_label_set_text(ui->confirm_prompt, prompt ? prompt : "");
    lv_label_set_text(ui->confirm_rows[0], yes ? yes : "确定");
    lv_label_set_text(ui->confirm_rows[1], no ? no : "取消");
    stella_ui_confirm_select(ui, selected);
}

void stella_ui_notice(stella_ui_t *ui, const char *text)
{
    if (!ui || !ui->notice_label) return;
    if (ui->notice_text) lv_label_set_text(ui->notice_text, text ? text : "");
    lv_obj_remove_flag(ui->notice_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ui->notice_label);
}

void stella_ui_notice_hide(stella_ui_t *ui)
{
    if (ui && ui->notice_label) lv_obj_add_flag(ui->notice_label, LV_OBJ_FLAG_HIDDEN);
}

void stella_ui_confirm_select(stella_ui_t *ui, int selected)
{
    if (!ui) return;
    ui->confirm_selected = selected;
    row_style(ui->confirm_rows[0], selected == 0, false);
    row_style(ui->confirm_rows[1], selected == 1, false);
}
