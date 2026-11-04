// main/anemoi/anemoi_render.c —— LVGL 渲染层。
//
// 版面（320x240 横屏）：
//   0..179   画面区（背景/CG/立绘都画在这块画布上）
//   0..17    顶部章节/Day 条（半透明）
//   180..239 对话框（纸色不透明）：说话人 + 两行正文 + 页码
//   列表页   整屏纸色面板：标题 + 最多 10 行，选中行加“▶”
#include "anemoi_render.h"

#include "anemoi_canvas.h"

#include "lvgl.h"

#include <stdio.h>
#include <string.h>

#define PAPER 0xFCFAF0u
#define INK 0x3F3129u
#define ACCENT 0x614331u
#define MUTED 0x748C82u

LV_FONT_DECLARE(gal_cjk_16);

static lv_obj_t *s_art;
static lv_obj_t *s_header_bar;
static lv_obj_t *s_header;
static lv_obj_t *s_panel;
static lv_obj_t *s_speaker;
static lv_obj_t *s_page;
static lv_obj_t *s_line[ANEM_LINE_MAX];
static lv_obj_t *s_list;
static lv_obj_t *s_list_title;
static lv_obj_t *s_row[ANEM_ROW_MAX];
static lv_obj_t *s_notice;
static lv_obj_t *s_caption;

static void style_label(lv_obj_t *label, uint32_t color)
{
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(label, &gal_cjk_16, 0);
    lv_obj_set_style_text_letter_space(label, 0, 0);
}

bool anem_render_init(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    s_art = lv_image_create(screen);
    lv_obj_set_pos(s_art, 0, 0);

    s_header_bar = lv_obj_create(screen);
    lv_obj_remove_flag(s_header_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_header_bar, 0, 0);
    lv_obj_set_size(s_header_bar, 320, 18);
    lv_obj_set_style_radius(s_header_bar, 0, 0);
    lv_obj_set_style_border_width(s_header_bar, 0, 0);
    lv_obj_set_style_bg_color(s_header_bar, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_bg_opa(s_header_bar, LV_OPA_60, 0);
    lv_obj_set_style_pad_all(s_header_bar, 0, 0);

    s_header = lv_label_create(screen);
    lv_obj_set_pos(s_header, 8, 1);
    lv_obj_set_width(s_header, 304);
    style_label(s_header, 0xFFFFFF);

    s_panel = lv_obj_create(screen);
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);
    // 面板下移加高：名字放面板外（图上），正文 4 行全部落在面板内。
    lv_obj_set_pos(s_panel, 0, 176);
    lv_obj_set_size(s_panel, 320, 64);   // 176..240：3 行正文
    lv_obj_set_style_radius(s_panel, 0, 0);
    lv_obj_set_style_border_width(s_panel, 0, 0);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(PAPER), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);   // 用户要求：阅读对话框不透明（面板 156..240，4 行）
    lv_obj_set_style_pad_all(s_panel, 0, 0);

    s_speaker = lv_label_create(screen);
    lv_obj_set_pos(s_speaker, 10, 152);   // 面板外：叠在画面上
    lv_obj_set_width(s_speaker, LV_SIZE_CONTENT);   // 底条只包住文字
    style_label(s_speaker, 0xFFFFFF);
    lv_obj_set_style_bg_color(s_speaker, lv_color_hex(0x1A140F), 0);
    lv_obj_set_style_bg_opa(s_speaker, 170, 0);
    lv_obj_set_style_pad_hor(s_speaker, 6, 0);
    lv_obj_set_style_pad_left(s_speaker, 6, 0);
    lv_obj_set_style_pad_ver(s_speaker, 2, 0);

    s_page = lv_label_create(screen);
    lv_obj_set_pos(s_page, 262, 178);
    lv_obj_set_width(s_page, 60);
    style_label(s_page, MUTED);

    for (int i = 0; i < ANEM_LINE_MAX; i++) {
        s_line[i] = lv_label_create(screen);
        lv_obj_set_pos(s_line[i], 10, 182 + i * 19);   // 182/201/220，3 行（面板 176..240）
        lv_obj_set_width(s_line[i], 302);
        lv_label_set_long_mode(s_line[i], LV_LABEL_LONG_MODE_CLIP);
        style_label(s_line[i], INK);
    }

    s_list = lv_obj_create(screen);
    lv_obj_remove_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_list, 0, 0);
    lv_obj_set_size(s_list, 320, 240);
    lv_obj_set_style_radius(s_list, 0, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_bg_color(s_list, lv_color_hex(PAPER), 0);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);

    s_list_title = lv_label_create(s_list);
    lv_obj_set_pos(s_list_title, 8, 6);
    lv_obj_set_width(s_list_title, 304);
    style_label(s_list_title, ACCENT);

    for (int i = 0; i < ANEM_ROW_MAX; i++) {
        s_row[i] = lv_label_create(s_list);
        lv_obj_set_pos(s_row[i], 8, 32 + i * 20);
        lv_obj_set_width(s_row[i], 304);
        lv_label_set_long_mode(s_row[i], LV_LABEL_LONG_MODE_CLIP);
        style_label(s_row[i], INK);
    }

    s_caption = lv_label_create(screen);
    lv_obj_set_pos(s_caption, 8, 214);
    lv_obj_set_width(s_caption, 304);
    style_label(s_caption, 0xFFFFFF);

    s_notice = lv_label_create(screen);
    lv_obj_set_pos(s_notice, 8, 214);
    lv_obj_set_width(s_notice, 304);
    style_label(s_notice, ACCENT);

    lv_obj_add_flag(s_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_caption, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_notice, LV_OBJ_FLAG_HIDDEN);
    return true;
}

void anem_render_set_art(const void *dsc, uint16_t w, uint16_t h)
{
    if (s_art == NULL) {
        return;
    }
    if (dsc == NULL) {
        lv_image_set_src(s_art, NULL);
        return;
    }
    lv_image_set_src(s_art, dsc);
    lv_obj_set_size(s_art, w, h);
    lv_obj_set_pos(s_art, 0, 0);
    lv_obj_invalidate(s_art);
}

void anem_render_invalidate_art(void)
{
    if (s_art != NULL) {
        lv_obj_invalidate(s_art);
    }
}

static void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

void anem_render_apply(const anem_view_t *view, uint8_t panel_alpha)
{
    if (view == NULL) {
        return;
    }
    const bool list_mode = view->row_count > 0 && view->screen != ANEM_SCR_READ &&
                           view->screen != ANEM_SCR_CHOICE;
    const bool reading = view->screen == ANEM_SCR_READ || view->screen == ANEM_SCR_CHOICE;
    const bool art = view->show_art;

    set_hidden(s_art, !art);
    set_hidden(s_header_bar, view->header[0] == 0);
    set_hidden(s_header, view->header[0] == 0);
    lv_label_set_text(s_header, view->header);

    set_hidden(s_panel, !reading);
    if (panel_alpha >= 100) {
        lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);   // 用户要求：阅读对话框不透明（面板 156..240，4 行）
    } else {
        lv_obj_set_style_bg_opa(s_panel, (lv_opa_t)(panel_alpha * 255 / 100), 0);
    }
    set_hidden(s_speaker, !reading || view->speaker[0] == 0);
    lv_label_set_text(s_speaker, view->speaker);
    char page[16];
    if (reading && view->pages > 1) {
        snprintf(page, sizeof(page), "%u/%u", (unsigned)(view->page + 1),
                 (unsigned)view->pages);
    } else {
        page[0] = 0;
    }
    set_hidden(s_page, page[0] == 0);
    lv_label_set_text(s_page, page);
    for (int i = 0; i < ANEM_LINE_MAX; i++) {
        const bool show = reading && i < view->line_count;
        set_hidden(s_line[i], !show);
        if (show) {
            lv_label_set_text(s_line[i], view->line[i]);
        }
    }

    set_hidden(s_list, !list_mode);
    if (list_mode) {
        const bool title_screen = view->fullscreen_art && view->screen == ANEM_SCR_TITLE;
        if (title_screen) {
            // 标题页：列表改为下半屏半透明面板，上半屏留给标题图（否则全屏不透明面板会盖住图）。
            lv_obj_set_pos(s_list, 0, 146);
            lv_obj_set_size(s_list, 320, 94);
            // 标题菜单：不透明纸色底 + 深色字，保证在封面图上可读（用户要求不要透明）。
            lv_obj_set_style_bg_color(s_list, lv_color_hex(PAPER), 0);
            lv_obj_set_style_bg_opa(s_list, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_pos(s_list, 0, 0);
            lv_obj_set_size(s_list, 320, 240);
            lv_obj_set_style_bg_color(s_list, lv_color_hex(PAPER), 0);
            lv_obj_set_style_bg_opa(s_list, LV_OPA_COVER, 0);
        }
        lv_label_set_text(s_list_title, view->header);
        // 选项页行距拉开，避免文字重叠；选中项行高足够不遮挡下一行。
        if (view->screen == ANEM_SCR_CHOICE) {
            for (int i = 0; i < ANEM_ROW_MAX; i++) {
                lv_obj_set_pos(s_row[i], 8, 24 + i * 26);
            }
        }
        // 标题页面板只有 94px，6 行放不下 → 只显示 4 行，按选中项滚动（保证选中可见）。
        const bool title_list = (view->screen == ANEM_SCR_TITLE);
        uint16_t first_visible = 0;
        if (title_list && view->row_count > 4) {
            first_visible = (uint16_t)((view->sel / 4) * 4);
            if (first_visible + 4 > view->row_count) {
                first_visible = (uint16_t)(view->row_count - 4);
            }
        }
        // 普通列表（鉴赏/章节/菜单/存档/设置/关于）垂直居中，避免整块偏上、下半屏空着。
        const bool plain_list = !title_list && view->screen != ANEM_SCR_CHOICE;
        int list_top = 32;
        if (plain_list) {
            int rows = (int)view->row_count;
            if (rows > ANEM_ROW_MAX) rows = ANEM_ROW_MAX;
            list_top = (240 - rows * 20) / 2;
            if (list_top < 40) list_top = 40;   // 不压住顶部标题行
        }
        for (int i = 0; i < ANEM_ROW_MAX; i++) {
            const bool in_window = !title_list || (i >= first_visible && i < first_visible + 4);
            const bool show = i < view->row_count && in_window;
            if (title_list) {
                lv_obj_set_pos(s_row[i], 8, 24 + (i - (int)first_visible) * 17);
            } else if (plain_list) {
                lv_obj_set_pos(s_row[i], 8, list_top + i * 20);
            }
            set_hidden(s_row[i], !show);
            if (!show) {
                continue;
            }
            const anem_view_row_t *row = &view->row[i];
            char text[ANEM_ROW_TEXT + 4];
            snprintf(text, sizeof(text), "%s%s", row->selected ? "▶ " : "  ", row->text);
            lv_label_set_text(s_row[i], text);
            lv_obj_set_style_text_color(s_row[i],
                                        lv_color_hex(row->selected ? ACCENT
                                                                    : (row->enabled ? INK
                                                                                    : MUTED)),
                                        0);
        }
    }

    set_hidden(s_caption, view->screen != ANEM_SCR_CG);
    if (view->screen == ANEM_SCR_CG) {
        lv_label_set_text(s_caption, view->header);
    }
    set_hidden(s_notice, view->notice[0] == 0);
    if (view->notice[0] != 0) {
        lv_label_set_text(s_notice, view->notice);
        lv_obj_set_style_text_color(s_notice,
                                    lv_color_hex(view->notice_kind == 2 ? 0xB03030 : ACCENT), 0);
    }
}
