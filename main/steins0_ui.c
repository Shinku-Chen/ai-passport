// main/steins0_ui.c —— 作品专属界面(深色世界线主题)。
#include "steins0_ui.h"

#include <stdio.h>
#include <string.h>

#define C_BG 0x0B0D12
#define C_PANEL 0x121722
#define C_PANEL_DARK 0x07090D
#define C_TEXT 0xE8E4DC
#define C_DIM 0x9A958C
#define C_ACCENT 0xD24A3E
#define C_ACCENT_DIM 0x7A2A24
#define C_SELECT 0xF2C14E
#define C_BORDER 0x2A303C

#if defined(ESP_PLATFORM)
#include "esp_log.h"
#endif

static void ui_mem_trace(const char *phase, steins0_page_t page)
{
#if defined(ESP_PLATFORM)
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    ESP_LOGI("steins0_ui", "mem %s page=%d free=%u largest=%u max_used=%u frag=%u",
             phase, (int)page, (unsigned)mon.free_size,
             (unsigned)mon.free_biggest_size, (unsigned)mon.max_used,
             (unsigned)mon.frag_pct);
#else
    (void)phase;
    (void)page;
#endif
}

static uint32_t ui_next_codepoint(const char **cursor)
{
    const unsigned char *p = (const unsigned char *)*cursor;
    if (!p || !*p) return 0;
    uint32_t cp;
    if (p[0] < 0x80) {
        cp = p[0];
        *cursor = (const char *)(p + 1);
    } else if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        cp = ((uint32_t)(p[0] & 0x1Fu) << 6) | (uint32_t)(p[1] & 0x3Fu);
        *cursor = (const char *)(p + 2);
    } else if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 &&
               (p[2] & 0xC0) == 0x80) {
        cp = ((uint32_t)(p[0] & 0x0Fu) << 12) | ((uint32_t)(p[1] & 0x3Fu) << 6) |
             (uint32_t)(p[2] & 0x3Fu);
        *cursor = (const char *)(p + 3);
    } else if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 &&
               (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        cp = ((uint32_t)(p[0] & 0x07u) << 18) | ((uint32_t)(p[1] & 0x3Fu) << 12) |
             ((uint32_t)(p[2] & 0x3Fu) << 6) | (uint32_t)(p[3] & 0x3Fu);
        *cursor = (const char *)(p + 4);
    } else {
        *cursor = (const char *)(p + 1);
        return 0xFFFDu;
    }
    return cp;
}

static bool ui_font_covers(const lv_font_t *font, const char *text)
{
    if (!font || !text) return false;
    const char *cursor = text;
    while (*cursor) {
        const uint32_t cp = ui_next_codepoint(&cursor);
        if (cp == 0 || cp == 0xFFFDu) return false;
        lv_font_glyph_dsc_t glyph;
        if (!lv_font_get_glyph_dsc(font, &glyph, cp, 0)) return false;
    }
    return true;
}

/* Best-effort, allocation-free failure notice. The label text is static. */
static void ui_fail_notice(steins0_ui_t *ui)
{
    if (!ui || !ui->notice_box || !ui->notice_text) return;
    const char *message = "ERR";
    if (ui_font_covers(ui->font, "内存不足")) message = "内存不足";
    else if (ui_font_covers(ui->font, "请稍后重试")) message = "请稍后重试";
    lv_label_set_text_static(ui->notice_text, message);
    lv_obj_remove_flag(ui->notice_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ui->notice_box);
}

static bool s_page_alloc_failed;

static lv_obj_t *ui_obj_create(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    if (!obj) s_page_alloc_failed = true;
    return obj;
}

static lv_obj_t *panel(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, int opa,
                       int radius, int border, uint32_t border_color)
{
    lv_obj_t *obj = ui_obj_create(parent);
    if (!obj) return NULL;
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, (lv_opa_t)opa, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    if (border) {
        lv_obj_set_style_border_width(obj, 1, 0);
        lv_obj_set_style_border_color(obj, lv_color_hex(border_color), 0);
    }
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

static lv_obj_t *page_label_obj(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_label_create(parent);
    if (!obj) s_page_alloc_failed = true;
    return obj;
}

static lv_obj_t *label(lv_obj_t *parent, int x, int y, int w, int h, const char *text,
                       const lv_font_t *font, uint32_t color)
{
    lv_obj_t *obj = page_label_obj(parent);
    if (!obj) return NULL;
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_CLIP);
    lv_label_set_text(obj, text ? text : "");
    return obj;
}

static void hide_all(steins0_ui_t *ui)
{
    lv_obj_t *pages[] = {
        ui->page_title, ui->page_game, ui->page_list, ui->page_cg,
        ui->page_history, ui->page_about, ui->page_ending, ui->page_confirm,
        ui->page_phone, ui->page_phone_thread, ui->page_card,
    };
    for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); ++i) {
        if (pages[i]) lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t *page_object(steins0_ui_t *ui, steins0_page_t page)
{
    if (!ui) return NULL;
    switch (page) {
    case SG0_PAGE_TITLE: return ui->page_title;
    case SG0_PAGE_GAME: return ui->page_game;
    case SG0_PAGE_LIST: return ui->page_list;
    case SG0_PAGE_CG: return ui->page_cg;
    case SG0_PAGE_HISTORY: return ui->page_history;
    case SG0_PAGE_ABOUT: return ui->page_about;
    case SG0_PAGE_ENDING: return ui->page_ending;
    case SG0_PAGE_CONFIRM: return ui->page_confirm;
    case SG0_PAGE_PHONE: return ui->page_phone;
    case SG0_PAGE_PHONE_THREAD: return ui->page_phone_thread;
    case SG0_PAGE_CARD: return ui->page_card;
    default: return NULL;
    }
}

static void clear_page_ptrs(steins0_ui_t *ui, steins0_page_t page)
{
    if (!ui) return;
    switch (page) {
    case SG0_PAGE_TITLE:
        ui->page_title = NULL;
        ui->title_label = NULL;
        ui->title_battery = NULL;
        for (int i = 0; i < 6; ++i) ui->title_rows[i] = NULL;
        break;
    case SG0_PAGE_GAME:
        ui->page_game = NULL;
        ui->game_chapter = NULL;
        ui->game_battery = NULL;
        ui->game_rine = NULL;
        ui->speaker_label = NULL;
        ui->page_label = NULL;
        ui->choice_box = NULL;
        ui->choice_heading = NULL;
        for (int i = 0; i < 3; ++i) ui->text_lines[i] = NULL;
        for (int i = 0; i < 2; ++i) ui->choice_rows[i] = NULL;
        break;
    case SG0_PAGE_LIST:
        ui->page_list = NULL;
        ui->list_title = NULL;
        ui->list_footer = NULL;
        for (int i = 0; i < SG0_LIST_ROWS; ++i) {
            ui->list_rows[i] = NULL;
            ui->list_values[i] = NULL;
        }
        break;
    case SG0_PAGE_CG:
        ui->page_cg = NULL;
        ui->cg_caption = NULL;
        ui->cg_variant = NULL;
        break;
    case SG0_PAGE_HISTORY:
        ui->page_history = NULL;
        ui->history_view = NULL;
        ui->history_label = NULL;
        ui->history_hint = NULL;
        break;
    case SG0_PAGE_ABOUT:
        ui->page_about = NULL;
        ui->about_view = NULL;
        ui->about_label = NULL;
        break;
    case SG0_PAGE_ENDING:
        ui->page_ending = NULL;
        ui->ending_title = NULL;
        ui->ending_label = NULL;
        ui->ending_hint = NULL;
        break;
    case SG0_PAGE_CONFIRM:
        ui->page_confirm = NULL;
        ui->confirm_prompt = NULL;
        ui->confirm_rows[0] = NULL;
        ui->confirm_rows[1] = NULL;
        break;
    case SG0_PAGE_PHONE:
        ui->page_phone = NULL;
        ui->phone_title = NULL;
        ui->phone_sub = NULL;
        ui->phone_page_label = NULL;
        ui->phone_footer = NULL;
        for (int i = 0; i < SG0_LIST_ROWS; ++i) {
            ui->phone_rows[i] = NULL;
            ui->phone_values[i] = NULL;
        }
        break;
    case SG0_PAGE_PHONE_THREAD:
        ui->page_phone_thread = NULL;
        ui->phone_thread_header = NULL;
        ui->phone_thread_sender = NULL;
        ui->phone_thread_hint = NULL;
        for (int i = 0; i < 3; ++i) ui->phone_lines[i] = NULL;
        memset(&ui->stamp, 0, sizeof(ui->stamp));
        break;
    case SG0_PAGE_CARD:
        ui->page_card = NULL;
        ui->card_title = NULL;
        break;
    default:
        break;
    }
}

static bool create_page_title(steins0_ui_t *ui)
{
    // ---- 标题页
    ui->page_title = ui_obj_create(ui->root);
    if (!ui->page_title) return false;
    lv_obj_remove_style_all(ui->page_title);
    lv_obj_set_size(ui->page_title, SG0_SCREEN_W, SG0_SCREEN_H);
    lv_obj_remove_flag(ui->page_title, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title_chip = panel(ui->page_title, 4, 4, 176, 22, C_PANEL_DARK, 210, 3, 0, 0);
    (void)title_chip;
    ui->title_label = label(ui->page_title, 10, 5, 166, 20, "命运石之门 0", ui->font, C_TEXT);
    ui->title_battery = label(ui->page_title, 254, 5, 62, 20, "", ui->font, C_DIM);
    lv_obj_set_style_text_align(ui->title_battery, LV_TEXT_ALIGN_RIGHT, 0);
    panel(ui->page_title, 0, 180, SG0_SCREEN_W, 60, C_PANEL_DARK, 235, 0, 1, C_BORDER);
    for (int i = 0; i < 6; ++i) {
        const int col = i % 3;
        const int row = i / 3;
        ui->title_rows[i] = label(ui->page_title, 2 + col * 106, 184 + row * 26, 104, 22, "",
                                  ui->font, C_TEXT);
        lv_obj_set_style_text_align(ui->title_rows[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_bg_opa(ui->title_rows[i], LV_OPA_TRANSP, 0);
    }

    return ui->page_title != NULL;
}

static bool create_page_game(steins0_ui_t *ui)
{
    // ---- 正文页
    ui->page_game = ui_obj_create(ui->root);
    if (!ui->page_game) return false;
    lv_obj_remove_style_all(ui->page_game);
    lv_obj_set_size(ui->page_game, SG0_SCREEN_W, SG0_SCREEN_H);
    lv_obj_remove_flag(ui->page_game, LV_OBJ_FLAG_SCROLLABLE);
    // The chapter/date row has no background bar; keep the labels visible.
    ui->game_chapter = label(ui->page_game, 9, 4, 180, 19, "", ui->font, C_DIM);
    lv_obj_set_style_bg_opa(ui->game_chapter, LV_OPA_TRANSP, 0);
    ui->game_battery = label(ui->page_game, 256, 4, 60, 19, "", ui->font, C_DIM);
    lv_obj_set_style_text_align(ui->game_battery, LV_TEXT_ALIGN_RIGHT, 0);
    ui->game_rine = label(ui->page_game, 192, 4, 60, 19, "", ui->font, C_SELECT);
    panel(ui->page_game, 0, 168, SG0_SCREEN_W, 72, C_PANEL, 180, 0, 1, C_ACCENT_DIM);
    // 名字移到框外（压在画面上），用面板同色半透明底条保证对比；页码独占框内右上。
    ui->speaker_label = label(ui->page_game, 8, 149, 220, 19, "", ui->font, C_SELECT);
    lv_obj_set_width(ui->speaker_label, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_left(ui->speaker_label, 6, 0);
    lv_obj_set_style_pad_right(ui->speaker_label, 6, 0);
    lv_obj_set_style_bg_color(ui->speaker_label, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_bg_opa(ui->speaker_label, 200, 0);
    ui->page_label = label(ui->page_game, 250, 152, 64, 19, "", ui->font, C_DIM);
    lv_obj_set_style_text_align(ui->page_label, LV_TEXT_ALIGN_RIGHT, 0);
    for (int i = 0; i < 3; ++i) {
        ui->text_lines[i] = label(ui->page_game, 8, 172 + i * 19, 304, 19, "", ui->font, C_TEXT);
    }

    ui->choice_box = ui_obj_create(ui->page_game);
    if (!ui->choice_box) return false;
    lv_obj_remove_style_all(ui->choice_box);
    lv_obj_set_pos(ui->choice_box, 0, 64);
    lv_obj_set_size(ui->choice_box, SG0_SCREEN_W, 176);
    lv_obj_set_style_bg_color(ui->choice_box, lv_color_hex(C_PANEL_DARK), 0);
    lv_obj_set_style_bg_opa(ui->choice_box, 238, 0);
    lv_obj_set_style_border_width(ui->choice_box, 1, 0);
    lv_obj_set_style_border_color(ui->choice_box, lv_color_hex(C_ACCENT_DIM), 0);
    lv_obj_remove_flag(ui->choice_box, LV_OBJ_FLAG_SCROLLABLE);
    ui->choice_heading = label(ui->choice_box, 10, 10, 300, 22, "", ui->font, C_DIM);
    for (int i = 0; i < 2; ++i) {
        ui->choice_rows[i] = label(ui->choice_box, 14, 46 + i * 30, 292, 26, "", ui->font, C_TEXT);
        lv_obj_set_style_bg_opa(ui->choice_rows[i], LV_OPA_TRANSP, 0);
    }
    label(ui->choice_box, 10, 142, 300, 20, "短按 OK 确认 / 长按 OK 菜单", ui->font, C_DIM);
    lv_obj_add_flag(ui->choice_box, LV_OBJ_FLAG_HIDDEN);

    return ui->page_game != NULL;
}

static bool create_page_list(steins0_ui_t *ui)
{
    // ---- 列表页(菜单/存档/设置/章节/鉴赏)
    ui->page_list = ui_obj_create(ui->root);
    if (!ui->page_list) return false;
    lv_obj_remove_style_all(ui->page_list);
    lv_obj_set_size(ui->page_list, SG0_SCREEN_W, SG0_SCREEN_H);
    lv_obj_set_style_bg_color(ui->page_list, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(ui->page_list, LV_OPA_COVER, 0);
    lv_obj_remove_flag(ui->page_list, LV_OBJ_FLAG_SCROLLABLE);
    ui->list_title = label(ui->page_list, 10, 4, 300, 22, "", ui->font, C_TEXT);
    panel(ui->page_list, 8, 27, 304, 1, C_BORDER, 255, 0, 0, 0);
    for (int i = 0; i < SG0_LIST_ROWS; ++i) {
        ui->list_rows[i] = label(ui->page_list, 12, 32 + i * 23, 230, 21, "", ui->font, C_TEXT);
        ui->list_values[i] = label(ui->page_list, 244, 32 + i * 23, 68, 21, "", ui->font, C_DIM);
        lv_obj_set_style_text_align(ui->list_values[i], LV_TEXT_ALIGN_RIGHT, 0);
    }
    ui->list_footer = label(ui->page_list, 10, 220, 300, 18, "", ui->font, C_DIM);

    return ui->page_list != NULL;
}

static bool create_page_cg(steins0_ui_t *ui)
{
    // ---- CG 全屏
    ui->page_cg = ui_obj_create(ui->root);
    if (!ui->page_cg) return false;
    lv_obj_remove_style_all(ui->page_cg);
    lv_obj_set_size(ui->page_cg, SG0_SCREEN_W, SG0_SCREEN_H);
    lv_obj_remove_flag(ui->page_cg, LV_OBJ_FLAG_SCROLLABLE);
    panel(ui->page_cg, 0, 206, SG0_SCREEN_W, 34, C_PANEL_DARK, 235, 0, 1, C_BORDER);
    ui->cg_caption = label(ui->page_cg, 8, 208, 250, 20, "", ui->font, C_TEXT);
    ui->cg_variant = label(ui->page_cg, 240, 208, 74, 20, "", ui->font, C_DIM);
    lv_obj_set_style_text_align(ui->cg_variant, LV_TEXT_ALIGN_RIGHT, 0);
    label(ui->page_cg, 8, 224, 300, 18, "上/下 切换差分 / 长按 OK 返回", ui->font, C_DIM);

    return ui->page_cg != NULL;
}

static bool create_page_history(steins0_ui_t *ui)
{
    // ---- 历史
    ui->page_history = ui_obj_create(ui->root);
    if (!ui->page_history) return false;
    lv_obj_remove_style_all(ui->page_history);
    lv_obj_set_size(ui->page_history, SG0_SCREEN_W, SG0_SCREEN_H);
    lv_obj_set_style_bg_color(ui->page_history, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(ui->page_history, LV_OPA_COVER, 0);
    lv_obj_remove_flag(ui->page_history, LV_OBJ_FLAG_SCROLLABLE);
    label(ui->page_history, 10, 4, 300, 22, "历史记录", ui->font, C_TEXT);
    ui->history_view = ui_obj_create(ui->page_history);
    if (!ui->history_view) return false;
    lv_obj_remove_style_all(ui->history_view);
    lv_obj_set_pos(ui->history_view, 4, 30);
    lv_obj_set_size(ui->history_view, 312, 182);
    lv_obj_set_style_bg_color(ui->history_view, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_bg_opa(ui->history_view, 200, 0);
    lv_obj_set_scroll_dir(ui->history_view, LV_DIR_VER);
    ui->history_label = label(ui->history_view, 6, 6, 294, 1200, "", ui->font, C_TEXT);
    lv_label_set_long_mode(ui->history_label, LV_LABEL_LONG_WRAP);
    ui->history_hint = label(ui->page_history, 8, 216, 304, 18, "OK / 长按 OK 返回",
                             ui->font, C_DIM);

    return ui->page_history != NULL;
}

static bool create_page_about(steins0_ui_t *ui)
{
    // ---- 关于
    ui->page_about = ui_obj_create(ui->root);
    if (!ui->page_about) return false;
    lv_obj_remove_style_all(ui->page_about);
    lv_obj_set_size(ui->page_about, SG0_SCREEN_W, SG0_SCREEN_H);
    lv_obj_set_style_bg_color(ui->page_about, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(ui->page_about, LV_OPA_COVER, 0);
    lv_obj_remove_flag(ui->page_about, LV_OBJ_FLAG_SCROLLABLE);
    label(ui->page_about, 10, 4, 300, 22, "关于本作", ui->font, C_TEXT);
    ui->about_view = ui_obj_create(ui->page_about);
    if (!ui->about_view) return false;
    lv_obj_remove_style_all(ui->about_view);
    lv_obj_set_pos(ui->about_view, 4, 30);
    lv_obj_set_size(ui->about_view, 312, 202);
    lv_obj_set_style_bg_color(ui->about_view, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_bg_opa(ui->about_view, 200, 0);
    lv_obj_set_scroll_dir(ui->about_view, LV_DIR_VER);
    ui->about_label = label(ui->about_view, 6, 6, 294, 900, "", ui->font, C_TEXT);
    lv_label_set_long_mode(ui->about_label, LV_LABEL_LONG_WRAP);

    return ui->page_about != NULL;
}

static bool create_page_ending(steins0_ui_t *ui)
{
    // ---- 结局
    ui->page_ending = ui_obj_create(ui->root);
    if (!ui->page_ending) return false;
    lv_obj_remove_style_all(ui->page_ending);
    lv_obj_set_size(ui->page_ending, SG0_SCREEN_W, SG0_SCREEN_H);
    lv_obj_set_style_bg_color(ui->page_ending, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(ui->page_ending, LV_OPA_COVER, 0);
    lv_obj_remove_flag(ui->page_ending, LV_OBJ_FLAG_SCROLLABLE);
    ui->ending_title = label(ui->page_ending, 16, 52, 288, 20, "ENDING", ui->font, C_DIM);
    lv_obj_set_style_text_align(ui->ending_title, LV_TEXT_ALIGN_CENTER, 0);
    ui->ending_label = label(ui->page_ending, 16, 84, 288, 90, "", ui->font, C_TEXT);
    lv_obj_set_style_text_align(ui->ending_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(ui->ending_label, LV_LABEL_LONG_WRAP);
    ui->ending_hint = label(ui->page_ending, 16, 200, 288, 20, "", ui->font, C_DIM);
    lv_obj_set_style_text_align(ui->ending_hint, LV_TEXT_ALIGN_CENTER, 0);

    return ui->page_ending != NULL;
}

static bool create_page_confirm(steins0_ui_t *ui)
{
    // ---- 确认弹窗
    ui->page_confirm = panel(ui->root, 24, 64, 272, 116, C_PANEL, 245, 6, 1, C_ACCENT_DIM);
    if (!ui->page_confirm) return false;
    ui->confirm_prompt = label(ui->page_confirm, 12, 10, 248, 56, "", ui->font, C_TEXT);
    lv_label_set_long_mode(ui->confirm_prompt, LV_LABEL_LONG_WRAP);
    ui->confirm_rows[0] = label(ui->page_confirm, 28, 76, 96, 26, "确定", ui->font, C_TEXT);
    ui->confirm_rows[1] = label(ui->page_confirm, 148, 76, 96, 26, "取消", ui->font, C_TEXT);
    lv_obj_set_style_text_align(ui->confirm_rows[0], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(ui->confirm_rows[1], LV_TEXT_ALIGN_CENTER, 0);

    return ui->page_confirm != NULL;
}

static bool create_page_phone(steins0_ui_t *ui)
{
    // ---- 手机
    ui->page_phone = ui_obj_create(ui->root);
    if (!ui->page_phone) return false;
    lv_obj_remove_style_all(ui->page_phone);
    lv_obj_set_size(ui->page_phone, SG0_SCREEN_W, SG0_SCREEN_H);
    lv_obj_set_style_bg_color(ui->page_phone, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(ui->page_phone, LV_OPA_COVER, 0);
    lv_obj_remove_flag(ui->page_phone, LV_OBJ_FLAG_SCROLLABLE);
    ui->phone_title = label(ui->page_phone, 10, 4, 190, 22, "", ui->font, C_TEXT);
    ui->phone_sub = label(ui->page_phone, 10, 25, 250, 18, "", ui->font, C_DIM);
    ui->phone_page_label = label(ui->page_phone, 240, 4, 72, 22, "", ui->font, C_DIM);
    lv_obj_set_style_text_align(ui->phone_page_label, LV_TEXT_ALIGN_RIGHT, 0);
    for (int i = 0; i < SG0_LIST_ROWS; ++i) {
        ui->phone_rows[i] = label(ui->page_phone, 12, 46 + i * 21, 226, 20, "", ui->font, C_TEXT);
        ui->phone_values[i] = label(ui->page_phone, 242, 46 + i * 21, 68, 20, "", ui->font, C_DIM);
        lv_obj_set_style_text_align(ui->phone_values[i], LV_TEXT_ALIGN_RIGHT, 0);
    }
    ui->phone_footer = label(ui->page_phone, 10, 218, 300, 18, "", ui->font, C_DIM);

    return ui->page_phone != NULL;
}

static bool create_page_phone_thread(steins0_ui_t *ui)
{
    // ---- 手机会话
    ui->page_phone_thread = ui_obj_create(ui->root);
    if (!ui->page_phone_thread) return false;
    lv_obj_remove_style_all(ui->page_phone_thread);
    lv_obj_set_size(ui->page_phone_thread, SG0_SCREEN_W, SG0_SCREEN_H);
    lv_obj_set_style_bg_color(ui->page_phone_thread, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(ui->page_phone_thread, LV_OPA_COVER, 0);
    lv_obj_remove_flag(ui->page_phone_thread, LV_OBJ_FLAG_SCROLLABLE);
    ui->phone_thread_header = label(ui->page_phone_thread, 8, 4, 304, 19, "", ui->font, C_TEXT);
    ui->phone_thread_sender = label(ui->page_phone_thread, 8, 24, 304, 19, "", ui->font, C_SELECT);
    for (int i = 0; i < 3; ++i) {
        ui->phone_lines[i] = label(ui->page_phone_thread, 8, 52 + i * 19, 304, 19, "", ui->font,
                                   C_TEXT);
    }
    steins0_image_init_ex(&ui->stamp, ui->page_phone_thread, ui->stamp_pixels, 96, 96);
    if (!ui->stamp.canvas) s_page_alloc_failed = true;
    if (ui->stamp.canvas) {
        lv_obj_set_pos(ui->stamp.canvas, 8, 52);
        lv_obj_add_flag(ui->stamp.canvas, LV_OBJ_FLAG_HIDDEN);
    }
    ui->phone_thread_hint = label(ui->page_phone_thread, 8, 220, 304, 18, "", ui->font, C_DIM);

    return ui->page_phone_thread != NULL;
}

static bool create_page_card(steins0_ui_t *ui)
{
    // ---- 章节卡
    ui->page_card = ui_obj_create(ui->root);
    if (!ui->page_card) return false;
    lv_obj_remove_style_all(ui->page_card);
    lv_obj_set_size(ui->page_card, SG0_SCREEN_W, SG0_SCREEN_H);
    lv_obj_remove_flag(ui->page_card, LV_OBJ_FLAG_SCROLLABLE);
    panel(ui->page_card, 0, 180, SG0_SCREEN_W, 60, C_PANEL_DARK, 235, 0, 1, C_BORDER);
    ui->card_title = label(ui->page_card, 8, 190, 304, 22, "", ui->font, C_TEXT);
    lv_label_set_long_mode(ui->card_title, LV_LABEL_LONG_WRAP);
    label(ui->page_card, 8, 216, 304, 18, "任意键继续", ui->font, C_DIM);

    return ui->page_card != NULL;
}

static bool ensure_page(steins0_ui_t *ui, steins0_page_t page)
{
    if (!ui || page < SG0_PAGE_TITLE || page > SG0_PAGE_CARD) return false;
    if (ui->page_ready_mask & (uint16_t)(1u << page)) return true;
    ui_mem_trace("before", page);
    s_page_alloc_failed = false;
    bool ok = false;
    switch (page) {
    case SG0_PAGE_TITLE: ok = create_page_title(ui); break;
    case SG0_PAGE_GAME: ok = create_page_game(ui); break;
    case SG0_PAGE_LIST: ok = create_page_list(ui); break;
    case SG0_PAGE_CG: ok = create_page_cg(ui); break;
    case SG0_PAGE_HISTORY: ok = create_page_history(ui); break;
    case SG0_PAGE_ABOUT: ok = create_page_about(ui); break;
    case SG0_PAGE_ENDING: ok = create_page_ending(ui); break;
    case SG0_PAGE_CONFIRM: ok = create_page_confirm(ui); break;
    case SG0_PAGE_PHONE: ok = create_page_phone(ui); break;
    case SG0_PAGE_PHONE_THREAD: ok = create_page_phone_thread(ui); break;
    case SG0_PAGE_CARD: ok = create_page_card(ui); break;
    default: return false;
    }
    if (!ok || s_page_alloc_failed) {
        /* Remove a partially built page before clearing its pointers. */
        lv_obj_t *partial = page_object(ui, page);
        if (partial) lv_obj_del(partial);
        clear_page_ptrs(ui, page);
        ui_fail_notice(ui);
        return false;
    }
    ui->page_ready_mask |= (uint16_t)(1u << page);
    ui_mem_trace("after", page);
    return true;
}

static void drop_page(steins0_ui_t *ui, steins0_page_t page)
{
    if (!ui || !(ui->page_ready_mask & (uint16_t)(1u << page))) return;
    lv_obj_t *obj = page_object(ui, page);
    if (obj) {
        /* lv_obj_del recursively removes label callbacks in their destructors;
         * do not run a full layout update from the input task while dropping. */
        ui_mem_trace("drop", page);
        lv_obj_del(obj);
        ui_mem_trace("dropped", page);
    }
    clear_page_ptrs(ui, page);
    ui->page_ready_mask &= (uint16_t)~(1u << page);
}

bool steins0_ui_create(steins0_ui_t *ui, lv_obj_t *root, uint16_t *pixels,
                       uint16_t *stamp_pixels, const lv_font_t *font)
{
    if (!ui || !root || !pixels || !stamp_pixels || !font) return false;
    memset(ui, 0, sizeof(*ui));
    ui->root = root;
    ui->font = font;
    ui->stamp_pixels = stamp_pixels;
    if (!steins0_image_init(&ui->image, root, pixels)) return false;
    if (!ensure_page(ui, SG0_PAGE_TITLE)) return false;

    ui->notice_box = panel(root, 16, 24, 288, 32, C_PANEL_DARK, 240, 4, 1, C_ACCENT_DIM);
    if (!ui->notice_box) return false;
    ui->notice_text = label(ui->notice_box, 8, 6, 272, 20, "", font, C_SELECT);
    if (!ui->notice_text) return false;
    lv_obj_add_flag(ui->notice_box, LV_OBJ_FLAG_HIDDEN);
    steins0_ui_show(ui, SG0_PAGE_TITLE);
    return true;
}

bool steins0_ui_show(steins0_ui_t *ui, steins0_page_t page)
{
    if (!ui) return false;
    if (!ensure_page(ui, page)) {
        /* ensure_page already emitted the allocation notice; keep old page. */
        return false;
    }
    hide_all(ui);
    lv_obj_t *obj = page_object(ui, page);
    if (obj) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);

    /* Keep only the active secondary page. Title is the boot/home page and
     * stays resident; all other hidden pages release their LVGL allocations. */
    for (int p = SG0_PAGE_GAME; p <= SG0_PAGE_CARD; ++p) {
        if (p != (int)page) drop_page(ui, (steins0_page_t)p);
    }
    if (ui->notice_box) lv_obj_move_foreground(ui->notice_box);
    return true;
}

static void select_style(lv_obj_t *row, bool selected)
{
    if (!row) return;
    lv_obj_set_style_text_color(row, lv_color_hex(selected ? C_SELECT : C_TEXT), 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(C_ACCENT_DIM), 0);
    lv_obj_set_style_bg_opa(row, (lv_opa_t)(selected ? 120 : 0), 0);
}

void steins0_ui_title(steins0_ui_t *ui, const char *const *rows, int count, int selected)
{
    if (!ui || !ensure_page(ui, SG0_PAGE_TITLE)) return;
    ui->title_count = count > 6 ? 6 : count;
    ui->title_selected = selected;
    for (int i = 0; i < 6; ++i) {
        lv_label_set_text(ui->title_rows[i], i < ui->title_count ? rows[i] : "");
        select_style(ui->title_rows[i], i == selected && i < ui->title_count);
    }
}

void steins0_ui_title_battery(steins0_ui_t *ui, int percent)
{
    if (!ui || !ensure_page(ui, SG0_PAGE_TITLE)) return;
    char buf[16];
    if (percent < 0) snprintf(buf, sizeof(buf), "--");
    else snprintf(buf, sizeof(buf), "%d%%", percent);
    lv_label_set_text(ui->title_battery, buf);
}

void steins0_ui_battery(steins0_ui_t *ui, int percent)
{
    if (!ui || !ensure_page(ui, SG0_PAGE_GAME)) return;
    char buf[16];
    if (percent < 0) snprintf(buf, sizeof(buf), "--");
    else snprintf(buf, sizeof(buf), "%d%%", percent);
    lv_label_set_text(ui->game_battery, buf);
}

void steins0_ui_game(steins0_ui_t *ui, const char *chapter, int battery)
{
    if (!ui || !ensure_page(ui, SG0_PAGE_GAME)) return;
    lv_label_set_text(ui->game_chapter, chapter ? chapter : "");
    steins0_ui_battery(ui, battery);
}

void steins0_ui_game_text(steins0_ui_t *ui, const char *speaker, const char *const lines[3],
                          int line_count)
{
    if (!ui || !ensure_page(ui, SG0_PAGE_GAME)) return;
    lv_label_set_text(ui->speaker_label, speaker ? speaker : "");
    for (int i = 0; i < 3; ++i) {
        lv_label_set_text(ui->text_lines[i], (lines && i < line_count && lines[i]) ? lines[i] : "");
    }
}

void steins0_ui_game_page(steins0_ui_t *ui, int page, int pages)
{
    if (!ui || !ensure_page(ui, SG0_PAGE_GAME)) return;
    char buf[24];
    if (pages > 1) snprintf(buf, sizeof(buf), "%d/%d", page + 1, pages);
    else buf[0] = '\0';
    lv_label_set_text(ui->page_label, buf);
}

void steins0_ui_choice(steins0_ui_t *ui, const char *heading, const char *const *rows,
                       int count, int selected)
{
    if (!ui || !ensure_page(ui, SG0_PAGE_GAME)) return;
    ui->choice_count = count > 2 ? 2 : count;
    ui->choice_selected = selected;
    lv_label_set_text(ui->choice_heading, heading ? heading : "");
    for (int i = 0; i < 2; ++i) {
        lv_label_set_text(ui->choice_rows[i], i < count ? rows[i] : "");
        select_style(ui->choice_rows[i], i == selected && i < count);
    }
    lv_obj_remove_flag(ui->choice_box, LV_OBJ_FLAG_HIDDEN);
}

void steins0_ui_choice_hide(steins0_ui_t *ui)
{
    if (!ui || !ui->choice_box) return;
    lv_obj_add_flag(ui->choice_box, LV_OBJ_FLAG_HIDDEN);
}

void steins0_ui_list(steins0_ui_t *ui, const char *title, const char *const *rows,
                     const char *const *values, int count, int selected, const char *footer)
{
    if (!ui || !ensure_page(ui, SG0_PAGE_LIST)) return;
    ui->list_count = count > SG0_LIST_ROWS ? SG0_LIST_ROWS : count;
    ui->list_selected = selected;
    lv_label_set_text(ui->list_title, title ? title : "");
    for (int i = 0; i < SG0_LIST_ROWS; ++i) {
        lv_label_set_text(ui->list_rows[i], i < ui->list_count ? rows[i] : "");
        lv_label_set_text(ui->list_values[i], i < ui->list_count && values ? values[i] : "");
        select_style(ui->list_rows[i], i == selected && i < ui->list_count);
        lv_obj_set_style_text_color(ui->list_values[i],
                                    lv_color_hex(i == selected ? C_SELECT : C_DIM), 0);
    }
    lv_label_set_text(ui->list_footer, footer ? footer : "");
}

void steins0_ui_cg(steins0_ui_t *ui, const char *caption, const char *variant)
{
    if (!ui || !ensure_page(ui, SG0_PAGE_CG)) return;
    lv_label_set_text(ui->cg_caption, caption ? caption : "");
    lv_label_set_text(ui->cg_variant, variant ? variant : "");
}

void steins0_ui_history(steins0_ui_t *ui, const char *text)
{
    if (!ui || !ensure_page(ui, SG0_PAGE_HISTORY)) return;
    lv_label_set_text(ui->history_label, text ? text : "");
    lv_obj_scroll_to_y(ui->history_view, 0, LV_ANIM_OFF);
}

void steins0_ui_about(steins0_ui_t *ui, const char *text)
{
    if (!ui || !ensure_page(ui, SG0_PAGE_ABOUT)) return;
    lv_label_set_text(ui->about_label, text ? text : "");
    lv_obj_scroll_to_y(ui->about_view, 0, LV_ANIM_OFF);
}

void steins0_ui_ending(steins0_ui_t *ui, const char *title, const char *body, const char *hint)
{
    if (!ui || !ensure_page(ui, SG0_PAGE_ENDING)) return;
    lv_label_set_text(ui->ending_title, title ? title : "ENDING");
    lv_label_set_text(ui->ending_label, body ? body : "");
    lv_label_set_text(ui->ending_hint, hint ? hint : "");
}

void steins0_ui_confirm(steins0_ui_t *ui, const char *prompt, const char *yes, const char *no,
                        int selected)
{
    if (!ui || !ensure_page(ui, SG0_PAGE_CONFIRM)) return;
    ui->confirm_selected = selected;
    lv_label_set_text(ui->confirm_prompt, prompt ? prompt : "");
    lv_label_set_text(ui->confirm_rows[0], yes ? yes : "确定");
    lv_label_set_text(ui->confirm_rows[1], no ? no : "取消");
    select_style(ui->confirm_rows[0], selected == 0);
    select_style(ui->confirm_rows[1], selected == 1);
}

void steins0_ui_notice(steins0_ui_t *ui, const char *text)
{
    if (!ui || !ui->notice_box || !ui->notice_text) return;
    if (!text || !text[0]) {
        lv_label_set_text_static(ui->notice_text, "");
        steins0_ui_notice_hide(ui);
        return;
    }
    lv_label_set_text(ui->notice_text, text);
    lv_obj_remove_flag(ui->notice_box, LV_OBJ_FLAG_HIDDEN);
}

void steins0_ui_notice_hide(steins0_ui_t *ui)
{
    if (!ui || !ui->notice_box) return;
    lv_obj_add_flag(ui->notice_box, LV_OBJ_FLAG_HIDDEN);
}

void steins0_ui_scroll(steins0_ui_t *ui, steins0_page_t page, int delta)
{
    if (!ui) return;
    lv_obj_t *view = page == SG0_PAGE_HISTORY ? ui->history_view
                     : page == SG0_PAGE_ABOUT ? ui->about_view : NULL;
    if (!view) return;
    lv_obj_scroll_by(view, 0, -delta, LV_ANIM_OFF);
}

void steins0_ui_game_rine(steins0_ui_t *ui, const char *text)
{
    if (ui && ensure_page(ui, SG0_PAGE_GAME) && ui->game_rine) lv_label_set_text(ui->game_rine, text ? text : "");
}

void steins0_ui_phone(steins0_ui_t *ui, const char *title, const char *sub,
                      const char *page_label, const char *const *rows,
                      const char *const *values, int count, int selected, const char *footer)
{
    if (!ui || !ensure_page(ui, SG0_PAGE_PHONE)) return;
    ui->phone_count = count > SG0_LIST_ROWS ? SG0_LIST_ROWS : count;
    ui->phone_selected = selected;
    lv_label_set_text(ui->phone_title, title ? title : "");
    lv_label_set_text(ui->phone_sub, sub ? sub : "");
    lv_label_set_text(ui->phone_page_label, page_label ? page_label : "");
    for (int i = 0; i < SG0_LIST_ROWS; ++i) {
        const bool active = i < ui->phone_count;
        lv_label_set_text(ui->phone_rows[i], active ? rows[i] : "");
        lv_label_set_text(ui->phone_values[i], active && values ? values[i] : "");
        select_style(ui->phone_rows[i], active && i == selected);
        lv_obj_set_style_text_color(ui->phone_values[i],
                                    lv_color_hex(active && i == selected ? C_SELECT : C_DIM), 0);
    }
    lv_label_set_text(ui->phone_footer, footer ? footer : "");
}

void steins0_ui_phone_thread(steins0_ui_t *ui, const char *header, const char *sender,
                             const char *const lines[3], int line_count, bool stamp,
                             const char *hint)
{
    if (!ui || !ensure_page(ui, SG0_PAGE_PHONE_THREAD)) return;
    lv_label_set_text(ui->phone_thread_header, header ? header : "");
    lv_label_set_text(ui->phone_thread_sender, sender ? sender : "");
    for (int i = 0; i < 3; ++i) {
        lv_label_set_text(ui->phone_lines[i], (!stamp && lines && i < line_count) ? lines[i] : "");
    }
    if (ui->stamp.canvas) {
        if (stamp) lv_obj_remove_flag(ui->stamp.canvas, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(ui->stamp.canvas, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(ui->phone_thread_hint, hint ? hint : "");
}

void steins0_ui_card(steins0_ui_t *ui, const char *title)
{
    if (ui && ensure_page(ui, SG0_PAGE_CARD)) lv_label_set_text(ui->card_title, title ? title : "");
}
