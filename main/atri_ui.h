// main/atri_ui.h —— 《ATRI -My Dear Moments-》的 LVGL 界面层。
//
// 版面(竖屏 240x320):
//   y 0..209    画面区:一张 RGB565 画布(背景 + 叠加);标题页复用同一张画布
//   y 210..319  文本框: 说话人名牌压在画面区下沿,正文 5 行 x 13 个全角字
// 状态浮层(章节进度 / 电量)直接画在画面区角落;列表页、选项页、结局页等是整屏页面。
//
// 本模块只负责"把给定的内容画出来",不决定剧情走向 —— 状态机在 atri_app.c。
#pragma once

#include "atri_image.h"
#include "atri_model.h"
#include "atri_pack.h"

#include "lvgl.h"   // lv_font_t / lv_obj_t:界面层本身就依赖 LVGL

#include <stdbool.h>
#include <stdint.h>

#define ATRI_UI_W 240
#define ATRI_UI_H 320
// 正文带在画面区下沿(与源工程 text_bg 换算一致):画布是整屏 240x320,
// 这条带由 atri_image 以半透明蓝底画进画布,LVGL 只负责把文字压在上面。
#define ATRI_BOX_Y 210
#define ATRI_BOX_H (ATRI_UI_H - ATRI_BOX_Y)

// 正文排版:16px 字体、20px 行距;一行 13 个全角字(13 x 2 = 26 单位,
// 文本框宽 224px,留一点余量给半角字符),一屏 5 行。
#define ATRI_LINE_H 20
#define ATRI_TEXT_LINES 5
#define ATRI_UNITS_PER_LINE 26
#define ATRI_UI_MAX_ROWS 7

typedef enum {
    ATRI_PAGE_WARNING = 0,
    ATRI_PAGE_TITLE,
    ATRI_PAGE_GAME,
    ATRI_PAGE_MENU,
    ATRI_PAGE_SETTINGS,
    ATRI_PAGE_SLOTS,
    ATRI_PAGE_ENDING,
    ATRI_PAGE_ABOUT,
} atri_page_t;

typedef enum {
    ATRI_LIST_TITLE = 0,
    ATRI_LIST_MENU,
    ATRI_LIST_SETTINGS,
    ATRI_LIST_SLOTS,
    ATRI_LIST_COUNT,
} atri_list_id_t;

// 通用"行列表":一行 = 左侧文字 + 右侧取值(可空)。
typedef struct {
    struct _lv_obj_t *page;
    struct _lv_obj_t *title;
    struct _lv_obj_t *hint;
    struct _lv_obj_t *rows[ATRI_UI_MAX_ROWS];
    struct _lv_obj_t *values[ATRI_UI_MAX_ROWS];
    int count;
    int selected;
    int row_h;
    int top;
} atri_list_t;

typedef struct {
    atri_image_t art;                 // 画面区画布

    struct _lv_obj_t *box;            // 正文文本框
    struct _lv_obj_t *name_plate;     // 说话人名牌
    struct _lv_obj_t *name_text;
    struct _lv_obj_t *body;           // 正文
    struct _lv_obj_t *progress;       // 画面区左上角:章节进度
    struct _lv_obj_t *page_hint;      // 画面区右下角:第几页
    struct _lv_obj_t *battery;        // 画面区右上角:电量

    struct _lv_obj_t *choice_box;     // 选项(盖在画面区上)
    struct _lv_obj_t *choice_rows[2];
    struct _lv_obj_t *choice_texts[2];
    int choice_count;
    int choice_selected;

    atri_list_t lists[ATRI_LIST_COUNT];

    struct _lv_obj_t *page_warning;
    struct _lv_obj_t *page_title;
    struct _lv_obj_t *page_game;
    struct _lv_obj_t *page_settings;
    struct _lv_obj_t *page_slots;
    struct _lv_obj_t *page_ending;
    struct _lv_obj_t *page_about;

    struct _lv_obj_t *notice;         // 瞬时提示(保存成功 / 无法跳过等)

    struct _lv_obj_t *warning_body;
    struct _lv_obj_t *warning_hint;
    struct _lv_obj_t *ending_kicker;
    struct _lv_obj_t *ending_name;
    struct _lv_obj_t *ending_hint;
    struct _lv_obj_t *about_body;
    struct _lv_obj_t *slots_hint;

    const lv_font_t *font_cjk;        // 16px 中文子集(正文/名字/菜单)
    const lv_font_t *font_tiny;       // Montserrat 14(浮层数字)

    char last_speaker[64];            // 只用于日志:说话人变化时打一行

    lv_timer_t *typewriter;
    char typing_target[ATRI_TEXT_BUFFER];   // 本页完整文本(稳定缓冲)
    char typing_shown[ATRI_TEXT_BUFFER];    // 已显示部分
    uint32_t typing_index;
    uint32_t typing_total;
    uint32_t typing_step_ms;
    bool typing_active;
} atri_ui_t;

// 建界面(需持有 LVGL 锁)。画面区缓冲由调用方静态提供并保证生命周期。
bool atri_ui_create(atri_ui_t *ui, uint16_t *art_pixels, const lv_font_t *font_cjk);

void atri_ui_show_page(atri_ui_t *ui, atri_page_t page);

// 画面区:把背景 + 叠加画进画布(标题页也用同一个函数)。
bool atri_ui_set_art(atri_ui_t *ui, const atri_pack_t *pack, uint16_t bg, uint16_t ovl,
                     int16_t x, int16_t y);

// 正文页:设置说话人与本页文本,并按 ms_per_char 逐字显示。
void atri_ui_set_text(atri_ui_t *ui, const char *speaker, const char *text,
                      uint32_t ms_per_char);
void atri_ui_finish_typing(atri_ui_t *ui);
bool atri_ui_typing(const atri_ui_t *ui);

// 画面区浮层:章节(1 起,0 = 不显示)与页码;电量 -- 用 -1 表示未知。
void atri_ui_set_progress(atri_ui_t *ui, int chapter, int page, int pages);
void atri_ui_set_battery(atri_ui_t *ui, int percent);

// 选项
void atri_ui_show_choices(atri_ui_t *ui, const char *first, const char *second);
void atri_ui_hide_choices(atri_ui_t *ui);
void atri_ui_set_choice_selected(atri_ui_t *ui, int index);
int atri_ui_choice_selected(const atri_ui_t *ui);

// 列表页:labels 必填,values 可为 NULL。selected 会被夹到合法范围。
void atri_ui_set_list(atri_ui_t *ui, atri_list_id_t id, const char *title, const char *hint,
                      const char *const *labels, const char *const *values, int count,
                      int selected);
void atri_ui_set_list_selected(atri_ui_t *ui, atri_list_id_t id, int selected);
void atri_ui_set_list_value(atri_ui_t *ui, atri_list_id_t id, int row, const char *value);

// 结局页与警告页
void atri_ui_set_ending(atri_ui_t *ui, const char *kicker, const char *name, const char *hint);
void atri_ui_set_warning(atri_ui_t *ui, const char *body, const char *hint);
void atri_ui_set_about(atri_ui_t *ui, const char *body);
// 关于页滚动:y 会被夹在 [0, 最大滚动量]，返回实际位置。三键设备没有触摸滑屏,
// 所以滚动完全由按键驱动(atri_app 调它)。
int atri_ui_set_about_scroll(atri_ui_t *ui, int y);

// 瞬时提示(空串隐去)。它总在最上层,任何页面都能用。
void atri_ui_notice(atri_ui_t *ui, const char *text);
