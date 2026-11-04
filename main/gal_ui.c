// main/gal_ui.c —— 横屏 320x240 界面实现。
#include "gal_ui.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "gal_ui";

#define COL_BG      0x0B0F18
#define COL_PANEL   0x090D15
#define COL_TEXT    0xEDEFF2
#define COL_DIM     0x8B93A1
#define COL_ACCENT  0x7FD1A8
#define COL_SEL     0x24493B
#define COL_ROW     0x141A24
#define COL_FRAME   0x3A4556

static lv_obj_t *mk_page(lv_obj_t *root)
{
    lv_obj_t *page = lv_obj_create(root);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, GAL_UI_W, GAL_UI_H);
    lv_obj_set_pos(page, 0, 0);
    lv_obj_set_style_bg_color(page, lv_color_hex(COL_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
    return page;
}

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                          const char *text, int x, int y, int w)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text ? text : "");
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    if (w > 0) {
        lv_obj_set_width(label, w);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    }
    lv_obj_set_pos(label, x, y);
    return label;
}

// 列表行专用：定宽 + CLIP，单行不折行（长选项末尾截断）。
static lv_obj_t *mk_label_clip(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                               const char *text, int x, int y, int w)
{
    lv_obj_t *label = mk_label(parent, font, color, text, x, y, 0);
    if (w > 0) {
        lv_obj_set_width(label, w);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
    }
    return label;
}

static void style_row(lv_obj_t *row, bool selected)
{
    lv_obj_set_style_bg_color(row, lv_color_hex(selected ? COL_SEL : COL_ROW), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 4, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, lv_color_hex(selected ? COL_ACCENT : COL_FRAME), LV_PART_MAIN);
}

static lv_obj_t *mk_row(lv_obj_t *parent, int x, int y, int w, int h, bool selected)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, w, h);
    lv_obj_set_pos(row, x, y);
    style_row(row, selected);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}


// ---------------------------------------------------------------- 打字机
static void typing_update(gal_ui_t *ui)
{
    if (!ui->text) return;
    const uint32_t cap = sizeof(ui->typing_shown) - 1;
    uint32_t n = ui->typing_index;
    if (n > ui->typing_total) n = ui->typing_total;
    if (n > cap) n = cap;
    memcpy(ui->typing_shown, ui->typing_target, n);
    ui->typing_shown[n] = '\0';
    lv_label_set_text(ui->text, ui->typing_shown);
}

static void typing_timer_cb(lv_timer_t *timer)
{
    gal_ui_t *ui = (gal_ui_t *)lv_timer_get_user_data(timer);
    if (!ui || !ui->typing_active) return;
    const char *s = ui->typing_target;
    uint32_t step = 1;
    while (ui->typing_index + step < ui->typing_total &&
           ((unsigned char)s[ui->typing_index + step] & 0xC0) == 0x80) {
        step++;
    }
    ui->typing_index += step;
    if (ui->typing_index >= ui->typing_total) {
        ui->typing_index = ui->typing_total;
        ui->typing_active = false;
        lv_timer_pause(timer);
    }
    typing_update(ui);
}

// ---------------------------------------------------------------- 创建
static void build_game_page(gal_ui_t *ui)
{
    lv_obj_t *page = ui->pages[GAL_PAGE_GAME];
    ui->box = lv_obj_create(page);
    lv_obj_remove_style_all(ui->box);
    lv_obj_set_size(ui->box, GAL_UI_W, GAL_BOX_H);
    lv_obj_set_pos(ui->box, 0, GAL_BOX_Y);
    lv_obj_set_style_bg_color(ui->box, lv_color_hex(COL_PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->box, 180, LV_PART_MAIN);   // 0xB4
    lv_obj_set_style_border_width(ui->box, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(ui->box, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_set_style_border_color(ui->box, lv_color_hex(COL_FRAME), LV_PART_MAIN);
    lv_obj_remove_flag(ui->box, LV_OBJ_FLAG_SCROLLABLE);

    // 页码挪到面板外右上（避免贴顶正文与页码相撞）。
    ui->page_label = mk_label(page, ui->font_small, COL_DIM, "", GAL_UI_W - 58,
                              GAL_BOX_Y - 18, 50);
    lv_obj_set_style_text_align(ui->page_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    // 正文贴面板顶 +4（原 21，垂直偏下）。
    ui->text = mk_label(ui->box, ui->font_small, COL_TEXT, "", GAL_TEXT_X, 4, GAL_TEXT_W);
    lv_obj_set_style_text_line_space(ui->text, 0, LV_PART_MAIN);

    // 姓名条必须"贴紧"对话框顶：底边 == GAL_BOX_Y(152)，高 20 → y=132。
    // 之前写死 119 会留 13px 空档，实机看着就是"名字悬空"。
    ui->name = mk_label(page, ui->font_small, COL_ACCENT, "",
                        8, GAL_BOX_Y - 20, 0);
    // 名字底条只包文字（内容宽 + 左右 6px），不拉满整行。
    lv_obj_set_width(ui->name, LV_SIZE_CONTENT);
    lv_obj_set_height(ui->name, 20);
    lv_obj_set_style_pad_hor(ui->name, 6, LV_PART_MAIN);
    // 名字在框外（压在画面上）：面板同色半透明底条保对比。
    lv_obj_set_style_bg_color(ui->name, lv_color_hex(COL_PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->name, 180, LV_PART_MAIN);
    ui->chapter = mk_label(page, ui->font_small, COL_DIM, "", GAL_UI_W - 132, 4, 124);
    lv_obj_set_style_text_align(ui->chapter, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    ui->battery = mk_label(page, ui->font_small, COL_DIM, "", 6, 4, 60);
    ui->mode = mk_label(page, ui->font_small, COL_ACCENT, "", GAL_UI_W - 52, 119, 44);
    lv_obj_set_style_text_align(ui->mode, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    // 提示条放到全局层(见 gal_ui_create),不随正文页隐藏 —— 否则在存读档/设置页
    // 弹的"已保存/未解锁"提示会被祖先 hidden 一起藏掉。
    ui->notice = NULL;
}

static void build_choice(gal_ui_t *ui)
{
    ui->choice_box = lv_obj_create(ui->pages[GAL_PAGE_GAME]);
    lv_obj_remove_style_all(ui->choice_box);
    lv_obj_set_size(ui->choice_box, GAL_UI_W - 24, 120);
    lv_obj_set_pos(ui->choice_box, 12, 12);
    lv_obj_set_style_bg_color(ui->choice_box, lv_color_hex(COL_PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->choice_box, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui->choice_box, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(ui->choice_box, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_radius(ui->choice_box, 6, LV_PART_MAIN);
    lv_obj_remove_flag(ui->choice_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ui->choice_box, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 5; ++i) {
        ui->choice_rows[i] = mk_row(ui->choice_box, 6, 6 + i * 22, GAL_UI_W - 36, 20, false);
        lv_obj_add_flag(ui->choice_rows[i], LV_OBJ_FLAG_HIDDEN);
        ui->choice_labels[i] = mk_label(ui->choice_rows[i], ui->font_small, COL_TEXT, "", 6, 2,
                                        GAL_UI_W - 48);
    }
    ui->choice_count = 0;
    ui->choice_sel = 0;
}

static void build_title(gal_ui_t *ui)
{
    lv_obj_t *page = ui->pages[GAL_PAGE_TITLE];
    // 半透明压暗层,让标题文字在任意背景上都能看清。
    lv_obj_t *scrim = lv_obj_create(page);
    lv_obj_remove_style_all(scrim);
    lv_obj_set_size(scrim, GAL_UI_W, GAL_UI_H);
    lv_obj_set_style_bg_color(scrim, lv_color_hex(0x05070C), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_60, LV_PART_MAIN);
    lv_obj_remove_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);

    ui->title_label = mk_label(page, ui->font_large, COL_TEXT, "", 16, 18, GAL_UI_W - 32);
    lv_obj_set_style_text_align(ui->title_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    ui->title_sub = mk_label(page, ui->font_small, COL_DIM, "", 16, 74, GAL_UI_W - 32);
    lv_obj_set_style_text_align(ui->title_sub, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    static const char *const menu_items[] = {
        "继续游戏", "开始新游戏", "读取存档", "鉴赏室", "设置",
    };
    for (int i = 0; i < 5; ++i) {
        ui->title_menu_text[i] = mk_label(page, ui->font_small, COL_TEXT, menu_items[i],
                                          96, 100 + i * 26, 170);
        lv_obj_set_style_text_align(ui->title_menu_text[i], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    }
    ui->title_sel = 0;
}

static void build_ending(gal_ui_t *ui)
{
    lv_obj_t *page = ui->pages[GAL_PAGE_ENDING];
    ui->ending_eyebrow = mk_label(page, ui->font_small, COL_ACCENT, "", 20, 40, GAL_UI_W - 40);
    lv_obj_set_style_text_align(ui->ending_eyebrow, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    ui->ending_title = mk_label(page, ui->font_large, COL_TEXT, "", 20, 70, GAL_UI_W - 40);
    lv_obj_set_style_text_align(ui->ending_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    ui->ending_text = mk_label(page, ui->font_small, COL_DIM, "", 24, 120, GAL_UI_W - 48);
    lv_obj_set_style_text_align(ui->ending_text, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    ui->ending_hint = mk_label(page, ui->font_small, COL_DIM, "OK 回到标题", 90, 208, 140);
    lv_obj_set_style_text_align(ui->ending_hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
}

static void build_confirm(gal_ui_t *ui)
{
    ui->confirm_box = lv_obj_create(ui->root);
    lv_obj_remove_style_all(ui->confirm_box);
    lv_obj_set_size(ui->confirm_box, 240, 110);
    lv_obj_set_pos(ui->confirm_box, 40, 66);
    lv_obj_set_style_bg_color(ui->confirm_box, lv_color_hex(COL_PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->confirm_box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui->confirm_box, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(ui->confirm_box, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_radius(ui->confirm_box, 6, LV_PART_MAIN);
    lv_obj_remove_flag(ui->confirm_box, LV_OBJ_FLAG_SCROLLABLE);
    ui->confirm_text = mk_label(ui->confirm_box, ui->font_small, COL_TEXT, "", 10, 12, 220);
    lv_obj_set_style_text_align(ui->confirm_text, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    static const char *const labels[2] = { "取消", "覆盖" };
    for (int i = 0; i < 2; ++i) {
        ui->confirm_rows[i] = mk_row(ui->confirm_box, 30 + i * 100, 58, 90, 30, i == 0);
        ui->confirm_labels[i] = mk_label(ui->confirm_rows[i], ui->font_small, COL_TEXT, labels[i],
                                         10, 6, 70);
        lv_obj_set_style_text_align(ui->confirm_labels[i], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    }
    lv_obj_add_flag(ui->confirm_box, LV_OBJ_FLAG_HIDDEN);
    ui->confirm_active = false;
}

static void build_transition(gal_ui_t *ui)
{
    ui->transition_box = lv_obj_create(ui->root);
    lv_obj_remove_style_all(ui->transition_box);
    lv_obj_set_size(ui->transition_box, GAL_UI_W, GAL_UI_H);
    lv_obj_set_pos(ui->transition_box, 0, 0);
    lv_obj_set_style_bg_color(ui->transition_box, lv_color_hex(0x05070C), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->transition_box, LV_OPA_80, LV_PART_MAIN);
    lv_obj_remove_flag(ui->transition_box, LV_OBJ_FLAG_SCROLLABLE);
    ui->transition_title = mk_label(ui->transition_box, ui->font_large, COL_TEXT, "",
                                    16, 96, GAL_UI_W - 32);
    lv_obj_set_style_text_align(ui->transition_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    ui->transition_sub = mk_label(ui->transition_box, ui->font_small, COL_ACCENT, "",
                                  16, 132, GAL_UI_W - 32);
    lv_obj_set_style_text_align(ui->transition_sub, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_add_flag(ui->transition_box, LV_OBJ_FLAG_HIDDEN);
}

static void build_cgview(gal_ui_t *ui)
{
    lv_obj_t *page = ui->pages[GAL_PAGE_CGVIEW];
    lv_obj_t *bar = lv_obj_create(page);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, GAL_UI_W, 34);
    lv_obj_set_pos(bar, 0, GAL_UI_H - 34);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x05070C), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_80, LV_PART_MAIN);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    ui->cg_caption = mk_label(bar, ui->font_small, COL_TEXT, "", 10, 6, 220);
    ui->cg_hint = mk_label(bar, ui->font_small, COL_DIM, "OK 返回", GAL_UI_W - 76, 6, 70);
    lv_obj_set_style_text_align(ui->cg_hint, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
}

bool gal_ui_create(gal_ui_t *ui, uint16_t *art_px, uint16_t *strip_px,
                   uint8_t *scratch, uint32_t scratch_size,
                   const lv_font_t *font_small, const lv_font_t *font_large)
{
    if (!ui || !art_px || !strip_px || !scratch || !font_small || !font_large) return false;
    memset(ui, 0, sizeof(*ui));
    ui->font_small = font_small;
    ui->font_large = font_large;
    ui->root = lv_screen_active();
    lv_obj_set_style_bg_color(ui->root, lv_color_hex(COL_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->root, LV_OPA_COVER, LV_PART_MAIN);

    // 画面层先建,文本层后建,保证文字盖在画面上。
    ui->pages[GAL_PAGE_GAME] = mk_page(ui->root);
    lv_obj_set_style_bg_opa(ui->pages[GAL_PAGE_GAME], LV_OPA_TRANSP, LV_PART_MAIN);
    if (!gal_image_init(&ui->image, ui->pages[GAL_PAGE_GAME], art_px, strip_px,
                        scratch, scratch_size)) {
        ESP_LOGE(TAG, "画面缓冲初始化失败");
        return false;
    }

    // 注意：页枚举从 GAL_PAGE_TITLE(0) 开始。旧代码从 GAL_PAGE_MENU(2) 起循环，
    // 导致标题页 ui->pages[0] 一直是 NULL —— build_title() 拿到 NULL 父对象后
    // 在 LVGL 里新建了一张独立屏幕，标题/菜单永远不显示（实机表现：黑屏、按键无反应）。
    for (int p = GAL_PAGE_TITLE; p <= GAL_PAGE_ENDINGS; ++p) {
        if (p == GAL_PAGE_GAME) continue;
        ui->pages[p] = mk_page(ui->root);
    }
    // CG 浏览页只要底部条,画面复用 game 页的画布。
    lv_obj_set_style_bg_opa(ui->pages[GAL_PAGE_CGVIEW], LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->pages[GAL_PAGE_TITLE], LV_OPA_TRANSP, LV_PART_MAIN);

    build_game_page(ui);
    build_choice(ui);
    build_title(ui);
    build_ending(ui);
    build_cgview(ui);

    ui->history_box = lv_obj_create(ui->pages[GAL_PAGE_HISTORY]);
    lv_obj_remove_style_all(ui->history_box);
    lv_obj_set_size(ui->history_box, GAL_UI_W - 16, GAL_UI_H - 44);
    lv_obj_set_pos(ui->history_box, 8, 36);
    lv_obj_set_style_bg_opa(ui->history_box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_scroll_dir(ui->history_box, LV_DIR_VER);
    lv_obj_set_style_pad_all(ui->history_box, 4, LV_PART_MAIN);

    ui->chapter_box = lv_obj_create(ui->pages[GAL_PAGE_CHAPTERS]);
    lv_obj_remove_style_all(ui->chapter_box);
    lv_obj_set_size(ui->chapter_box, GAL_UI_W - 16, GAL_UI_H - 44);
    lv_obj_set_pos(ui->chapter_box, 8, 36);
    lv_obj_set_style_bg_opa(ui->chapter_box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_scroll_dir(ui->chapter_box, LV_DIR_VER);

    ui->gallery_page_label = mk_label(ui->pages[GAL_PAGE_GALLERY], ui->font_small, COL_DIM,
                                      "", GAL_UI_W - 90, 8, 82);
    lv_obj_set_style_text_align(ui->gallery_page_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    ui->gallery_box = lv_obj_create(ui->pages[GAL_PAGE_GALLERY]);
    lv_obj_remove_style_all(ui->gallery_box);
    lv_obj_set_size(ui->gallery_box, GAL_UI_W - 16, GAL_UI_H - 44);
    lv_obj_set_pos(ui->gallery_box, 8, 36);
    lv_obj_set_style_bg_opa(ui->gallery_box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(ui->gallery_box, LV_OBJ_FLAG_SCROLLABLE);

    ui->typing_timer = lv_timer_create(typing_timer_cb, 25, ui);
    if (!ui->typing_timer) return false;
    lv_timer_pause(ui->typing_timer);
    // 全局提示层:直接挂在 root 上,位于所有页面/弹窗之后(最后创建 = 最上层)。
    ui->notice = mk_label(ui->root, ui->font_small, COL_ACCENT, "", 8, GAL_UI_H - 26, GAL_UI_W - 16);
    lv_obj_set_style_bg_color(ui->notice, lv_color_hex(0x05070C), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->notice, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ui->notice, 3, LV_PART_MAIN);
    lv_obj_add_flag(ui->notice, LV_OBJ_FLAG_HIDDEN);
    build_transition(ui);
    build_confirm(ui);
    return true;
}

gal_page_t gal_ui_page(const gal_ui_t *ui)
{
    for (int p = 0; p <= GAL_PAGE_ENDINGS; ++p) {
        if (ui->pages[p] && !lv_obj_has_flag(ui->pages[p], LV_OBJ_FLAG_HIDDEN)) return (gal_page_t)p;
    }
    return GAL_PAGE_TITLE;
}

void gal_ui_show_page(gal_ui_t *ui, gal_page_t page)
{
    if (!ui) return;
    // 画面层属于 game 页;标题页与 CG 浏览也要用它,所以不随页面隐藏。
    const bool stage = (page == GAL_PAGE_GAME || page == GAL_PAGE_TITLE || page == GAL_PAGE_CGVIEW);
    if (ui->pages[GAL_PAGE_GAME]) {
        if (stage) lv_obj_remove_flag(ui->pages[GAL_PAGE_GAME], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(ui->pages[GAL_PAGE_GAME], LV_OBJ_FLAG_HIDDEN);
    }
    for (int p = 0; p <= GAL_PAGE_ENDINGS; ++p) {
        if (p == GAL_PAGE_GAME || !ui->pages[p]) continue;
        if (p == (int)page) lv_obj_remove_flag(ui->pages[p], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(ui->pages[p], LV_OBJ_FLAG_HIDDEN);
    }
    // 正文控件只在正文页显示。
    const bool game = (page == GAL_PAGE_GAME);
    // 注意:全局提示条(notice)不属于正文页,不能跟着隐藏。
    lv_obj_t *widgets[] = { ui->box, ui->name, ui->chapter, ui->battery, ui->mode };
    for (size_t i = 0; i < sizeof(widgets) / sizeof(widgets[0]); ++i) {
        if (!widgets[i]) continue;
        if (game) lv_obj_remove_flag(widgets[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(widgets[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (page == GAL_PAGE_GAME) lv_obj_move_foreground(ui->pages[GAL_PAGE_GAME]);
    if (page != GAL_PAGE_GAME && ui->pages[page]) lv_obj_move_foreground(ui->pages[page]);
    // 提示条永远保持在最上层(切页不能把提示藏到页面后面)。
    if (ui->notice) {
        lv_obj_move_foreground(ui->notice);
        if (ui->notice_until_ms == 0) lv_obj_add_flag(ui->notice, LV_OBJ_FLAG_HIDDEN);
    }
}

// ---------------------------------------------------------------- 正文
void gal_ui_set_page(gal_ui_t *ui, int page, int pages)
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

void gal_ui_set_text(gal_ui_t *ui, const char *speaker, const char *page_text,
                     uint32_t ms_per_char)
{
    if (!ui || !ui->text) return;
    const size_t len = strlen(page_text ? page_text : "");
    const size_t cap = sizeof(ui->typing_target) - 1;
    const size_t n = len < cap ? len : cap;
    memcpy(ui->typing_target, page_text, n);
    ui->typing_target[n] = '\0';
    ui->typing_total = (uint32_t)n;
    lv_label_set_text(ui->name, speaker ? speaker : "");
    if (ui->typing_timer && ms_per_char > 0) {
        ui->typing_index = 0;
        ui->typing_active = true;
        lv_label_set_text(ui->text, "");
        lv_timer_set_period(ui->typing_timer, ms_per_char);
        lv_timer_resume(ui->typing_timer);
        lv_timer_reset(ui->typing_timer);
    } else {
        gal_ui_show_full_text(ui);
    }
}

void gal_ui_show_full_text(gal_ui_t *ui)
{
    if (!ui || !ui->text) return;
    ui->typing_active = false;
    if (ui->typing_timer) lv_timer_pause(ui->typing_timer);
    ui->typing_index = ui->typing_total;
    memcpy(ui->typing_shown, ui->typing_target, ui->typing_total);
    ui->typing_shown[ui->typing_total] = '\0';
    lv_label_set_text(ui->text, ui->typing_shown);
}

bool gal_ui_typing(const gal_ui_t *ui)
{
    return ui && ui->typing_active;
}

void gal_ui_set_chapter(gal_ui_t *ui, const char *text)
{
    if (ui && ui->chapter) lv_label_set_text(ui->chapter, text ? text : "");
}

void gal_ui_set_battery(gal_ui_t *ui, int percent)
{
    if (!ui || !ui->battery) return;
    char buf[32];
    if (percent < 0) snprintf(buf, sizeof(buf), "--");
    else snprintf(buf, sizeof(buf), "%d%%", percent);
    lv_label_set_text(ui->battery, buf);
}

void gal_ui_set_mode(gal_ui_t *ui, bool auto_play)
{
    if (ui && ui->mode) lv_label_set_text(ui->mode, auto_play ? "自动" : "");
}

void gal_ui_toast(gal_ui_t *ui, const char *text, uint32_t now_ms)
{
    if (!ui || !ui->notice) return;
    lv_label_set_text(ui->notice, text ? text : "");
    ui->notice_until_ms = now_ms + 1600;
    if (text && *text) lv_obj_remove_flag(ui->notice, LV_OBJ_FLAG_HIDDEN);
}

void gal_ui_tick(gal_ui_t *ui, uint32_t now_ms)
{
    if (!ui || !ui->notice) return;
    if (ui->notice_until_ms && (int32_t)(now_ms - ui->notice_until_ms) >= 0) {
        ui->notice_until_ms = 0;
        lv_obj_add_flag(ui->notice, LV_OBJ_FLAG_HIDDEN);
    }
}

// ---------------------------------------------------------------- 选项
void gal_ui_set_choice(gal_ui_t *ui, const gal_pack_t *pack, const uint16_t *options,
                       int count, int selected)
{
    if (!ui || !pack || !options) return;
    ui->choice_count = count > 5 ? 5 : count;
    ui->choice_sel = selected;
    for (int i = 0; i < 5; ++i) {
        const bool show = i < ui->choice_count;
        if (!show) {
            lv_obj_add_flag(ui->choice_rows[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const gal_option_t *opt = &pack->options[options[i]];
        lv_label_set_text(ui->choice_labels[i], gal_pack_name(pack, opt->text));
        lv_obj_set_style_text_color(ui->choice_labels[i], lv_color_hex(COL_TEXT), LV_PART_MAIN);
        style_row(ui->choice_rows[i], i == selected);
        lv_obj_remove_flag(ui->choice_rows[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_size(ui->choice_box, GAL_UI_W - 24, ui->choice_count * 22 + 12);
    lv_obj_remove_flag(ui->choice_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ui->choice_box);
}

void gal_ui_hide_choice(gal_ui_t *ui)
{
    if (ui && ui->choice_box) lv_obj_add_flag(ui->choice_box, LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------- 列表
void gal_ui_set_list(gal_ui_t *ui, gal_list_t *list, gal_page_t page, const char *title,
                     const char *const *labels, const char *const *values, int count,
                     int selected, int row_h)
{
    if (!ui || !list) return;
    lv_obj_t *page_obj = ui->pages[page];
    if (!page_obj) return;
    lv_obj_clean(page_obj);
    memset(list, 0, sizeof(*list));
    list->page = page_obj;
    list->row_h = row_h;
    list->visible = GAL_LIST_ROWS;
    list->font = ui->font_small;
    if (title && *title) {
        mk_label(page_obj, ui->font_small, COL_ACCENT, title, 12, 8, GAL_UI_W - 24);
    }
    const int max_rows = count < GAL_LIST_ROWS ? count : GAL_LIST_ROWS;
    list->count = count;
    list->selected = selected;
    list->top = 0;
    if (selected >= max_rows) list->top = selected - max_rows + 1;
    for (int i = 0; i < max_rows; ++i) {
        const int idx = list->top + i;
        lv_obj_t *row = mk_row(page_obj, 8, 34 + i * row_h, GAL_UI_W - 16, row_h - 2, idx == selected);
        list->rows[i] = row;
        list->values[i] = mk_label(row, ui->font_small, COL_DIM, (values && values[idx]) ? values[idx] : "",
                                   GAL_UI_W - 120, 3, 100);
        lv_obj_set_style_text_align(list->values[i], LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
        mk_label_clip(row, ui->font_small, COL_TEXT, labels[idx] ? labels[idx] : "", 8, 3,
                      GAL_UI_W - 120);
    }
    const char *hint = (page == GAL_PAGE_SLOTS) ? "上/下 选择   OK 确认   长按 OK 返回"
                                                : "上/下 选择   OK 切换   长按 OK 返回";
    mk_label(page_obj, ui->font_small, COL_DIM, hint, 12, GAL_UI_H - 20, GAL_UI_W - 24);
}

void gal_ui_list_select(gal_ui_t *ui, gal_list_t *list, int selected)
{
    (void)ui;
    if (!list || !list->page) return;
    const int max_rows = list->count < GAL_LIST_ROWS ? list->count : GAL_LIST_ROWS;
    if (selected < list->top) list->top = selected;
    if (selected >= list->top + max_rows) list->top = selected - max_rows + 1;
    if (list->top < 0) list->top = 0;
    list->selected = selected;
    for (int i = 0; i < max_rows; ++i) {
        if (list->rows[i]) style_row(list->rows[i], list->top + i == selected);
    }
}

void gal_ui_list_set_value(gal_ui_t *ui, gal_list_t *list, int row, const char *value)
{
    (void)ui;
    if (!list || !list->page) return;
    const int slot = row - list->top;
    if (slot < 0 || slot >= GAL_LIST_ROWS || !list->values[slot]) return;
    lv_label_set_text(list->values[slot], value ? value : "");
}

// ---------------------------------------------------------------- 历史
void gal_ui_set_history(gal_ui_t *ui, const gal_pack_t *pack, const uint16_t *lines,
                        int count)
{
    if (!ui || !pack || !lines) return;
    lv_obj_clean(ui->history_box);
    const int row_h = ui->large_font ? 24 : 20;
    // 只展示"最近 48 条":显示窗口从 count-48 开始,最新的永远在最后一行。
    const int start = count > 48 ? count - 48 : 0;
    int shown = 0;
    for (int i = start; i < count; ++i) {
        const gal_line_t *ln = &pack->lines[lines[i]];
        const char *speaker = gal_pack_name(pack, ln->speaker);
        char buf[GAL_TEXT_BUFFER];
        if (speaker && *speaker) {
            snprintf(buf, sizeof(buf), "%s：%s", speaker, gal_line_text(pack, lines[i]));
        } else {
            snprintf(buf, sizeof(buf), "%s", gal_line_text(pack, lines[i]));
        }
        // 每条一行、固定行高、显式 y:不依赖 flex,滚动范围由子对象位置决定。
        lv_obj_t *row = lv_obj_create(ui->history_box);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, GAL_UI_W - 28, row_h);
        lv_obj_set_pos(row, 0, shown * row_h);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *label = mk_label(row, ui->font_small,
                                   (speaker && *speaker) ? COL_ACCENT : COL_TEXT, buf,
                                   0, 0, GAL_UI_W - 32);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        ui->history_rows[shown++] = row;
    }
    ui->history_count = shown;
    lv_obj_update_layout(ui->history_box);
    lv_obj_scroll_to_y(ui->history_box, LV_COORD_MAX, LV_ANIM_OFF);
}

void gal_ui_history_scroll(gal_ui_t *ui, int delta)
{
    if (!ui || !ui->history_box) return;
    const int y = lv_obj_get_scroll_y(ui->history_box);
    const int row_h = ui->large_font ? 24 : 20;
    lv_obj_scroll_to_y(ui->history_box, y + delta * row_h * 2, LV_ANIM_OFF);
}

void gal_ui_chapters_scroll(gal_ui_t *ui, int delta)
{
    if (!ui || !ui->chapter_box) return;
    const int y = lv_obj_get_scroll_y(ui->chapter_box);
    lv_obj_scroll_to_y(ui->chapter_box, y + delta * 26, LV_ANIM_OFF);
}

// ---------------------------------------------------------------- 章节
void gal_ui_set_chapters(gal_ui_t *ui, const gal_pack_t *pack, const gal_unlock_t *unlock,
                         int reachable)
{
    if (!ui || !pack) return;
    lv_obj_clean(ui->chapter_box);
    for (uint32_t i = 0; i < pack->chapter_count && i < 18; ++i) {
        const bool open = gal_unlock_chapter_get(unlock, (int)i);
        const bool at = ((int)i == reachable);
        lv_obj_t *row = mk_row(ui->chapter_box, 0, i * 26, GAL_UI_W - 40, 24, at);
        const char *name = gal_pack_name(pack, pack->chapters[i].name);
        lv_obj_t *label = mk_label_clip(row, ui->font_small, open ? COL_TEXT : COL_DIM, name,
                                        8, 3, GAL_UI_W - 150);
        if (!open) lv_label_set_text_fmt(label, "%s", name);
        mk_label_clip(row, ui->font_small, open ? COL_ACCENT : COL_DIM,
                      open ? "已读" : "未读", GAL_UI_W - 110, 3, 76);
        ui->chapter_rows[i] = row;
    }
    lv_obj_update_layout(ui->chapter_box);
    if (reachable >= 0) lv_obj_scroll_to_y(ui->chapter_box, reachable * 26 - 60, LV_ANIM_OFF);
}

// ---------------------------------------------------------------- 鉴赏
void gal_ui_set_gallery(gal_ui_t *ui, const gal_pack_t *pack, const gal_unlock_t *unlock,
                        int page, int selected)
{
    if (!ui || !pack) return;
    ui->gallery_page = page;
    ui->gallery_sel = selected;
    lv_obj_clean(ui->gallery_box);
    const int per = GAL_GALLERY_PER_PAGE;
    const int first = page * per;
    for (int i = 0; i < per; ++i) {
        const int idx = first + i;
        if (idx >= (int)pack->gallery_count) continue;
        const int col = i % GAL_GALLERY_COLS;
        const int row = i / GAL_GALLERY_COLS;
        const int x = col * 74;
        const int y = row * 62;
        const bool open = gal_unlock_cg_get(unlock, idx);
        const bool sel = (idx == selected);
        lv_obj_t *tile = mk_row(ui->gallery_box, x, y, 70, 58, sel);
        ui->gallery_tiles[i] = tile;
        const char *name = gal_pack_name(pack, pack->gallery[idx].name);
        ui->gallery_labels[i] = mk_label(tile, ui->font_small, open ? COL_TEXT : COL_DIM,
                                         open ? name : "？", 4, 18, 62);
        lv_obj_set_style_text_align(ui->gallery_labels[i], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        mk_label(tile, ui->font_small, COL_DIM, "CG", 4, 2, 30);
    }
    const int pages = ((int)pack->gallery_count + per - 1) / per;
    lv_label_set_text_fmt(ui->gallery_page_label, "%d / %d 页", page + 1, pages);
    lv_obj_set_style_text_color(ui->gallery_page_label, lv_color_hex(COL_DIM), LV_PART_MAIN);
}

void gal_ui_gallery_select(gal_ui_t *ui, int selected)
{
    if (!ui) return;
    ui->gallery_sel = selected;
    for (int i = 0; i < GAL_GALLERY_PER_PAGE; ++i) {
        if (ui->gallery_tiles[i]) {
            style_row(ui->gallery_tiles[i],
                      (ui->gallery_page * GAL_GALLERY_PER_PAGE + i) == selected);
        }
    }
}

void gal_ui_set_cg_caption(gal_ui_t *ui, const char *text)
{
    if (ui && ui->cg_caption) lv_label_set_text(ui->cg_caption, text ? text : "");
}

// ---------------------------------------------------------------- 结局/过场
void gal_ui_set_ending(gal_ui_t *ui, const char *eyebrow, const char *title,
                       const char *text)
{
    if (!ui) return;
    lv_label_set_text(ui->ending_eyebrow, eyebrow ? eyebrow : "");
    lv_label_set_text(ui->ending_title, title ? title : "");
    lv_label_set_text(ui->ending_text, text ? text : "");
}

void gal_ui_show_confirm(gal_ui_t *ui, const char *text, int selected)
{
    if (!ui || !ui->confirm_box) return;
    lv_label_set_text(ui->confirm_text, text ? text : "");
    gal_ui_confirm_select(ui, selected);
    ui->confirm_active = true;
    lv_obj_remove_flag(ui->confirm_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ui->confirm_box);
}

void gal_ui_confirm_select(gal_ui_t *ui, int selected)
{
    if (!ui) return;
    for (int i = 0; i < 2; ++i) {
        if (ui->confirm_rows[i]) style_row(ui->confirm_rows[i], i == selected);
    }
}

void gal_ui_hide_confirm(gal_ui_t *ui)
{
    if (!ui || !ui->confirm_box) return;
    ui->confirm_active = false;
    lv_obj_add_flag(ui->confirm_box, LV_OBJ_FLAG_HIDDEN);
}

void gal_ui_set_transition(gal_ui_t *ui, const char *title, const char *sub)
{
    if (!ui || !ui->transition_box) return;
    lv_label_set_text(ui->transition_title, title ? title : "");
    lv_label_set_text(ui->transition_sub, sub ? sub : "");
    lv_obj_remove_flag(ui->transition_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ui->transition_box);
}

void gal_ui_hide_transition(gal_ui_t *ui)
{
    if (ui && ui->transition_box) lv_obj_add_flag(ui->transition_box, LV_OBJ_FLAG_HIDDEN);
}

void gal_ui_apply_panel_alpha(gal_ui_t *ui, uint8_t percent)
{
    if (!ui || !ui->box) return;
    if (percent > 100) percent = 100;
    lv_obj_set_style_bg_opa(ui->box, (lv_opa_t)(percent * 255 / 100), LV_PART_MAIN);
}

// ---------------------------------------------------------------- 字号
int gal_ui_units_per_line(const gal_ui_t *ui)
{
    return ui && ui->large_font ? 30 : 38;
}

int gal_ui_lines_per_page(const gal_ui_t *ui)
{
    return ui && ui->large_font ? GAL_TEXT_LINES_20 : GAL_TEXT_LINES_16;
}

bool gal_ui_large_font(const gal_ui_t *ui) { return ui && ui->large_font; }

void gal_ui_set_large_font(gal_ui_t *ui, bool large)
{
    if (!ui) return;
    ui->large_font = large;
    const lv_font_t *font = large ? ui->font_large : ui->font_small;
    if (ui->text) lv_obj_set_style_text_font(ui->text, font, LV_PART_MAIN);
    if (ui->name) lv_obj_set_style_text_font(ui->name, font, LV_PART_MAIN);
    if (ui->chapter) lv_obj_set_style_text_font(ui->chapter, font, LV_PART_MAIN);
    if (ui->mode) lv_obj_set_style_text_font(ui->mode, font, LV_PART_MAIN);
    if (ui->notice) lv_obj_set_style_text_font(ui->notice, font, LV_PART_MAIN);
    for (int i = 0; i < 5; ++i) {
        if (ui->choice_labels[i]) lv_obj_set_style_text_font(ui->choice_labels[i], font, LV_PART_MAIN);
    }
}
