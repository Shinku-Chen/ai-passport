// main/dracu_ui.c —— LVGL 界面实现(《千恋＊万花》阅读器,从 ATRI 阅读器移植)。
#include "dracu_ui.h"

#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "dracu_ui";

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

static void list_row_apply(dracu_ui_t *ui, dracu_list_t *list, int index)
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
    dracu_ui_t *ui = (dracu_ui_t *)lv_timer_get_user_data(timer);
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
static void list_build(dracu_ui_t *ui, dracu_list_t *list, lv_obj_t *page, int row_h, int top)
{
    list->page = page;
    list->row_h = row_h;
    list->top = top;
    list->title = new_label(page, ui->font_cjk, COL_TEXT, "");
    lv_obj_set_pos(list->title, 12, 8);
    // 提示行是中文,必须用中文字体(Montserrat 没有汉字,会显示成乱码)。
    list->hint = new_label(page, ui->font_cjk, COL_DIM, "");
    lv_obj_set_pos(list->hint, 12, DRACU_UI_H - 22);
    for (int i = 0; i < DRACU_UI_MAX_ROWS; ++i) {
        lv_obj_t *row = new_box(page, 12, top + i * (row_h + 2), DRACU_UI_W - 24, row_h,
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

static void list_update(dracu_ui_t *ui, dracu_list_t *list, const char *title, const char *hint,
                        const char *const *labels, const char *const *values, int count,
                        int selected)
{
    if (count > DRACU_UI_MAX_ROWS) count = DRACU_UI_MAX_ROWS;
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
    for (int i = 0; i < DRACU_UI_MAX_ROWS; ++i) {
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
static void build_overlays(dracu_ui_t *ui, lv_obj_t *screen)
{
    // 正文带本身(半透明蓝底)是画布的一部分,见 dracu_image.c 的 draw_text_band();
    // 这里只放一个透明容器装文字,不再盖一层不透明底色,立绘才能透出来。
    ui->box = new_box(screen, 0, DRACU_BOX_Y, DRACU_UI_W, DRACU_BOX_H, COL_BOX_BG, LV_OPA_TRANSP);

    // 说话人名牌:直接挂在屏幕上(不挂在文本框里),位置固定在正文带上沿。
    // 挂在父容器里时,LVGL 默认会把超出父容器的子对象裁掉,名牌会被整块切没。
    ui->name_plate = new_box(screen, 6, DRACU_BOX_Y - 28, 10, 26, COL_DEEP, LV_OPA_COVER);
    lv_obj_set_style_radius(ui->name_plate, 6, 0);
    lv_obj_set_style_pad_hor(ui->name_plate, 8, 0);
    ui->name_text = new_label(ui->name_plate, ui->font_cjk, COL_TEXT, "");
    lv_obj_align(ui->name_text, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_align(ui->name_text, LV_TEXT_ALIGN_CENTER, 0);

    ui->body = new_label(ui->box, ui->font_cjk, COL_TEXT, "");
    lv_obj_set_size(ui->body, DRACU_UI_W - 16, DRACU_TEXT_LINES * DRACU_LINE_H);
    lv_obj_set_pos(ui->body, 8, 7);
    lv_label_set_long_mode(ui->body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(ui->body, DRACU_LINE_H - 16, 0);

    // 画面区浮层:章节 / 电量 / 页码。章节标签是中文,用中文字体。
    ui->progress = new_label(screen, ui->font_cjk, COL_DIM, "");
    lv_obj_set_pos(ui->progress, 8, 6);
    lv_obj_set_style_text_opa(ui->progress, LV_OPA_70, 0);
    ui->battery = new_label(screen, ui->font_tiny, COL_DIM, "");
    lv_obj_align(ui->battery, LV_ALIGN_TOP_RIGHT, -8, 6);
    lv_obj_set_style_text_opa(ui->battery, LV_OPA_70, 0);
    // 自动阅读指示:常驻在画面区右下角(用文字,不用图标,省得字体缺字形)。
    ui->auto_hint = new_label(screen, ui->font_cjk, COL_ACCENT, "自动");
    lv_obj_align(ui->auto_hint, LV_ALIGN_BOTTOM_RIGHT, -8,
                 -(DRACU_UI_H - DRACU_BOX_Y) - 6);
    lv_obj_set_style_text_opa(ui->auto_hint, LV_OPA_80, 0);
    set_hidden(ui->auto_hint, true);

    // 标题文字由标题图自带(官方主视觉上就是「千恋＊万花」),不再另外画横幅。

    ui->page_hint = new_label(screen, ui->font_tiny, COL_DIM, "");
    // 页码提示:右下角"自动"指示上方一行,避免两个标签重叠。
    lv_obj_align(ui->page_hint, LV_ALIGN_BOTTOM_RIGHT, -8, -(DRACU_UI_H - DRACU_BOX_Y) - 28);
    lv_obj_set_style_text_opa(ui->page_hint, LV_OPA_70, 0);

    // 选项:盖在画面区上的多行按钮(源工程是两个 250x61 的按钮,这里最多
    // DRACU_CHOICE_MAX 行:实测剧本每处最多 3 个选项)。
    ui->choice_box = new_box(screen, 0, 0, DRACU_UI_W, DRACU_ART_H, COL_PAGE_BG, LV_OPA_TRANSP);
    for (int i = 0; i < DRACU_UI_CHOICE_MAX; ++i) {
        lv_obj_t *row = new_box(ui->choice_box, 18, 54 + i * 58, DRACU_UI_W - 36, 46, COL_ROW,
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

static void build_pages(dracu_ui_t *ui, lv_obj_t *screen)
{
    // 标题页:标题图整屏铺底(在画布层),这里是叠在下方那条正文带上的菜单。
    ui->page_title = new_box(screen, 0, DRACU_BOX_Y, DRACU_UI_W, DRACU_BOX_H, COL_BOX_BG,
                             LV_OPA_TRANSP);
    list_build(ui, &ui->lists[DRACU_LIST_TITLE], ui->page_title, 20, 6);
    set_hidden(ui->lists[DRACU_LIST_TITLE].title, true);
    set_hidden(ui->lists[DRACU_LIST_TITLE].hint, true);

    // 章节列表页(源工程的流程图页对应物):一页 7 行,超出由应用滚动。
    ui->page_chapters = new_box(screen, 0, 0, DRACU_UI_W, DRACU_UI_H, COL_PAGE_BG, LV_OPA_COVER);
    list_build(ui, &ui->lists[DRACU_LIST_CHAPTERS], ui->page_chapters, 28, 40);

    // 整屏页:菜单 / 设置 / 存档 / 结局 / 关于
    ui->page_settings = new_box(screen, 0, 0, DRACU_UI_W, DRACU_UI_H, COL_PAGE_BG, LV_OPA_COVER);
    list_build(ui, &ui->lists[DRACU_LIST_SETTINGS], ui->page_settings, 30, 48);

    lv_obj_t *page_menu = new_box(screen, 0, 0, DRACU_UI_W, DRACU_UI_H, COL_PAGE_BG, LV_OPA_90);
    list_build(ui, &ui->lists[DRACU_LIST_MENU], page_menu, 30, 48);
    ui->lists[DRACU_LIST_MENU].page = page_menu;

    ui->page_slots = new_box(screen, 0, 0, DRACU_UI_W, DRACU_UI_H, COL_PAGE_BG, LV_OPA_COVER);
    list_build(ui, &ui->lists[DRACU_LIST_SLOTS], ui->page_slots, 30, 44);
    // 关于页:内容比一屏长,靠按键滚动(三键设备没有触摸滑屏)。
    ui->page_about = new_box(screen, 0, 0, DRACU_UI_W, DRACU_UI_H, COL_PAGE_BG, LV_OPA_COVER);
    lv_obj_set_scrollable(ui->page_about, true);
    lv_obj_set_scroll_dir(ui->page_about, LV_DIR_VER);
    lv_obj_set_style_pad_all(ui->page_about, 0, 0);
    ui->about_body = new_label(ui->page_about, ui->font_cjk, COL_TEXT, "");
    lv_obj_set_width(ui->about_body, DRACU_UI_W - 24);
    lv_obj_set_pos(ui->about_body, 12, 16);
    lv_label_set_long_mode(ui->about_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(ui->about_body, 4, 0);
    lv_obj_set_style_pad_bottom(ui->about_body, 20, 0);

    ui->page_ending = new_box(screen, 0, 0, DRACU_UI_W, DRACU_UI_H, COL_PAGE_BG, LV_OPA_COVER);
    ui->ending_kicker = new_label(ui->page_ending, ui->font_cjk, COL_DIM, "达成结局");
    lv_obj_align(ui->ending_kicker, LV_ALIGN_TOP_MID, 0, 90);
    ui->ending_name = new_label(ui->page_ending, ui->font_cjk, COL_ACCENT, "");
    lv_obj_align(ui->ending_name, LV_ALIGN_TOP_MID, 0, 130);
    ui->ending_hint = new_label(ui->page_ending, ui->font_cjk, COL_TEXT, "");
    lv_obj_align(ui->ending_hint, LV_ALIGN_BOTTOM_MID, 0, -34);
    lv_obj_set_style_text_align(ui->ending_hint, LV_TEXT_ALIGN_CENTER, 0);

    // 瞬时提示:最后创建,永远在最上层。
    ui->notice = new_box(screen, 24, DRACU_UI_H - 66, DRACU_UI_W - 48, 34, COL_DEEP, LV_OPA_90);
    lv_obj_set_style_radius(ui->notice, 8, 0);
    lv_obj_t *notice_text = new_label(ui->notice, ui->font_cjk, COL_TEXT, "");
    lv_obj_align(notice_text, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_align(notice_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_user_data(ui->notice, notice_text);
    set_hidden(ui->notice, true);

    ui->page_game = NULL;   // 正文页由 box / 浮层组成,没有单独容器
}

bool dracu_ui_create(dracu_ui_t *ui, uint16_t *art_pixels, const lv_font_t *font_cjk,
                     const dracu_pack_t *pack)
{
    if (!ui || !art_pixels || !font_cjk || !pack) return false;
    memset(ui, 0, sizeof(*ui));
    ui->font_cjk = font_cjk;
    ui->font_tiny = &lv_font_montserrat_14;

    lv_obj_t *screen = lv_screen_active();
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    if (!dracu_image_init(&ui->art, screen, art_pixels, pack)) return false;
    build_overlays(ui, screen);
    build_pages(ui, screen);

    ui->typewriter = lv_timer_create(typewriter_tick, 35, ui);
    if (!ui->typewriter) {
        ESP_LOGE(TAG, "打字机定时器创建失败");
        return false;
    }
    lv_timer_pause(ui->typewriter);
    dracu_ui_show_page(ui, DRACU_PAGE_TITLE);
    return true;
}

void dracu_ui_show_page(dracu_ui_t *ui, dracu_screen_t page)
{
    if (!ui) return;
    ui->page_current = page;
    set_hidden(ui->box, page != DRACU_PAGE_GAME);
    set_hidden(ui->auto_hint, page != DRACU_PAGE_GAME || !ui->auto_on);
    set_hidden(ui->progress, page != DRACU_PAGE_GAME);
    // 电量在标题页也显示(同一位于画面区右上角)。
    set_hidden(ui->battery, page != DRACU_PAGE_GAME && page != DRACU_PAGE_TITLE);
    set_hidden(ui->page_hint, page != DRACU_PAGE_GAME);
    set_hidden(ui->choice_box, page != DRACU_PAGE_GAME || ui->choice_count == 0);
    // 名牌在正文页由 set_text 按"这句有没有说话人"决定显隐,离开正文页一律收起。
    if (page != DRACU_PAGE_GAME) set_hidden(ui->name_plate, true);
    set_hidden(ui->page_title, page != DRACU_PAGE_TITLE);
    set_hidden(ui->lists[DRACU_LIST_TITLE].page, page != DRACU_PAGE_TITLE);
    set_hidden(ui->page_chapters, page != DRACU_PAGE_CHAPTERS);
    set_hidden(ui->page_settings, page != DRACU_PAGE_SETTINGS);
    set_hidden(ui->lists[DRACU_LIST_MENU].page, page != DRACU_PAGE_MENU);
    set_hidden(ui->page_slots, page != DRACU_PAGE_SLOTS);
    set_hidden(ui->page_ending, page != DRACU_PAGE_ENDING);
    set_hidden(ui->page_about, page != DRACU_PAGE_ABOUT);

    if (page != DRACU_PAGE_GAME) {
        ui->typing_active = false;
        if (ui->typewriter) lv_timer_pause(ui->typewriter);
    }
    // 整屏页面按创建顺序叠放,把当前页提到最前,避免被后建的页面盖住。
    lv_obj_t *front = NULL;
    switch (page) {
    case DRACU_PAGE_TITLE: front = ui->lists[DRACU_LIST_TITLE].page; break;
    case DRACU_PAGE_CHAPTERS: front = ui->page_chapters; break;
    case DRACU_PAGE_MENU: front = ui->lists[DRACU_LIST_MENU].page; break;
    case DRACU_PAGE_SETTINGS: front = ui->page_settings; break;
    case DRACU_PAGE_SLOTS: front = ui->page_slots; break;
    case DRACU_PAGE_ENDING: front = ui->page_ending; break;
    case DRACU_PAGE_ABOUT: front = ui->page_about; break;
    case DRACU_PAGE_GAME: break;
    }
    if (front) lv_obj_move_foreground(front);
}

void dracu_ui_compose(dracu_ui_t *ui, const dracu_layers_t *layers)
{
    if (!ui || !layers) return;
    dracu_image_compose(&ui->art, layers);
}

void dracu_ui_set_text(dracu_ui_t *ui, const char *speaker, const char *text, uint32_t ms_per_char)
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
        dracu_ui_finish_typing(ui);
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

void dracu_ui_finish_typing(dracu_ui_t *ui)
{
    if (!ui) return;
    ui->typing_active = false;
    if (ui->typewriter) lv_timer_pause(ui->typewriter);
    ui->typing_index = ui->typing_total;
    memcpy(ui->typing_shown, ui->typing_target, ui->typing_total + 1);
    lv_label_set_text(ui->body, ui->typing_shown);
}

bool dracu_ui_typing(const dracu_ui_t *ui)
{
    return ui && ui->typing_active;
}

// 画面区浮层:章节与页码用中文(“第N章”),电量用半角数字。
void dracu_ui_set_progress(dracu_ui_t *ui, const char *chapter, int page, int pages)
{
    if (!ui) return;
    char buf[24];
    // 章节文案由调用方给(源数据是 CHAPTERx-y,画面上要显示 x-y)。
    if (chapter != NULL && chapter[0] != '\0') {
        lv_snprintf(buf, sizeof(buf), "%s", chapter);
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

void dracu_ui_set_auto(dracu_ui_t *ui, bool on)
{
    if (!ui) return;
    ui->auto_on = on;
    set_hidden(ui->auto_hint, !on || ui->page_current != DRACU_PAGE_GAME);
}

void dracu_ui_set_battery(dracu_ui_t *ui, int percent)
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

void dracu_ui_show_choices(dracu_ui_t *ui, const char *const *texts, int count)
{
    if (!ui) return;
    if (count > DRACU_UI_CHOICE_MAX) count = DRACU_UI_CHOICE_MAX;
    if (count < 0) count = 0;
    for (int i = 0; i < DRACU_UI_CHOICE_MAX; ++i) {
        const bool used = i < count;
        lv_label_set_text(ui->choice_texts[i], (used && texts && texts[i]) ? texts[i] : "");
        lv_obj_set_style_text_align(ui->choice_texts[i], LV_TEXT_ALIGN_CENTER, 0);
        set_hidden(ui->choice_rows[i], !used);
    }
    ui->choice_count = count;
    ui->choice_selected = 0;
    dracu_ui_set_choice_selected(ui, 0);
    set_hidden(ui->choice_box, count == 0);
}

void dracu_ui_hide_choices(dracu_ui_t *ui)
{
    if (!ui) return;
    ui->choice_count = 0;
    set_hidden(ui->choice_box, true);
}

void dracu_ui_set_choice_selected(dracu_ui_t *ui, int index)
{
    if (!ui) return;
    if (index < 0) index = 0;
    if (ui->choice_count > 0 && index >= ui->choice_count) index = ui->choice_count - 1;
    ui->choice_selected = index;
    for (int i = 0; i < DRACU_UI_CHOICE_MAX; ++i) {
        const bool on = (i == index);
        lv_obj_set_style_bg_color(ui->choice_rows[i], on ? COL_ACCENT : COL_ROW, 0);
        lv_obj_set_style_bg_opa(ui->choice_rows[i], on ? LV_OPA_COVER : LV_OPA_80, 0);
        lv_obj_set_style_text_color(ui->choice_texts[i], on ? COL_DARK : COL_TEXT, 0);
    }
}

int dracu_ui_choice_selected(const dracu_ui_t *ui)
{
    return ui ? ui->choice_selected : 0;
}

void dracu_ui_set_list(dracu_ui_t *ui, dracu_list_id_t id, const char *title, const char *hint,
                      const char *const *labels, const char *const *values, int count,
                      int selected)
{
    if (!ui || id < 0 || id >= DRACU_LIST_COUNT) return;
    list_update(ui, &ui->lists[id], title, hint, labels, values, count, selected);
}

void dracu_ui_set_list_selected(dracu_ui_t *ui, dracu_list_id_t id, int selected)
{
    if (!ui || id < 0 || id >= DRACU_LIST_COUNT) return;
    dracu_list_t *list = &ui->lists[id];
    if (selected < 0) selected = 0;
    if (selected >= list->count) selected = list->count > 0 ? list->count - 1 : 0;
    list->selected = selected;
    for (int i = 0; i < list->count; ++i) list_row_apply(ui, list, i);
}

void dracu_ui_set_list_value(dracu_ui_t *ui, dracu_list_id_t id, int row, const char *value)
{
    if (!ui || id < 0 || id >= DRACU_LIST_COUNT) return;
    dracu_list_t *list = &ui->lists[id];
    if (row < 0 || row >= list->count) return;
    lv_label_set_text(list->values[row], value ? value : "");
    set_hidden(list->values[row], !value || !value[0]);
}

void dracu_ui_set_ending(dracu_ui_t *ui, const char *kicker, const char *name, const char *hint)
{
    if (!ui) return;
    lv_label_set_text(ui->ending_kicker, kicker ? kicker : "");
    lv_label_set_text(ui->ending_name, name ? name : "");
    lv_label_set_text(ui->ending_hint, hint ? hint : "");
}

void dracu_ui_set_about(dracu_ui_t *ui, const char *body)
{
    if (!ui) return;
    lv_label_set_text(ui->about_body, body ? body : "");
    if (ui->page_about) lv_obj_scroll_to_y(ui->page_about, 0, LV_ANIM_OFF);
}

int dracu_ui_set_about_scroll(dracu_ui_t *ui, int y)
{
    if (!ui || !ui->page_about) return 0;
    const int max_y = lv_obj_get_scroll_bottom(ui->page_about) +
                      lv_obj_get_scroll_top(ui->page_about);
    if (y < 0) y = 0;
    if (y > max_y) y = max_y;
    lv_obj_scroll_to_y(ui->page_about, y, LV_ANIM_OFF);
    return y;
}

void dracu_ui_notice(dracu_ui_t *ui, const char *text)
{
    if (!ui || !ui->notice) return;
    lv_obj_t *label = (lv_obj_t *)lv_obj_get_user_data(ui->notice);
    if (label) lv_label_set_text(label, text ? text : "");
    const bool empty = !text || !text[0];
    set_hidden(ui->notice, empty);
    if (!empty) lv_obj_move_foreground(ui->notice);
}
