// main/limelight_ui.c —— LVGL 界面实现(从 ATRI 阅读器移植,资源接到 limelight 包)。
#include "limelight_ui.h"

#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "lime_ui";

// 配色沿用源工程 index.ux / detail.ux 的样式表(深蓝 + 天蓝 + 白字)。
#define COL_ACCENT lv_color_hex(0x50C0E7)
#define COL_DEEP lv_color_hex(0x0A55BC)
#define COL_BOX_BG lv_color_hex(0x08131F)
#define COL_PAGE_BG lv_color_hex(0x050C14)
#define COL_ROW lv_color_hex(0x123C5E)
#define COL_TEXT lv_color_hex(0xF2F6FA)
#define COL_DIM lv_color_hex(0x8FB3D0)

// 前向声明:列表函数要用它判断"这个列表属于当前页吗"。
static lv_obj_t *page_object(lime_ui_t *ui, lime_page_t page);

static lv_obj_t *new_box(lv_obj_t *parent, int x, int y, int w, int h, lv_color_t bg,
                         lv_opa_t opa)
{
    lv_obj_t *obj = lv_obj_create(parent);
    if (!obj) return NULL;          // LVGL 内存池吃紧时不要继续对 NULL 调 API
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
    if (!label) return NULL;
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_label_set_text(label, text ? text : "");
    return label;
}

static void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (obj) lv_obj_set_hidden(obj, hidden);
}

static void row_apply_style(lv_obj_t *row, bool selected)
{
    lv_obj_set_style_bg_color(row, selected ? COL_ACCENT : COL_ROW, 0);
    lv_obj_set_style_bg_opa(row, selected ? LV_OPA_COVER : LV_OPA_70, 0);
    lv_obj_t *label = (lv_obj_t *)lv_obj_get_user_data(row);
    if (label) {
        lv_obj_set_style_text_color(label, selected ? COL_PAGE_BG : COL_TEXT, 0);
    }
}

// ---------------------------------------------------------------- 打字机
static void typewriter_tick(lv_timer_t *timer)
{
    lime_ui_t *ui = (lime_ui_t *)lv_timer_get_user_data(timer);
    if (!ui || !ui->typing_active) return;
    if (ui->typing_index >= ui->typing_total) {
        ui->typing_active = false;
        lv_timer_pause(ui->typewriter);
        return;
    }
    const size_t step = lime_utf8_next_boundary(ui->typing_target, ui->typing_total,
                                                ui->typing_index);
    if (step <= ui->typing_index || step > ui->typing_total) {
        ui->typing_index = ui->typing_total;
    } else {
        ui->typing_index = (uint32_t)step;
    }
    memcpy(ui->typing_shown, ui->typing_target, ui->typing_index);
    ui->typing_shown[ui->typing_index] = '\0';
    lv_label_set_text(ui->body, ui->typing_shown);
}

// ---------------------------------------------------------------- 列表页
static void list_build(lime_ui_t *ui, lv_obj_t *screen)
{
    // 共享列表控件:5 个列表页共用一份,切页时重画(省 92 个 LVGL 对象)。
    ui->list_root = new_box(screen, 0, 0, LIME_UI_W, LIME_UI_H, COL_PAGE_BG, LV_OPA_TRANSP);
    ui->list_title = new_label(ui->list_root, ui->font_cjk, COL_TEXT, "");
    lv_obj_set_pos(ui->list_title, 12, 8);
    ui->list_hint = new_label(ui->list_root, ui->font_cjk, COL_DIM, "");
    lv_obj_set_pos(ui->list_hint, 12, LIME_UI_H - 22);
    for (int i = 0; i < LIME_UI_MAX_ROWS; ++i) {
        lv_obj_t *row = new_box(ui->list_root, 12, LIME_UI_LIST_TOP_Y + i * (LIME_UI_ROW_H + 2),
                                LIME_UI_W - 24, LIME_UI_ROW_H, COL_ROW, LV_OPA_70);
        lv_obj_set_style_radius(row, 5, 0);
        lv_obj_t *label = new_label(row, ui->font_cjk, COL_TEXT, "");
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 8, 0);
        lv_obj_set_user_data(row, label);
        lv_obj_t *value = new_label(row, ui->font_cjk, COL_DIM, "");
        lv_obj_align(value, LV_ALIGN_RIGHT_MID, -8, 0);
        ui->list_labels[i] = row;
        ui->list_values[i] = value;
        set_hidden(row, true);
    }
}

// 把某个列表的窗口画到共享控件上:显示 [top, top+rows),高亮 selected。
static void list_paint(lime_ui_t *ui, lime_list_t *list)
{
    const int visible = list->count < LIME_UI_MAX_ROWS ? list->count : LIME_UI_MAX_ROWS;
    for (int i = 0; i < LIME_UI_MAX_ROWS; ++i) {
        if (i >= visible || list->top + i >= list->count) {
            set_hidden(ui->list_labels[i], true);
            continue;
        }
        const int index = list->top + i;
        lv_obj_t *label = (lv_obj_t *)lv_obj_get_user_data(ui->list_labels[i]);
        if (label) {
            lv_label_set_text(label, list->labels && list->labels[index] ? list->labels[index] : "");
        }
        const char *value = list->values_src ? list->values_src[index] : NULL;
        lv_label_set_text(ui->list_values[i], value ? value : "");
        set_hidden(ui->list_values[i], !value || !value[0]);
        set_hidden(ui->list_labels[i], false);
        row_apply_style(ui->list_labels[i], index == list->selected);
    }
}

// 让光标可见:窗口顶部跟着光标滚动。
static void list_scroll_to_selected(lime_list_t *list)
{
    const int rows_max = LIME_UI_MAX_ROWS;
    int top = list->top;
    if (list->selected < top) {
        top = list->selected;
    } else if (list->selected >= top + rows_max) {
        top = list->selected - rows_max + 1;
    }
    const int max_top = list->count > rows_max ? list->count - rows_max : 0;
    if (top > max_top) top = max_top;
    if (top < 0) top = 0;
    list->top = top;
}

void lime_ui_set_list(lime_ui_t *ui, lime_list_id_t id, const char *title, const char *hint,
                      const char *const *labels, const char *const *values, int count,
                      int selected)
{
    if (!ui || id < 0 || id >= LIME_LIST_COUNT) return;
    lime_list_t *list = &ui->lists[id];
    if (count < 0) count = 0;
    if (selected < 0) selected = 0;
    if (selected >= count) selected = count > 0 ? count - 1 : 0;
    list->count = count;
    list->selected = selected;
    list->labels = labels;
    list->values_src = values;
    list->title_text = title;
    list->hint_text = hint;
    list_scroll_to_selected(list);
    if (list->page == page_object(ui, ui->page_current)) list_paint(ui, list);
}

void lime_ui_set_list_selected(lime_ui_t *ui, lime_list_id_t id, int selected)
{
    if (!ui || id < 0 || id >= LIME_LIST_COUNT) return;
    lime_list_t *list = &ui->lists[id];
    if (selected < 0) selected = 0;
    if (selected >= list->count) selected = list->count > 0 ? list->count - 1 : 0;
    list->selected = selected;
    list_scroll_to_selected(list);
    if (list->page == page_object(ui, ui->page_current)) list_paint(ui, list);
}

void lime_ui_set_list_value(lime_ui_t *ui, lime_list_id_t id, const char *value, int row)
{
    if (!ui || id < 0 || id >= LIME_LIST_COUNT) return;
    lime_list_t *list = &ui->lists[id];
    if (list->page != page_object(ui, ui->page_current)) return;          // 非当前页不用管
    if (row < list->top || row >= list->top + LIME_UI_MAX_ROWS) return;   // 窗口外不用管
    lv_label_set_text(ui->list_values[row - list->top], value ? value : "");
    set_hidden(ui->list_values[row - list->top], !value || !value[0]);
}

int lime_ui_list_selected(const lime_ui_t *ui, lime_list_id_t id)
{
    if (!ui || id < 0 || id >= LIME_LIST_COUNT) return 0;
    return ui->lists[id].selected;
}

// ---------------------------------------------------------------- 创建
static void build_overlays(lime_ui_t *ui, lv_obj_t *screen)
{
    // 正文带:一块纯色半透明蓝(源工程 text_bg 的采样色 #498AD9)。
    // 不做渐变:LVGL 只能整块设透明度,分段逼近会在屏幕上留下横向接缝。
    // 透明度取 86%:白字在任何画面上都够清楚,又能透出一点立绘。
    ui->band = new_box(screen, 0, LIME_UI_BOX_Y, LIME_UI_W, LIME_UI_BOX_H,
                       lv_color_hex(0x498AD9), 0xDC);
    // 文字容器(透明)压在带子上。
    ui->box = new_box(screen, 0, LIME_UI_BOX_Y, LIME_UI_W, LIME_UI_BOX_H, COL_BOX_BG,
                      LV_OPA_TRANSP);

    // 说话人名牌挂在屏幕上(不挂在文本框里,否则超出父容器的部分会被裁掉)。
    ui->name_plate = new_box(screen, 6, LIME_UI_BOX_Y - 28, 10, 26, COL_DEEP, LV_OPA_COVER);
    lv_obj_set_style_radius(ui->name_plate, 6, 0);
    lv_obj_set_style_pad_hor(ui->name_plate, 8, 0);
    ui->name_text = new_label(ui->name_plate, ui->font_cjk, COL_TEXT, "");
    lv_obj_align(ui->name_text, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_align(ui->name_text, LV_TEXT_ALIGN_CENTER, 0);
    set_hidden(ui->name_plate, true);

    ui->body = new_label(ui->box, ui->font_cjk, COL_TEXT, "");
    // 正文框必须放得下 LIME_UI_LINES 行。行距算错(比如照抄 ATRI 里硬编码的
    // line_space=4)会让实际行距变成 24px,5 行要 120px 而框只有 100px,
    // 最后一行就溢出到对话框外 —— 真机上表现为"整屏都被文字填满"。
    _Static_assert(LIME_UI_LINES * LIME_UI_LINE_H + 7 <= LIME_UI_BOX_H,
                   "正文行数×行高必须放得进正文框(含 7px 上边距)");
    lv_obj_set_size(ui->body, LIME_UI_W - 16, LIME_UI_LINES * LIME_UI_LINE_H);
    lv_obj_set_pos(ui->body, 8, 7);
    lv_label_set_long_mode(ui->body, LV_LABEL_LONG_WRAP);
    // 行距 = 目标行高 - 字体自身行高(Noto Sans SC 16px 的 line_height 就是 20),
    // 所以这里算出来是 0。之前照抄 ATRI 的 "LINE_H - 16" 硬编码成 4,
    // 实际行距变成 24px:5 行需要 120px,而正文框只有 100px,
    // 真机上表现为最后一行溢出到对话框外(整屏被文字填满)。
    lv_obj_set_style_text_line_space(ui->body,
                                     LIME_UI_LINE_H - (int32_t)ui->font_cjk->line_height, 0);
    lv_obj_set_style_max_height(ui->body, LIME_UI_LINES * LIME_UI_LINE_H, 0);

    ui->progress = new_label(screen, ui->font_cjk, COL_DIM, "");
    lv_obj_set_pos(ui->progress, 8, 6);
    lv_obj_set_style_text_opa(ui->progress, LV_OPA_70, 0);
    ui->battery = new_label(screen, ui->font_tiny, COL_DIM, "");
    lv_obj_align(ui->battery, LV_ALIGN_TOP_RIGHT, -8, 6);
    lv_obj_set_style_text_opa(ui->battery, LV_OPA_70, 0);
    ui->auto_hint = new_label(screen, ui->font_cjk, COL_ACCENT, "自动");
    lv_obj_align(ui->auto_hint, LV_ALIGN_BOTTOM_RIGHT, -8, -(LIME_UI_H - LIME_UI_BOX_Y) - 6);
    lv_obj_set_style_text_opa(ui->auto_hint, LV_OPA_80, 0);
    set_hidden(ui->auto_hint, true);
    ui->page_hint = new_label(screen, ui->font_tiny, COL_DIM, "");
    lv_obj_align(ui->page_hint, LV_ALIGN_BOTTOM_RIGHT, -8, -(LIME_UI_H - LIME_UI_BOX_Y) - 28);
    lv_obj_set_style_text_opa(ui->page_hint, LV_OPA_70, 0);

    // 选项:最多 4 行,按条数在画面区里居中。
    ui->choice_box = new_box(screen, 0, 0, LIME_UI_W, LIME_UI_BOX_Y, COL_PAGE_BG, LV_OPA_TRANSP);
    // 与 ATRI 的选项按钮同几何:204x46、圆角 8、1px 天蓝描边、行底色 80%。
    const int row_h = 46, gap = 4;
    const int block_h = LIME_UI_MAX_CHOICES * row_h + (LIME_UI_MAX_CHOICES - 1) * gap;
    ui->choice_top_y = (LIME_UI_BOX_Y - block_h) / 2;
    for (int i = 0; i < LIME_UI_MAX_CHOICES; ++i) {
        lv_obj_t *row = new_box(ui->choice_box, 18, ui->choice_top_y + i * (row_h + gap),
                                LIME_UI_W - 36, row_h, COL_ROW, LV_OPA_80);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_border_color(row, COL_ACCENT, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_t *text = new_label(row, ui->font_cjk, COL_TEXT, "");
        lv_obj_align(text, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);
        ui->choice_rows[i] = row;
        ui->choice_texts[i] = text;
        lv_obj_set_user_data(row, text);
    }
    ui->choice_count = 0;
    ui->choice_selected = 0;
    set_hidden(ui->choice_box, true);
}

static void build_pages(lime_ui_t *ui, lv_obj_t *screen)
{
    // 警告页(首次运行)
    ui->page_warning = new_box(screen, 0, 0, LIME_UI_W, LIME_UI_H, COL_PAGE_BG, LV_OPA_COVER);
    ui->warning_body = new_label(ui->page_warning, ui->font_cjk, COL_TEXT, "");
    lv_obj_set_size(ui->warning_body, LIME_UI_W - 24, 240);
    lv_obj_set_pos(ui->warning_body, 12, 30);
    lv_label_set_long_mode(ui->warning_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(ui->warning_body,
                                     LIME_UI_LINE_H - (int32_t)ui->font_cjk->line_height, 0);
    ui->warning_hint = new_label(ui->page_warning, ui->font_cjk, COL_ACCENT, "");
    lv_obj_align(ui->warning_hint, LV_ALIGN_BOTTOM_MID, 0, -18);

    // 标题页:画面区留给标题画,下面一屏放菜单。
    ui->page_title = new_box(screen, 0, LIME_UI_BOX_Y, LIME_UI_W, LIME_UI_BOX_H, COL_BOX_BG,
                             LV_OPA_80);
    ui->lists[LIME_LIST_TITLE].page = ui->page_title;

    // 整屏页:菜单 / 设置 / 存档 / 章节 / 鉴赏 / 关于 / 结局
    ui->page_menu = new_box(screen, 0, 0, LIME_UI_W, LIME_UI_H, COL_PAGE_BG, LV_OPA_90);
    ui->lists[LIME_LIST_MENU].page = ui->page_menu;

    ui->page_settings = new_box(screen, 0, 0, LIME_UI_W, LIME_UI_H, COL_PAGE_BG, LV_OPA_COVER);
    ui->lists[LIME_LIST_SETTINGS].page = ui->page_settings;

    ui->page_slots = new_box(screen, 0, 0, LIME_UI_W, LIME_UI_H, COL_PAGE_BG, LV_OPA_COVER);
    ui->lists[LIME_LIST_SLOTS].page = ui->page_slots;

    // 章节:230 个标记,靠窗口滚动(三键设备没有触摸滑屏)。
    ui->page_chapters = new_box(screen, 0, 0, LIME_UI_W, LIME_UI_H, COL_PAGE_BG, LV_OPA_COVER);
    ui->lists[LIME_LIST_CHAPTERS].page = ui->page_chapters;

    // 鉴赏:整屏看图,LVGL 只放一张信息行;图本身复用画面区画布。
    ui->page_gallery = new_box(screen, 0, 0, LIME_UI_W, LIME_UI_H, COL_PAGE_BG, LV_OPA_TRANSP);
    ui->gallery_info = new_label(screen, ui->font_tiny, COL_TEXT, "");
    lv_obj_align(ui->gallery_info, LV_ALIGN_BOTTOM_MID, 0, -6);

    // 关于页
    ui->page_about = new_box(screen, 0, 0, LIME_UI_W, LIME_UI_H, COL_PAGE_BG, LV_OPA_COVER);
    ui->about_body = new_label(ui->page_about, ui->font_cjk, COL_TEXT, "");
    lv_obj_set_width(ui->about_body, LIME_UI_W - 24);
    lv_obj_set_pos(ui->about_body, 12, 16);
    lv_label_set_long_mode(ui->about_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(ui->about_body,
                                     LIME_UI_LINE_H - (int32_t)ui->font_cjk->line_height, 0);

    ui->page_ending = new_box(screen, 0, 0, LIME_UI_W, LIME_UI_H, COL_PAGE_BG, LV_OPA_COVER);
    ui->ending_kicker = new_label(ui->page_ending, ui->font_cjk, COL_DIM, "达成结局");
    lv_obj_align(ui->ending_kicker, LV_ALIGN_TOP_MID, 0, 90);
    ui->ending_name = new_label(ui->page_ending, ui->font_cjk, COL_ACCENT, "");
    lv_obj_align(ui->ending_name, LV_ALIGN_TOP_MID, 0, 130);
    ui->ending_hint = new_label(ui->page_ending, ui->font_cjk, COL_TEXT, "");
    lv_obj_align(ui->ending_hint, LV_ALIGN_BOTTOM_MID, 0, -34);
    lv_obj_set_style_text_align(ui->ending_hint, LV_TEXT_ALIGN_CENTER, 0);

    // 瞬时提示:最后创建,永远在最上层。
    ui->notice = new_box(screen, 24, LIME_UI_H - 66, LIME_UI_W - 48, 34, COL_DEEP, LV_OPA_90);
    lv_obj_set_style_radius(ui->notice, 8, 0);
    ui->notice_text = new_label(ui->notice, ui->font_cjk, COL_TEXT, "");
    lv_obj_align(ui->notice_text, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_align(ui->notice_text, LV_TEXT_ALIGN_CENTER, 0);
    set_hidden(ui->notice, true);
}

bool lime_ui_create(lime_ui_t *ui, uint16_t *art_pixels, uint8_t *sprite_buffer,
                    uint32_t sprite_capacity, uint8_t *mask_buffer, uint32_t mask_capacity,
                    const lv_font_t *font_cjk)
{
    if (!ui || !art_pixels || !font_cjk) return false;
    memset(ui, 0, sizeof(*ui));
    ui->font_cjk = font_cjk;
    ui->font_tiny = &lv_font_montserrat_14;

    lv_obj_t *screen = lv_screen_active();
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    if (!lime_image_init(&ui->art, screen, art_pixels, sprite_buffer, sprite_capacity,
                         mask_buffer, mask_capacity)) {
        return false;
    }
    build_overlays(ui, screen);
    build_pages(ui, screen);
    list_build(ui, screen);       // 最后建:列表控件压在各页之上(提示条仍在最上层)

    ui->typewriter = lv_timer_create(typewriter_tick, 30, ui);
    if (!ui->typewriter) {
        // LVGL 内存池吃紧时定时器可能建不出来:正文改为整段直接显示,不影响阅读。
        ESP_LOGW(TAG, "打字机定时器创建失败,正文将整段显示");
    } else {
        lv_timer_pause(ui->typewriter);
    }

    // 关键控件少一个都说明内存池不够:明确失败(而不是留下半截界面在跑)。
    if (!ui->band || !ui->box || !ui->body || !ui->list_root || !ui->choice_box ||
        !ui->page_warning || !ui->page_ending || !ui->notice) {
        ESP_LOGE(TAG, "界面控件创建不完整,LVGL 内存池不足");
        return false;
    }

    ui->page_current = LIME_PAGE_TITLE;
    lime_ui_show_page(ui, LIME_PAGE_TITLE);
    return true;
}

// ---------------------------------------------------------------- 页面
static lv_obj_t *page_object(lime_ui_t *ui, lime_page_t page)
{
    switch (page) {
    case LIME_PAGE_WARNING: return ui->page_warning;
    case LIME_PAGE_TITLE: return ui->page_title;
    case LIME_PAGE_MENU: return ui->page_menu;
    case LIME_PAGE_SETTINGS: return ui->page_settings;
    case LIME_PAGE_SLOTS: return ui->page_slots;
    case LIME_PAGE_CHAPTERS: return ui->page_chapters;
    case LIME_PAGE_GALLERY: return ui->page_gallery;
    case LIME_PAGE_ABOUT: return ui->page_about;
    case LIME_PAGE_ENDING: return ui->page_ending;
    case LIME_PAGE_GAME: return NULL;      // 正文页由画布 + 浮层组成
    default: return NULL;
    }
}

// 所有整屏页面(标题页是一个容器,其余都是整屏块)。页面显隐 = 整屏块的显隐,
// 列表行跟着所属页面一起显隐,不再单独处理。
static const lime_page_t PAGE_OBJECTS[] = {
    LIME_PAGE_WARNING, LIME_PAGE_TITLE, LIME_PAGE_MENU, LIME_PAGE_SETTINGS,
    LIME_PAGE_SLOTS, LIME_PAGE_CHAPTERS, LIME_PAGE_GALLERY, LIME_PAGE_ABOUT,
    LIME_PAGE_ENDING,
};

void lime_ui_show_page(lime_ui_t *ui, lime_page_t page)
{
    if (!ui) return;
    ui->page_current = page;
    const bool game = page == LIME_PAGE_GAME;
    const bool gallery = page == LIME_PAGE_GALLERY;

    // 先全部隐掉,再显示目标页。
    for (size_t i = 0; i < sizeof(PAGE_OBJECTS) / sizeof(PAGE_OBJECTS[0]); ++i) {
        lv_obj_t *obj = page_object(ui, PAGE_OBJECTS[i]);
        if (obj) set_hidden(obj, PAGE_OBJECTS[i] != page);
    }

    // 正文页的元素(画布上的浮层)只在正文页出现;鉴赏页只留画面区与信息行。
    set_hidden(ui->band, !(game || gallery));
    set_hidden(ui->box, !game);
    set_hidden(ui->name_plate, !game || !lv_obj_get_child_count(ui->name_plate));
    set_hidden(ui->body, !game);
    set_hidden(ui->progress, !(game || gallery));
    set_hidden(ui->page_hint, !game);
    set_hidden(ui->auto_hint, !(game && ui->auto_on));
    set_hidden(ui->gallery_info, !gallery);
    if (!game) lime_ui_hide_choices(ui);

    // 共享列表控件:只在"当前页拥有列表"时显示,并按该列表重画一次。
    lv_obj_t *owner = NULL;
    for (int i = 0; i < LIME_LIST_COUNT; ++i) {
        if (ui->lists[i].page && ui->lists[i].page == page_object(ui, page)) {
            lime_list_t *list = &ui->lists[i];
            lv_label_set_text(ui->list_title, list->title_text ? list->title_text : "");
            lv_label_set_text(ui->list_hint, list->hint_text ? list->hint_text : "");
            set_hidden(ui->list_title, !list->title_text || !list->title_text[0]);
            set_hidden(ui->list_hint, !list->hint_text || !list->hint_text[0]);
            // 列表控件是屏幕级的共享对象,而列表页可能是"屏幕下部的容器"
            // (标题页在 y=210),所以行的 y 要加上所属页面的偏移。
            const int row_h = list->row_h > 0 ? list->row_h : LIME_UI_ROW_H;
            const int top_y = list->top_y > 0 ? list->top_y : LIME_UI_LIST_TOP_Y;
            const int base_y = lv_obj_get_y(list->page);
            const int base_x = lv_obj_get_x(list->page);
            lv_obj_set_pos(ui->list_title, base_x + 12, base_y + 8);
            lv_obj_set_pos(ui->list_hint, base_x + 12, LIME_UI_H - 22);
            for (int r = 0; r < LIME_UI_MAX_ROWS; ++r) {
                lv_obj_set_pos(ui->list_labels[r], base_x + 12,
                               base_y + top_y + r * (row_h + 2));
                lv_obj_set_height(ui->list_labels[r], row_h);
            }
            list_scroll_to_selected(list);
            list_paint(ui, list);
            owner = ui->list_root;
            break;
        }
    }
    set_hidden(ui->list_root, owner == NULL);

    if (!game) {
        ui->typing_active = false;
        if (ui->typewriter) lv_timer_pause(ui->typewriter);
    }
}

// ---------------------------------------------------------------- 画面区
bool lime_ui_set_art(lime_ui_t *ui, const lime_assets_t *assets, int bg_index, int sprite_index)
{
    if (!ui || !assets) return false;
    return lime_image_show(&ui->art, assets, bg_index, sprite_index);
}

bool lime_ui_set_backdrop(lime_ui_t *ui, const lime_assets_t *assets, int bg_index)
{
    if (!ui || !assets) return false;
    return lime_image_show_backdrop(&ui->art, assets, bg_index);
}

// ---------------------------------------------------------------- 正文
void lime_ui_set_text(lime_ui_t *ui, const char *speaker, const char *text, uint32_t ms_per_char)
{
    if (!ui) return;
    const char *safe = text ? text : "";
    const size_t length = strlen(safe);
    const size_t copy = length < sizeof(ui->typing_target) - 1 ? length
                                                               : sizeof(ui->typing_target) - 1;
    memcpy(ui->typing_target, safe, copy);
    ui->typing_target[copy] = '\0';
    ui->typing_total = (uint32_t)copy;
    ui->typing_index = 0;
    ui->typing_shown[0] = '\0';
    lv_label_set_text(ui->body, "");

    if (speaker && speaker[0]) {
        lv_label_set_text(ui->name_text, speaker);
        lv_obj_set_width(ui->name_plate, LV_SIZE_CONTENT);
        lv_obj_set_style_min_width(ui->name_plate, 40, 0);
        set_hidden(ui->name_plate, false);
    } else {
        set_hidden(ui->name_plate, true);
    }

    if (ms_per_char == 0 || copy == 0) {
        memcpy(ui->typing_shown, ui->typing_target, copy + 1);
        lv_label_set_text(ui->body, ui->typing_shown);
        ui->typing_index = ui->typing_total;
        ui->typing_active = false;
        if (ui->typewriter) lv_timer_pause(ui->typewriter);
        return;
    }
    ui->typing_step_ms = ms_per_char;
    ui->typing_active = true;
    if (ui->typewriter) {
        lv_timer_set_period(ui->typewriter, ms_per_char);
        lv_timer_resume(ui->typewriter);
        lv_timer_reset(ui->typewriter);
    }
}

void lime_ui_finish_typing(lime_ui_t *ui)
{
    if (!ui || !ui->typing_active) return;
    ui->typing_index = ui->typing_total;
    memcpy(ui->typing_shown, ui->typing_target, ui->typing_total + 1);
    lv_label_set_text(ui->body, ui->typing_shown);
    ui->typing_active = false;
    if (ui->typewriter) lv_timer_pause(ui->typewriter);
}

bool lime_ui_typing(const lime_ui_t *ui)
{
    return ui && ui->typing_active;
}

// ---------------------------------------------------------------- 浮层
// 章节显示为 X-X(如 10-3),major < 0 表示当前没有章节信息。
void lime_ui_set_progress(lime_ui_t *ui, int major, int minor, int page, int pages)
{
    if (!ui) return;
    char buffer[48];
    if (major >= 0) {
        snprintf(buffer, sizeof(buffer), "%d-%d", major, minor);
        lv_label_set_text(ui->progress, buffer);
    } else {
        lv_label_set_text(ui->progress, "");
    }
    if (page > 0 && pages > 1) {
        snprintf(buffer, sizeof(buffer), "%d/%d", page, pages);
        lv_label_set_text(ui->page_hint, buffer);
    } else {
        lv_label_set_text(ui->page_hint, "");
    }
}

void lime_ui_set_battery(lime_ui_t *ui, int percent)
{
    if (!ui) return;
    char buffer[16];
    if (percent < 0) {
        lv_label_set_text(ui->battery, "");
        return;
    }
    snprintf(buffer, sizeof(buffer), "%d%%", percent);
    lv_label_set_text(ui->battery, buffer);
}

void lime_ui_set_auto(lime_ui_t *ui, bool on)
{
    if (!ui) return;
    ui->auto_on = on;
    set_hidden(ui->auto_hint, !(on && ui->page_current == LIME_PAGE_GAME));
}

// ---------------------------------------------------------------- 选项
void lime_ui_show_choices(lime_ui_t *ui, const char *const *texts, int count)
{
    if (!ui) return;
    if (count > LIME_UI_MAX_CHOICES) count = LIME_UI_MAX_CHOICES;
    if (count < 0) count = 0;
    ui->choice_count = count;
    ui->choice_selected = 0;
    // 按条数把选项块重新居中(4 行与 2 行的高度不一样)。
    const int row_h = 46, gap = 4;
    const int block_h = count > 0 ? count * row_h + (count - 1) * gap : 0;
    const int top = (LIME_UI_BOX_Y - block_h) / 2;
    for (int i = 0; i < LIME_UI_MAX_CHOICES; ++i) {
        if (i >= count) {
            set_hidden(ui->choice_rows[i], true);
            continue;
        }
        lv_obj_set_y(ui->choice_rows[i], top + i * (row_h + gap));
        lv_label_set_text(ui->choice_texts[i], texts && texts[i] ? texts[i] : "");
        set_hidden(ui->choice_rows[i], false);
        row_apply_style(ui->choice_rows[i], i == 0);
    }
    set_hidden(ui->choice_box, count == 0);
}

void lime_ui_hide_choices(lime_ui_t *ui)
{
    if (!ui) return;
    ui->choice_count = 0;
    set_hidden(ui->choice_box, true);
}

void lime_ui_set_choice_selected(lime_ui_t *ui, int index)
{
    if (!ui) return;
    if (index < 0) index = 0;
    if (index >= ui->choice_count) index = ui->choice_count > 0 ? ui->choice_count - 1 : 0;
    ui->choice_selected = index;
    for (int i = 0; i < ui->choice_count; ++i) {
        row_apply_style(ui->choice_rows[i], i == index);
    }
}

int lime_ui_choice_selected(const lime_ui_t *ui)
{
    return ui ? ui->choice_selected : 0;
}

// ---------------------------------------------------------------- 单独页面
void lime_ui_set_ending(lime_ui_t *ui, const char *kicker, const char *name, const char *hint)
{
    if (!ui) return;
    if (kicker) lv_label_set_text(ui->ending_kicker, kicker);
    lv_label_set_text(ui->ending_name, name ? name : "");
    lv_label_set_text(ui->ending_hint, hint ? hint : "");
}

void lime_ui_set_warning(lime_ui_t *ui, const char *body, const char *hint)
{
    if (!ui) return;
    lv_label_set_text(ui->warning_body, body ? body : "");
    lv_label_set_text(ui->warning_hint, hint ? hint : "");
}

void lime_ui_set_about(lime_ui_t *ui, const char *body)
{
    if (!ui) return;
    lv_label_set_text(ui->about_body, body ? body : "");
}

void lime_ui_set_gallery_info(lime_ui_t *ui, int index, int count)
{
    if (!ui) return;
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%d/%d", index + 1, count);
    lv_label_set_text(ui->gallery_info, buffer);
}

void lime_ui_notice(lime_ui_t *ui, const char *text)
{
    if (!ui) return;
    lv_label_set_text(ui->notice_text, text ? text : "");
    set_hidden(ui->notice, !text || !text[0]);
}
