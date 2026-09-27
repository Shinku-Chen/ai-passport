// main/dracu_ui.h —— 界面层(《千恋＊万花》阅读器,从 ATRI 阅读器 main/dracu_ui.h 移植)。
//
// 版面(竖屏 240x320):
//   y 0..209    画面区:一张 RGB565 画布(背景 + 立绘);标题页复用同一张画布
//   y 210..319  文本框: 说话人名牌压在画面区下沿,正文 5 行 x 13 个全角字
// 状态浮层(章节进度 / 电量)直接画在画面区角落;列表页、选项页、结局页等是整屏页面。
//
// 本模块只负责"把给定的内容画出来",不决定剧情走向 —— 状态机在 dracu_app.c。
#pragma once

#include "dracu_image.h"
#include "dracu_model.h"
#include "dracu_pack.h"

#include "lvgl.h"   // lv_font_t / lv_obj_t:界面层本身就依赖 LVGL

#include <stdbool.h>
#include <stdint.h>

#define DRACU_UI_W 240
#define DRACU_UI_H 320
// 正文带在画面区下沿(与源工程 text_bg 换算一致):画布是整屏 240x320,
// 这条带由 dracu_image 以半透明蓝底画进画布,LVGL 只负责把文字压在上面。
#define DRACU_BOX_Y 210
#define DRACU_BOX_H (DRACU_UI_H - DRACU_BOX_Y)

// 正文排版:16px 字体、20px 行距;一行 13 个全角字(13 x 2 = 26 单位,
// 文本框宽 224px,留一点余量给半角字符),一屏 5 行。
#define DRACU_LINE_H 20
#define DRACU_TEXT_LINES 5
#define DRACU_UNITS_PER_LINE 26
#define DRACU_UI_MAX_ROWS 7
// 选项最多几行(与 dracu_model.h 的 DRACU_CHOICE_MAX 一致;实测最多 3 个)
#define DRACU_UI_CHOICE_MAX DRACU_CHOICE_MAX

typedef enum {
    DRACU_PAGE_TITLE = 0,
    DRACU_PAGE_CHAPTERS,
    DRACU_PAGE_GAME,
    DRACU_PAGE_MENU,
    DRACU_PAGE_SETTINGS,
    DRACU_PAGE_SLOTS,
    DRACU_PAGE_ENDING,
    DRACU_PAGE_ABOUT,
} dracu_screen_t;

typedef enum {
    DRACU_LIST_TITLE = 0,
    DRACU_LIST_CHAPTERS,
    DRACU_LIST_MENU,
    DRACU_LIST_SETTINGS,
    DRACU_LIST_SLOTS,
    DRACU_LIST_COUNT,
} dracu_list_id_t;

// 通用"行列表":一行 = 左侧文字 + 右侧取值(可空)。
typedef struct {
    struct _lv_obj_t *page;
    struct _lv_obj_t *title;
    struct _lv_obj_t *hint;
    struct _lv_obj_t *rows[DRACU_UI_MAX_ROWS];
    struct _lv_obj_t *values[DRACU_UI_MAX_ROWS];
    int count;
    int selected;
    int row_h;
    int top;
} dracu_list_t;

typedef struct {
    dracu_image_t art;                 // 画面区画布

    struct _lv_obj_t *box;            // 正文文本框
    struct _lv_obj_t *name_plate;     // 说话人名牌
    struct _lv_obj_t *name_text;
    struct _lv_obj_t *body;           // 正文
    struct _lv_obj_t *progress;       // 画面区左上角:章节进度
    struct _lv_obj_t *page_hint;      // 画面区右下角:第几页
    struct _lv_obj_t *battery;        // 画面区右上角:电量

    struct _lv_obj_t *choice_box;     // 选项(盖在画面区上)
    struct _lv_obj_t *choice_rows[DRACU_UI_CHOICE_MAX];
    struct _lv_obj_t *choice_texts[DRACU_UI_CHOICE_MAX];
    int choice_count;
    int choice_selected;

    dracu_list_t lists[DRACU_LIST_COUNT];

    struct _lv_obj_t *page_title;
    struct _lv_obj_t *page_chapters;
    struct _lv_obj_t *page_game;
    struct _lv_obj_t *page_settings;
    struct _lv_obj_t *page_slots;
    struct _lv_obj_t *page_ending;
    struct _lv_obj_t *page_about;

    // 标题页不再需要单独的横幅标签:标题文字就在标题图上(见 DRACU_TITLE_ART_NAME)。
    struct _lv_obj_t *auto_hint;      // 自动阅读模式指示
    struct _lv_obj_t *notice;         // 瞬时提示(保存成功 / 无法跳过等)

    struct _lv_obj_t *ending_kicker;
    struct _lv_obj_t *ending_name;
    struct _lv_obj_t *ending_hint;
    struct _lv_obj_t *about_body;
    struct _lv_obj_t *slots_hint;

    const lv_font_t *font_cjk;        // 16px 中文子集(正文/名字/菜单)
    const lv_font_t *font_tiny;       // Montserrat 14(浮层数字)

    char last_speaker[DRACU_NAME_MAX];   // 只用于日志:说话人变化时打一行
    bool auto_on;                     // 自动阅读模式
    dracu_screen_t page_current;         // 显隐恢复时要知道当前页

    lv_timer_t *typewriter;
    char typing_target[DRACU_TEXT_BUFFER];   // 本页完整文本(稳定缓冲)
    char typing_shown[DRACU_TEXT_BUFFER];    // 已显示部分
    uint32_t typing_index;
    uint32_t typing_total;
    uint32_t typing_step_ms;
    bool typing_active;
} dracu_ui_t;

// 建界面(需持有 LVGL 锁)。画面区缓冲由调用方静态提供并保证生命周期;
// pack 用于画面区的 JPEG / 分块调色板解码。
bool dracu_ui_create(dracu_ui_t *ui, uint16_t *art_pixels, const lv_font_t *font_cjk,
                     const dracu_pack_t *pack);

void dracu_ui_show_page(dracu_ui_t *ui, dracu_screen_t page);

// 画面区:按当前页的四个图层合成一帧(背景 -> 事件 CG 或 立绘/SD -> 正文带)。
// 四层都没变时只重画正文带,不会反复解 JPEG。
void dracu_ui_compose(dracu_ui_t *ui, const dracu_layers_t *layers);

// 正文页:设置说话人与本页文本,并按 ms_per_char 逐字显示。
void dracu_ui_set_text(dracu_ui_t *ui, const char *speaker, const char *text,
                      uint32_t ms_per_char);
void dracu_ui_finish_typing(dracu_ui_t *ui);
bool dracu_ui_typing(const dracu_ui_t *ui);

// 画面区浮层:章节文案(形如 "1-2";NULL 或空串则不显示)与页码;电量 -- 用 -1 表示未知。
void dracu_ui_set_progress(dracu_ui_t *ui, const char *chapter, int page, int pages);

// 自动阅读模式的常驻指示("自动"两个字,画面区左下角)。
void dracu_ui_set_auto(dracu_ui_t *ui, bool on);
void dracu_ui_set_battery(dracu_ui_t *ui, int percent);

// 选项:texts 是 count 条文案(最多 DRACU_UI_CHOICE_MAX 条),第一行默认选中。
void dracu_ui_show_choices(dracu_ui_t *ui, const char *const *texts, int count);
void dracu_ui_hide_choices(dracu_ui_t *ui);
void dracu_ui_set_choice_selected(dracu_ui_t *ui, int index);
int dracu_ui_choice_selected(const dracu_ui_t *ui);

// 列表页:labels 必填,values 可为 NULL。selected 会被夹到合法范围。
void dracu_ui_set_list(dracu_ui_t *ui, dracu_list_id_t id, const char *title, const char *hint,
                      const char *const *labels, const char *const *values, int count,
                      int selected);
void dracu_ui_set_list_selected(dracu_ui_t *ui, dracu_list_id_t id, int selected);
void dracu_ui_set_list_value(dracu_ui_t *ui, dracu_list_id_t id, int row, const char *value);

// 结局页
void dracu_ui_set_ending(dracu_ui_t *ui, const char *kicker, const char *name, const char *hint);
void dracu_ui_set_about(dracu_ui_t *ui, const char *body);
// 关于页滚动:y 会被夹在 [0, 最大滚动量]，返回实际位置。三键设备没有触摸滑屏,
// 所以滚动完全由按键驱动(dracu_app 调它)。
int dracu_ui_set_about_scroll(dracu_ui_t *ui, int y);

// 瞬时提示(空串隐去)。它总在最上层,任何页面都能用。
void dracu_ui_notice(dracu_ui_t *ui, const char *text);
