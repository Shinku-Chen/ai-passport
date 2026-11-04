// main/gal/gal_app.c —— 《PARQUET》横屏 320x240 阅读器界面。
//
// 版面：上方 320x150 画面区（背景 + 立绘 + cut-in 合成），下方 90px 不透明对话框。
// 作品专属 UI：标题=标题背景 + Logo + 底部两列菜单；正文=青色描边对白带；列表页
// =5 行窗口 + 顶部标题 + 底部按键提示。设置里只列真实可用的选项（16px 字体）。
#include "gal_app.h"

#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(gal_cjk_16);
LV_FONT_DECLARE(gal_cjk_20);

static const char *TAG = "parquet_ui";

#define SCR_W 320
#define SCR_H 240
#define ART_H 150
#define ART_W 320
#define BOX_Y ART_H
#define BOX_H (SCR_H - ART_H)
#define TEXT_X 8
#define TEXT_W 296
#define TEXT_LINES 3
#define TEXT_Y (BOX_Y + 4)    // 正文贴面板顶（不垂直居中）：154 起，3 行 x19 = 154..211
#define LIST_TOP 27
#define LIST_ROW_H 34
#define NOTICE_MS 1800

#define COLOR_BG 0x04070C
#define COLOR_PANEL 0x061018
#define COLOR_ACCENT 0x2B7E8C
#define COLOR_TEXT 0xEDF7FA
#define COLOR_DIM 0x9FB9C2
#define COLOR_CYAN 0x7FE3EA
#define COLOR_ROW 0x0B1A22
#define COLOR_ROW_SEL 0x17414F
#define COLOR_LOCK 0x5C6E75

static const char *const s_game_menu[] = {
    "继续阅读", "保存进度", "读取存档", "历史文本", "CG 鉴赏", "章节选择", "设置", "返回标题",
};
static const int s_game_menu_count = (int)(sizeof(s_game_menu) / sizeof(s_game_menu[0]));

static const char *const s_settings_names[] = {
    "文字速度", "自动速度", "字号", "标题背景 · 默认", "标题背景 · 梨乃",
    "标题背景 · 翼", "返回",
};
static const int s_settings_count = (int)(sizeof(s_settings_names) / sizeof(s_settings_names[0]));

static const char *const s_speed_names[] = {"慢", "中", "快", "瞬间"};
static const char *const s_auto_names[] = {"慢", "标准", "快"};

// 源 catalog 的 3 条路线 / 标题菜单顺序
static const char *const s_title_items[] = {
    "AFTER STORY", "START", "SIDE STORY", "CHAPTER", "LOAD", "SETTINGS", "GALLERY",
};

static int s_var_rin = -1;
static int s_var_tsu = -1;
static int s_var_main = -1;
static int s_var_side = -1;

static uint16_t s_page_start[8];
static uint16_t s_page_len[8];

// ---------------------------------------------------------------- 小工具
static void set_bg(lv_obj_t *o, uint32_t hex, lv_opa_t opa)
{
    lv_obj_set_style_bg_color(o, lv_color_hex(hex), 0);
    lv_obj_set_style_bg_opa(o, opa, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
}

static lv_obj_t *mk_label(lv_obj_t *parent, const char *text, int x, int y, int w, int h)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &gal_cjk_16, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_pos(l, x, y);
    if (w > 0) {
        lv_obj_set_width(l, w);
    }
    if (h > 0) {
        lv_obj_set_size(l, w, h);
    }
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    return l;
}

// 列表行专用：单行 + 末尾截断（长选项折行会压到下一行）。
static lv_obj_t *mk_label_clip(lv_obj_t *parent, const char *text, int x, int y, int w, int h)
{
    lv_obj_t *l = mk_label(parent, text, x, y, w, h);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    return l;
}

static lv_obj_t *mk_row(lv_obj_t *parent, int i)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_set_size(r, SCR_W - 24, LIST_ROW_H - 4);
    lv_obj_set_pos(r, 12, LIST_TOP + i * LIST_ROW_H);
    set_bg(r, COLOR_ROW, LV_OPA_COVER);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_style_border_color(r, lv_color_hex(0x1E3A46), 0);
    lv_obj_set_scrollbar_mode(r, LV_SCROLLBAR_MODE_OFF);
    return r;
}

static void row_set(lv_obj_t *row, const char *text, bool selected)
{
    lv_obj_t *lbl = lv_obj_get_child(row, 0);
    if (!lbl) {
        lbl = mk_label_clip(row, text, 8, 4, SCR_W - 44, 0);
    } else {
        lv_label_set_text(lbl, text);
    }
    lv_obj_set_style_bg_color(row, lv_color_hex(selected ? COLOR_ROW_SEL : COLOR_ROW), 0);
    lv_obj_set_style_border_color(row, lv_color_hex(selected ? COLOR_ACCENT : 0x1E3A46), 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(selected ? 0xFFFFFF : 0xC7D8DE), 0);
}

static int utf8_next(const char *s, int i)
{
    unsigned char c = (unsigned char)s[i];
    if (!c) {
        return i;
    }
    if (c < 0x80) {
        return i + 1;
    }
    if ((c >> 5) == 6) {
        return i + 2;
    }
    if ((c >> 4) == 14) {
        return i + 3;
    }
    return i + 4;
}

static int utf8_count(const char *s)
{
    int n = 0;
    for (int i = 0; s && s[i]; i = utf8_next(s, i)) {
        n++;
    }
    return n;
}

static const lv_font_t *body_font(const gal_app_t *app)
{
    return app->settings.font ? &gal_cjk_20 : &gal_cjk_16;
}

static int cjk_width(const gal_app_t *app)
{
    return app->settings.font ? 20 : 16;
}

static int glyph_w(const char *s, int i, int cjk_w)
{
    return (unsigned char)s[i] < 0x80 ? cjk_w / 2 : cjk_w;
}

static void copy_utf8(char *dst, size_t cap, const char *src)
{
    if (cap == 0) {
        return;
    }
    size_t n = 0;
    while (src && src[n] && n + 1 < cap) {
        n++;
    }
    // 不要把一个 UTF-8 序列截成半个字符。
    while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) {
        n--;
    }
    memcpy(dst, src ? src : "", n);
    dst[n] = 0;
}

// ---------------------------------------------------------------- 分页
static int paginate(const char *text, int cjk_w, uint16_t *starts, uint16_t *lens,
                    int max_pages)
{
    int len = (int)strlen(text);
    int pages = 0;
    int i = 0;
    while (i < len && pages < max_pages) {
        int start = i;
        for (int line = 0; line < TEXT_LINES && i < len; line++) {
            int w = 0;
            while (i < len) {
                unsigned char c = (unsigned char)text[i];
                if (c == '\n') {
                    i++;
                    break;
                }
                int gw = glyph_w(text, i, cjk_w);
                if (w + gw > TEXT_W) {
                    break;
                }
                w += gw;
                i = utf8_next(text, i);
            }
        }
        if (i == start) {
            i = utf8_next(text, i);   // 防御：异常超宽字符至少前进一个码点
        }
        starts[pages] = (uint16_t)start;
        lens[pages] = (uint16_t)(i - start);
        pages++;
    }
    return pages ? pages : 1;
}

static void reader_refresh_text(gal_app_t *app)
{
    if (!app->lbl_text) {
        return;
    }
    const char *src = app->text + s_page_start[app->page_index];
    int n = s_page_len[app->page_index];
    if (app->typing && app->typed < n) {
        n = app->typed;
    }
    char buf[TEXT_LINES * 140 + 8];
    if (n > (int)sizeof(buf) - 1) {
        n = (int)sizeof(buf) - 1;
    }
    memcpy(buf, src, n);
    buf[n] = 0;
    lv_label_set_text(app->lbl_text, buf);
    if (app->lbl_page) {
        char page[32];
        if (app->page_count > 1) {
            snprintf(page, sizeof(page), "%d/%d", app->page_index + 1, app->page_count);
        } else {
            page[0] = '\0';
        }
        lv_label_set_text(app->lbl_page, page);
    }
}

static void reader_refresh_hint(gal_app_t *app)
{
    if (!app->lbl_tag) {
        return;
    }
    const char *hint = NULL;
    if (app->notice_ms) {
        hint = app->notice;
    } else if (app->draw_error) {
        hint = "素材读取失败";
    } else if (app->fast_forward) {
        hint = "快进";
    } else if (app->auto_mode) {
        hint = "自动";
    }
    if (hint) {
        lv_label_set_text(app->lbl_tag, hint);
        lv_obj_set_style_text_color(app->lbl_tag, lv_color_hex(COLOR_CYAN), 0);
    } else {
        lv_label_set_text(app->lbl_tag, pq_tag_label(&app->game.pack, app->tag));
        lv_obj_set_style_text_color(app->lbl_tag, lv_color_hex(COLOR_DIM), 0);
    }
}

// ---------------------------------------------------------------- 画面
static void clear_root(gal_app_t *app)
{
    app->reader_built = false;
    if (app->root) {
        lv_obj_del(app->root);
    }
    app->root = lv_obj_create(lv_screen_active());
    lv_obj_set_size(app->root, SCR_W, SCR_H);
    lv_obj_set_pos(app->root, 0, 0);
    set_bg(app->root, COLOR_BG, LV_OPA_COVER);
    lv_obj_set_scrollbar_mode(app->root, LV_SCROLLBAR_MODE_OFF);
    app->img_art = NULL;
    app->img_art2 = NULL;
    app->box = NULL;
    app->lbl_speaker = NULL;
    app->lbl_tag = NULL;
    app->lbl_page = NULL;
    app->lbl_text = NULL;
    app->lbl_hint = NULL;
    app->header = NULL;
    for (int i = 0; i < PQ_LIST_ROWS; i++) {
        app->rows[i] = NULL;
        app->row_labels[i] = NULL;
    }
}

static void title_art(gal_app_t *app);

static void attach_art(gal_app_t *app);
static void invalidate_art(gal_app_t *app);

#ifdef PQ_UI_TEST_HOOKS
int s_draw_scene_calls;
#endif

static bool draw_scene(gal_app_t *app)
{
#ifdef PQ_UI_TEST_HOOKS
    s_draw_scene_calls++;
#endif
    pq_pack_t *pack = &app->game.pack;
    bool ok = true;
    pq_gfx_clear(0x0000);
    if (app->bg != PQ_NO_IMAGE) {
        uint16_t img = pq_asset_image(pack, app->bg);
        const pq_img_entry_t *e = pq_img_entry(pack, img);
        if (img == PQ_NO_IMAGE || !e) {
            ok = false;
        } else {
            // 背景铺满整屏 0..239（原图只有 320x150，1:1 会在 150 截断）：
            // 等比 cover 到 320xcanvas_h，多出的部分居中裁。
            ok = pq_gfx_draw_jpeg_cover(pack, img, 0, 0, ART_W,
                                        pq_gfx_canvas_height()) && ok;
        }
    }
    uint16_t order[3];
    int n = 0;
    for (int i = 0; i < 3; i++) {
        if (app->bodies[i] != PQ_NO_IMAGE) {
            order[n++] = app->bodies[i];
        }
    }
    static const int center1[1] = {160};
    static const int center2[2] = {93, 227};
    static const int center3[3] = {67, 160, 253};
    const int *centers = n == 1 ? center1 : n == 2 ? center2 : center3;
    for (int i = 0; i < n; i++) {
        bool body_ok = false, patch_ok = true;
        (void)pq_gfx_draw_portrait(pack, order[i], centers[i], &body_ok, &patch_ok);
        if (!body_ok || !patch_ok) {
            ok = false;
        }
    }
    if (app->cutin != PQ_NO_IMAGE) {
        pq_gfx_dim(150);
        if (!pq_gfx_draw_cutin(pack, app->cutin, app->cutin_opacity ? app->cutin_opacity : 200)) {
            ok = false;
        }
    }
    if (!ok) {
        pq_gfx_clear(0x2000);   // 明确的错误底色，不静默缺图
    } else if (app->bg != PQ_NO_IMAGE) {
        pq_game_mark_cg(&app->game, app->bg);   // 完整成功才解锁该 CG 组
    }
    app->draw_error = !ok;
    invalidate_art(app);
    return ok;
}

// 画布条带 -> LVGL 图像控件。双带模式两条竖着拼成 320x240，单块回退只有第 0 条。
static void attach_art(gal_app_t *app)
{
    app->img_art = lv_image_create(app->root);
    lv_image_set_src(app->img_art, pq_gfx_canvas(0));
    lv_obj_set_pos(app->img_art, 0, 0);
    app->img_art2 = NULL;
    if (pq_gfx_band_count() > 1) {
        app->img_art2 = lv_image_create(app->root);
        lv_image_set_src(app->img_art2, pq_gfx_canvas(1));
        lv_obj_set_pos(app->img_art2, 0, PQ_BAND_H);
    }
}

static void invalidate_art(gal_app_t *app)
{
    if (app->img_art) {
        lv_obj_invalidate(app->img_art);
    }
    if (app->img_art2) {
        lv_obj_invalidate(app->img_art2);
    }
}

static void build_reader_objects(gal_app_t *app)
{
    clear_root(app);
    app->reader_built = true;
    attach_art(app);

    app->box = lv_obj_create(app->root);
    lv_obj_set_size(app->box, SCR_W, BOX_H);
    lv_obj_set_pos(app->box, 0, BOX_Y);
    set_bg(app->box, COLOR_PANEL, 180);   // 0xB4：半透
    lv_obj_set_style_border_width(app->box, 1, 0);
    lv_obj_set_style_border_color(app->box, lv_color_hex(COLOR_ACCENT), 0);

    app->lbl_speaker = mk_label(app->root, pq_speaker(&app->game.pack, app->speaker),
                                10, 128, 0, 18);
    lv_obj_set_style_text_color(app->lbl_speaker, lv_color_hex(COLOR_CYAN), 0);
    // 名字在框外：底条只包文字（内容宽度 + 左右 6px），不再固定 160 拉满。
    lv_obj_set_width(app->lbl_speaker, LV_SIZE_CONTENT);
    lv_obj_set_height(app->lbl_speaker, 18);
    lv_obj_set_style_pad_hor(app->lbl_speaker, 6, 0);
    lv_obj_set_style_bg_color(app->lbl_speaker, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_bg_opa(app->lbl_speaker, 180, 0);
    app->lbl_tag = mk_label(app->root, pq_tag_label(&app->game.pack, app->tag),
                            170, 128, 142, 18);
    lv_obj_set_style_text_align(app->lbl_tag, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(app->lbl_tag, lv_color_hex(COLOR_DIM), 0);
    // 页码独立 label：挪到面板外的右上（132..150），与左侧名字同一行；
    // 这样正文可以贴到面板顶 y=154 而不与页码相撞。
    app->lbl_page = mk_label(app->root, "", 262, 128, 50, 18);
    lv_obj_set_style_text_align(app->lbl_page, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(app->lbl_page, lv_color_hex(COLOR_DIM), 0);
    app->lbl_text = mk_label(app->root, "", TEXT_X, TEXT_Y, TEXT_W, 0);
    lv_obj_set_style_text_font(app->lbl_text, body_font(app), 0);
    lv_obj_set_style_text_line_space(app->lbl_text, 0, 0);
    if (app->view == PQ_VIEW_ANOTHER) {
        lv_obj_t *banner = mk_label(app->root, "ANOTHER VIEW", 4, 2, 120, 18);
        lv_obj_set_style_bg_color(banner, lv_color_hex(0x04070C), 0);
        lv_obj_set_style_bg_opa(banner, LV_OPA_60, 0);
        lv_obj_set_style_text_color(banner, lv_color_hex(COLOR_CYAN), 0);
    }
    reader_refresh_text(app);
    reader_refresh_hint(app);
}

// ---------------------------------------------------------------- 列表
static int clamp_top(int sel, int count)
{
    int top = sel - PQ_LIST_ROWS / 2;
    if (top > count - PQ_LIST_ROWS) {
        top = count - PQ_LIST_ROWS;
    }
    if (top < 0) {
        top = 0;
    }
    return top;
}

static void list_begin(gal_app_t *app, const char *title, int count, int sel, int *top_out)
{
    clear_root(app);
    app->header = mk_label(app->root, title, 12, 4, SCR_W - 24, 20);
    lv_obj_set_style_text_color(app->header, lv_color_hex(COLOR_CYAN), 0);
    int top = clamp_top(sel, count);
    if (top_out) {
        *top_out = top;
    }
    app->list_top = top;
    int shown = count - top;
    if (shown > PQ_LIST_ROWS) {
        shown = PQ_LIST_ROWS;
    }
    for (int i = 0; i < shown; i++) {
        app->rows[i] = mk_row(app->root, i);
        app->row_labels[i] = lv_obj_get_child(app->rows[i], 0);
    }
    if (count == 0) {
        mk_label(app->root, "（没有可显示的条目）", 16, LIST_TOP + 4, SCR_W - 32, 20);
    }
    lv_obj_t *hint = mk_label(app->root,
                              app->notice_ms ? app->notice
                                             : "UP/DOWN 选择 · OK 确认 · 长按 OK 返回",
                              12, SCR_H - 16, SCR_W - 24, 18);
    lv_obj_set_style_text_color(hint, lv_color_hex(app->notice_ms ? COLOR_CYAN : COLOR_DIM),
                                0);
}

static void list_row_text(gal_app_t *app, int i, int index, int sel, const char *text)
{
    if (i < 0 || i >= PQ_LIST_ROWS || !app->rows[i]) {
        return;
    }
    row_set(app->rows[i], text, index == sel);
}

static void slot_label(const pq_pack_t *pack, int slot, int index, char *out, size_t cap)
{
    char name[24];
    if (slot == -1) {
        snprintf(name, sizeof(name), "自动存档");
    } else {
        snprintf(name, sizeof(name), "存档 %02d", slot + 1);
    }
    pq_save_info_t info;
    if (!pq_game_slot_peek(index, pack, &info)) {
        snprintf(out, cap, "%s · 空", name);
        return;
    }
    if (!info.valid) {
        snprintf(out, cap, "%s · 存档无法读取", name);
        return;
    }
    const char *label = pq_tag_label(pack, info.tag);
    char preview[40];
    copy_utf8(preview, sizeof(preview), info.preview);
    if (info.mode == 1) {
        snprintf(out, cap, "%s · %s · 待选", name, label);
    } else {
        snprintf(out, cap, "%s · %.14s · %.10s", name, label, preview);
    }
}

static int title_bg_unlocked(gal_app_t *app, int index)
{
    if (index == 1) {
        int32_t v = 0;
        return s_var_rin >= 0 && pq_game_global_var(&app->game, (uint16_t)s_var_rin, &v) && v;
    }
    if (index == 2) {
        int32_t v = 0;
        return s_var_tsu >= 0 && pq_game_global_var(&app->game, (uint16_t)s_var_tsu, &v) && v;
    }
    return 1;
}

static int resolve_title_bg(gal_app_t *app)
{
    int idx = app->settings.title_bg;
    if (idx < 0 || idx > 2 || !title_bg_unlocked(app, idx)) {
        idx = 0;
    }
    return idx;
}

static void title_art(gal_app_t *app)
{
    int idx = resolve_title_bg(app);
    const pq_title_t *art = pq_title(&app->game.pack, (uint8_t)idx);
    pq_gfx_clear(0x0000);
    if (art) {
        // 标题背景 320x150 == 画布尺寸：cover 后正好 1:1 铺满、不裁。
        // （pack 里的 title 矩形按 320x240 布局给的是 158 的 y，画布只有 150 高，
        //   照抄会画到画布外，所以这里按画布矩形做 cover。）
        (void)pq_gfx_draw_jpeg_cover(&app->game.pack, art->image, 0, 0, ART_W, ART_H);
    }
    const pq_title_t *logo = pq_title(&app->game.pack, 3);
    if (logo) {
        (void)pq_gfx_draw_image(&app->game.pack, logo->image, 10, 8);
    }
    invalidate_art(app);
}

static int title_item_count(gal_app_t *app)
{
    int32_t v = 0;
    bool after = s_var_main >= 0 && pq_game_global_var(&app->game, (uint16_t)s_var_main, &v) && v;
    return after ? 7 : 6;
}

static const char *title_item_label(int i, bool after)
{
    if (after) {
        return (i >= 0 && i < 7) ? s_title_items[i] : "";
    }
    return (i >= 0 && i + 1 < 7) ? s_title_items[i + 1] : "";
}

// ---------------------------------------------------------------- 页面
static void build_title(gal_app_t *app);
static void build_reader(gal_app_t *app);
static void build_choice(gal_app_t *app);
static void build_menu(gal_app_t *app);
static void build_slots(gal_app_t *app, bool save);
static void build_history(gal_app_t *app);
static void build_settings(gal_app_t *app);
static void build_gallery(gal_app_t *app);
static void build_cg(gal_app_t *app);
static void build_routes(gal_app_t *app);
static void build_chapters(gal_app_t *app);
static void build_ending(gal_app_t *app);
static void build_about(gal_app_t *app);
static void build_overwrite(gal_app_t *app);
static void build_error(gal_app_t *app);

static void rebuild_current_page(gal_app_t *app)
{
    switch (app->page) {
    case PQ_PAGE_TITLE:
        build_title(app);
        break;
    case PQ_PAGE_READER:
        build_reader(app);
        break;
    case PQ_PAGE_CHOICE:
        build_choice(app);
        break;
    case PQ_PAGE_MENU:
        build_menu(app);
        break;
    case PQ_PAGE_SAVE:
        build_slots(app, true);
        break;
    case PQ_PAGE_LOAD:
        build_slots(app, false);
        break;
    case PQ_PAGE_HISTORY:
        build_history(app);
        break;
    case PQ_PAGE_SETTINGS:
        build_settings(app);
        break;
    case PQ_PAGE_GALLERY:
        build_gallery(app);
        break;
    case PQ_PAGE_CG:
        build_cg(app);
        break;
    case PQ_PAGE_ROUTES:
        build_routes(app);
        break;
    case PQ_PAGE_CHAPTERS:
        build_chapters(app);
        break;
    case PQ_PAGE_ENDING:
        build_ending(app);
        break;
    case PQ_PAGE_ABOUT:
        build_about(app);
        break;
    case PQ_PAGE_OVERWRITE:
        build_overwrite(app);
        break;
    case PQ_PAGE_ERROR:
        build_error(app);
        break;
    default:
        break;
    }
}

static void goto_page(gal_app_t *app, pq_page_t page)
{
    if (page != app->page) {
        if (app->page_depth < (int)(sizeof(app->page_stack) / sizeof(app->page_stack[0]))) {
            app->page_stack[app->page_depth++] = app->page;
        }
    }
    app->page = page;
    rebuild_current_page(app);
}

static void go_back(gal_app_t *app)
{
    if (app->page_depth > 0) {
        app->page = app->page_stack[--app->page_depth];
    } else {
        app->page = PQ_PAGE_TITLE;
        app->title_sel = 0;
    }
    rebuild_current_page(app);
}

// ---------------------------------------------------------------- 正文推进
static void apply_visual(gal_app_t *app, uint16_t visual_index)
{
    app->visual_fp = visual_index;
    if (visual_index < app->game.pack.visual_count) {
        const pq_visual_t *v = &app->game.pack.visuals[visual_index];
        app->bg = v->bg;
        app->bodies[0] = v->body[0];
        app->bodies[1] = v->body[1];
        app->bodies[2] = v->body[2];
        app->cutin = v->cutin;
        app->view = v->view;
        app->cutin_opacity = v->opacity;
        return;
    }
    app->bg = PQ_NO_IMAGE;
    app->bodies[0] = app->bodies[1] = app->bodies[2] = PQ_NO_IMAGE;
    app->cutin = PQ_NO_IMAGE;
    app->view = PQ_VIEW_NORMAL;
    app->cutin_opacity = 0;
}

static bool has_reading_checkpoint(const gal_app_t *app)
{
    return app->game.mode == 1 || app->game.text_op != 0xFFFFFFFF;
}

// 自动档：正文/待选的有效阅读点，无论当前停在哪个子页都保存同一句/同一页/同一待选。
static bool save_auto_checkpoint(gal_app_t *app)
{
    if (!has_reading_checkpoint(app)) {
        return false;
    }
    uint8_t page = app->game.mode == 1 ? 0 : (uint8_t)app->page_index;
    const bool ok = pq_game_save(&app->game, PQ_SLOT_AUTO, page);
    app->auto_dirty = !ok;
    if (!ok) {
        ESP_LOGW(TAG, "自动档写入失败（2s 后重试）");
    }
    return ok;
}

static void reader_enter_page(gal_app_t *app)
{
    app->typed = 0;
    app->type_accum_ms = 0;
    // 快进时整页瞬显（自动阅读仍逐字）。
    app->typing = app->settings.speed != 3 && !app->fast_forward;
    if (app->fast_forward) {
        app->typed = s_page_len[app->page_index];
    }
    app->auto_accum_ms = 0;
    reader_refresh_text(app);
    reader_refresh_hint(app);
}

static void start_chapter(gal_app_t *app, uint16_t scene);
static bool auto_next_chapter(gal_app_t *app);

static void load_step(gal_app_t *app)
{
    pq_step_t step;
    for (int guard = 0; guard < 8; guard++) {
        if (!pq_game_step(&app->game, &step)) {
            snprintf(app->error_text, sizeof(app->error_text), "%s",
                     step.error ? step.error : "剧情执行失败");
            app->page = PQ_PAGE_ERROR;
            rebuild_current_page(app);
            return;
        }
        if (step.kind == PQ_STEP_TEXT) {
            copy_utf8(app->text, sizeof(app->text), step.text ? step.text : "");
            app->page_count = paginate(app->text, cjk_width(app), s_page_start, s_page_len, 8);
            app->page_index = 0;
            app->speaker = step.speaker;
            app->tag = step.tag;
            app->shown_op = step.op;
            const uint16_t visual_before = app->visual_fp;
            apply_visual(app, step.visual);
            app->page = PQ_PAGE_READER;
            app->page_index = 0;
            // 快进时进页就整页显示（不启动打字机）。
            app->typing = (app->settings.speed != 3) && !app->fast_forward;
            app->typed = app->fast_forward ? s_page_len[0] : 0;
            // 快进/同画面连续若干句：对象不重建、画面不重解码，只换文本。
            const bool same_visual = (visual_before == app->visual_fp) && app->reader_built;
            if (!same_visual) {
                build_reader_objects(app);
                (void)draw_scene(app);
            }
            if (app->game.scene != app->auto_scene) {
                app->auto_scene = app->game.scene;
                (void)save_auto_checkpoint(app);
            }
            reader_enter_page(app);
            return;
        }
        if (step.kind == PQ_STEP_CHOICE) {
            apply_visual(app, app->game.current_visual);
            app->page = PQ_PAGE_CHOICE;
            app->auto_paused = true;
            app->fast_forward = false;
            build_choice(app);   // 保留待选光标
            return;
        }
        if (step.kind == PQ_STEP_END) {
            // 章节播完先自动接下一章（同一路线）；只有最后一章才进结局页。
            if (auto_next_chapter(app)) {
                return;
            }
            app->ending_index = (uint16_t)(pq_ending_index(&app->game.pack, app->game.scene));
            if (app->ending_index > 0xFF) {
                app->ending_index = 0;
            }
            // 后日谈通关后自动切到对应标题背景（源 0.1.5 语义）
            const pq_scene_t *scene = pq_scene_by_id(&app->game.pack, app->game.scene);
            if (app->game.story && scene) {
                if (s_var_rin >= 0 && scene->clear_var == s_var_rin) {
                    app->settings.title_bg = 1;
                    (void)pq_settings_save(&app->settings);
                } else if (s_var_tsu >= 0 && scene->clear_var == s_var_tsu) {
                    app->settings.title_bg = 2;
                    (void)pq_settings_save(&app->settings);
                }
            }
            app->auto_mode = false;
            app->fast_forward = false;
            app->page = PQ_PAGE_ENDING;
            (void)pq_game_save_meta(&app->game);
            rebuild_current_page(app);
            return;
        }
        if (step.kind == PQ_STEP_ERROR) {
            snprintf(app->error_text, sizeof(app->error_text), "%s",
                     step.error ? step.error : "剧情执行失败");
            app->page = PQ_PAGE_ERROR;
            rebuild_current_page(app);
            return;
        }
    }
    snprintf(app->error_text, sizeof(app->error_text), "跳转次数过多");
    app->page = PQ_PAGE_ERROR;
    rebuild_current_page(app);
}

static void advance_reader(gal_app_t *app)
{
    if (app->typing) {
        app->typed = s_page_len[app->page_index];
        app->typing = false;
        app->type_accum_ms = 0;
        reader_refresh_text(app);
        app->auto_accum_ms = 0;
        return;
    }
    if (app->page_index + 1 < app->page_count) {
        app->page_index++;
        reader_enter_page(app);
        return;
    }
    load_step(app);
}

static void show_title(gal_app_t *app)
{
    if (app->game.meta_dirty) {
        pq_game_flush_meta(&app->game);
    }
    (void)save_auto_checkpoint(app);
    pq_game_flush_meta(&app->game);
    app->page_depth = 0;
    app->page = PQ_PAGE_TITLE;
    app->title_sel = 0;
    app->auto_mode = false;
    app->fast_forward = false;
    app->auto_scene = 0xFFFF;
    rebuild_current_page(app);
}

// ---------------------------------------------------------------- 构建：标题
static void build_title(gal_app_t *app)
{
    clear_root(app);
    attach_art(app);
    title_art(app);

    int count = title_item_count(app);
    int32_t v = 0;
    bool after = s_var_main >= 0 && pq_game_global_var(&app->game, (uint16_t)s_var_main, &v) && v;
    if (app->title_sel >= count) {
        app->title_sel = count - 1;
    }
    for (int i = 0; i < count; i++) {
        int col = i % 2;
        int row = i / 2;
        lv_obj_t *cell = lv_obj_create(app->root);
        lv_obj_set_size(cell, 154, 20);
        lv_obj_set_pos(cell, col == 0 ? 6 : 162, BOX_Y + 2 + row * 21);
        set_bg(cell, i == app->title_sel ? COLOR_ROW_SEL : COLOR_ROW, LV_OPA_COVER);
        lv_obj_set_style_border_width(cell, 1, 0);
        lv_obj_set_style_border_color(cell, lv_color_hex(i == app->title_sel ? COLOR_ACCENT
                                                                          : 0x162A34), 0);
        lv_obj_set_scrollbar_mode(cell, LV_SCROLLBAR_MODE_OFF);
        lv_obj_t *lbl = mk_label(cell, title_item_label(i, after), 7, 1, 140, 18);
        lv_obj_set_style_text_color(lbl, lv_color_hex(i == app->title_sel ? 0xFFFFFF
                                                                         : 0xBFD2D9), 0);
        if (i >= PQ_LIST_ROWS) {
            continue;
        }
        app->rows[i] = cell;
        app->row_labels[i] = lbl;
    }
    const char *hint = app->notice_ms ? app->notice : "UP/DOWN · OK";
    lv_obj_t *h = app->notice_ms ? mk_label(app->root, hint, 8, SCR_H - 16, SCR_W - 16, 16)
                                 : mk_label(app->root, hint, 168, 128, 146, 18);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(h, lv_color_hex(COLOR_DIM), 0);
}

// ---------------------------------------------------------------- 构建：正文
static void build_reader(gal_app_t *app)
{
    build_reader_objects(app);
    (void)draw_scene(app);
    reader_refresh_text(app);
    reader_refresh_hint(app);
}

// ---------------------------------------------------------------- 构建：选项
static void build_choice(gal_app_t *app)
{
    build_reader_objects(app);
    (void)draw_scene(app);
    int count = pq_game_choice_count(&app->game);
    if (app->game.choice_sel >= count) {
        app->game.choice_sel = 0;
    }
    // 覆盖对话框区：选项列表放在下半屏
    if (app->box) {
        lv_obj_del(app->box);
        app->box = NULL;
    }
    lv_obj_t *card = lv_obj_create(app->root);
    lv_obj_set_size(card, SCR_W, BOX_H);
    lv_obj_set_pos(card, 0, BOX_Y);
    set_bg(card, COLOR_PANEL, LV_OPA_COVER);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);
    lv_obj_t *title = mk_label(card, "选择下一步", 8, 2, 200, 18);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_CYAN), 0);
    for (int i = 0; i < count && i < PQ_LIST_ROWS; i++) {
        const pq_choice_item_t *item = pq_game_choice_item(&app->game, i);
        char text[96] = "?";
        if (item) {
            copy_utf8(text, sizeof(text),
                      pq_text_get(&app->game.pack, item->text_off, item->text_len));
        }
        lv_obj_t *row = lv_obj_create(card);
        lv_obj_set_size(row, SCR_W - 16, 26);
        lv_obj_set_pos(row, 8, 22 + i * 30);
        set_bg(row, i == app->game.choice_sel ? COLOR_ROW_SEL : COLOR_ROW, LV_OPA_COVER);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(
            row, lv_color_hex(i == app->game.choice_sel ? COLOR_ACCENT : 0x1E3A46), 0);
        lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
        lv_obj_t *lbl = mk_label_clip(row, text, 8, 4, SCR_W - 40, 20);
        lv_obj_set_style_text_color(lbl, lv_color_hex(i == app->game.choice_sel ? 0xFFFFFF
                                                                                : 0xC7D8DE), 0);
        app->rows[i] = row;
        app->row_labels[i] = lbl;
    }
    lv_obj_t *hint = mk_label(card, "OK 确认 · 长按 OK 菜单", 8, BOX_H - 18, SCR_W - 16, 16);
    lv_obj_set_style_text_color(hint, lv_color_hex(COLOR_DIM), 0);
}

// ---------------------------------------------------------------- 构建：菜单
static void build_menu(gal_app_t *app)
{
    int top = 0;
    list_begin(app, "菜单", s_game_menu_count, app->menu_sel, &top);
    app->list_top = top;
    for (int i = 0; i < PQ_LIST_ROWS; i++) {
        int idx = top + i;
        if (idx >= s_game_menu_count) {
            break;
        }
        list_row_text(app, i, idx, app->menu_sel, s_game_menu[idx]);
    }
}

static void build_slots(gal_app_t *app, bool save)
{
    int count = save ? PQ_SLOT_COUNT : PQ_SLOT_COUNT + 1;   // 读取页首行是自动档
    int *sel = save ? &app->save_sel : &app->load_sel;
    if (*sel >= count) {
        *sel = count - 1;
    }
    int top = 0;
    list_begin(app, save ? "保存进度" : "读取存档", count, *sel, &top);
    for (int i = 0; i < PQ_LIST_ROWS; i++) {
        int idx = top + i;
        if (idx >= count) {
            break;
        }
        char text[96];
        if (save) {
            slot_label(&app->game.pack, idx, idx, text, sizeof(text));
        } else if (idx == 0) {
            slot_label(&app->game.pack, -1, PQ_SLOT_AUTO, text, sizeof(text));
        } else {
            slot_label(&app->game.pack, idx - 1, idx - 1, text, sizeof(text));
        }
        list_row_text(app, i, idx, *sel, text);
        pq_save_info_t info;
        bool exists = save ? pq_game_slot_peek(idx, &app->game.pack, &info)
                           : pq_game_slot_peek(idx == 0 ? PQ_SLOT_AUTO : idx - 1,
                                               &app->game.pack, &info);
        if (!exists && app->rows[i] && app->row_labels[i]) {
            lv_obj_set_style_text_color(app->row_labels[i], lv_color_hex(COLOR_DIM), 0);
        }
    }
}

static void build_history(gal_app_t *app)
{
    int count = pq_game_history_count(&app->game);
    if (app->list_sel >= count) {
        app->list_sel = count ? count - 1 : 0;
    }
    int top = 0;
    list_begin(app, "历史文本", count, app->list_sel, &top);
    for (int i = 0; i < PQ_LIST_ROWS; i++) {
        int idx = top + i;
        if (idx >= count) {
            break;
        }
        const pq_history_t *h = pq_game_history_at(&app->game, idx);
        if (!h) {
            continue;
        }
        char text[128];
        snprintf(text, sizeof(text), "%s%s%s", pq_speaker(&app->game.pack, h->speaker),
                 h->speaker == 0xFFFF ? "" : "：", h->text);
        list_row_text(app, i, idx, app->list_sel, text);
    }
}

static void build_settings(gal_app_t *app)
{
    int top = 0;
    list_begin(app, "设置", s_settings_count, app->settings_sel, &top);
    for (int i = 0; i < PQ_LIST_ROWS; i++) {
        int idx = top + i;
        if (idx >= s_settings_count) {
            break;
        }
        char text[64];
        if (idx == 0) {
            snprintf(text, sizeof(text), "文字速度 · %s", s_speed_names[app->settings.speed]);
        } else if (idx == 1) {
            snprintf(text, sizeof(text), "自动速度 · %s",
                     s_auto_names[app->settings.auto_speed]);
        } else if (idx == 2) {
            snprintf(text, sizeof(text), "字号 · %s", app->settings.font ? "大 20px"
                                                                        : "标准 16px");
        } else if (idx >= 3 && idx <= 5) {
            int bg = idx - 3;
            snprintf(text, sizeof(text), "%s%s", s_settings_names[idx],
                     title_bg_unlocked(app, bg) ? "" : " · 未解锁");
        } else {
            snprintf(text, sizeof(text), "%s", s_settings_names[idx]);
        }
        list_row_text(app, i, idx, app->settings_sel, text);
        if (idx >= 3 && idx <= 5 && !title_bg_unlocked(app, idx - 3) && app->row_labels[i]) {
            lv_obj_set_style_text_color(app->row_labels[i], lv_color_hex(COLOR_LOCK), 0);
        }
    }
}

static void build_gallery(gal_app_t *app)
{
    int count = (int)app->game.pack.gallery_group_count;
    if (app->gallery_sel >= count) {
        app->gallery_sel = count - 1;
    }
    int top = 0;
    list_begin(app, "CG 鉴赏", count, app->gallery_sel, &top);
    for (int i = 0; i < PQ_LIST_ROWS; i++) {
        int idx = top + i;
        if (idx >= count) {
            break;
        }
        bool unlocked = pq_game_cg_unlocked(&app->game, (uint16_t)idx);
        char text[64];
        snprintf(text, sizeof(text), "%s %s · %u 张", unlocked ? "◆" : "◇",
                 pq_gallery_label(&app->game.pack, (uint16_t)idx),
                 (unsigned)pq_gallery_count(&app->game.pack, (uint16_t)idx));
        list_row_text(app, i, idx, app->gallery_sel, text);
        if (!unlocked && app->row_labels[i]) {
            lv_obj_set_style_text_color(app->row_labels[i], lv_color_hex(COLOR_LOCK), 0);
        }
    }
}

static void build_cg(gal_app_t *app)
{
    clear_root(app);
    attach_art(app);
    int count = (int)pq_gallery_count(&app->game.pack, (uint16_t)app->cg_group);
    if (app->cg_member >= count) {
        app->cg_member = count ? count - 1 : 0;
    }
    uint16_t asset = pq_gallery_member(&app->game.pack, (uint16_t)app->cg_group,
                                       (uint16_t)app->cg_member);
    uint16_t image = pq_asset_image(&app->game.pack, asset);
    const pq_img_entry_t *e = pq_img_entry(&app->game.pack, image);
    pq_gfx_clear(0x0000);
    bool ok = false;
    if (e) {
        // cover：等比放大到宽高都盖住 320x150，多的部分居中裁（不非等比拉伸）。
        // 画布有 240 高时铺满整屏（CG 页不再留 90px 空条）；内存不够回落到 150 时
        // 只铺满画面区，底部仍由对话框/留白承担。
        ok = pq_gfx_draw_jpeg_cover(&app->game.pack, image, 0, 0, ART_W,
                                    pq_gfx_canvas_height());
    }
    app->draw_error = !ok;
    invalidate_art(app);   // 双带：两条都要重新取像素
    char caption[96];
    snprintf(caption, sizeof(caption), "%s · %d/%d%s",
             pq_gallery_label(&app->game.pack, (uint16_t)app->cg_group), app->cg_member + 1,
             count, count > 1 ? " · UP/DOWN 切换差分" : "");
    lv_obj_t *cap = mk_label(app->root, ok ? caption : "素材读取失败", 8, BOX_Y + 6,
                             SCR_W - 16, 20);
    lv_obj_set_style_text_color(cap, lv_color_hex(ok ? COLOR_TEXT : 0xFF8080), 0);
    const char *label = pq_gallery_label(&app->game.pack, (uint16_t)app->cg_group);
    lv_obj_t *name = mk_label(app->root, label, 8, BOX_Y + 34, SCR_W - 16, 20);
    lv_obj_set_style_text_color(name, lv_color_hex(COLOR_CYAN), 0);
    lv_obj_t *hint = mk_label(app->root, "OK 返回列表 · 长按 OK 返回", 8, SCR_H - 18,
                              SCR_W - 16, 16);
    lv_obj_set_style_text_color(hint, lv_color_hex(COLOR_DIM), 0);
    if (ok) {
        pq_game_mark_cg(&app->game, asset);
    }
}

static void build_routes(gal_app_t *app)
{
    int count = (int)app->game.pack.route_count;
    if (app->route_sel >= count) {
        app->route_sel = count - 1;
    }
    int top = 0;
    list_begin(app, "章节选择 · 路线", count, app->route_sel, &top);
    for (int i = 0; i < PQ_LIST_ROWS; i++) {
        int idx = top + i;
        if (idx >= count) {
            break;
        }
        char text[80];
        snprintf(text, sizeof(text), "%s  (%u 章)", pq_route_name(&app->game.pack,
                                                                  (uint16_t)idx),
                 (unsigned)pq_route_chapter_count(&app->game.pack, (uint16_t)idx));
        list_row_text(app, i, idx, app->route_sel, text);
    }
}

static void build_chapters(gal_app_t *app)
{
    int route = app->route_sel;
    int first = (int)pq_route_first_chapter(&app->game.pack, (uint16_t)route);
    int count = (int)pq_route_chapter_count(&app->game.pack, (uint16_t)route);
    if (app->chapter_sel >= count) {
        app->chapter_sel = count ? count - 1 : 0;
    }
    int top = 0;
    list_begin(app, pq_route_name(&app->game.pack, (uint16_t)route), count, app->chapter_sel,
               &top);
    for (int i = 0; i < PQ_LIST_ROWS; i++) {
        int idx = top + i;
        if (idx >= count) {
            break;
        }
        list_row_text(app, i, idx, app->chapter_sel,
                      pq_chapter_label(&app->game.pack, (uint16_t)(first + idx)));
    }
}

static void build_ending(gal_app_t *app)
{
    clear_root(app);
    attach_art(app);
    pq_gfx_clear(0x0000);
    char title[64];
    snprintf(title, sizeof(title), "%s · 阅读完毕",
             pq_ending_name(&app->game.pack, app->game.scene));
    if (title[0] == ' ') {
        snprintf(title, sizeof(title), "阅读完毕");
    }
    lv_obj_t *t = mk_label(app->root, title, 12, 8, SCR_W - 24, 22);
    lv_obj_set_style_text_color(t, lv_color_hex(COLOR_CYAN), 0);
    char body[160];
    int unlocked = 0;
    for (uint16_t i = 0; i < app->game.pack.ending_name_count; i++) {
        if (pq_game_ending_seen(&app->game, i)) {
            unlocked++;
        }
    }
    snprintf(body, sizeof(body), "已解锁结局 %d / %u\n\nOK 返回标题", unlocked,
             (unsigned)app->game.pack.ending_name_count);
    lv_obj_t *b = mk_label(app->root, body, 12, 40, SCR_W - 24, 100);
    lv_obj_set_style_text_color(b, lv_color_hex(COLOR_TEXT), 0);
}

static void build_about(gal_app_t *app)
{
    clear_root(app);
    app->header = mk_label(app->root, "关于", 12, 4, SCR_W - 24, 20);
    lv_obj_set_style_text_color(app->header, lv_color_hex(COLOR_CYAN), 0);
    lv_obj_t *body = mk_label(app->root,
                              "《PARQUET》横屏阅读器\n"
                              "基于 PARQUET RW5 源快照移植\n"
                              "保留 107 个场景 / 2 处分支 / 4 个结局\n"
                              "全部 CG（37 组 367 张）与表情差分\n"
                              "\n"
                              "UP 推进 / 长按快进\n"
                              "DOWN 历史 / 长按自动\n"
                              "OK 菜单 / 长按 OK 列表返回\n"
                              "\n"
                              "本固件仅供个人学习交流，\n"
                              "原作版权归 YUZUSOFTSOUR 等权利人。",
                              12, 28, SCR_W - 24, 0);
    lv_obj_set_style_text_color(body, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_t *hint = mk_label(app->root, "长按 OK 返回", 12, SCR_H - 18, SCR_W - 24, 16);
    lv_obj_set_style_text_color(hint, lv_color_hex(COLOR_DIM), 0);
}

static void build_overwrite(gal_app_t *app)
{
    clear_root(app);
    app->header = mk_label(app->root, "覆盖存档？", 12, 4, SCR_W - 24, 20);
    lv_obj_set_style_text_color(app->header, lv_color_hex(COLOR_CYAN), 0);
    char text[96];
    slot_label(&app->game.pack, app->overwrite_slot, app->overwrite_slot, text, sizeof(text));
    lv_obj_t *detail = mk_label(app->root, text, 12, 32, SCR_W - 24, 40);
    lv_obj_set_style_text_color(detail, lv_color_hex(COLOR_TEXT), 0);
    app->rows[0] = mk_row(app->root, 0);
    app->rows[1] = mk_row(app->root, 1);
    list_row_text(app, 0, 0, app->list_sel, "确认覆盖");
    list_row_text(app, 1, 1, app->list_sel, "取消");
    lv_obj_t *hint = mk_label(app->root, "UP/DOWN 选择 · OK 确认", 12, SCR_H - 18,
                              SCR_W - 24, 16);
    lv_obj_set_style_text_color(hint, lv_color_hex(COLOR_DIM), 0);
}

static void build_error(gal_app_t *app)
{
    clear_root(app);
    lv_obj_t *t = mk_label(app->root, "暂时无法继续", 12, 20, SCR_W - 24, 24);
    lv_obj_set_style_text_color(t, lv_color_hex(0xFF9090), 0);
    lv_obj_t *b = mk_label(app->root, app->error_text, 12, 56, SCR_W - 24, 90);
    lv_obj_set_style_text_color(b, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_t *hint = mk_label(app->root, "OK 返回标题", 12, SCR_H - 18, SCR_W - 24, 16);
    lv_obj_set_style_text_color(hint, lv_color_hex(COLOR_DIM), 0);
}

// ---------------------------------------------------------------- 动作
static void start_story(gal_app_t *app, uint16_t scene)
{
    pq_game_start(&app->game, scene, true);
    app->auto_scene = 0xFFFF;
    app->auto_mode = false;
    app->fast_forward = false;
    app->auto_paused = false;
    app->page_depth = 0;
    load_step(app);
}


// 当前场景所在的章节下标（找不到返回 -1）。
static int chapter_index_of_scene(const gal_app_t *app, uint16_t scene)
{
    const pq_pack_t *pack = &app->game.pack;
    for (uint16_t i = 0; i < pack->chapter_count; i++) {
        if (pq_chapter_scene(pack, i) == scene) {
            return (int)i;
        }
    }
    return -1;
}

// 章节播完：同一路线还有下一章就自动接着播（重置章节内 cursor、保留全局
// 解锁/已读/设置，背景立绘音乐由 start_chapter 重新初始化），并把自动档
// 落到新章开头；只有最后一章才回主菜单（结局页）。
static bool auto_next_chapter(gal_app_t *app)
{
    const pq_pack_t *pack = &app->game.pack;
    const int cur = chapter_index_of_scene(app, app->game.scene);
    if (cur < 0) {
        return false;
    }
    const int next = cur + 1;
    if (next >= (int)pack->chapter_count) {
        return false;                       // 最后一章
    }
    if (pq_chapter_route(pack, (uint16_t)next) != pq_chapter_route(pack, (uint16_t)cur)) {
        return false;                       // 跨路线不自动接，交给用户选
    }
    const uint16_t scene = pq_chapter_scene(pack, (uint16_t)next);
    if (scene == 0xFFFF) {
        return false;
    }
    start_chapter(app, scene);
    // 新章开头的自动档（cursor=0），保证读档落在这里。
    (void)save_auto_checkpoint(app);
    return true;
}

static void start_chapter(gal_app_t *app, uint16_t scene)
{
    pq_game_start(&app->game, scene, false);
    app->auto_scene = 0xFFFF;
    app->auto_mode = false;
    app->fast_forward = false;
    app->auto_paused = false;
    app->page_depth = 0;
    load_step(app);
}

static void open_menu(gal_app_t *app)
{
    if (app->page != PQ_PAGE_READER && app->page != PQ_PAGE_CHOICE) {
        return;
    }
    app->menu_return = app->page;
    app->menu_sel = 0;
    app->auto_paused = true;
    app->fast_forward = false;
    if (app->typing) {
        app->typed = s_page_len[app->page_index];
        app->typing = false;
    }
    app->page = PQ_PAGE_MENU;
    rebuild_current_page(app);
}

static void save_settings(gal_app_t *app) { (void)pq_settings_save(&app->settings); }

static void do_save(gal_app_t *app, int slot)
{
    bool ok = pq_game_save(&app->game, slot, (uint8_t)app->page_index);
    snprintf(app->notice, sizeof(app->notice), "%s", ok ? "已保存" : "保存失败");
    app->notice_ms = NOTICE_MS;
    if (!ok) {
        ESP_LOGW(TAG, "保存失败（NVS 不可用或写入失败）");
    }
}

static void do_load(gal_app_t *app, int slot)
{
    uint8_t page = 0;
    if (!pq_game_load(&app->game, slot, &page)) {
        snprintf(app->notice, sizeof(app->notice), "该栏位没有可用存档");
        app->notice_ms = NOTICE_MS;
        rebuild_current_page(app);
        return;
    }
    app->auto_scene = 0xFFFF;
    app->auto_mode = false;
    app->fast_forward = false;
    app->auto_paused = false;
    app->page_depth = 0;
    pq_step_t step;
    if (!pq_game_step(&app->game, &step)) {
        snprintf(app->error_text, sizeof(app->error_text), "%s",
                 step.error ? step.error : "读档后剧情执行失败");
        app->page = PQ_PAGE_ERROR;
        rebuild_current_page(app);
        return;
    }
    if (step.kind == PQ_STEP_TEXT) {
        copy_utf8(app->text, sizeof(app->text), step.text ? step.text : "");
        app->page_count = paginate(app->text, cjk_width(app), s_page_start, s_page_len, 8);
        app->page_index = page < app->page_count ? page : 0;
        app->speaker = step.speaker;
        app->tag = step.tag;
        app->shown_op = step.op;
        apply_visual(app, step.visual);
        app->page = PQ_PAGE_READER;
        build_reader_objects(app);
        (void)draw_scene(app);
        app->typed = s_page_len[app->page_index];
        app->typing = false;
        app->type_accum_ms = 0;
        reader_refresh_text(app);
    } else if (step.kind == PQ_STEP_CHOICE) {
        apply_visual(app, app->game.current_visual);
        app->page = PQ_PAGE_CHOICE;
        app->auto_paused = true;
        build_choice(app);
    } else if (step.kind == PQ_STEP_END) {
        app->ending_index = 0;
        app->page = PQ_PAGE_ENDING;
        rebuild_current_page(app);
    } else {
        snprintf(app->error_text, sizeof(app->error_text), "读档状态无效");
        app->page = PQ_PAGE_ERROR;
        rebuild_current_page(app);
    }
    app->notice_ms = 0;
    ESP_LOGI(TAG, "读档 %d 场景 %u 页 %u", slot + 1, app->game.scene, app->page_index);
}

static void repaginate_at(gal_app_t *app, int byte_off)
{
    int cjk = cjk_width(app);
    app->page_count = paginate(app->text, cjk, s_page_start, s_page_len, 8);
    int page = 0;
    for (int i = 0; i < app->page_count; i++) {
        if (byte_off >= s_page_start[i]) {
            page = i;
        }
    }
    app->page_index = page;
    app->typed = s_page_len[page];
    app->typing = false;
}

static void toggle_auto(gal_app_t *app)
{
    if (app->page != PQ_PAGE_READER) {
        return;
    }
    app->auto_mode = !app->auto_mode;
    app->auto_paused = false;
    app->auto_accum_ms = 0;
    reader_refresh_hint(app);
}

// ---------------------------------------------------------------- 输入
static void handle_page_key(gal_app_t *app, const gal_key_t *key)
{
    const bool up = key->btn == BSP_BTN_UP;
    const bool down = key->btn == BSP_BTN_DOWN;
    const bool ok = key->btn == BSP_BTN_OK;
    const bool click = key->ev == BSP_BTN_CLICK;
    const bool longp = key->ev == BSP_BTN_LONG;
    if (!click && !longp) {
        return;
    }
    switch (app->page) {
    case PQ_PAGE_TITLE: {
        int count = title_item_count(app);
        if ((up || down) && click) {
            app->title_sel = (uint8_t)((app->title_sel + (down ? 1 : count - 1)) % count);
            rebuild_current_page(app);
            break;
        }
        if (ok && longp) {
            goto_page(app, PQ_PAGE_ABOUT);   // 长 OK = 关于/按键说明（TITLE 栈底，OK 可回）
            break;
        }
        if (!ok || !click) {
            break;
        }
        int32_t v = 0;
        bool after = s_var_main >= 0 &&
                     pq_game_global_var(&app->game, (uint16_t)s_var_main, &v) && v;
        int idx = after ? app->title_sel : app->title_sel + 1;
        switch (idx) {
        case 0:   // AFTER STORY
            start_story(app, app->game.pack.after);
            break;
        case 1: {   // START：前传完成后进正篇
            int32_t side = 0;
            bool cleared = s_var_side >= 0 &&
                           pq_game_global_var(&app->game, (uint16_t)s_var_side, &side) && side;
            start_story(app, cleared ? app->game.pack.flow : app->game.pack.side);
            break;
        }
        case 2:
            start_story(app, app->game.pack.side);
            break;
        case 3:
            app->route_sel = 0;
            goto_page(app, PQ_PAGE_ROUTES);
            break;
        case 4:
            app->load_sel = 0;
            goto_page(app, PQ_PAGE_LOAD);
            break;
        case 5:
            app->settings_sel = 0;
            goto_page(app, PQ_PAGE_SETTINGS);
            break;
        case 6:
            app->gallery_sel = 0;
            goto_page(app, PQ_PAGE_GALLERY);
            break;
        default:
            break;
        }
        break;
    }
    case PQ_PAGE_READER:
        if (ok) {
            // 短按 OK 与上键一致：推进下一句。长按 OK 才进菜单（长按后的补发
            // CLICK 已由 gal_app_key 的 suppress_click 吞掉，不会多推一句）。
            if (longp) {
                open_menu(app);
            } else if (click) {
                advance_reader(app);
            }
            break;
        }
        if (up) {
            if (longp) {
                app->fast_forward = true;
                app->ff_accum_ms = 0;
                // 快进一开就把当前页打满，不再逐字。
                app->typed = s_page_len[app->page_index];
                app->typing = false;
                app->type_accum_ms = 0;
                reader_refresh_text(app);
                reader_refresh_hint(app);
            } else if (click) {
                app->fast_forward = false;
                advance_reader(app);
            }
            break;
        }
        if (down) {
            if (longp) {
                toggle_auto(app);
            } else if (click) {
                app->list_sel = 0;
                goto_page(app, PQ_PAGE_HISTORY);
            }
        }
        break;
    case PQ_PAGE_CHOICE: {
        int count = pq_game_choice_count(&app->game);
        if (up && click && app->game.choice_sel > 0) {
            app->game.choice_sel--;
            rebuild_current_page(app);
        } else if (down && click && app->game.choice_sel + 1 < count) {
            app->game.choice_sel++;
            rebuild_current_page(app);
        } else if (ok && longp) {
            open_menu(app);
        } else if (ok && click) {
            if (pq_game_choose(&app->game, app->game.choice_sel)) {
                app->auto_paused = false;
                load_step(app);
            }
        }
        break;
    }
    case PQ_PAGE_MENU:
        if (up && click) {
            app->menu_sel = (app->menu_sel + s_game_menu_count - 1) % s_game_menu_count;
            rebuild_current_page(app);
        } else if (down && click) {
            app->menu_sel = (app->menu_sel + 1) % s_game_menu_count;
            rebuild_current_page(app);
        } else if (ok && longp) {
            app->page = app->menu_return;
            app->auto_paused = false;
            rebuild_current_page(app);
        } else if (ok && click) {
            switch (app->menu_sel) {
            case 0:
                app->page = app->menu_return;
                app->auto_paused = false;
                rebuild_current_page(app);
                break;
            case 1:
                app->save_sel = 0;
                goto_page(app, PQ_PAGE_SAVE);
                break;
            case 2:
                app->load_sel = 0;
                goto_page(app, PQ_PAGE_LOAD);
                break;
            case 3:
                app->list_sel = pq_game_history_count(&app->game) > 0
                                    ? pq_game_history_count(&app->game) - 1
                                    : 0;
                goto_page(app, PQ_PAGE_HISTORY);
                break;
            case 4:
                app->gallery_sel = 0;
                goto_page(app, PQ_PAGE_GALLERY);
                break;
            case 5:
                app->route_sel = 0;
                goto_page(app, PQ_PAGE_ROUTES);
                break;
            case 6:
                app->settings_sel = 0;
                goto_page(app, PQ_PAGE_SETTINGS);
                break;
            case 7:
                show_title(app);
                break;
            default:
                break;
            }
        }
        break;
    case PQ_PAGE_SAVE:
    case PQ_PAGE_LOAD: {
        const bool save = app->page == PQ_PAGE_SAVE;
        const int count = save ? PQ_SLOT_COUNT : PQ_SLOT_COUNT + 1;
        int *sel = save ? &app->save_sel : &app->load_sel;
        if (up && click) {
            *sel = (*sel + count - 1) % count;
            rebuild_current_page(app);
        } else if (down && click) {
            *sel = (*sel + 1) % count;
            rebuild_current_page(app);
        } else if (ok && longp) {
            go_back(app);
        } else if (ok && click) {
            if (save) {
                pq_save_info_t info;
                if (pq_game_slot_peek(*sel, &app->game.pack, &info)) {
                    app->overwrite_slot = *sel;
                    app->list_sel = 1;
                    app->page = PQ_PAGE_OVERWRITE;
                    rebuild_current_page(app);
                } else {
                    do_save(app, *sel);
                    rebuild_current_page(app);
                }
            } else {
                if (*sel == 0) {
                    do_load(app, PQ_SLOT_AUTO);
                } else {
                    do_load(app, *sel - 1);
                }
            }
        }
        break;
    }
    case PQ_PAGE_OVERWRITE:
        if ((up || down) && click) {
            app->list_sel = app->list_sel ? 0 : 1;
            rebuild_current_page(app);
        } else if (ok && click) {
            if (app->list_sel == 0) {
                do_save(app, app->overwrite_slot);
                app->page = PQ_PAGE_SAVE;
                rebuild_current_page(app);
            } else {
                app->page = PQ_PAGE_SAVE;
                rebuild_current_page(app);
            }
        } else if (ok && longp) {
            app->page = PQ_PAGE_SAVE;
            rebuild_current_page(app);
        }
        break;
    case PQ_PAGE_HISTORY: {
        int count = pq_game_history_count(&app->game);
        if (up && click && count) {
            app->list_sel = (app->list_sel + count - 1) % count;
            rebuild_current_page(app);
        } else if (down && click && count) {
            app->list_sel = (app->list_sel + 1) % count;
            rebuild_current_page(app);
        } else if (ok && (click || longp)) {
            go_back(app);
        }
        break;
    }
    case PQ_PAGE_SETTINGS:
        if ((up || down) && click) {
            app->settings_sel = (app->settings_sel + (down ? 1 : s_settings_count - 1)) %
                                s_settings_count;
            rebuild_current_page(app);
        } else if (ok && click) {
            if (app->settings_sel == 0) {
                app->settings.speed = (uint8_t)((app->settings.speed + 1) % 4);
                save_settings(app);
            } else if (app->settings_sel == 1) {
                app->settings.auto_speed = (uint8_t)((app->settings.auto_speed + 1) % 3);
                save_settings(app);
            } else if (app->settings_sel == 2) {
                int keep = (app->page_count > 0 && app->page_index < app->page_count)
                               ? s_page_start[app->page_index]
                               : 0;
                app->settings.font = app->settings.font ? 0 : 1;
                save_settings(app);
                repaginate_at(app, keep);
                if (app->lbl_text) {
                    lv_obj_set_style_text_font(app->lbl_text, body_font(app), 0);
                    reader_refresh_text(app);
                }
                snprintf(app->notice, sizeof(app->notice), "字号：%s",
                         app->settings.font ? "大 20px" : "标准 16px");
                app->notice_ms = NOTICE_MS;
            } else if (app->settings_sel >= 3 && app->settings_sel <= 5) {
                int bg = app->settings_sel - 3;
                if (title_bg_unlocked(app, bg)) {
                    app->settings.title_bg = (uint8_t)bg;
                    save_settings(app);
                    snprintf(app->notice, sizeof(app->notice), "已切换标题背景");
                    app->notice_ms = NOTICE_MS;
                } else {
                    snprintf(app->notice, sizeof(app->notice), "完成后日谈后解锁");
                    app->notice_ms = NOTICE_MS;
                }
            } else {
                go_back(app);
                break;
            }
            rebuild_current_page(app);
        } else if (ok && longp) {
            go_back(app);
        }
        break;
    case PQ_PAGE_GALLERY:
        if (up && click) {
            int count = (int)app->game.pack.gallery_group_count;
            app->gallery_sel = (app->gallery_sel + count - 1) % count;
            rebuild_current_page(app);
        } else if (down && click) {
            int count = (int)app->game.pack.gallery_group_count;
            app->gallery_sel = (app->gallery_sel + 1) % count;
            rebuild_current_page(app);
        } else if (ok && longp) {
            go_back(app);
        } else if (ok && click) {
            if (pq_game_cg_unlocked(&app->game, (uint16_t)app->gallery_sel)) {
                app->cg_group = app->gallery_sel;
                app->cg_member = 0;
                goto_page(app, PQ_PAGE_CG);
            } else {
                snprintf(app->notice, sizeof(app->notice), "尚未解锁");
                app->notice_ms = NOTICE_MS;
                rebuild_current_page(app);
            }
        }
        break;
    case PQ_PAGE_CG: {
        int count = (int)pq_gallery_count(&app->game.pack, (uint16_t)app->cg_group);
        if (up && click && count) {
            app->cg_member = (app->cg_member + 1) % count;
            rebuild_current_page(app);
        } else if (down && click && count) {
            app->cg_member = (app->cg_member + count - 1) % count;
            rebuild_current_page(app);
        } else if (ok && (click || longp)) {
            go_back(app);
        }
        break;
    }
    case PQ_PAGE_ROUTES:
        if (app->game.pack.route_count == 0) {
            if (ok && longp) go_back(app);   // 0 路线：不取模、不进入空章节，但仍可返回
            break;
        }
        if (up && click) {
            int count = (int)app->game.pack.route_count;
            app->route_sel = (app->route_sel + count - 1) % count;
            rebuild_current_page(app);
        } else if (down && click) {
            int count = (int)app->game.pack.route_count;
            app->route_sel = (app->route_sel + 1) % count;
            rebuild_current_page(app);
        } else if (ok && longp) {
            go_back(app);
        } else if (ok && click) {
            app->chapter_sel = 0;
            goto_page(app, PQ_PAGE_CHAPTERS);
        }
        break;
    case PQ_PAGE_CHAPTERS: {
        int count = (int)pq_route_chapter_count(&app->game.pack, (uint16_t)app->route_sel);
        if (up && click && count) {
            app->chapter_sel = (app->chapter_sel + count - 1) % count;
            rebuild_current_page(app);
        } else if (down && click && count) {
            app->chapter_sel = (app->chapter_sel + 1) % count;
            rebuild_current_page(app);
        } else if (ok && longp) {
            go_back(app);
        } else if (ok && click) {
            int first = (int)pq_route_first_chapter(&app->game.pack,
                                                    (uint16_t)app->route_sel);
            uint16_t scene = pq_chapter_scene(&app->game.pack,
                                             (uint16_t)(first + app->chapter_sel));
            if (scene != 0xFFFF) {
                start_chapter(app, scene);
            }
        }
        break;
    }
    case PQ_PAGE_ENDING:
        if (ok && (click || longp)) {
            show_title(app);
        }
        break;
    case PQ_PAGE_ABOUT:
        if (ok && (click || longp)) {
            go_back(app);
        }
        break;
    case PQ_PAGE_ERROR:
        if (ok && (click || longp)) {
            show_title(app);
        }
        break;
    default:
        break;
    }
}

void gal_app_key(gal_app_t *app, const gal_key_t *key)
{
    app->idle_ms = 0;
    if (!bsp_lvgl_lock(200)) {
        return;
    }
    const bool up = key->btn == BSP_BTN_UP;
    // 松手：停止快进。
    if (key->ev == BSP_BTN_RELEASE) {
        if (up) {
            app->fast_forward = false;
            app->ff_accum_ms = 0;
            reader_refresh_hint(app);
        }
        bsp_lvgl_unlock();
        return;
    }
    // 新的按下先解除"长按后吞 CLICK"的守卫：长按自身可能补发 CLICK（要吞），
    // 但下一次独立短按的 CLICK 不能受影响。
    if (key->ev == BSP_BTN_PRESS) {
        app->suppress_click = false;
        bsp_lvgl_unlock();
        return;   // 导航只在 CLICK/LONG 上触发，PRESS 不动作
    }
    if (key->ev == BSP_BTN_CLICK && app->suppress_click) {
        app->suppress_click = false;
        bsp_lvgl_unlock();
        return;
    }
    if (key->ev == BSP_BTN_LONG) {
        app->suppress_click = true;   // 驱动可能再补一个 CLICK，吞掉
    }
    if (app->notice_ms) {
        app->notice_ms = 0;
    }
    handle_page_key(app, key);
    bsp_lvgl_unlock();
}

// ---------------------------------------------------------------- tick
static uint32_t speed_ms(const gal_app_t *app)
{
    static const uint32_t table[4] = {55, 25, 10, 0};
    return table[app->settings.speed & 3];
}

static uint32_t auto_wait_ms(const gal_app_t *app)
{
    static const uint32_t base[3] = {2600, 1500, 800};
    int chars = utf8_count(app->text);
    uint32_t extra = chars > 60 ? (uint32_t)(chars - 60) * 55u : 0;
    if (extra > 5000) {
        extra = 5000;
    }
    return base[app->settings.auto_speed % 3] + extra;
}

void gal_app_tick(gal_app_t *app, uint32_t elapsed_ms)
{
    if (app->auto_mode || app->fast_forward) app->idle_ms = 0;
    else app->idle_ms += elapsed_ms;
    const bool notice_expired = app->notice_ms != 0 && app->notice_ms <= elapsed_ms;
    if (app->notice_ms) {
        app->notice_ms = app->notice_ms > elapsed_ms ? app->notice_ms - elapsed_ms : 0;
    }
    if (app->game.meta_dirty) {
        app->meta_retry_ms += elapsed_ms;
        if (app->meta_retry_ms >= 2000) {
            app->meta_retry_ms = 0;
            pq_game_flush_meta(&app->game);
        }
    } else {
        app->meta_retry_ms = 0;
    }
    if (app->auto_dirty) {
        app->auto_retry_ms += elapsed_ms;
        if (app->auto_retry_ms >= 2000) {
            app->auto_retry_ms = 0;
            (void)save_auto_checkpoint(app);
        }
    } else {
        app->auto_retry_ms = 0;
    }
    app->battery_accum_ms += elapsed_ms;
    if (app->battery_accum_ms >= 10000) {
        app->battery_accum_ms = 0;
        app->battery_percent = bsp_battery_soc();
    }
    if (app->page != PQ_PAGE_READER) {
        // 提示到期后重建一次列表页，把页脚恢复成按键提示（提示本身在 list_begin 里）。
        if (!notice_expired) {
            return;
        }
        if (!bsp_lvgl_lock(50)) {
            return;
        }
        rebuild_current_page(app);
        bsp_lvgl_unlock();
        return;
    }
    if (!bsp_lvgl_lock(50)) {
        return;
    }
    if (app->fast_forward && app->typing) {
        // 快进期间任何残留的打字都立即补满。
        app->typed = s_page_len[app->page_index];
        app->typing = false;
        app->type_accum_ms = 0;
        reader_refresh_text(app);
    }
    if (app->typing) {
        uint32_t step = speed_ms(app);
        if (step == 0) {
            app->typed = s_page_len[app->page_index];
            app->typing = false;
            app->type_accum_ms = 0;
        } else {
            // 输入节拍 50ms 与最慢打字 55ms/字不同步：按累计时间算，保留余数，
            // 不规则短 tick 也不会丢时间或停顿。
            app->type_accum_ms += elapsed_ms;
            uint32_t chars = app->type_accum_ms / step;
            if (chars > 0) {
                app->type_accum_ms -= chars * step;
            }
            int n = s_page_len[app->page_index];
            while (chars-- > 0 && app->typed < n) {
                app->typed = utf8_next(app->text + s_page_start[app->page_index], app->typed);
            }
            if (app->typed >= n) {
                app->typed = n;
                app->typing = false;
                app->type_accum_ms = 0;
                app->auto_accum_ms = 0;
            }
        }
        reader_refresh_text(app);
    } else if (app->fast_forward) {
        // 快进：每个 tick 立刻推进，不再等 40ms 累积、也不等打字机
        // （用户反馈“快进时每句之间还有停顿”，那正是累积+打字等待造成的）。
        app->ff_accum_ms = 0;
        int stepped = 0;
        while (stepped < 4 && app->page_index + 1 < app->page_count) {
            app->page_index++;
            stepped++;
        }
        if (stepped > 0) {
            app->typed = s_page_len[app->page_index];
            app->typing = false;
            reader_refresh_text(app);
        } else {
            load_step(app);   // 换下一句/下一个事件
            // load_step 可能带来新文本：快进时立刻整页显示，不逐字。
            if (app->typing) {
                app->typed = s_page_len[app->page_index];
                app->typing = false;
                app->type_accum_ms = 0;
                reader_refresh_text(app);
            }
        }
    } else if (app->auto_mode && !app->auto_paused) {
        app->auto_accum_ms += elapsed_ms;
        if (app->auto_accum_ms >= auto_wait_ms(app)) {
            app->auto_accum_ms = 0;
            advance_reader(app);
        }
    }
    reader_refresh_hint(app);
    bsp_lvgl_unlock();
}

uint32_t gal_app_idle_ms(const gal_app_t *app)
{
    // 自动/快进期间不得调暗/熄屏/深睡。
    if (app && (app->auto_mode || app->fast_forward)) return 0;
    return app ? app->idle_ms : 0;
}

void gal_app_flush(gal_app_t *app)
{
    (void)save_auto_checkpoint(app);
    pq_game_flush_meta(&app->game);
}

void gal_app_show_sleeping(gal_app_t *app)
{
    if (!bsp_lvgl_lock(200)) {
        return;
    }
    clear_root(app);
    lv_obj_t *l = mk_label(app->root, "休眠中 · 按任意键唤醒", 16, 108, SCR_W - 32, 24);
    lv_obj_set_style_text_color(l, lv_color_hex(COLOR_CYAN), 0);
    bsp_lvgl_unlock();
    app->sleeping = true;
}

// ---------------------------------------------------------------- 测试钩子

// 测试钩子：当前页的字节长度（用于断言快进时 typed == 页长）。
int gal_app_test_page_len(const gal_app_t *app);
int gal_app_test_page_len(const gal_app_t *app)
{
    if (!app || app->page_index >= app->page_count) return 0;
    return (int)s_page_len[app->page_index];
}

#ifdef PQ_UI_TEST_HOOKS
// 测试钩子：章节自动续播。
void gal_app_test_start_chapter(gal_app_t *app, uint16_t scene);
int gal_app_test_draw_scene_count(void);
void gal_app_test_reset_draw_scene_count(void);
bool gal_app_test_draw_scene(gal_app_t *app);
void gal_app_test_start_chapter(gal_app_t *app, uint16_t scene);
bool gal_app_test_auto_next_chapter(gal_app_t *app);
void gal_app_test_start_chapter(gal_app_t *app, uint16_t scene)
{
    start_chapter(app, scene);
}
bool gal_app_test_auto_next_chapter(gal_app_t *app)
{
    return auto_next_chapter(app);
}
bool gal_app_test_draw_scene(gal_app_t *app)
{
    return draw_scene(app);
}
int gal_app_test_draw_scene_count(void) { return s_draw_scene_calls; }
void gal_app_test_reset_draw_scene_count(void) { s_draw_scene_calls = 0; }

void gal_app_test_goto_title(gal_app_t *app)
{
    if (!bsp_lvgl_lock(200)) {
        return;
    }
    show_title(app);
    bsp_lvgl_unlock();
}

bool gal_app_test_goto_choice(gal_app_t *app)
{
    if (!bsp_lvgl_lock(200)) {
        return false;
    }
    pq_game_start(&app->game, app->game.pack.after, true);
    for (int i = 0; i < 400; i++) {
        load_step(app);
        if (app->page == PQ_PAGE_CHOICE) {
            break;
        }
        if (app->page != PQ_PAGE_READER) {
            break;
        }
    }
    bool ok = app->page == PQ_PAGE_CHOICE;
    bsp_lvgl_unlock();
    return ok;
}

bool gal_app_test_advance_to_scene(gal_app_t *app, uint16_t start_scene, uint16_t target_scene)
{
    if (!bsp_lvgl_lock(200)) {
        return false;
    }
    pq_game_start(&app->game, start_scene, true);
    bool found = false;
    for (int i = 0; i < 3000; i++) {
        load_step(app);
        if (app->game.scene == target_scene) {
            found = true;
            break;
        }
        if (app->page == PQ_PAGE_CHOICE) {
            if (!pq_game_choose(&app->game, 0)) {
                break;
            }
        } else if (app->page == PQ_PAGE_ENDING ||
                   app->page == PQ_PAGE_ERROR) {
            break;
        }
    }
    bsp_lvgl_unlock();
    return found;
}

bool gal_app_test_reload_line(gal_app_t *app)
{
    if (!bsp_lvgl_lock(200)) {
        return false;
    }
    app->typing = true;
    app->typed = 0;
    app->type_accum_ms = 0;
    app->page_index = 0;
    reader_refresh_text(app);
    bsp_lvgl_unlock();
    return true;
}

bool gal_app_test_render_scene(gal_app_t *app, uint16_t bg, uint16_t a, uint16_t b, uint16_t c,
                               uint16_t cutin)
{
    if (!bsp_lvgl_lock(200)) {
        return false;
    }
    app->bg = bg;
    app->bodies[0] = a;
    app->bodies[1] = b;
    app->bodies[2] = c;
    app->cutin = cutin;
    app->cutin_opacity = cutin == PQ_NO_IMAGE ? 0 : 200;
    app->view = 0;
    app->page = PQ_PAGE_READER;
    build_reader_objects(app);
    bool ok = draw_scene(app);
    bsp_lvgl_unlock();
    return ok;
}

void gal_app_test_set_page(gal_app_t *app, pq_page_t page)
{
    if (!bsp_lvgl_lock(200)) {
        return;
    }
    app->page = page;
    rebuild_current_page(app);
    bsp_lvgl_unlock();
}
#endif

// ---------------------------------------------------------------- init
bool gal_app_init(gal_app_t *app)
{
    memset(app, 0, sizeof(*app));
    app->auto_scene = 0xFFFF;
    app->bg = PQ_NO_IMAGE;
    app->bodies[0] = app->bodies[1] = app->bodies[2] = PQ_NO_IMAGE;
    app->cutin = PQ_NO_IMAGE;
    app->battery_percent = -1;
    if (!pq_game_init(&app->game)) {
        return false;
    }
    (void)pq_settings_load(&app->settings);
    s_var_rin = pq_var_find(&app->game.pack, "sf.parquet_after_rin_clear");
    s_var_tsu = pq_var_find(&app->game.pack, "sf.parquet_after_tsu_clear");
    s_var_main = pq_var_find(&app->game.pack, "sf.parquet_main_clear");
    s_var_side = pq_var_find(&app->game.pack, "sf.parquet_side_clear");

    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "LVGL 锁超时");
        return false;
    }
    bsp_lvgl_set_landscape(true);
    bsp_lvgl_unlock();

    if (!pq_gfx_init()) {
        return false;
    }
    if (!bsp_lvgl_lock(1000)) {
        return false;
    }
    app->page = PQ_PAGE_TITLE;
    rebuild_current_page(app);
    bsp_lvgl_unlock();

    size_t canvas = 0, scratch = 0;
    pq_gfx_stats(&canvas, &scratch);
    ESP_LOGI(TAG, "就绪: 场景 %u / op %u / 图 %u / 立绘 %u / 鉴赏 %u 组 | 画布 %u B, 工作区 %u B, 空闲堆 %u B",
             (unsigned)app->game.pack.scene_count, (unsigned)app->game.pack.op_count,
             (unsigned)app->game.pack.img_entry_count,
             (unsigned)app->game.pack.sprite_count,
             (unsigned)app->game.pack.gallery_group_count, (unsigned)canvas,
             (unsigned)scratch,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
    return true;
}
