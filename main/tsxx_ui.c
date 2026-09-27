// main/tsxx_ui.c —— LVGL 界面实现。
//
// 配色是这套阅读器自己的:深夜蓝底 + 香槟金强调 + 白字,
// 刻意避开基线 demo 的浅蓝/云朵视觉,也不复用 ATRI 阅读器的深蓝天蓝主题。
#include "tsxx_ui.h"

#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "tsxx_ui";

#define COL_BG lv_color_hex(0x12172B)        // 深夜蓝:底色/正文带
#define COL_GOLD lv_color_hex(0xF3C87B)      // 香槟金:强调
#define COL_TEXT lv_color_hex(0xFFFFFF)      // 正文白
#define COL_NARRATION lv_color_hex(0xD8DCF0) // 旁白
#define COL_DIM lv_color_hex(0x9AA3C4)       // 次要文字(提示/取值)
#define COL_ROW lv_color_hex(0x1B2240)       // 列表行底
#define COL_ROW_EDGE lv_color_hex(0x39406A)  // 列表行描边

// 列表页几何:页眉 8..28,金线 34..35,行从 48 开始,行高 26、间距 6。
#define LIST_HEADER_Y 8
#define LIST_RULE_Y 34
#define LIST_TOP 48
#define ROW_H 26
#define ROW_PITCH 32
#define ROW_X 12
#define ROW_W (TSXX_UI_W - 2 * ROW_X)
#define LIST_HINT_Y (TSXX_UI_H - 32)
// 标题页:菜单压在背景画上,所以从 84 开始,字号和行高与列表页一致。
#define TITLE_TOP 84

static lv_obj_t *new_box(lv_obj_t *parent, int x, int y, int w, int h, lv_color_t bg,
                         lv_opa_t opa)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_style_bg_color(obj, bg, 0);
    lv_obj_set_style_bg_opa(obj, opa, 0);
    lv_obj_set_scrollable(obj, false);
    return obj;
}

static lv_obj_t *new_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color,
                           const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_label_set_text(label, text ? text : "");
    return label;
}

static void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (!obj) return;
    lv_obj_set_hidden(obj, hidden);
}

static void box_set_color(lv_obj_t *obj, lv_color_t bg, lv_opa_t opa, lv_color_t edge)
{
    lv_obj_set_style_bg_color(obj, bg, 0);
    lv_obj_set_style_bg_opa(obj, opa, 0);
    lv_obj_set_style_border_color(obj, edge, 0);
}

// ---------------------------------------------------------------- 列表
static void row_apply_style(tsxx_list_t *list, int index)
{
    if (index < 0 || index >= list->count) return;
    const bool on = (index == list->selected);
    box_set_color(list->rows[index], on ? COL_GOLD : COL_ROW, on ? LV_OPA_COVER : LV_OPA_80,
                  on ? COL_GOLD : COL_ROW_EDGE);
    lv_obj_set_style_text_color(list->labels[index], on ? COL_BG : COL_TEXT, 0);
    lv_obj_set_style_text_color(list->values[index], on ? COL_BG : COL_DIM, 0);
}

static void list_move_cursor(tsxx_list_t *list, int selected)
{
    if (selected < 0) selected = 0;
    if (selected >= list->count) selected = list->count > 0 ? list->count - 1 : 0;
    const int previous = list->selected;
    list->selected = selected;
    // 只改两行的样式:整表重刷在 C3 上是能省则省的绘制量。
    row_apply_style(list, previous);
    row_apply_style(list, selected);
}

static void list_build(tsxx_ui_t *ui, tsxx_list_t *list, lv_obj_t *page, lv_obj_t *header_parent,
                       int top)
{
    list->page = page;
    list->header = new_label(header_parent, ui->font_cjk, COL_GOLD, "");
    lv_obj_set_pos(list->header, ROW_X, LIST_HEADER_Y);
    list->rule = new_box(header_parent, ROW_X, LIST_RULE_Y, ROW_W, 2, COL_GOLD, LV_OPA_70);
    list->counter = new_label(header_parent, ui->font_num, COL_DIM, "");
    lv_obj_align(list->counter, LV_ALIGN_TOP_RIGHT, -8, LIST_HEADER_Y + 3);
    list->hint = new_label(header_parent, ui->font_cjk, COL_DIM, "");
    lv_obj_set_pos(list->hint, ROW_X, LIST_HINT_Y);

    for (int i = 0; i < TSXX_UI_MAX_ROWS; ++i) {
        lv_obj_t *row = new_box(page, ROW_X, top + i * ROW_PITCH, ROW_W, ROW_H, COL_ROW,
                                LV_OPA_80);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_t *label = new_label(row, ui->font_cjk, COL_TEXT, "");
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 10, 0);
        lv_obj_t *value = new_label(row, ui->font_cjk, COL_DIM, "");
        lv_obj_align(value, LV_ALIGN_RIGHT_MID, -10, 0);
        list->rows[i] = row;
        list->labels[i] = label;
        list->values[i] = value;
        set_hidden(row, true);
    }
    list->count = 0;
    list->selected = 0;
}

static void list_update(tsxx_ui_t *ui, tsxx_list_t *list, const char *title, const char *hint,
                        const char *counter, const char *const *labels,
                        const char *const *values, int count, int selected)
{
    (void)ui;
    if (count > TSXX_UI_MAX_ROWS) count = TSXX_UI_MAX_ROWS;
    if (count < 0) count = 0;
    lv_label_set_text(list->header, title ? title : "");
    set_hidden(list->header, !title || !title[0]);
    set_hidden(list->rule, !title || !title[0]);
    lv_label_set_text(list->hint, hint ? hint : "");
    set_hidden(list->hint, !hint || !hint[0]);
    lv_label_set_text(list->counter, counter ? counter : "");
    set_hidden(list->counter, !counter || !counter[0]);

    list->count = count;
    list->selected = 0;
    for (int i = 0; i < TSXX_UI_MAX_ROWS; ++i) {
        if (i >= count) {
            set_hidden(list->rows[i], true);
            continue;
        }
        lv_label_set_text(list->labels[i], labels && labels[i] ? labels[i] : "");
        lv_label_set_text(list->values[i], values && values[i] ? values[i] : "");
        set_hidden(list->values[i], !values || !values[i] || !values[i][0]);
        set_hidden(list->rows[i], false);
    }
    if (selected < 0) selected = 0;
    if (selected >= count) selected = count > 0 ? count - 1 : 0;
    list->selected = selected;
    for (int i = 0; i < count; ++i) row_apply_style(list, i);
}

// ---------------------------------------------------------------- 可滚动正文页
static void scroll_build(tsxx_ui_t *ui, tsxx_scroll_t *scroll, lv_obj_t *page)
{
    scroll->page = page;
    scroll->scroll = new_box(page, 0, 0, TSXX_UI_W, LIST_HINT_Y - 16, COL_BG, LV_OPA_TRANSP);
    lv_obj_set_scrollable(scroll->scroll, true);
    lv_obj_set_scroll_dir(scroll->scroll, LV_DIR_VER);
    lv_obj_set_style_pad_all(scroll->scroll, 0, 0);
    scroll->body = new_label(scroll->scroll, ui->font_cjk, COL_TEXT, "");
    lv_obj_set_width(scroll->body, TSXX_UI_W - 2 * ROW_X);
    lv_obj_set_pos(scroll->body, ROW_X, 12);
    lv_label_set_long_mode(scroll->body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(scroll->body, 4, 0);
    lv_obj_set_style_pad_bottom(scroll->body, 12, 0);
    scroll->hint = new_label(page, ui->font_cjk, COL_GOLD, "");
    lv_obj_set_pos(scroll->hint, ROW_X, LIST_HINT_Y);
}

static void scroll_set(tsxx_scroll_t *scroll, const char *body, const char *hint)
{
    // body 为 NULL 时表示"只换底部提示"(警告页滚动时改提示,但不要复位正文与位置)。
    if (body) {
        lv_label_set_text(scroll->body, body);
        lv_obj_scroll_to_y(scroll->scroll, 0, LV_ANIM_OFF);
    }
    lv_label_set_text(scroll->hint, hint ? hint : "");
    set_hidden(scroll->hint, !hint || !hint[0]);
}

static bool scroll_at_bottom(tsxx_scroll_t *scroll)
{
    if (!scroll->scroll) return true;
    // 刚改过文本时布局还没算,必须先跑一次布局,否则 scroll_bottom 读到旧值。
    lv_obj_update_layout(scroll->page);
    return lv_obj_get_scroll_bottom(scroll->scroll) <= 1;
}

static bool scroll_by(tsxx_scroll_t *scroll, int delta)
{
    if (!scroll->scroll) return true;
    lv_obj_scroll_by(scroll->scroll, 0, delta, LV_ANIM_OFF);
    return scroll_at_bottom(scroll);
}

// ---------------------------------------------------------------- 正文页浮层
// 正文页有三种体态:普通正文 / 选项 / 结局(或休眠)覆盖层。三者互斥。
static void game_mode_apply(tsxx_ui_t *ui)
{
    const bool game = (ui->page_current == TSXX_PAGE_GAME);
    const bool ending = game && ui->ending_visible;
    const bool choices = game && !ending && ui->choice_count > 0;
    const bool text = game && !ending && !choices;

    set_hidden(ui->band, !text);
    set_hidden(ui->band_rule, !text);
    set_hidden(ui->body, !text);
    if (!text) set_hidden(ui->name_tag, true);
    set_hidden(ui->chapter, !game || ending);
    set_hidden(ui->choice_box, !choices);
    set_hidden(ui->ending, !ending);
    set_hidden(ui->mode, !game || ending || (!ui->auto_on && !ui->fast_on));
}

// ---------------------------------------------------------------- 打字机
// 一次推进一个 UTF-8 字符(多字节序列不能拆开,否则 LVGL 会渲染出乱码方块)。
static uint32_t utf8_step(const char *text, uint32_t index)
{
    const uint8_t *p = (const uint8_t *)text;
    const uint8_t lead = p[index];
    if (lead == 0) return 0;
    if ((lead & 0xE0u) == 0xC0u) return 2;
    if ((lead & 0xF0u) == 0xE0u) return 3;
    if ((lead & 0xF8u) == 0xF0u) return 4;
    return 1;
}

static void typing_render(tsxx_ui_t *ui)
{
    memcpy(ui->typing_shown, ui->typing_target, ui->typing_index);
    ui->typing_shown[ui->typing_index] = '\0';
    lv_label_set_text(ui->body, ui->typing_shown);
}

// ---------------------------------------------------------------- 创建
static void build_game_layers(tsxx_ui_t *ui, lv_obj_t *screen)
{
    // 正文带:半透明深夜蓝,盖在放大后的画布上,顶边一条金色细线。
    ui->band = new_box(screen, 0, TSXX_BAND_Y, TSXX_UI_W, TSXX_BAND_H, COL_BG, LV_OPA_80);
    ui->band_rule = new_box(screen, 0, TSXX_BAND_Y, TSXX_UI_W, 2, COL_GOLD, LV_OPA_90);

    // 说话人名牌:挂在屏幕上而不是正文带里 —— 挂在父容器里时超出的部分会被裁掉。
    ui->name_tag = new_box(screen, 8, TSXX_TAG_Y, 60, 24, COL_GOLD, LV_OPA_COVER);
    lv_obj_set_style_radius(ui->name_tag, 5, 0);
    lv_obj_set_style_pad_hor(ui->name_tag, 8, 0);
    ui->name_text = new_label(ui->name_tag, ui->font_cjk, COL_BG, "");
    lv_obj_align(ui->name_text, LV_ALIGN_CENTER, 0, 0);
    set_hidden(ui->name_tag, true);

    ui->body = new_label(screen, ui->font_cjk, COL_TEXT, "");
    lv_obj_set_size(ui->body, TSXX_BODY_W, TSXX_TEXT_LINES * TSXX_LINE_H);
    lv_obj_set_pos(ui->body, TSXX_BODY_X, TSXX_BAND_Y + 2);
    lv_label_set_long_mode(ui->body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(ui->body, TSXX_LINE_H - 16, 0);

    // 左上角:当前章节;右上角:电量;正文带上方右侧:自动/快进状态。
    ui->chapter = new_box(screen, 6, 6, 60, 22, COL_BG, LV_OPA_70);
    lv_obj_set_style_radius(ui->chapter, 5, 0);
    lv_obj_t *chapter_text = new_label(ui->chapter, ui->font_cjk, COL_GOLD, "");
    lv_obj_align(chapter_text, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_user_data(ui->chapter, chapter_text);
    set_hidden(ui->chapter, true);

    ui->battery = new_label(screen, ui->font_num, COL_GOLD, "");
    lv_obj_align(ui->battery, LV_ALIGN_TOP_RIGHT, -8, 10);
    set_hidden(ui->battery, true);

    ui->mode = new_box(screen, 0, TSXX_TAG_Y, 52, 24, COL_BG, LV_OPA_80);
    lv_obj_set_style_radius(ui->mode, 5, 0);
    lv_obj_align(ui->mode, LV_ALIGN_TOP_RIGHT, -8, 0);
    lv_obj_set_y(ui->mode, TSXX_TAG_Y);
    lv_obj_t *mode_text = new_label(ui->mode, ui->font_cjk, COL_GOLD, "");
    lv_obj_align(mode_text, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_user_data(ui->mode, mode_text);
    set_hidden(ui->mode, true);
}

static void build_choices(tsxx_ui_t *ui, lv_obj_t *screen)
{
    ui->choice_box = new_box(screen, 0, 0, TSXX_UI_W, TSXX_UI_H, COL_BG, LV_OPA_TRANSP);
    for (int i = 0; i < TSXX_CHOICE_MAX; ++i) {
        lv_obj_t *row = new_box(ui->choice_box, 16, 24 + i * 48, TSXX_UI_W - 32, 42, COL_BG,
                                LV_OPA_90);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_border_width(row, 2, 0);
        lv_obj_set_style_border_color(row, COL_ROW_EDGE, 0);
        lv_obj_t *text = new_label(row, ui->font_cjk, COL_TEXT, "");
        lv_obj_set_width(text, TSXX_UI_W - 32 - 20);
        lv_obj_align(text, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
        ui->choice_rows[i] = row;
        ui->choice_texts[i] = text;
        set_hidden(row, true);
    }
}

static void build_pages(tsxx_ui_t *ui, lv_obj_t *screen)
{
    // 警告页(首次开机):整屏底 + 可滚动正文 + 底部提示。
    lv_obj_t *warning = new_box(screen, 0, 0, TSXX_UI_W, TSXX_UI_H, COL_BG, LV_OPA_COVER);
    scroll_build(ui, &ui->warning, warning);

    // 标题页:背景画由画布提供,这里只有文字与菜单,所以页面容器是透明的。
    lv_obj_t *title = new_box(screen, 0, 0, TSXX_UI_W, TSXX_UI_H, COL_BG, LV_OPA_TRANSP);
    ui->title_big = new_label(title, ui->font_cjk, COL_GOLD, "");
    lv_obj_set_width(ui->title_big, TSXX_UI_W - 20);
    lv_obj_set_pos(ui->title_big, 10, 16);
    lv_obj_set_style_text_align(ui->title_big, LV_TEXT_ALIGN_CENTER, 0);
    ui->title_sub = new_label(title, ui->font_cjk, COL_DIM, "");
    lv_obj_set_width(ui->title_sub, TSXX_UI_W - 20);
    lv_obj_set_pos(ui->title_sub, 10, 42);
    lv_obj_set_style_text_align(ui->title_sub, LV_TEXT_ALIGN_CENTER, 0);
    ui->title_hint = new_label(title, ui->font_cjk, COL_NARRATION, "");
    lv_obj_set_width(ui->title_hint, TSXX_UI_W - 20);
    lv_obj_set_pos(ui->title_hint, 10, TSXX_UI_H - 42);
    lv_obj_set_style_text_align(ui->title_hint, LV_TEXT_ALIGN_CENTER, 0);
    list_build(ui, &ui->lists[TSXX_LIST_TITLE], title, title, TITLE_TOP);

    // 整屏列表页:菜单 / 保存 / 读取 / 章节 / 设置。
    static const struct {
        tsxx_page_id_t page;
        tsxx_list_id_t list;
    } kLists[] = {
        { TSXX_PAGE_MENU, TSXX_LIST_MENU },   { TSXX_PAGE_SAVE, TSXX_LIST_SAVE },
        { TSXX_PAGE_LOAD, TSXX_LIST_LOAD },   { TSXX_PAGE_CHAPTERS, TSXX_LIST_CHAPTERS },
        { TSXX_PAGE_SETTINGS, TSXX_LIST_SETTINGS },
    };
    for (size_t i = 0; i < sizeof(kLists) / sizeof(kLists[0]); ++i) {
        lv_obj_t *page = new_box(screen, 0, 0, TSXX_UI_W, TSXX_UI_H, COL_BG, LV_OPA_COVER);
        list_build(ui, &ui->lists[kLists[i].list], page, page, LIST_TOP);
    }

    // 关于页:与警告页同一套可滚动正文。
    lv_obj_t *about = new_box(screen, 0, 0, TSXX_UI_W, TSXX_UI_H, COL_BG, LV_OPA_COVER);
    scroll_build(ui, &ui->about, about);

    // 结局 / 休眠覆盖层:最后建普通页面,保证它盖在这些页面之上。
    ui->ending = new_box(screen, 0, 0, TSXX_UI_W, TSXX_UI_H, COL_BG, LV_OPA_90);
    ui->ending_title = new_label(ui->ending, ui->font_cjk, COL_GOLD, "");
    lv_obj_align(ui->ending_title, LV_ALIGN_TOP_MID, 0, 140);
    ui->ending_hint = new_label(ui->ending, ui->font_cjk, COL_NARRATION, "");
    lv_obj_set_width(ui->ending_hint, TSXX_UI_W - 24);
    lv_obj_align(ui->ending_hint, LV_ALIGN_BOTTOM_MID, 0, -48);
    lv_obj_set_style_text_align(ui->ending_hint, LV_TEXT_ALIGN_CENTER, 0);
    set_hidden(ui->ending, true);

    // 瞬时提示:最后创建,永远在最上层。
    ui->notice = new_box(screen, 24, 150, TSXX_UI_W - 48, 34, COL_BG, LV_OPA_90);
    lv_obj_set_style_radius(ui->notice, 8, 0);
    lv_obj_set_style_border_width(ui->notice, 1, 0);
    lv_obj_set_style_border_color(ui->notice, COL_GOLD, 0);
    ui->notice_text = new_label(ui->notice, ui->font_cjk, COL_GOLD, "");
    lv_obj_align(ui->notice_text, LV_ALIGN_CENTER, 0, 0);
    set_hidden(ui->notice, true);
}

bool tsxx_ui_create(tsxx_ui_t *ui, uint16_t *art_pixels, const lv_font_t *font_cjk)
{
    if (!ui || !art_pixels || !font_cjk) return false;
    memset(ui, 0, sizeof(*ui));
    ui->font_cjk = font_cjk;
    ui->font_num = &lv_font_montserrat_14;

    lv_obj_t *screen = lv_screen_active();
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, COL_BG, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    if (!tsxx_art_init(&ui->art, screen, art_pixels)) return false;
    build_game_layers(ui, screen);
    build_choices(ui, screen);
    build_pages(ui, screen);

    tsxx_ui_show_page(ui, TSXX_PAGE_TITLE);
    ESP_LOGI(TAG, "界面就绪:画布 %dx%d 放大 4/3 到 %dx%d,正文带 y=%d",
             TSXX_ART_W, TSXX_ART_H, TSXX_UI_W, TSXX_UI_H, TSXX_BAND_Y);
    return true;
}

void tsxx_ui_show_page(tsxx_ui_t *ui, tsxx_page_id_t page)
{
    if (!ui) return;
    ui->page_current = page;

    set_hidden(ui->warning.page, page != TSXX_PAGE_WARNING);
    set_hidden(ui->lists[TSXX_LIST_TITLE].page, page != TSXX_PAGE_TITLE);
    set_hidden(ui->lists[TSXX_LIST_MENU].page, page != TSXX_PAGE_MENU);
    set_hidden(ui->lists[TSXX_LIST_SAVE].page, page != TSXX_PAGE_SAVE);
    set_hidden(ui->lists[TSXX_LIST_LOAD].page, page != TSXX_PAGE_LOAD);
    set_hidden(ui->lists[TSXX_LIST_CHAPTERS].page, page != TSXX_PAGE_CHAPTERS);
    set_hidden(ui->lists[TSXX_LIST_SETTINGS].page, page != TSXX_PAGE_SETTINGS);
    set_hidden(ui->about.page, page != TSXX_PAGE_ABOUT);

    // 离开正文页时打字机必须停下:它的标签属于正文页。
    if (page != TSXX_PAGE_GAME) {
        ui->typing_active = false;
    }
    game_mode_apply(ui);
}

bool tsxx_ui_set_art(tsxx_ui_t *ui, const tsxx_pack_t *pack, uint8_t bg, const tsxx_cg_t *cg,
                     uint8_t sprite)
{
    if (!ui) return false;
    return tsxx_art_show(&ui->art, pack, bg, cg, sprite);
}

void tsxx_ui_clear_art(tsxx_ui_t *ui)
{
    if (!ui) return;
    tsxx_art_clear(&ui->art);
}

void tsxx_ui_set_text(tsxx_ui_t *ui, const char *speaker, const char *text, uint32_t ms_per_char)
{
    if (!ui) return;
    const bool has_name = speaker && speaker[0];
    if (has_name) {
        lv_label_set_text(ui->name_text, speaker);
        lv_obj_set_width(ui->name_tag, LV_SIZE_CONTENT);
        lv_obj_set_style_min_width(ui->name_tag, 48, 0);
        lv_obj_set_style_max_width(ui->name_tag, 168, 0);
    }
    set_hidden(ui->name_tag, !has_name);
    // 旁白比有说话人的正文略暗一点,读起来更容易分辨谁在说。
    lv_obj_set_style_text_color(ui->body, has_name ? COL_TEXT : COL_NARRATION, 0);

    const size_t len = text ? strlen(text) : 0;
    const size_t take = len < sizeof(ui->typing_target) - 1 ? len : sizeof(ui->typing_target) - 1;
    memcpy(ui->typing_target, text ? text : "", take);
    ui->typing_target[take] = '\0';
    ui->typing_total = (uint32_t)take;
    ui->typing_index = 0;
    ui->typing_accum_ms = 0;
    ui->typing_shown[0] = '\0';
    lv_label_set_text(ui->body, "");

    if (ms_per_char == 0 || take == 0) {
        tsxx_ui_finish_typing(ui);
        return;
    }
    ui->typing_step_ms = ms_per_char;
    ui->typing_active = true;
    // 直接画出一个字,免得"点下去没反应"的错觉。
    tsxx_ui_typing_tick(ui, ms_per_char, ms_per_char);
}

void tsxx_ui_typing_tick(tsxx_ui_t *ui, uint32_t elapsed_ms, uint32_t ms_per_char)
{
    if (!ui || !ui->typing_active) return;
    if (ms_per_char == 0) {
        tsxx_ui_finish_typing(ui);
        return;
    }
    ui->typing_step_ms = ms_per_char;
    ui->typing_accum_ms += elapsed_ms;
    while (ui->typing_accum_ms >= ms_per_char && ui->typing_active) {
        ui->typing_accum_ms -= ms_per_char;
        const uint32_t step = utf8_step(ui->typing_target, ui->typing_index);
        if (step == 0 || ui->typing_index + step > ui->typing_total) {
            ui->typing_index = ui->typing_total;
            ui->typing_active = false;
        } else {
            ui->typing_index += step;
        }
    }
    typing_render(ui);
}

void tsxx_ui_finish_typing(tsxx_ui_t *ui)
{
    if (!ui) return;
    ui->typing_active = false;
    ui->typing_index = ui->typing_total;
    typing_render(ui);
}

bool tsxx_ui_typing(const tsxx_ui_t *ui)
{
    return ui && ui->typing_active;
}

void tsxx_ui_set_chapter(tsxx_ui_t *ui, const char *label)
{
    if (!ui) return;
    lv_obj_t *text = (lv_obj_t *)lv_obj_get_user_data(ui->chapter);
    const bool show = label && label[0];
    if (show && text) lv_label_set_text(text, label);
    lv_obj_set_width(ui->chapter, LV_SIZE_CONTENT);
    lv_obj_set_style_min_width(ui->chapter, 48, 0);
    lv_obj_set_style_max_width(ui->chapter, 132, 0);
    set_hidden(ui->chapter, !show);
}

void tsxx_ui_set_battery(tsxx_ui_t *ui, int percent)
{
    if (!ui) return;
    char buf[8];
    if (percent < 0) {
        // 读不到电量就不画数字(右上角有画面装饰,画 "--%" 反而更乱)。
        set_hidden(ui->battery, true);
        return;
    }
    lv_snprintf(buf, sizeof(buf), "%d%%", percent);
    lv_label_set_text(ui->battery, buf);
    set_hidden(ui->battery, false);
}

void tsxx_ui_set_mode(tsxx_ui_t *ui, bool auto_on, bool fast_on)
{
    if (!ui) return;
    ui->auto_on = auto_on;
    ui->fast_on = fast_on;
    lv_obj_t *text = (lv_obj_t *)lv_obj_get_user_data(ui->mode);
    if (fast_on) {
        // 快进:金底深字,和自动阅读的深底金字区分开。
        box_set_color(ui->mode, COL_GOLD, LV_OPA_COVER, COL_GOLD);
        if (text) {
            lv_label_set_text(text, "快进");
            lv_obj_set_style_text_color(text, COL_BG, 0);
        }
    } else {
        box_set_color(ui->mode, COL_BG, LV_OPA_80, COL_ROW_EDGE);
        if (text) {
            lv_label_set_text(text, "自动");
            lv_obj_set_style_text_color(text, COL_GOLD, 0);
        }
    }
    game_mode_apply(ui);
}

void tsxx_ui_show_choices(tsxx_ui_t *ui, const char *const *labels, int count)
{
    if (!ui) return;
    if (count > TSXX_CHOICE_MAX) count = TSXX_CHOICE_MAX;
    if (count < 0) count = 0;
    ui->choice_count = count;
    ui->choice_selected = 0;
    for (int i = 0; i < TSXX_CHOICE_MAX; ++i) {
        if (i >= count) {
            set_hidden(ui->choice_rows[i], true);
            continue;
        }
        lv_label_set_text(ui->choice_texts[i], labels && labels[i] ? labels[i] : "");
        set_hidden(ui->choice_rows[i], false);
    }
    // 选项少的时候整体居中一些,不要都挤在顶部。
    const int top = 24 + (TSXX_CHOICE_MAX - count) * 12;
    for (int i = 0; i < count; ++i) {
        lv_obj_set_y(ui->choice_rows[i], top + i * 48);
    }
    tsxx_ui_set_choice_selected(ui, 0);
    game_mode_apply(ui);
}

void tsxx_ui_hide_choices(tsxx_ui_t *ui)
{
    if (!ui) return;
    ui->choice_count = 0;
    game_mode_apply(ui);
}

void tsxx_ui_set_choice_selected(tsxx_ui_t *ui, int index)
{
    if (!ui) return;
    if (index < 0) index = 0;
    if (ui->choice_count > 0 && index >= ui->choice_count) index = ui->choice_count - 1;
    ui->choice_selected = index;
    for (int i = 0; i < TSXX_CHOICE_MAX; ++i) {
        const bool on = (i == index && i < ui->choice_count);
        box_set_color(ui->choice_rows[i], on ? COL_GOLD : COL_BG, on ? LV_OPA_COVER : LV_OPA_90,
                      on ? COL_GOLD : COL_ROW_EDGE);
        lv_obj_set_style_text_color(ui->choice_texts[i], on ? COL_BG : COL_TEXT, 0);
    }
}

int tsxx_ui_choice_selected(const tsxx_ui_t *ui)
{
    return ui ? ui->choice_selected : 0;
}

void tsxx_ui_ending_overlay(tsxx_ui_t *ui, const char *title, const char *hint, bool visible)
{
    if (!ui) return;
    lv_label_set_text(ui->ending_title, title ? title : "");
    lv_label_set_text(ui->ending_hint, hint ? hint : "");
    ui->ending_visible = visible;
    game_mode_apply(ui);
}

void tsxx_ui_set_title(tsxx_ui_t *ui, const char *title, const char *subtitle, const char *hint)
{
    if (!ui) return;
    lv_label_set_text(ui->title_big, title ? title : "");
    lv_label_set_text(ui->title_sub, subtitle ? subtitle : "");
    lv_label_set_text(ui->title_hint, hint ? hint : "");
}

void tsxx_ui_set_list(tsxx_ui_t *ui, tsxx_list_id_t id, const char *title, const char *hint,
                      const char *counter, const char *const *labels, const char *const *values,
                      int count, int selected)
{
    if (!ui || id < 0 || id >= TSXX_LIST_COUNT) return;
    list_update(ui, &ui->lists[id], title, hint, counter, labels, values, count, selected);
}

void tsxx_ui_set_list_selected(tsxx_ui_t *ui, tsxx_list_id_t id, int selected)
{
    if (!ui || id < 0 || id >= TSXX_LIST_COUNT) return;
    list_move_cursor(&ui->lists[id], selected);
}

void tsxx_ui_set_list_value(tsxx_ui_t *ui, tsxx_list_id_t id, int row, const char *value)
{
    if (!ui || id < 0 || id >= TSXX_LIST_COUNT) return;
    tsxx_list_t *list = &ui->lists[id];
    if (row < 0 || row >= list->count) return;
    lv_label_set_text(list->values[row], value ? value : "");
    set_hidden(list->values[row], !value || !value[0]);
}

void tsxx_ui_set_warning(tsxx_ui_t *ui, const char *body, const char *hint)
{
    if (!ui) return;
    scroll_set(&ui->warning, body, hint);
}

void tsxx_ui_set_about(tsxx_ui_t *ui, const char *body, const char *hint)
{
    if (!ui) return;
    scroll_set(&ui->about, body, hint);
}

static tsxx_scroll_t *scroll_page(tsxx_ui_t *ui, tsxx_page_id_t page)
{
    if (!ui) return NULL;
    if (page == TSXX_PAGE_WARNING) return &ui->warning;
    if (page == TSXX_PAGE_ABOUT) return &ui->about;
    return NULL;
}

bool tsxx_ui_scroll_by(tsxx_ui_t *ui, tsxx_page_id_t page, int delta)
{
    tsxx_scroll_t *scroll = scroll_page(ui, page);
    if (!scroll) return true;
    return scroll_by(scroll, delta);
}

bool tsxx_ui_scrolled_to_bottom(tsxx_ui_t *ui, tsxx_page_id_t page)
{
    tsxx_scroll_t *scroll = scroll_page(ui, page);
    if (!scroll) return true;
    return scroll_at_bottom(scroll);
}

void tsxx_ui_notice(tsxx_ui_t *ui, const char *text)
{
    if (!ui || !ui->notice) return;
    lv_label_set_text(ui->notice_text, text ? text : "");
    const bool empty = !text || !text[0];
    set_hidden(ui->notice, empty);
    if (!empty) lv_obj_move_foreground(ui->notice);
}
