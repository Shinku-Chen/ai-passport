// main/nn_ui.c —— 横屏阅读器界面实现。
#include "nn_ui.h"


#include "nn_canvas.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(gal_cjk_16);
LV_FONT_DECLARE(gal_cjk_20);

// ---------------------------------------------------------------- 版面常量
// 竖屏 240x320：面板 224..320（96 高），正文 4 行 x19 = 76 贴在面板顶 +4。
#define W NN_SCREEN_W
#define H NN_SCREEN_H
#define PANEL_H 96
#define PANEL_Y (H - PANEL_H)
#define TEXT_X 8
#define TEXT_W (W - 2 * TEXT_X)
#define TEXT_Y (PANEL_Y + 4)    // 正文贴面板顶：页码已挪到面板外，无需再留一行
// 正文可用高度：16px 字 4 行 x 19px = 76；20px 字 3 行 x 23px = 69，都放得下。
#define TEXT_H 88          // 面板 96 - 顶部 4px = 92，4 行 x19 = 76 放得下
#define NAME_Y (PANEL_Y - 21)   // 名字在面板外（压画面），带底条

static const lv_font_t *font_for(const nn_ui_t *ui)
{
    return ui->font_large ? &gal_cjk_20 : &gal_cjk_16;
}

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, "");
    return l;
}

static void hide(lv_obj_t *obj, bool h)
{
    if (!obj) return;
    if (h) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

bool nn_ui_create(nn_ui_t *ui)
{
    if (!ui) return false;
    memset(ui, 0, sizeof(*ui));

    ui->root = lv_scr_act();
    lv_obj_clear_flag(ui->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(ui->root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(ui->root, lv_color_hex(0x000000), 0);

    // 场景画布：铺满整屏。
    // 分带画布：每条带一个 lv_image + **各自的缓冲/dsc**（共用一块缓冲会让三带
    // 都显示最后写入的那份内容——实机“同一张图重复三份”）。
    const int nbands = nn_canvas_band_count();
    const int band_h = nn_canvas_band_h();
    for (int i = 0; i < 6; ++i) {
        ui->art_bands[i] = NULL;
    }
    for (int i = 0; i < nbands && i < 6; ++i) {
        ui->art_bands[i] = lv_image_create(ui->root);
        // 尺寸必须显式给：真机上 lv_image_set_src() 之后对象仍是 0x0（LVGL 没从 dsc
        // 反推尺寸），四条带全是 0 尺寸 → 屏幕中间整块黑。这里直接钉死 240 x band_h。
        lv_image_set_src(ui->art_bands[i], nn_canvas_band_dsc(i));
        // 先给 src 再钉尺寸：lv_image_set_src 会把尺寸重置成"内容尺寸"，
        // 若 dsc 没被识别就是 0（实机四条带全是 0x0 → 中间整块黑）。
        int band_vis = band_h;
        if (i * band_h + band_vis > NN_SCREEN_H) {
            band_vis = NN_SCREEN_H - i * band_h;   // 6x56=336 > 320，最后一条要裁到 40
            if (band_vis < 1) band_vis = 1;
        }
        lv_obj_set_size(ui->art_bands[i], NN_SCREEN_W, band_vis);
        lv_obj_set_pos(ui->art_bands[i], 0, i * band_h);
        lv_image_set_inner_align(ui->art_bands[i], LV_IMAGE_ALIGN_TOP_LEFT);
        lv_obj_clear_flag(ui->art_bands[i], LV_OBJ_FLAG_SCROLLABLE);
    }
    ui->art = ui->art_bands[0];

    const lv_font_t *f16 = &gal_cjk_16;

    ui->header = mk_label(ui->root, f16, lv_color_hex(0xD8DEE9));
    lv_obj_set_pos(ui->header, 8, 5);

    ui->battery = mk_label(ui->root, f16, lv_color_hex(0x9AA7BD));
    lv_obj_set_width(ui->battery, 60);
    lv_label_set_long_mode(ui->battery, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(ui->battery, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(ui->battery, W - 68, 5);

    // 对话框：贴底半透明（透明度由设置控制）。
    ui->panel = lv_obj_create(ui->root);
    lv_obj_set_scrollbar_mode(ui->panel, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(ui->panel, W, PANEL_H);
    lv_obj_set_pos(ui->panel, 0, PANEL_Y);
    lv_obj_set_style_radius(ui->panel, 0, 0);
    lv_obj_set_style_border_width(ui->panel, 0, 0);
    lv_obj_set_style_pad_all(ui->panel, 0, 0);
    lv_obj_set_style_bg_color(ui->panel, lv_color_hex(0x0B1020), 0);

    ui->name = mk_label(ui->root, f16, lv_color_hex(0xF1C40F));
    lv_obj_set_pos(ui->name, TEXT_X + 2, NAME_Y);
    // 名字压在画面上：深色半透明底条保对比。
    lv_obj_set_style_bg_color(ui->name, lv_color_hex(0x0B1020), 0);
    lv_obj_set_style_bg_opa(ui->name, 180, 0);
    // 页码独立 label：面板内右上（142..158），正文从 161 起，不相交。
    // 页码挪到**面板外**右上（面板顶 - 18 .. 面板顶），正文才能贴面板顶 +4。
    ui->page_label = mk_label(ui->root, f16, lv_color_hex(0x9AA7BD));
    lv_obj_set_pos(ui->page_label, W - 58, PANEL_Y - 18);
    lv_obj_set_width(ui->page_label, 50);
    lv_obj_set_style_text_align(ui->page_label, LV_TEXT_ALIGN_RIGHT, 0);

    ui->body = mk_label(ui->root, f16, lv_color_hex(0xF5F6FA));
    lv_obj_set_pos(ui->body, TEXT_X, TEXT_Y);
    lv_obj_set_width(ui->body, TEXT_W);
    lv_obj_set_height(ui->body, TEXT_H);
    // 正文必须**换行**：竖屏宽 224px 只有 14 个全角字，CLIP 会把后面的字直接截掉。
    lv_label_set_long_mode(ui->body, LV_LABEL_LONG_WRAP);
    // 行高用字体自带的行高（16px 档 19px / 20px 档 23px），不再加额外行距：
    // 4 行 x 19 = 76px、3 行 x 23 = 69px，正好落在 90px 的对话框内。
    lv_obj_set_style_text_line_space(ui->body, 0, 0);

    // 按键提示放在名牌同一行的右侧（对话框上沿外），把对话框整块留给正文。
    ui->hint = mk_label(ui->root, f16, lv_color_hex(0x7F8C9B));
    lv_obj_set_width(ui->hint, 70);
    lv_obj_set_style_text_align(ui->hint, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(ui->hint, W - 78, NAME_Y);

    // 列表浮层：覆盖整屏的深色层（对话框之上）。
    ui->overlay = lv_obj_create(ui->root);
    lv_obj_set_scrollbar_mode(ui->overlay, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(ui->overlay, W, H);
    lv_obj_set_pos(ui->overlay, 0, 0);
    lv_obj_set_style_radius(ui->overlay, 0, 0);
    lv_obj_set_style_border_width(ui->overlay, 0, 0);
    lv_obj_set_style_pad_all(ui->overlay, 0, 0);
    lv_obj_set_style_bg_color(ui->overlay, lv_color_hex(0x05070C), 0);
    lv_obj_set_style_bg_opa(ui->overlay, LV_OPA_90, 0);
    lv_obj_clear_flag(ui->overlay, LV_OBJ_FLAG_SCROLLABLE);

    ui->ov_title = mk_label(ui->overlay, f16, lv_color_hex(0xF1C40F));
    lv_obj_set_pos(ui->ov_title, 10, 8);

    for (int i = 0; i < NN_UI_MAX_ROWS; ++i) {
        ui->ov_rows[i] = mk_label(ui->overlay, f16, lv_color_hex(0xE6E9EF));
        lv_obj_set_pos(ui->ov_rows[i], 18, 40 + i * 26);
        lv_obj_set_width(ui->ov_rows[i], W - 36);
        lv_label_set_long_mode(ui->ov_rows[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_pad_left(ui->ov_rows[i], 4, 0);
        lv_obj_set_style_bg_opa(ui->ov_rows[i], LV_OPA_TRANSP, 0);
    }
    ui->ov_foot = mk_label(ui->overlay, f16, lv_color_hex(0x7F8C9B));
    lv_obj_set_pos(ui->ov_foot, 10, H - 22);

    // 鉴赏室网格（3x2 缩略图 + 标题）。
    ui->grid = lv_obj_create(ui->root);
    lv_obj_set_scrollbar_mode(ui->grid, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(ui->grid, W, H);
    lv_obj_set_pos(ui->grid, 0, 0);
    lv_obj_set_style_radius(ui->grid, 0, 0);
    lv_obj_set_style_border_width(ui->grid, 0, 0);
    lv_obj_set_style_pad_all(ui->grid, 0, 0);
    lv_obj_set_style_bg_color(ui->grid, lv_color_hex(0x05070C), 0);
    lv_obj_set_style_bg_opa(ui->grid, LV_OPA_90, 0);
    lv_obj_clear_flag(ui->grid, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 6; ++i) {
        ui->grid_img[i] = lv_image_create(ui->grid);
        lv_obj_add_flag(ui->grid_img[i], LV_OBJ_FLAG_HIDDEN);
    }
    ui->grid_cap = mk_label(ui->grid, f16, lv_color_hex(0xE6E9EF));
    lv_obj_set_pos(ui->grid_cap, 10, H - 22);

    // 结局页 / 关于页共用的居中文本块。
    ui->center_name = mk_label(ui->root, f16, lv_color_hex(0xF1C40F));
    lv_obj_set_width(ui->center_name, W - 40);
    lv_obj_set_style_text_align(ui->center_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(ui->center_name, 20, 110);

    ui->center_body = mk_label(ui->root, f16, lv_color_hex(0xE6E9EF));
    lv_obj_set_width(ui->center_body, W - 48);
    lv_label_set_long_mode(ui->center_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(ui->center_body, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(ui->center_body, 4, 0);
    lv_obj_set_pos(ui->center_body, 24, 140);

    ui->about_body = mk_label(ui->root, f16, lv_color_hex(0xD8DEE9));
    lv_obj_set_width(ui->about_body, W - 40);
    lv_label_set_long_mode(ui->about_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(ui->about_body, 20, 52);

    nn_ui_show(ui, NN_PAGE_TITLE);
    return true;
}

// 每一页要显示哪些层。
static void apply_visibility(nn_ui_t *ui)
{
    const nn_page_t p = ui->page;
    const bool game_like = (p == NN_PAGE_GAME);
    const bool overlay_like = (p == NN_PAGE_TITLE || p == NN_PAGE_MENU ||
                              p == NN_PAGE_CHOICE || p == NN_PAGE_SLOTS ||
                              p == NN_PAGE_SETTINGS || p == NN_PAGE_HISTORY);
    // 选项页要保留对话框（玩家需要看到提问的那一句），所以面板按“正文页”显示。
    const bool panel_like = game_like || p == NN_PAGE_CHOICE;
    const bool grid_like = (p == NN_PAGE_GALLERY);
    const bool ending_like = (p == NN_PAGE_ENDING);
    const bool about_like = (p == NN_PAGE_ABOUT);

    for (int i = 0; i < 6; ++i) {
        if (ui->art_bands[i]) hide(ui->art_bands[i], false);   // 画布在所有页面都留着
    }
    hide(ui->panel, !panel_like);
    hide(ui->name, !panel_like);
    hide(ui->body, !panel_like);
    hide(ui->hint, !(panel_like || p == NN_PAGE_CGVIEW));
    hide(ui->header, p == NN_PAGE_TITLE || p == NN_PAGE_ENDING);
    hide(ui->battery, false);
    hide(ui->overlay, !overlay_like);
    hide(ui->grid, !grid_like);
    hide(ui->center_name, !(ending_like || about_like));
    hide(ui->center_body, !ending_like);
    hide(ui->about_body, !about_like);
    if (p == NN_PAGE_CGVIEW) {
        hide(ui->hint, false);
    }
}

void nn_ui_show(nn_ui_t *ui, nn_page_t page)
{
    if (!ui) return;
    ui->page = page;
    apply_visibility(ui);
}

void nn_ui_refresh_art(nn_ui_t *ui)
{
    if (!ui || !ui->art) return;
    for (int i = 0; i < nn_canvas_band_count() && i < 6; ++i) {
        if (ui->art_bands[i]) lv_obj_invalidate(ui->art_bands[i]);
    }
}

void nn_ui_refresh_band(nn_ui_t *ui, int band)
{
    if (!ui || band < 0 || band >= 6 || !ui->art_bands[band]) return;
    // 独立缓冲：只标脏本带；整帧由调用方统一刷一次（不在循环里 refr_now/延时）。
    lv_obj_invalidate(ui->art_bands[band]);
}

void nn_ui_set_font_large(nn_ui_t *ui, bool large)
{
    if (!ui) return;
    ui->font_large = large;
    const lv_font_t *f = font_for(ui);
    lv_obj_set_style_text_font(ui->body, f, 0);
    lv_obj_set_style_text_font(ui->name, f, 0);
    lv_obj_set_style_text_font(ui->hint, f, 0);
    lv_obj_set_style_text_line_space(ui->body, 0, 0);
    lv_obj_set_style_text_font(ui->ov_title, f, 0);
    lv_obj_set_style_text_font(ui->ov_foot, f, 0);
    for (int i = 0; i < NN_UI_MAX_ROWS; ++i) {
        lv_obj_set_style_text_font(ui->ov_rows[i], f, 0);
    }
    lv_obj_set_style_text_font(ui->grid_cap, f, 0);
    lv_obj_set_style_text_font(ui->center_name, f, 0);
    lv_obj_set_style_text_font(ui->center_body, f, 0);
    lv_obj_set_style_text_font(ui->about_body, f, 0);
}

void nn_ui_set_dialogue(nn_ui_t *ui, const char *speaker, const char *text,
                        int shown_bytes)
{
    if (!ui) return;
    lv_label_set_text(ui->name, (speaker && speaker[0]) ? speaker : "");

    const char *src = text ? text : "";
    if (shown_bytes <= 0) {
        lv_label_set_text(ui->body, src);
        return;
    }
    // 按 UTF-8 边界截断：不能把多字节字符切成半个
    int cut = shown_bytes;
    const int len = (int)strlen(src);
    if (cut > len) cut = len;
    while (cut > 0 && (((unsigned char)src[cut] & 0xC0u) == 0x80u)) cut--;
    const size_t n = (size_t)cut;
    if (n >= sizeof(ui->text_buf)) return;
    memcpy(ui->text_buf, src, n);
    ui->text_buf[n] = '\0';
    lv_label_set_text(ui->body, ui->text_buf);
}

void nn_ui_set_page(nn_ui_t *ui, int page, int pages)
{
    if (!ui || !ui->page_label) return;
    if (pages > 1) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d/%d", page + 1, pages);
        lv_label_set_text(ui->page_label, buf);
        hide(ui->page_label, false);
    } else {
        lv_label_set_text(ui->page_label, "");
        hide(ui->page_label, true);
    }
}

void nn_ui_set_hint(nn_ui_t *ui, const char *text)
{
    if (ui) lv_label_set_text(ui->hint, text ? text : "");
}

void nn_ui_set_header(nn_ui_t *ui, const char *text)
{
    if (ui) lv_label_set_text(ui->header, text ? text : "");
}

void nn_ui_set_battery(nn_ui_t *ui, int percent)
{
    if (!ui) return;
    if (percent < 0) {
        lv_label_set_text(ui->battery, "--");
        return;
    }
    char buf[12];
    snprintf(buf, sizeof(buf), "%d%%", percent > 100 ? 100 : percent);
    lv_label_set_text(ui->battery, buf);
}

void nn_ui_set_panel_opa(nn_ui_t *ui, uint8_t percent)
{
    if (!ui) return;
    lv_obj_set_style_bg_opa(ui->panel, (lv_opa_t)(percent * 255 / 100), 0);
}

void nn_ui_set_overlay(nn_ui_t *ui, const char *title, const char *const *rows,
                       const char *const *values, int count, int selected)
{
    if (!ui) return;
    lv_label_set_text(ui->ov_title, title ? title : "");
    if (count > NN_UI_MAX_ROWS) count = NN_UI_MAX_ROWS;
    for (int i = 0; i < NN_UI_MAX_ROWS; ++i) {
        if (i >= count) {
            hide(ui->ov_rows[i], true);
            continue;
        }
        hide(ui->ov_rows[i], false);
        char buf[96];
        const char *label = rows[i] ? rows[i] : "";
        if (values && values[i] && values[i][0]) {
            snprintf(buf, sizeof(buf), "%s  %s", label, values[i]);
        } else {
            snprintf(buf, sizeof(buf), "%s", label);
        }
        lv_label_set_text(ui->ov_rows[i], buf);
        const bool sel = (i == selected);
        lv_obj_set_style_bg_opa(ui->ov_rows[i], sel ? LV_OPA_40 : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(ui->ov_rows[i], lv_color_hex(0x3B4A63), 0);
        lv_obj_set_style_text_color(ui->ov_rows[i],
                                    sel ? lv_color_hex(0xFFFFFF) : lv_color_hex(0xB9C3D3), 0);
    }
}

void nn_ui_overlay_foot(nn_ui_t *ui, const char *text)
{
    if (ui) lv_label_set_text(ui->ov_foot, text ? text : "");
}

void nn_ui_overlay_opa(nn_ui_t *ui, uint8_t percent)
{
    if (!ui) return;
    lv_obj_set_style_bg_opa(ui->overlay, (lv_opa_t)(percent * 255 / 100), 0);
}

void nn_ui_overlay_compact(nn_ui_t *ui, bool compact)
{
    if (!ui) return;
    lv_obj_set_size(ui->overlay, W, compact ? 122 : H);
    hide(ui->ov_foot, compact);
    for (int i = 0; i < NN_UI_MAX_ROWS; ++i) {
        lv_obj_set_width(ui->ov_rows[i], W - 36);
        lv_obj_set_pos(ui->ov_rows[i], 18, (compact ? 32 : 40) + i * 26);
    }
}

// 标题页专用：画布只有 150 高（画面区），菜单放进画布下方的黑带里，
// 排成 3 列 x 2 行，画面区（0..150）只被 62% 遮罩压暗、不被菜单压住。
void nn_ui_overlay_title_layout(nn_ui_t *ui, bool grid)
{
    if (!ui) return;
    if (!grid) {
        nn_ui_overlay_compact(ui, false);
        return;
    }
    lv_obj_set_size(ui->overlay, W, H);
    hide(ui->ov_foot, true);
    // 竖屏：2 列 x 3 行，放在画面区下方的菜单带里。
    // 行高 24（16px 档）/ 26（20px 档）：底边最多 284+26=310，距屏底留 ≥10px。
    static const int col_x[2] = {12, 126};
    const int row_h = ui->font_large ? 26 : 24;
    const int y0 = ui->font_large ? 232 : 236;
    for (int i = 0; i < NN_UI_MAX_ROWS; ++i) {
        lv_obj_set_width(ui->ov_rows[i], 102);
        lv_obj_set_height(ui->ov_rows[i], row_h);
        lv_obj_set_pos(ui->ov_rows[i], col_x[i % 2], y0 + (i / 2) * row_h);
    }
}

void nn_ui_set_gallery(nn_ui_t *ui, const void *const *thumbs, int count, int selected,
                       const char *caption)
{
    if (!ui) return;
    if (count > 6) count = 6;
    for (int i = 0; i < 6; ++i) {
        if (i >= count || !thumbs[i]) {
            hide(ui->grid_img[i], true);
            continue;
        }
        hide(ui->grid_img[i], false);
        lv_image_set_src(ui->grid_img[i], thumbs[i]);
        const int col = i % 2;
        const int row = i / 2;
        lv_obj_set_pos(ui->grid_img[i], 12 + col * 112, 30 + row * 88);
        // 选中框：给缩略图加一圈高亮
        lv_obj_set_style_outline_width(ui->grid_img[i], (i == selected) ? 2 : 0, 0);
        lv_obj_set_style_outline_color(ui->grid_img[i], lv_color_hex(0xF1C40F), 0);
        lv_obj_set_style_outline_pad(ui->grid_img[i], 1, 0);
    }
    lv_label_set_text(ui->grid_cap, caption ? caption : "");
}

void nn_ui_gallery_caption(nn_ui_t *ui, const char *caption)
{
    if (ui) lv_label_set_text(ui->grid_cap, caption ? caption : "");
}

void nn_ui_set_ending(nn_ui_t *ui, const char *name, const char *text)
{
    if (!ui) return;
    lv_label_set_text(ui->center_name, name ? name : "");
    lv_label_set_text(ui->center_body, text ? text : "");
}

void nn_ui_set_about(nn_ui_t *ui, const char *text)
{
    if (!ui) return;
    lv_label_set_text(ui->about_body, text ? text : "");
}
