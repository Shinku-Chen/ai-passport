// main/atri_ui.c —— LVGL 界面实现。
#include "atri_ui.h"

#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "atri_ui";

// 配色沿用源工程 index.ux / detail.ux 的样式表(深蓝 + 天蓝 + 白字)。
#define COL_ACCENT lv_color_hex(0x50C0E7)
#define COL_DEEP lv_color_hex(0x0A55BC)
#define COL_BOX_BG lv_color_hex(0x08131F)
#define COL_PAGE_BG lv_color_hex(0x050C14)
#define COL_ROW lv_color_hex(0x123C5E)
#define COL_TEXT lv_color_hex(0xF2F6FA)
#define COL_DIM lv_color_hex(0x8FB3D0)
#define COL_DARK lv_color_hex(0x06121E)

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

static void row_apply_style(lv_obj_t *row, bool selected)
{
    lv_obj_set_style_bg_color(row, selected ? COL_ACCENT : COL_ROW, 0);
    lv_obj_set_style_bg_opa(row, selected ? LV_OPA_COVER : LV_OPA_70, 0);
    lv_obj_t *label = (lv_obj_t *)lv_obj_get_user_data(row);
    if (label) {
        lv_obj_set_style_text_color(label, selected ? COL_DARK : COL_TEXT, 0);
    }
    // 右侧取值标签是行的兄弟对象,按行下标另外取色。
}

static void list_row_apply(atri_ui_t *ui, atri_list_t *list, int index)
{
    if (index < 0 || index >= list->count) return;
    row_apply_style(list->rows[index], index == list->selected);
    if (list->values[index]) {
        lv_obj_set_style_text_color(list->values[index],
                                    index == list->selected ? COL_DARK : COL_DIM, 0);
    }
}

// ---------------------------------------------------------------- 打字机
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

static void typewriter_tick(lv_timer_t *timer)
{
    atri_ui_t *ui = (atri_ui_t *)lv_timer_get_user_data(timer);
    if (!ui || !ui->typing_active) return;
    if (ui->typing_index >= ui->typing_total) {
        ui->typing_active = false;
        lv_timer_pause(ui->typewriter);
        return;
    }
    const uint32_t step = utf8_step(ui->typing_target, ui->typing_index);
    if (step == 0 || ui->typing_index + step > ui->typing_total) {
        ui->typing_index = ui->typing_total;
    } else {
        ui->typing_index += step;
    }
    memcpy(ui->typing_shown, ui->typing_target, ui->typing_index);
    ui->typing_shown[ui->typing_index] = '\0';
    lv_label_set_text(ui->body, ui->typing_shown);
}

// ---------------------------------------------------------------- 列表页
static void list_build(atri_ui_t *ui, atri_list_t *list, lv_obj_t *page, int row_h, int top)
{
    list->page = page;
    list->row_h = row_h;
    list->top = top;
    list->title = new_label(page, ui->font_cjk, COL_TEXT, "");
    lv_obj_set_pos(list->title, 12, 8);
    // 提示行是中文,必须用中文字体(Montserrat 没有汉字,会显示成乱码)。
    list->hint = new_label(page, ui->font_cjk, COL_DIM, "");
    lv_obj_set_pos(list->hint, 12, ATRI_UI_H - 22);
    for (int i = 0; i < ATRI_UI_MAX_ROWS; ++i) {
        lv_obj_t *row = new_box(page, 12, top + i * (row_h + 2), ATRI_UI_W - 24, row_h,
                                COL_ROW, LV_OPA_70);
        lv_obj_set_style_radius(row, 5, 0);
        lv_obj_t *label = new_label(row, ui->font_cjk, COL_TEXT, "");
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 8, 0);
        lv_obj_set_user_data(row, label);
        lv_obj_t *value = new_label(row, ui->font_cjk, COL_DIM, "");
        lv_obj_align(value, LV_ALIGN_RIGHT_MID, -8, 0);
        list->rows[i] = row;
        list->values[i] = value;
        set_hidden(row, true);
        set_hidden(value, true);
    }
    list->count = 0;
    list->selected = 0;
}

static void list_update(atri_ui_t *ui, atri_list_t *list, const char *title, const char *hint,
                        const char *const *labels, const char *const *values, int count,
                        int selected)
{
    if (count > ATRI_UI_MAX_ROWS) count = ATRI_UI_MAX_ROWS;
    if (count < 0) count = 0;
    if (title) lv_label_set_text(list->title, title);
    if (list->hint) {
        lv_label_set_text(list->hint, hint ? hint : "");
        set_hidden(list->hint, !hint || !hint[0]);
    }
    if (selected < 0) selected = 0;
    if (selected >= count) selected = count > 0 ? count - 1 : 0;
    list->count = count;
    list->selected = selected;
    for (int i = 0; i < ATRI_UI_MAX_ROWS; ++i) {
        if (i >= count) {
            set_hidden(list->rows[i], true);
            set_hidden(list->values[i], true);
            continue;
        }
        lv_obj_t *label = (lv_obj_t *)lv_obj_get_user_data(list->rows[i]);
        if (label) lv_label_set_text(label, labels && labels[i] ? labels[i] : "");
        if (values) {
            lv_label_set_text(list->values[i], values[i] ? values[i] : "");
            set_hidden(list->values[i], !values[i] || !values[i][0]);
        } else {
            set_hidden(list->values[i], true);
        }
        set_hidden(list->rows[i], false);
    }
    for (int i = 0; i < count; ++i) list_row_apply(ui, list, i);
}

// ---------------------------------------------------------------- 创建
static void build_overlays(atri_ui_t *ui, lv_obj_t *screen)
{
    // 正文带本身(半透明蓝底)是画布的一部分,见 atri_image.c 的 draw_text_band();
    // 这里只放一个透明容器装文字,不再盖一层不透明底色,立绘才能透出来。
    ui->box = new_box(screen, 0, ATRI_BOX_Y, ATRI_UI_W, ATRI_BOX_H, COL_BOX_BG, LV_OPA_TRANSP);

    // 说话人名牌:直接挂在屏幕上(不挂在文本框里),位置固定在正文带上沿。
    // 挂在父容器里时,LVGL 默认会把超出父容器的子对象裁掉,名牌会被整块切没。
    ui->name_plate = new_box(screen, 6, ATRI_BOX_Y - 28, 10, 26, COL_DEEP, LV_OPA_COVER);
    lv_obj_set_style_radius(ui->name_plate, 6, 0);
    lv_obj_set_style_pad_hor(ui->name_plate, 8, 0);
    ui->name_text = new_label(ui->name_plate, ui->font_cjk, COL_TEXT, "");
    lv_obj_align(ui->name_text, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_align(ui->name_text, LV_TEXT_ALIGN_CENTER, 0);

    ui->body = new_label(ui->box, ui->font_cjk, COL_TEXT, "");
    lv_obj_set_size(ui->body, ATRI_UI_W - 16, ATRI_TEXT_LINES * ATRI_LINE_H);
    lv_obj_set_pos(ui->body, 8, 7);
    lv_label_set_long_mode(ui->body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(ui->body, ATRI_LINE_H - 16, 0);

    // 画面区浮层:章节 / 电量 / 页码。章节标签是中文,用中文字体。
    ui->progress = new_label(screen, ui->font_cjk, COL_DIM, "");
    lv_obj_set_pos(ui->progress, 8, 6);
    lv_obj_set_style_text_opa(ui->progress, LV_OPA_70, 0);
    ui->battery = new_label(screen, ui->font_tiny, COL_DIM, "");
    lv_obj_align(ui->battery, LV_ALIGN_TOP_RIGHT, -8, 6);
    lv_obj_set_style_text_opa(ui->battery, LV_OPA_70, 0);
    // 自动阅读指示:常驻在画面区左下角(用文字,不用图标,省得字体缺字形)。
    ui->auto_hint = new_label(screen, ui->font_cjk, COL_ACCENT, "自动");
    lv_obj_align(ui->auto_hint, LV_ALIGN_BOTTOM_LEFT, 8,
                 -(ATRI_UI_H - ATRI_BOX_Y) - 6);
    lv_obj_set_style_text_opa(ui->auto_hint, LV_OPA_80, 0);
    set_hidden(ui->auto_hint, true);

    ui->page_hint = new_label(screen, ui->font_tiny, COL_DIM, "");
    lv_obj_align(ui->page_hint, LV_ALIGN_TOP_RIGHT, -8, ATRI_BOX_Y - 24);
    lv_obj_set_style_text_opa(ui->page_hint, LV_OPA_70, 0);

    // 选项:盖在画面区上的两行按钮(源工程是两个 250x61 的按钮)。
    ui->choice_box = new_box(screen, 0, 0, ATRI_UI_W, ATRI_ART_H, COL_PAGE_BG, LV_OPA_TRANSP);
    for (int i = 0; i < 2; ++i) {
        lv_obj_t *row = new_box(ui->choice_box, 18, 54 + i * 58, ATRI_UI_W - 36, 46, COL_ROW,
                                LV_OPA_80);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_border_color(row, COL_ACCENT, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_t *text = new_label(row, ui->font_cjk, COL_TEXT, "");
        lv_obj_align(text, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);
        ui->choice_rows[i] = row;
        ui->choice_texts[i] = text;
    }
    ui->choice_count = 0;
    ui->choice_selected = 0;
    set_hidden(ui->choice_box, true);
}

static void build_pages(atri_ui_t *ui, lv_obj_t *screen)
{
    // 警告页(首次运行)
    ui->page_warning = new_box(screen, 0, 0, ATRI_UI_W, ATRI_UI_H, COL_PAGE_BG, LV_OPA_COVER);
    lv_obj_set_style_pad_all(ui->page_warning, 0, 0);
    ui->warning_body = new_label(ui->page_warning, ui->font_cjk, COL_TEXT, "");
    lv_obj_set_size(ui->warning_body, ATRI_UI_W - 24, 240);
    lv_obj_set_pos(ui->warning_body, 12, 30);
    lv_label_set_long_mode(ui->warning_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(ui->warning_body, 4, 0);
    ui->warning_hint = new_label(ui->page_warning, ui->font_cjk, COL_ACCENT, "");
    lv_obj_align(ui->warning_hint, LV_ALIGN_BOTTOM_MID, 0, -18);

    // 标题页:画面区留给标题画,下面 110px 放菜单。
    ui->page_title = new_box(screen, 0, ATRI_BOX_Y, ATRI_UI_W, ATRI_BOX_H, COL_BOX_BG,
                             LV_OPA_80);
    list_build(ui, &ui->lists[ATRI_LIST_TITLE], ui->page_title, 20, 6);
    set_hidden(ui->lists[ATRI_LIST_TITLE].title, true);
    set_hidden(ui->lists[ATRI_LIST_TITLE].hint, true);

    // 整屏页:菜单 / 设置 / 存档 / 结局 / 关于
    ui->page_settings = new_box(screen, 0, 0, ATRI_UI_W, ATRI_UI_H, COL_PAGE_BG, LV_OPA_COVER);
    list_build(ui, &ui->lists[ATRI_LIST_SETTINGS], ui->page_settings, 30, 48);

    lv_obj_t *page_menu = new_box(screen, 0, 0, ATRI_UI_W, ATRI_UI_H, COL_PAGE_BG, LV_OPA_90);
    list_build(ui, &ui->lists[ATRI_LIST_MENU], page_menu, 30, 48);
    ui->lists[ATRI_LIST_MENU].page = page_menu;

    ui->page_slots = new_box(screen, 0, 0, ATRI_UI_W, ATRI_UI_H, COL_PAGE_BG, LV_OPA_COVER);
    list_build(ui, &ui->lists[ATRI_LIST_SLOTS], ui->page_slots, 30, 44);
    // 关于页:内容比一屏长,靠按键滚动(三键设备没有触摸滑屏)。
    ui->page_about = new_box(screen, 0, 0, ATRI_UI_W, ATRI_UI_H, COL_PAGE_BG, LV_OPA_COVER);
    lv_obj_set_scrollable(ui->page_about, true);
    lv_obj_set_scroll_dir(ui->page_about, LV_DIR_VER);
    lv_obj_set_style_pad_all(ui->page_about, 0, 0);
    ui->about_body = new_label(ui->page_about, ui->font_cjk, COL_TEXT, "");
    lv_obj_set_width(ui->about_body, ATRI_UI_W - 24);
    lv_obj_set_pos(ui->about_body, 12, 16);
    lv_label_set_long_mode(ui->about_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(ui->about_body, 4, 0);
    lv_obj_set_style_pad_bottom(ui->about_body, 20, 0);

    ui->page_ending = new_box(screen, 0, 0, ATRI_UI_W, ATRI_UI_H, COL_PAGE_BG, LV_OPA_COVER);
    ui->ending_kicker = new_label(ui->page_ending, ui->font_cjk, COL_DIM, "达成结局");
    lv_obj_align(ui->ending_kicker, LV_ALIGN_TOP_MID, 0, 90);
    ui->ending_name = new_label(ui->page_ending, ui->font_cjk, COL_ACCENT, "");
    lv_obj_align(ui->ending_name, LV_ALIGN_TOP_MID, 0, 130);
    ui->ending_hint = new_label(ui->page_ending, ui->font_cjk, COL_TEXT, "");
    lv_obj_align(ui->ending_hint, LV_ALIGN_BOTTOM_MID, 0, -34);
    lv_obj_set_style_text_align(ui->ending_hint, LV_TEXT_ALIGN_CENTER, 0);

    // 瞬时提示:最后创建,永远在最上层。
    ui->notice = new_box(screen, 24, ATRI_UI_H - 66, ATRI_UI_W - 48, 34, COL_DEEP, LV_OPA_90);
    lv_obj_set_style_radius(ui->notice, 8, 0);
    lv_obj_t *notice_text = new_label(ui->notice, ui->font_cjk, COL_TEXT, "");
    lv_obj_align(notice_text, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_align(notice_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_user_data(ui->notice, notice_text);
    set_hidden(ui->notice, true);

    ui->page_game = NULL;   // 正文页由 box / 浮层组成,没有单独容器
}

bool atri_ui_create(atri_ui_t *ui, uint16_t *art_pixels, const lv_font_t *font_cjk)
{
    if (!ui || !art_pixels || !font_cjk) return false;
    memset(ui, 0, sizeof(*ui));
    ui->font_cjk = font_cjk;
    ui->font_tiny = &lv_font_montserrat_14;

    lv_obj_t *screen = lv_screen_active();
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    if (!atri_image_init(&ui->art, screen, art_pixels)) return false;
    build_overlays(ui, screen);
    build_pages(ui, screen);

    ui->typewriter = lv_timer_create(typewriter_tick, 35, ui);
    if (!ui->typewriter) {
        ESP_LOGE(TAG, "打字机定时器创建失败");
        return false;
    }
    lv_timer_pause(ui->typewriter);
    atri_ui_show_page(ui, ATRI_PAGE_TITLE);
    return true;
}

void atri_ui_show_page(atri_ui_t *ui, atri_page_t page)
{
    if (!ui) return;
    ui->page_current = page;
    set_hidden(ui->box, page != ATRI_PAGE_GAME);
    set_hidden(ui->auto_hint, page != ATRI_PAGE_GAME || !ui->auto_on);
    set_hidden(ui->progress, page != ATRI_PAGE_GAME);
    // 电量在标题页也显示(同一位于画面区右上角)。
    set_hidden(ui->battery, page != ATRI_PAGE_GAME && page != ATRI_PAGE_TITLE);
    set_hidden(ui->page_hint, page != ATRI_PAGE_GAME);
    set_hidden(ui->choice_box, page != ATRI_PAGE_GAME || ui->choice_count == 0);
    // 名牌在正文页由 set_text 按"这句有没有说话人"决定显隐,离开正文页一律收起。
    if (page != ATRI_PAGE_GAME) set_hidden(ui->name_plate, true);
    set_hidden(ui->page_warning, page != ATRI_PAGE_WARNING);
    set_hidden(ui->page_title, page != ATRI_PAGE_TITLE);
    set_hidden(ui->lists[ATRI_LIST_TITLE].page, page != ATRI_PAGE_TITLE);
    set_hidden(ui->page_settings, page != ATRI_PAGE_SETTINGS);
    set_hidden(ui->lists[ATRI_LIST_MENU].page, page != ATRI_PAGE_MENU);
    set_hidden(ui->page_slots, page != ATRI_PAGE_SLOTS);
    set_hidden(ui->page_ending, page != ATRI_PAGE_ENDING);
    set_hidden(ui->page_about, page != ATRI_PAGE_ABOUT);

    if (page != ATRI_PAGE_GAME) {
        ui->typing_active = false;
        if (ui->typewriter) lv_timer_pause(ui->typewriter);
    }
    // 整屏页面按创建顺序叠放,把当前页提到最前,避免被后建的页面盖住。
    lv_obj_t *front = NULL;
    switch (page) {
    case ATRI_PAGE_WARNING: front = ui->page_warning; break;
    case ATRI_PAGE_TITLE: front = ui->lists[ATRI_LIST_TITLE].page; break;
    case ATRI_PAGE_MENU: front = ui->lists[ATRI_LIST_MENU].page; break;
    case ATRI_PAGE_SETTINGS: front = ui->page_settings; break;
    case ATRI_PAGE_SLOTS: front = ui->page_slots; break;
    case ATRI_PAGE_ENDING: front = ui->page_ending; break;
    case ATRI_PAGE_ABOUT: front = ui->page_about; break;
    case ATRI_PAGE_GAME: break;
    }
    if (front) lv_obj_move_foreground(front);
}

bool atri_ui_set_art(atri_ui_t *ui, const atri_pack_t *pack, uint16_t bg, uint16_t ovl,
                     int16_t x, int16_t y, uint16_t chr)
{
    if (!ui) return false;
    return atri_image_show(&ui->art, pack, bg, ovl, x, y, chr);
}

void atri_ui_set_text(atri_ui_t *ui, const char *speaker, const char *text, uint32_t ms_per_char)
{
    if (!ui) return;
    const bool has_name = speaker && speaker[0];
    if (has_name) {
        if (strcmp(speaker, ui->last_speaker) != 0) {
            // 只在不同说话人切换时打一行,便于真机上核对名牌内容。
            snprintf(ui->last_speaker, sizeof(ui->last_speaker), "%s", speaker);
            ESP_LOGI(TAG, "名牌: %s", speaker);
        }
        lv_label_set_text(ui->name_text, speaker);
        lv_obj_set_width(ui->name_plate, LV_SIZE_CONTENT);
        lv_obj_set_style_min_width(ui->name_plate, 40, 0);
        lv_obj_set_style_max_width(ui->name_plate, 168, 0);
        set_hidden(ui->name_plate, false);
    } else {
        set_hidden(ui->name_plate, true);
    }

    const size_t len = text ? strlen(text) : 0;
    const size_t take = len < sizeof(ui->typing_target) - 1 ? len : sizeof(ui->typing_target) - 1;
    memcpy(ui->typing_target, text ? text : "", take);
    ui->typing_target[take] = '\0';
    ui->typing_total = (uint32_t)take;
    ui->typing_index = 0;
    ui->typing_shown[0] = '\0';
    lv_label_set_text(ui->body, "");

    if (ms_per_char == 0 || take == 0) {
        atri_ui_finish_typing(ui);
        return;
    }
    ui->typing_step_ms = ms_per_char;
    ui->typing_active = true;
    if (ui->typewriter) {
        lv_timer_set_period(ui->typewriter, ms_per_char);
        lv_timer_reset(ui->typewriter);
        lv_timer_resume(ui->typewriter);
    }
}

void atri_ui_finish_typing(atri_ui_t *ui)
{
    if (!ui) return;
    ui->typing_active = false;
    if (ui->typewriter) lv_timer_pause(ui->typewriter);
    ui->typing_index = ui->typing_total;
    memcpy(ui->typing_shown, ui->typing_target, ui->typing_total + 1);
    lv_label_set_text(ui->body, ui->typing_shown);
}

bool atri_ui_typing(const atri_ui_t *ui)
{
    return ui && ui->typing_active;
}

// 画面区浮层:章节与页码用中文(“第N章”),电量用半角数字。
void atri_ui_set_progress(atri_ui_t *ui, int chapter, int page, int pages)
{
    if (!ui) return;
    char buf[24];
    if (chapter > 0) {
        lv_snprintf(buf, sizeof(buf), "第%d章", chapter);
        lv_label_set_text(ui->progress, buf);
        set_hidden(ui->progress, false);
    } else {
        set_hidden(ui->progress, true);
    }
    if (pages > 1) {
        lv_snprintf(buf, sizeof(buf), "%d/%d", page, pages);
        lv_label_set_text(ui->page_hint, buf);
        set_hidden(ui->page_hint, false);
    } else {
        set_hidden(ui->page_hint, true);
    }
}

void atri_ui_set_auto(atri_ui_t *ui, bool on)
{
    if (!ui) return;
    ui->auto_on = on;
    set_hidden(ui->auto_hint, !on || ui->page_current != ATRI_PAGE_GAME);
}

void atri_ui_set_battery(atri_ui_t *ui, int percent)
{
    if (!ui) return;
    char buf[8];
    if (percent < 0) {
        lv_label_set_text(ui->battery, "--%");
    } else {
        lv_snprintf(buf, sizeof(buf), "%d%%", percent);
        lv_label_set_text(ui->battery, buf);
    }
}

void atri_ui_show_choices(atri_ui_t *ui, const char *first, const char *second)
{
    if (!ui) return;
    lv_label_set_text(ui->choice_texts[0], first ? first : "");
    lv_obj_set_style_text_align(ui->choice_texts[0], LV_TEXT_ALIGN_CENTER, 0);
    if (second) {
        lv_label_set_text(ui->choice_texts[1], second);
        ui->choice_count = 2;
        set_hidden(ui->choice_rows[1], false);
    } else {
        ui->choice_count = 1;
        set_hidden(ui->choice_rows[1], true);
    }
    ui->choice_selected = 0;
    atri_ui_set_choice_selected(ui, 0);
    set_hidden(ui->choice_box, false);
}

void atri_ui_hide_choices(atri_ui_t *ui)
{
    if (!ui) return;
    ui->choice_count = 0;
    set_hidden(ui->choice_box, true);
}

void atri_ui_set_choice_selected(atri_ui_t *ui, int index)
{
    if (!ui) return;
    if (index < 0) index = 0;
    if (ui->choice_count > 0 && index >= ui->choice_count) index = ui->choice_count - 1;
    ui->choice_selected = index;
    for (int i = 0; i < 2; ++i) {
        const bool on = (i == index);
        lv_obj_set_style_bg_color(ui->choice_rows[i], on ? COL_ACCENT : COL_ROW, 0);
        lv_obj_set_style_bg_opa(ui->choice_rows[i], on ? LV_OPA_COVER : LV_OPA_80, 0);
        lv_obj_set_style_text_color(ui->choice_texts[i], on ? COL_DARK : COL_TEXT, 0);
    }
}

int atri_ui_choice_selected(const atri_ui_t *ui)
{
    return ui ? ui->choice_selected : 0;
}

void atri_ui_set_list(atri_ui_t *ui, atri_list_id_t id, const char *title, const char *hint,
                      const char *const *labels, const char *const *values, int count,
                      int selected)
{
    if (!ui || id < 0 || id >= ATRI_LIST_COUNT) return;
    list_update(ui, &ui->lists[id], title, hint, labels, values, count, selected);
}

void atri_ui_set_list_selected(atri_ui_t *ui, atri_list_id_t id, int selected)
{
    if (!ui || id < 0 || id >= ATRI_LIST_COUNT) return;
    atri_list_t *list = &ui->lists[id];
    if (selected < 0) selected = 0;
    if (selected >= list->count) selected = list->count > 0 ? list->count - 1 : 0;
    list->selected = selected;
    for (int i = 0; i < list->count; ++i) list_row_apply(ui, list, i);
}

void atri_ui_set_list_value(atri_ui_t *ui, atri_list_id_t id, int row, const char *value)
{
    if (!ui || id < 0 || id >= ATRI_LIST_COUNT) return;
    atri_list_t *list = &ui->lists[id];
    if (row < 0 || row >= list->count) return;
    lv_label_set_text(list->values[row], value ? value : "");
    set_hidden(list->values[row], !value || !value[0]);
}

void atri_ui_set_ending(atri_ui_t *ui, const char *kicker, const char *name, const char *hint)
{
    if (!ui) return;
    lv_label_set_text(ui->ending_kicker, kicker ? kicker : "");
    lv_label_set_text(ui->ending_name, name ? name : "");
    lv_label_set_text(ui->ending_hint, hint ? hint : "");
}

void atri_ui_set_warning(atri_ui_t *ui, const char *body, const char *hint)
{
    if (!ui) return;
    lv_label_set_text(ui->warning_body, body ? body : "");
    lv_label_set_text(ui->warning_hint, hint ? hint : "");
}

void atri_ui_set_about(atri_ui_t *ui, const char *body)
{
    if (!ui) return;
    lv_label_set_text(ui->about_body, body ? body : "");
    if (ui->page_about) lv_obj_scroll_to_y(ui->page_about, 0, LV_ANIM_OFF);
}

int atri_ui_set_about_scroll(atri_ui_t *ui, int y)
{
    if (!ui || !ui->page_about) return 0;
    const int max_y = lv_obj_get_scroll_bottom(ui->page_about) +
                      lv_obj_get_scroll_top(ui->page_about);
    if (y < 0) y = 0;
    if (y > max_y) y = max_y;
    lv_obj_scroll_to_y(ui->page_about, y, LV_ANIM_OFF);
    return y;
}

void atri_ui_notice(atri_ui_t *ui, const char *text)
{
    if (!ui || !ui->notice) return;
    lv_obj_t *label = (lv_obj_t *)lv_obj_get_user_data(ui->notice);
    if (label) lv_label_set_text(label, text ? text : "");
    const bool empty = !text || !text[0];
    set_hidden(ui->notice, empty);
    if (!empty) lv_obj_move_foreground(ui->notice);
}
