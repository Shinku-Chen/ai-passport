// main/eden_ui.c —— eden* 横屏界面实现。
//
// 与框架骨架 gal_ui 的关系:版式与控件全部按横屏 320x240 重写(骨架是竖屏 240x320),
// 画面层改成 LVGL 画布直读(骨架只吃 lv_image_dsc_t),CG 鉴赏改成按页翻,
// 章节列表改成可滚动。共用的是 LVGL 用法与"列表行懒创建"的思路。
#include "eden_ui.h"

#include "eden_image.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

#define EDEN_COL_BG        0x0B0F16u
#define EDEN_COL_BG_TOP    0x16202Eu
#define EDEN_COL_PANEL     0x121A26u
#define EDEN_COL_TEXT      0xECF2F8u
#define EDEN_COL_DIM       0x8FA0B4u
#define EDEN_COL_ACCENT    0x5BA8F0u
#define EDEN_COL_ROW       0x1A2433u
#define EDEN_COL_ROW_SEL   0x24344Au

static eden_page_t s_current = EDEN_PAGE_START;

// ---------------------------------------------------------------- 小工具
static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    return label;
}

static lv_obj_t *mk_page(lv_obj_t *parent, bool opaque)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_set_size(page, EDEN_UI_W, EDEN_UI_H);
    lv_obj_set_pos(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(page, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(page, 0, LV_PART_MAIN);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    if (opaque) {
        lv_obj_set_style_bg_color(page, lv_color_hex(EDEN_COL_BG), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(page, LV_OPA_COVER, LV_PART_MAIN);
    } else {
        lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, LV_PART_MAIN);
    }
    lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
    return page;
}

static lv_obj_t *list_create(eden_ui_t *ui, eden_list_t id, lv_obj_t *page, int row_h,
                             int top, int inset)
{
    eden_list_ui_t *list = &ui->lists[id];
    memset(list, 0, sizeof(*list));
    list->page = page;
    list->row_h = row_h;
    list->top = top;
    list->inset = inset;
    list->selected = 0;

    lv_obj_t *view = lv_obj_create(page);
    lv_obj_set_size(view, EDEN_UI_W - 2 * inset, EDEN_UI_H - top - 6);
    lv_obj_set_pos(view, inset, top);
    lv_obj_set_style_pad_all(view, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(view, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(view, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_radius(view, 0, LV_PART_MAIN);
    lv_obj_set_scroll_dir(view, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(view, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(view, LV_OBJ_FLAG_HIDDEN);
    list->view = view;
    return view;
}

static lv_obj_t *list_ensure_row(eden_ui_t *ui, eden_list_t id, int index)
{
    eden_list_ui_t *list = &ui->lists[id];
    if (index < 0 || index >= EDEN_ROWS_MAX) {
        return NULL;
    }
    if (list->rows[index] != NULL) {
        return list->rows[index];
    }
    lv_obj_t *row = lv_obj_create(list->view);
    lv_obj_set_size(row, EDEN_UI_W - 2 * list->inset - 4, list->row_h);
    lv_obj_set_pos(row, 0, index * (list->row_h + 2));
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row, lv_color_hex(EDEN_COL_ROW), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = mk_label(row, ui->font_small, EDEN_COL_TEXT);
    lv_obj_set_pos(label, 10, (list->row_h - 20) / 2);
    // 行标题必须单行（LV_LABEL_LONG_DOT 是“多行+省略号”，会把长条目折成两行）；
    // 这里用 LVGL9 的单行点选模式，并把宽度放大到 >=200（数值区缩到 64）。
    const int label_w = EDEN_UI_W - 2 * list->inset - 4 - 10 - 64 - 6;
    lv_obj_set_width(label, label_w);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(label, "");
    lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);

    // 右侧数值:设置项的值、存档槽位的时间提示都放这里。
    lv_obj_t *value = mk_label(row, ui->font_small, EDEN_COL_DIM);
    lv_obj_set_width(value, 64);
    lv_obj_set_pos(value, EDEN_UI_W - 2 * list->inset - 4 - 64 - 10, (list->row_h - 20) / 2);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_label_set_text(value, "");
    lv_obj_add_flag(value, LV_OBJ_FLAG_HIDDEN);

    list->rows[index] = row;
    return row;
}

static lv_obj_t *row_label(lv_obj_t *row)
{
    return lv_obj_get_child(row, 0);
}

static lv_obj_t *row_value(lv_obj_t *row)
{
    return lv_obj_get_child(row, 1);
}

static void list_paint_selection(eden_ui_t *ui, eden_list_t id)
{
    eden_list_ui_t *list = &ui->lists[id];
    for (int index = 0; index < list->count; index++) {
        lv_obj_t *row = list->rows[index];
        if (row == NULL) {
            continue;
        }
        const bool selected = index == list->selected;
        lv_obj_set_style_bg_color(row, lv_color_hex(selected ? EDEN_COL_ROW_SEL : EDEN_COL_ROW),
                                  LV_PART_MAIN);
        if (selected) {
            lv_obj_set_style_border_width(row, 2, LV_PART_MAIN);
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, LV_PART_MAIN);
            lv_obj_set_style_border_color(row, lv_color_hex(EDEN_COL_ACCENT), LV_PART_MAIN);
        } else {
            lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
        }
    }
    if (list->selected >= 0 && list->selected < EDEN_ROWS_MAX && list->rows[list->selected]) {
        lv_obj_scroll_to_view(list->rows[list->selected], LV_ANIM_OFF);
    }
}

// ---------------------------------------------------------------- 各页面
static void build_dialogue_page(eden_ui_t *ui)
{
    lv_obj_t *page = ui->pages[EDEN_PAGE_GAME];
    lv_obj_set_style_bg_color(page, lv_color_hex(0x000000u), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, LV_PART_MAIN);

    // 画布的父对象:app 之后把画布挂进来。
    ui->canvas_slot = lv_obj_create(page);
    lv_obj_set_size(ui->canvas_slot, EDEN_FRAME_W, EDEN_CANVAS_H);
    lv_obj_set_pos(ui->canvas_slot, EDEN_CANVAS_X, EDEN_CANVAS_Y);
    lv_obj_set_style_pad_all(ui->canvas_slot, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui->canvas_slot, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->canvas_slot, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(ui->canvas_slot, LV_OBJ_FLAG_SCROLLABLE);

    // 正文带:贴在画面下沿,盖住画布多出来的 3 行。
    ui->panel = lv_obj_create(page);
    lv_obj_set_size(ui->panel, EDEN_UI_W, EDEN_BAND_H);
    lv_obj_set_pos(ui->panel, 0, EDEN_BAND_Y);
    lv_obj_set_style_pad_all(ui->panel, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui->panel, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(ui->panel, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui->panel, lv_color_hex(EDEN_COL_PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->panel, LV_OPA_70, LV_PART_MAIN);
    lv_obj_remove_flag(ui->panel, LV_OBJ_FLAG_SCROLLABLE);

    ui->name = mk_label(page, ui->font_small, EDEN_COL_ACCENT);
    // 名字高度取 16px 字体的行高 19 +1：高 16 会把字形底部切掉（“只看得到上面 2/3”）。
    lv_obj_set_width(ui->name, LV_SIZE_CONTENT);
    lv_obj_set_height(ui->name, 20);
    lv_obj_set_pos(ui->name, EDEN_TEXT_X, EDEN_BAND_Y - 20 - 2);   // 146..166，底边不压面板(168)
    lv_obj_set_style_pad_hor(ui->name, 6, LV_PART_MAIN);
    // 名字也必须单行：LVGL9 的 LV_LABEL_LONG_DOT(=MODE_DOT) 是多行+省略号，
    // 长名字会被折成两行并在 20px 高里只露出大半截。
    lv_label_set_long_mode(ui->name, LV_LABEL_LONG_MODE_DOTS);
    // 名字在框外（压在画面上）：面板同色半透明底条保对比。
    lv_obj_set_style_bg_color(ui->name, lv_color_hex(EDEN_COL_PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->name, 180, LV_PART_MAIN);
    lv_label_set_text(ui->name, "");

    // 页码独立 label：挪到面板外的右上（150..165），与左侧名字同排；
    // 这样正文可以贴到面板顶 y=172 而不与页码相撞。
    ui->page_label = mk_label(page, ui->font_small, EDEN_COL_DIM);
    lv_obj_set_pos(ui->page_label, 262, EDEN_BAND_Y - 18);
    lv_obj_set_width(ui->page_label, 46);
    lv_obj_set_height(ui->page_label, 15);
    lv_obj_set_style_text_align(ui->page_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_label_set_text(ui->page_label, "");

    ui->text = mk_label(ui->panel, ui->font_small, EDEN_COL_TEXT);
    lv_obj_set_pos(ui->text, EDEN_TEXT_X, 4);    // 贴面板顶：172..229（末行不出 240）
    // 3 行用 18px 行距：17+3*18=71 <= 面板 72，页码独占 1..16，不重叠。
    lv_obj_set_style_text_line_space(ui->text, -1, LV_PART_MAIN);
    lv_obj_set_width(ui->text, EDEN_TEXT_W);
    lv_label_set_long_mode(ui->text, LV_LABEL_LONG_WRAP);
    lv_label_set_text(ui->text, "");

    ui->chapter = mk_label(page, ui->font_small, EDEN_COL_DIM);
    lv_obj_set_pos(ui->chapter, EDEN_TEXT_X, 8);
    lv_label_set_text(ui->chapter, "");

    ui->battery = mk_label(page, ui->font_small, EDEN_COL_DIM);
    lv_obj_set_pos(ui->battery, EDEN_UI_W - 62, 8);
    lv_obj_set_width(ui->battery, 54);
    lv_obj_set_style_text_align(ui->battery, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_label_set_text(ui->battery, "");

    ui->mode_auto = mk_label(page, ui->font_small, EDEN_COL_ACCENT);
    lv_obj_set_pos(ui->mode_auto, EDEN_UI_W - 52, EDEN_BAND_Y - 20);
    lv_label_set_text(ui->mode_auto, "自动");
    lv_obj_add_flag(ui->mode_auto, LV_OBJ_FLAG_HIDDEN);
}

static void build_start_page(eden_ui_t *ui)
{
    lv_obj_t *page = ui->pages[EDEN_PAGE_START];

    ui->brand = lv_image_create(page);
    lv_obj_set_pos(ui->brand, (EDEN_UI_W - 165) / 2, 52);
    lv_obj_add_flag(ui->brand, LV_OBJ_FLAG_HIDDEN);

    ui->brand_title = mk_label(page, ui->font_large, EDEN_COL_TEXT);
    lv_obj_set_width(ui->brand_title, EDEN_UI_W);
    lv_obj_set_pos(ui->brand_title, 0, 128);
    lv_obj_set_style_text_align(ui->brand_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(ui->brand_title, "");

    ui->brand_sub = mk_label(page, ui->font_small, EDEN_COL_DIM);
    lv_obj_set_width(ui->brand_sub, EDEN_UI_W);
    lv_obj_set_pos(ui->brand_sub, 0, 158);
    lv_obj_set_style_text_align(ui->brand_sub, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(ui->brand_sub, "");

    lv_obj_t *hint = mk_label(page, ui->font_small, EDEN_COL_DIM);
    lv_obj_set_width(hint, EDEN_UI_W);
    lv_obj_set_pos(hint, 0, EDEN_UI_H - 34);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(hint, "按 OK 进入标题画面");
}

static void build_title_page(eden_ui_t *ui)
{
    lv_obj_t *page = ui->pages[EDEN_PAGE_TITLE];
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, LV_PART_MAIN);

    // 标题背景槽：复用对话页那块 264x168 画布（零内存增量），
    // 由 place_canvas() 在切页时把画布搬进来；放在 logo/列表之前，保证它们在图上。
    ui->title_slot = lv_obj_create(page);
    lv_obj_set_size(ui->title_slot, EDEN_FRAME_W, EDEN_CANVAS_H);
    lv_obj_set_pos(ui->title_slot, EDEN_CANVAS_X, 0);
    lv_obj_set_style_pad_all(ui->title_slot, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui->title_slot, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->title_slot, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(ui->title_slot, LV_OBJ_FLAG_SCROLLABLE);

    // 标题页只放徽标 + 菜单;底色沿用全屏渐变。
    lv_obj_t *logo = lv_image_create(page);
    lv_obj_set_pos(logo, 16, 14);
    lv_obj_add_flag(logo, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_user_data(page, logo);

    list_create(ui, EDEN_LIST_TITLE, page, 22, 120, 40);
}

static void build_menu_page(eden_ui_t *ui)
{
    lv_obj_t *page = ui->pages[EDEN_PAGE_MENU];
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, LV_PART_MAIN);

    ui->dim = lv_obj_create(page);
    lv_obj_set_size(ui->dim, EDEN_UI_W, EDEN_UI_H);
    lv_obj_set_pos(ui->dim, 0, 0);
    lv_obj_set_style_pad_all(ui->dim, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui->dim, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(ui->dim, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui->dim, lv_color_hex(0x000000u), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->dim, LV_OPA_60, LV_PART_MAIN);

    list_create(ui, EDEN_LIST_MENU, page, 28, 22, 84);
}

static void build_list_page(eden_ui_t *ui, eden_page_t page_id, eden_list_t list,
                            int row_h, int top)
{
    lv_obj_t *page = ui->pages[page_id];
    lv_obj_t *title = mk_label(page, ui->font_large, EDEN_COL_TEXT);
    lv_obj_set_pos(title, 12, 8);
    lv_label_set_text(title, "");
    ui->titles[page_id] = title;

    lv_obj_t *footer = mk_label(page, ui->font_small, EDEN_COL_DIM);
    lv_obj_set_width(footer, EDEN_UI_W - 24);
    lv_obj_set_pos(footer, 12, EDEN_UI_H - 22);
    lv_label_set_text(footer, "");
    ui->footers[page_id] = footer;

    list_create(ui, list, page, row_h, top, 12);
}

static void build_gallery_page(eden_ui_t *ui)
{
    eden_page_t page_id = EDEN_PAGE_GALLERY;
    lv_obj_t *page = ui->pages[page_id];
    lv_obj_t *title = mk_label(page, ui->font_large, EDEN_COL_TEXT);
    lv_obj_set_pos(title, 12, 8);
    lv_label_set_text(title, "鉴赏");
    ui->titles[page_id] = title;

    lv_obj_t *footer = mk_label(page, ui->font_small, EDEN_COL_DIM);
    lv_obj_set_width(footer, EDEN_UI_W - 24);
    lv_obj_set_pos(footer, 12, EDEN_UI_H - 22);
    lv_label_set_text(footer, "长按 OK 返回标题");
    ui->footers[page_id] = footer;

    list_create(ui, EDEN_LIST_GALLERY, page, 40, 56, 24);
}

static void build_cg_list_page(eden_ui_t *ui)
{
    // CG 鉴赏用行列表而不是缩略图墙:本板无 PSRAM,一屏 6 张缩略图要同时持有 6 份
    // 解码缓冲(约 64 KB),代价比收益高。选中项按 OK 直接全屏看原图。
    eden_page_t page_id = EDEN_PAGE_CG_LIST;
    lv_obj_t *page = ui->pages[page_id];
    lv_obj_t *title = mk_label(page, ui->font_large, EDEN_COL_TEXT);
    lv_obj_set_pos(title, 12, 8);
    lv_label_set_text(title, "CG 鉴赏");
    ui->titles[page_id] = title;

    lv_obj_t *footer = mk_label(page, ui->font_small, EDEN_COL_DIM);
    lv_obj_set_width(footer, EDEN_UI_W - 24);
    lv_obj_set_pos(footer, 12, EDEN_UI_H - 22);
    lv_label_set_text(footer, "");
    ui->footers[page_id] = footer;

    list_create(ui, EDEN_LIST_CG, page, 26, 44, 12);
    (void)ui;   // cg_index 保留给别的页面复用
}

static void build_cg_page(eden_ui_t *ui)
{
    eden_page_t page_id = EDEN_PAGE_CG;
    lv_obj_t *page = ui->pages[page_id];

    // 画面槽先建,保证画布压在标题/说明下面。
    ui->cg_slot = lv_obj_create(page);
    lv_obj_set_size(ui->cg_slot, EDEN_FRAME_W, EDEN_CANVAS_H);
    lv_obj_set_pos(ui->cg_slot, EDEN_CANVAS_X, 34);
    lv_obj_set_style_pad_all(ui->cg_slot, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui->cg_slot, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->cg_slot, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(ui->cg_slot, LV_OBJ_FLAG_SCROLLABLE);

    ui->cg_name = mk_label(page, ui->font_small, EDEN_COL_TEXT);
    lv_obj_set_pos(ui->cg_name, 12, 6);
    lv_label_set_text(ui->cg_name, "");

    ui->cg_caption = mk_label(page, ui->font_small, EDEN_COL_DIM);
    lv_obj_set_width(ui->cg_caption, EDEN_UI_W - 24);
    // 画面底边在 34+168=202；说明行下移一行，把最底部让给按键提示。
    lv_obj_set_pos(ui->cg_caption, 12, EDEN_UI_H - 36);
    lv_label_set_text(ui->cg_caption, "");

    // 大图页也要有返回提示（之前 eden_ui_set_footer(EDEN_PAGE_CG, …) 是空操作）。
    lv_obj_t *footer = mk_label(page, ui->font_small, EDEN_COL_DIM);
    lv_obj_set_width(footer, EDEN_UI_W - 24);
    lv_obj_set_pos(footer, 12, EDEN_UI_H - 18);
    lv_label_set_text(footer, "");
    ui->footers[page_id] = footer;
}

static void build_transition_page(eden_ui_t *ui)
{
    eden_page_t page_id = EDEN_PAGE_TRANSITION;
    lv_obj_t *page = ui->pages[page_id];

    ui->transition_eyebrow = mk_label(page, ui->font_small, EDEN_COL_ACCENT);
    lv_obj_set_width(ui->transition_eyebrow, EDEN_UI_W);
    lv_obj_set_pos(ui->transition_eyebrow, 0, 70);
    lv_obj_set_style_text_align(ui->transition_eyebrow, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(ui->transition_eyebrow, "");

    ui->transition_title = mk_label(page, ui->font_large, EDEN_COL_TEXT);
    lv_obj_set_width(ui->transition_title, EDEN_UI_W);
    lv_obj_set_pos(ui->transition_title, 0, 96);
    lv_obj_set_style_text_align(ui->transition_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(ui->transition_title, "");

    ui->transition_sub = mk_label(page, ui->font_small, EDEN_COL_DIM);
    lv_obj_set_width(ui->transition_sub, EDEN_UI_W);
    lv_obj_set_pos(ui->transition_sub, 0, 130);
    lv_obj_set_style_text_align(ui->transition_sub, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(ui->transition_sub, "");

    lv_obj_t *hint = mk_label(page, ui->font_small, EDEN_COL_DIM);
    lv_obj_set_width(hint, EDEN_UI_W);
    lv_obj_set_pos(hint, 0, EDEN_UI_H - 30);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(hint, "按 OK 继续");
}

static void build_ending_page(eden_ui_t *ui)
{
    eden_page_t page_id = EDEN_PAGE_ENDING;
    lv_obj_t *page = ui->pages[page_id];

    ui->ending_eyebrow = mk_label(page, ui->font_small, EDEN_COL_ACCENT);
    lv_obj_set_width(ui->ending_eyebrow, EDEN_UI_W);
    lv_obj_set_pos(ui->ending_eyebrow, 0, 66);
    lv_obj_set_style_text_align(ui->ending_eyebrow, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(ui->ending_eyebrow, "");

    ui->ending_title = mk_label(page, ui->font_large, EDEN_COL_TEXT);
    lv_obj_set_width(ui->ending_title, EDEN_UI_W);
    lv_obj_set_pos(ui->ending_title, 0, 92);
    lv_obj_set_style_text_align(ui->ending_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(ui->ending_title, "");

    ui->ending_body = mk_label(page, ui->font_small, EDEN_COL_DIM);
    lv_obj_set_width(ui->ending_body, EDEN_UI_W - 40);
    lv_obj_set_pos(ui->ending_body, 20, 130);
    lv_obj_set_style_text_align(ui->ending_body, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(ui->ending_body, LV_LABEL_LONG_WRAP);
    lv_label_set_text(ui->ending_body, "");
}

static void build_modal(eden_ui_t *ui)
{
    ui->modal_box = lv_obj_create(lv_screen_active());
    lv_obj_set_size(ui->modal_box, EDEN_UI_W - 60, 110);
    lv_obj_set_pos(ui->modal_box, 30, 66);
    lv_obj_set_style_pad_all(ui->modal_box, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(ui->modal_box, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui->modal_box, lv_color_hex(EDEN_COL_PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->modal_box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui->modal_box, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(ui->modal_box, lv_color_hex(EDEN_COL_ACCENT), LV_PART_MAIN);
    lv_obj_remove_flag(ui->modal_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ui->modal_box, LV_OBJ_FLAG_HIDDEN);

    ui->modal_title = mk_label(ui->modal_box, ui->font_small, EDEN_COL_TEXT);
    lv_obj_set_pos(ui->modal_title, 14, 12);
    lv_label_set_text(ui->modal_title, "");

    ui->modal_body = mk_label(ui->modal_box, ui->font_small, EDEN_COL_DIM);
    lv_obj_set_width(ui->modal_body, EDEN_UI_W - 88);
    lv_obj_set_pos(ui->modal_body, 14, 40);
    lv_label_set_long_mode(ui->modal_body, LV_LABEL_LONG_WRAP);
    lv_label_set_text(ui->modal_body, "");
}

// ---------------------------------------------------------------- 打字机
static void typing_stop(eden_ui_t *ui)
{
    if (ui->typing_timer != NULL) {
        lv_timer_delete(ui->typing_timer);
        ui->typing_timer = NULL;
    }
    ui->typing_active = false;
}

static void typing_cb(lv_timer_t *timer)
{
    eden_ui_t *ui = (eden_ui_t *)lv_timer_get_user_data(timer);
    if (ui == NULL) {
        return;
    }
    if (ui->typing_index >= ui->typing_total) {
        typing_stop(ui);
        return;
    }
    // 一次输出一个 UTF-8 字符(不能把多字节字符截断)。
    uint32_t step = 1;
    while (ui->typing_index + step < ui->typing_total &&
           ((uint8_t)ui->typing_target[ui->typing_index + step] & 0xC0u) == 0x80u) {
        step++;
    }
    ui->typing_index += step;
    memcpy(ui->typing_shown, ui->typing_target, ui->typing_index);
    ui->typing_shown[ui->typing_index] = '\0';
    lv_label_set_text(ui->text, ui->typing_shown);
}

void eden_ui_show_full_text(eden_ui_t *ui)
{
    typing_stop(ui);
    lv_label_set_text(ui->text, ui->typing_target);
}

bool eden_ui_typing(const eden_ui_t *ui)
{
    return ui->typing_active;
}

// ---------------------------------------------------------------- 生命周期
bool eden_ui_create(eden_ui_t *ui, const lv_font_t *font_small, const lv_font_t *font_large)
{
    if (ui == NULL || font_small == NULL || font_large == NULL) {
        return false;
    }
    memset(ui, 0, sizeof(*ui));
    ui->font_small = font_small;
    ui->font_large = font_large;

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(EDEN_COL_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

    ui->bg = lv_obj_create(screen);
    lv_obj_set_size(ui->bg, EDEN_UI_W, EDEN_UI_H);
    lv_obj_set_pos(ui->bg, 0, 0);
    lv_obj_set_style_pad_all(ui->bg, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui->bg, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(ui->bg, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui->bg, lv_color_hex(EDEN_COL_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(ui->bg, lv_color_hex(EDEN_COL_BG_TOP), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(ui->bg, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->bg, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(ui->bg, LV_OBJ_FLAG_SCROLLABLE);

    for (int page = 0; page < EDEN_PAGE_COUNT; page++) {
        // 对话页自带黑底(画面之外的部分要纯黑);其余页用统一底色。
        ui->pages[page] = mk_page(ui->bg, page != EDEN_PAGE_GAME);
    }

    build_start_page(ui);
    build_title_page(ui);
    build_dialogue_page(ui);
    build_menu_page(ui);
    build_list_page(ui, EDEN_PAGE_SAVE, EDEN_LIST_SAVE, 40, 46);
    build_list_page(ui, EDEN_PAGE_LOAD, EDEN_LIST_LOAD, 40, 46);
    build_list_page(ui, EDEN_PAGE_SETTINGS, EDEN_LIST_SETTINGS, 26, 44);
    build_list_page(ui, EDEN_PAGE_HISTORY, EDEN_LIST_HISTORY, 34, 34);
    build_list_page(ui, EDEN_PAGE_CHAPTER, EDEN_LIST_CHAPTER, 30, 44);
    build_list_page(ui, EDEN_PAGE_CHOICE, EDEN_LIST_CHOICE, 34, 92);
    build_gallery_page(ui);
    build_cg_list_page(ui);
    build_cg_page(ui);
    build_transition_page(ui);
    build_ending_page(ui);

    // 提示条:顶部居中的一次性消息(存读档成功、章节锁定等)。
    ui->notice = mk_label(ui->bg, ui->font_small, EDEN_COL_TEXT);
    lv_obj_set_width(ui->notice, EDEN_UI_W - 24);
    lv_obj_set_pos(ui->notice, 12, 118);
    lv_obj_set_style_text_align(ui->notice, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(ui->notice, "");
    lv_obj_add_flag(ui->notice, LV_OBJ_FLAG_HIDDEN);

    build_modal(ui);
    eden_ui_show_page(ui, EDEN_PAGE_START);
    return true;
}

void eden_ui_attach_canvas(eden_ui_t *ui, lv_obj_t *canvas)
{
    if (ui == NULL || canvas == NULL || ui->canvas_slot == NULL) {
        return;
    }
    ui->scene_canvas = canvas;
    lv_obj_set_parent(canvas, ui->canvas_slot);
    lv_obj_set_pos(canvas, 0, 0);
}

// 全屏画面：**一个** lv_image 对象（零拷贝、零堆分配、无分带）。
//   * 对象尺寸必须就是屏幕 320x240（设成画布尺寸会被裁掉一块 → 曾出现“丢一块+黑块”）；
//   * 内容按 cover 等比放大：264x168 * 1.4286 = 377x240，居中后左右各裁 28px，零失真；
//   * dsc 直指静态画布 s_canvas_pixels。
#define EDEN_COVER_SCALE 366   // 256 = 1.0x；max(320/264, 240/168) ≈ 1.4286x

static void place_canvas(eden_ui_t *ui, eden_page_t page)
{
    if (ui->scene_canvas == NULL) {
        return;
    }
    lv_obj_t *img = ui->scene_canvas;
    lv_obj_set_style_bg_opa(img, LV_OPA_TRANSP, LV_PART_MAIN);   // 不许出现黑块
    lv_obj_set_style_border_width(img, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(img, 0, LV_PART_MAIN);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_SCROLLABLE);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CENTER);

    if (page == EDEN_PAGE_TITLE && ui->title_slot != NULL) {
        lv_obj_set_parent(img, ui->title_slot);
        lv_image_set_scale(img, 256);
        lv_obj_set_size(img, EDEN_FRAME_W, EDEN_CANVAS_H);
        lv_obj_set_pos(img, 0, 0);
        return;
    }
    // 对话页 / CG 页：对象 = 整屏，内容 = 等比 cover 放大。
    lv_obj_set_parent(img, ui->pages[page]);
    // set_parent 会把图挂成页面的**最后一个**子对象 = 画在最上层，
    // 于是全屏背景把对话框/名字/正文全盖住（实机表现为“画面只剩背景”）。
    // 必须显式挪到索引 0，让它永远在背景层。
    lv_obj_move_to_index(img, 0);
    lv_obj_set_pos(img, 0, 0);
    lv_obj_set_size(img, EDEN_UI_W, EDEN_UI_H);
    lv_image_set_scale(img, EDEN_COVER_SCALE);
}

void eden_ui_show_page(eden_ui_t *ui, eden_page_t page)
{
    if (ui == NULL || page >= EDEN_PAGE_COUNT) {
        return;
    }
    for (int index = 0; index < EDEN_PAGE_COUNT; index++) {
        if (index == (int)page) {
            lv_obj_remove_flag(ui->pages[index], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ui->pages[index], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (page != EDEN_PAGE_SAVE && page != EDEN_PAGE_LOAD) {
        lv_obj_add_flag(ui->modal_box, LV_OBJ_FLAG_HIDDEN);
    }
    place_canvas(ui, page);
    s_current = page;
}

eden_page_t eden_ui_page(const eden_ui_t *ui)
{
    (void)ui;
    return s_current;
}

// ---------------------------------------------------------------- 内容设置
void eden_ui_set_brand(eden_ui_t *ui, const char *title, const char *subtitle)
{
    if (ui == NULL) {
        return;
    }
    if (ui->brand_title != NULL) {
        lv_label_set_text(ui->brand_title, title != NULL ? title : "");
    }
    if (ui->brand_sub != NULL) {
        lv_label_set_text(ui->brand_sub, subtitle != NULL ? subtitle : "");
    }
}

void eden_ui_set_brand_image(eden_ui_t *ui, const void *dsc)
{
    if (ui == NULL || dsc == NULL) {
        return;
    }
    // 启动页与标题页各放一份同样的徽标(两个页面互斥显示)。
    if (ui->brand != NULL) {
        lv_image_set_src(ui->brand, dsc);
        lv_obj_remove_flag(ui->brand, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_t *title_logo = lv_obj_get_user_data(ui->pages[EDEN_PAGE_TITLE]);
    if (title_logo != NULL) {
        lv_image_set_src(title_logo, dsc);
        lv_obj_remove_flag(title_logo, LV_OBJ_FLAG_HIDDEN);
    }
}

void eden_ui_set_page(eden_ui_t *ui, int page, int pages)
{
    if (ui == NULL || ui->page_label == NULL) {
        return;
    }
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

void eden_ui_set_dialogue(eden_ui_t *ui, const char *speaker, const char *page_text,
                          uint32_t ms_per_char)
{
    if (ui == NULL) {
        return;
    }
    typing_stop(ui);
    if (speaker != NULL && speaker[0] != '\0') {
        lv_label_set_text(ui->name, speaker);
        lv_obj_remove_flag(ui->name, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(ui->name, "");
        lv_obj_add_flag(ui->name, LV_OBJ_FLAG_HIDDEN);
    }

    const char *text = page_text != NULL ? page_text : "";
    snprintf(ui->typing_target, sizeof(ui->typing_target), "%s", text);
    ui->typing_total = (uint32_t)strlen(ui->typing_target);
    ui->typing_index = 0;
    ui->typing_shown[0] = '\0';
    ui->typing_step_ms = ms_per_char;
    if (ms_per_char == 0 || ui->typing_total == 0) {
        lv_label_set_text(ui->text, text);
        return;
    }
    lv_label_set_text(ui->text, "");
    ui->typing_active = true;
    ui->typing_timer = lv_timer_create(typing_cb, ms_per_char, ui);
}

void eden_ui_set_choice(eden_ui_t *ui, const char *prompt, const char *const *labels,
                        int count, int selected)
{
    if (ui == NULL) {
        return;
    }
    eden_ui_set_page_title(ui, EDEN_PAGE_CHOICE, prompt != NULL ? prompt : "");
    eden_ui_set_rows(ui, EDEN_LIST_CHOICE, labels, NULL, count, selected);
}

void eden_ui_set_notice(eden_ui_t *ui, const char *text)
{
    if (ui == NULL) {
        return;
    }
    if (text == NULL || text[0] == '\0') {
        lv_obj_add_flag(ui->notice, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(ui->notice, text);
    lv_obj_remove_flag(ui->notice, LV_OBJ_FLAG_HIDDEN);
}

void eden_ui_set_chapter(eden_ui_t *ui, const char *text)
{
    if (ui != NULL && ui->chapter != NULL) {
        lv_label_set_text(ui->chapter, text != NULL ? text : "");
    }
}

void eden_ui_set_battery(eden_ui_t *ui, int percent)
{
    if (ui == NULL || ui->battery == NULL) {
        return;
    }
    if (percent < 0) {
        lv_label_set_text(ui->battery, "");
        return;
    }
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%d%%", percent);
    lv_label_set_text(ui->battery, buffer);
}

void eden_ui_set_mode(eden_ui_t *ui, bool auto_play)
{
    if (ui == NULL || ui->mode_auto == NULL) {
        return;
    }
    if (auto_play) {
        lv_obj_remove_flag(ui->mode_auto, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui->mode_auto, LV_OBJ_FLAG_HIDDEN);
    }
}

void eden_ui_set_font_large(eden_ui_t *ui, bool large)
{
    if (ui == NULL) {
        return;
    }
    const lv_font_t *font = large ? ui->font_large : ui->font_small;
    lv_obj_set_style_text_font(ui->text, font, LV_PART_MAIN);
    // 行距交给字体自身度量(16px 档行高 20、 20px 档行高 24),正文带 75px 放得下 3 行。
    lv_obj_set_style_text_line_space(ui->text, 0, LV_PART_MAIN);
    for (int list = 0; list < EDEN_LIST_COUNT; list++) {
        for (int row = 0; row < EDEN_ROWS_MAX; row++) {
            lv_obj_t *item = ui->lists[list].rows[row];
            if (item == NULL) {
                continue;
            }
            lv_obj_t *label = row_label(item);
            lv_obj_t *value = row_value(item);
            if (label != NULL) {
                lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
            }
            if (value != NULL) {
                lv_obj_set_style_text_font(value, font, LV_PART_MAIN);
            }
        }
    }
}

void eden_ui_set_panel_opacity(eden_ui_t *ui, uint8_t percent)
{
    if (ui == NULL || ui->panel == NULL) {
        return;
    }
    const uint32_t opacity = (uint32_t)percent * 255u / 100u;
    lv_obj_set_style_bg_opa(ui->panel, (lv_opa_t)opacity, LV_PART_MAIN);
}

void eden_ui_set_rows(eden_ui_t *ui, eden_list_t list_id, const char *const *labels,
                      const char *const *values, int count, int selected)
{
    if (ui == NULL || list_id >= EDEN_LIST_COUNT) {
        return;
    }
    eden_list_ui_t *list = &ui->lists[list_id];
    if (list->view == NULL) {
        return;
    }
    if (count > EDEN_ROWS_MAX) {
        count = EDEN_ROWS_MAX;
    }
    // 列表容器建的时候是隐藏的(每个页面都先建好再按需显示),填内容时必须显式打开,
    // 否则整页看起来是空白 —— 宿主机用对象可见性就能测出这一条。
    lv_obj_remove_flag(list->view, LV_OBJ_FLAG_HIDDEN);
    list->count = count;
    list->selected = selected >= 0 && selected < count ? selected : 0;
    for (int index = 0; index < count; index++) {
        lv_obj_t *row = list_ensure_row(ui, list_id, index);
        if (row == NULL) {
            continue;
        }
        lv_obj_remove_flag(row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t *label = row_label(row);
        if (label != NULL) {
            lv_label_set_text(label, labels != NULL && labels[index] != NULL ? labels[index] : "");
            lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_t *value = row_value(row);
        if (value != NULL) {
            if (values != NULL && values[index] != NULL && values[index][0] != '\0') {
                lv_label_set_text(value, values[index]);
                lv_obj_remove_flag(value, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_label_set_text(value, "");
                lv_obj_add_flag(value, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    for (int index = count; index < EDEN_ROWS_MAX; index++) {
        if (list->rows[index] != NULL) {
            lv_obj_add_flag(list->rows[index], LV_OBJ_FLAG_HIDDEN);
        }
    }
    list_paint_selection(ui, list_id);
}

void eden_ui_set_row_value(eden_ui_t *ui, eden_list_t list_id, int row, const char *value)
{
    if (ui == NULL || list_id >= EDEN_LIST_COUNT || row < 0 || row >= EDEN_ROWS_MAX) {
        return;
    }
    lv_obj_t *item = ui->lists[list_id].rows[row];
    if (item == NULL) {
        return;
    }
    lv_obj_t *label = row_value(item);
    if (label != NULL) {
        lv_label_set_text(label, value != NULL ? value : "");
        if (value != NULL && value[0] != '\0') {
            lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void eden_ui_set_row_enabled(eden_ui_t *ui, eden_list_t list_id, int row, bool enabled)
{
    if (ui == NULL || list_id >= EDEN_LIST_COUNT || row < 0 || row >= EDEN_ROWS_MAX) {
        return;
    }
    ui->lists[list_id].disabled[row] = !enabled;
    lv_obj_t *item = ui->lists[list_id].rows[row];
    if (item == NULL) {
        return;
    }
    lv_obj_t *label = row_label(item);
    if (label != NULL) {
        lv_obj_set_style_text_color(label, lv_color_hex(enabled ? EDEN_COL_TEXT : EDEN_COL_DIM),
                                    LV_PART_MAIN);
    }
}

void eden_ui_select_row(eden_ui_t *ui, eden_list_t list_id, int selected)
{
    if (ui == NULL || list_id >= EDEN_LIST_COUNT) {
        return;
    }
    eden_list_ui_t *list = &ui->lists[list_id];
    if (selected < 0 || selected >= list->count) {
        return;
    }
    list->selected = selected;
    list_paint_selection(ui, list_id);
}

void eden_ui_set_page_title(eden_ui_t *ui, eden_page_t page, const char *text)
{
    if (ui == NULL || page >= EDEN_PAGE_COUNT || ui->titles[page] == NULL) {
        return;
    }
    lv_label_set_text(ui->titles[page], text != NULL ? text : "");
}

void eden_ui_set_footer(eden_ui_t *ui, eden_page_t page, const char *text)
{
    if (ui == NULL || page >= EDEN_PAGE_COUNT || ui->footers[page] == NULL) {
        return;
    }
    lv_label_set_text(ui->footers[page], text != NULL ? text : "");
}

void eden_ui_set_cg_grid(eden_ui_t *ui, const void *const *thumbs, const char *const *labels,
                         const bool *unlocked, int count, int selected)
{
    if (ui == NULL) {
        return;
    }
    if (count > EDEN_CG_PER_PAGE) {
        count = EDEN_CG_PER_PAGE;
    }
    for (int index = 0; index < EDEN_CG_PER_PAGE; index++) {
        lv_obj_t *frame = ui->cg_tiles[index];
        if (frame == NULL) {
            continue;
        }
        const bool visible = index < count;
        const bool is_selected = visible && index == selected;
        lv_obj_set_style_border_color(frame, lv_color_hex(is_selected ? EDEN_COL_ACCENT
                                                                     : EDEN_COL_ROW),
                                      LV_PART_MAIN);
        lv_obj_t *image = lv_obj_get_child(frame, 0);
        lv_obj_t *label = ui->cg_labels[index];
        if (!visible) {
            lv_obj_add_flag(frame, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(frame, LV_OBJ_FLAG_HIDDEN);
        const bool open = unlocked != NULL && unlocked[index];
        if (open && thumbs != NULL && thumbs[index] != NULL && image != NULL) {
            lv_image_set_src(image, thumbs[index]);
            lv_obj_remove_flag(image, LV_OBJ_FLAG_HIDDEN);
            if (label != NULL) {
                lv_label_set_text(label, labels != NULL && labels[index] != NULL
                                             ? labels[index] : "");
                lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            if (image != NULL) {
                lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
            }
            if (label != NULL) {
                lv_label_set_text(label, "？");
                lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

void eden_ui_set_cg_page_info(eden_ui_t *ui, const char *text)
{
    if (ui != NULL && ui->cg_index != NULL) {
        lv_label_set_text(ui->cg_index, text != NULL ? text : "");
    }
}


void eden_ui_set_cg(eden_ui_t *ui, const char *name, const char *caption)
{
    if (ui == NULL) {
        return;
    }
    if (ui->cg_name != NULL) {
        lv_label_set_text(ui->cg_name, name != NULL ? name : "");
    }
    if (ui->cg_caption != NULL) {
        lv_label_set_text(ui->cg_caption, caption != NULL ? caption : "");
    }
}

void eden_ui_set_transition(eden_ui_t *ui, const char *eyebrow, const char *title,
                            const char *subtitle)
{
    if (ui == NULL) {
        return;
    }
    lv_label_set_text(ui->transition_eyebrow, eyebrow != NULL ? eyebrow : "");
    lv_label_set_text(ui->transition_title, title != NULL ? title : "");
    lv_label_set_text(ui->transition_sub, subtitle != NULL ? subtitle : "");
}

void eden_ui_set_ending(eden_ui_t *ui, const char *eyebrow, const char *title, const char *body)
{
    if (ui == NULL) {
        return;
    }
    lv_label_set_text(ui->ending_eyebrow, eyebrow != NULL ? eyebrow : "");
    lv_label_set_text(ui->ending_title, title != NULL ? title : "");
    lv_label_set_text(ui->ending_body, body != NULL ? body : "");
}

void eden_ui_show_modal(eden_ui_t *ui, const char *title, const char *body)
{
    if (ui == NULL) {
        return;
    }
    lv_label_set_text(ui->modal_title, title != NULL ? title : "");
    lv_label_set_text(ui->modal_body, body != NULL ? body : "");
    lv_obj_remove_flag(ui->modal_box, LV_OBJ_FLAG_HIDDEN);
}

void eden_ui_hide_modal(eden_ui_t *ui)
{
    if (ui != NULL) {
        lv_obj_add_flag(ui->modal_box, LV_OBJ_FLAG_HIDDEN);
    }
}

void eden_ui_scroll(eden_ui_t *ui, eden_page_t page, int dy)
{
    if (ui == NULL || page >= EDEN_PAGE_COUNT) {
        return;
    }
    lv_obj_t *target = NULL;
    switch (page) {
    case EDEN_PAGE_HISTORY: target = ui->lists[EDEN_LIST_HISTORY].view; break;
    case EDEN_PAGE_CHAPTER: target = ui->lists[EDEN_LIST_CHAPTER].view; break;
    default: return;
    }
    if (target != NULL) {
        lv_obj_scroll_by(target, 0, -dy, LV_ANIM_OFF);
    }
}
