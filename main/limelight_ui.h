// main/limelight_ui.h —— LVGL 界面层(从 ATRI 阅读器 main/atri_ui.[ch] 移植)。
//
// 版面(竖屏 240x320):
//   y   0..209   画面区:一张 RGB565 画布(背景 + 立绘 + 正文带),标题页/鉴赏页复用
//   y 210..319   文本框:说话人名牌压在画面区下沿,正文 5 行 x 13 个全角字
// 浮层(章节进度 / 电量 / 页码 / 自动阅读)直接画在画面区角落;列表页、选项页、
// 结局页、鉴赏页是整屏页面。
//
// 本模块只负责"把给定的内容画出来",不决定剧情走向 —— 状态机在 limelight_app.c。
#pragma once

#include "limelight_assets.h"
#include "limelight_image.h"
#include "limelight_model.h"

#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>

#define LIME_UI_W LIME_SCREEN_W
#define LIME_UI_H LIME_SCREEN_H
// 正文带顶边。画布只有 214 行(见 limelight_image_math.h),带子(210..319)是 LVGL
// 画的半透明渐变蓝底,文字压在上面。
#define LIME_UI_BOX_Y 210
#define LIME_UI_BOX_H (LIME_UI_H - LIME_UI_BOX_Y)

// 正文排版:16px 字体(LVGL 每次加 1 像素的行距 -> 20px 行高)、13 个全角字、
// 一屏 5 行。与 limelight_model.h 的 lime_layout_t 一一对应。
#define LIME_UI_LINE_H 20
#define LIME_UI_LINES 5
#define LIME_UI_UNITS_PER_LINE 26

#define LIME_UI_MAX_ROWS 7        // 列表一屏最多几行(超出的部分靠窗口滚动)
#define LIME_UI_ROW_H 30          // 列表行高
#define LIME_UI_LIST_TOP_Y 44     // 列表第一行的 y(标题在 y=8)
#define LIME_UI_MAX_CHOICES 4     // 源数据最多 4 个选项(detail.ux 的 c1..c4)

typedef enum {
    LIME_PAGE_WARNING = 0,
    LIME_PAGE_TITLE,
    LIME_PAGE_GAME,
    LIME_PAGE_MENU,
    LIME_PAGE_SETTINGS,
    LIME_PAGE_SLOTS,
    LIME_PAGE_CHAPTERS,
    LIME_PAGE_GALLERY,
    LIME_PAGE_ABOUT,
    LIME_PAGE_ENDING,
} lime_page_t;

typedef enum {
    LIME_LIST_TITLE = 0,
    LIME_LIST_MENU,
    LIME_LIST_SETTINGS,
    LIME_LIST_SLOTS,
    LIME_LIST_CHAPTERS,
    LIME_LIST_COUNT,
} lime_list_id_t;

// 通用"行列表":一行 = 左侧文字 + 右侧取值(可空)。列表比一屏长时只画一个窗口,
// 窗口顶部跟着光标走(三键设备没有触摸滑屏,滚动完全由按键驱动)。
typedef struct {
    struct _lv_obj_t *page;         // 这个列表属于哪个页面(共用一个控件,靠它挑)
    int count;          // 列表总行数
    int selected;       // 当前光标
    int top;            // 窗口第一行在列表里的下标
    int row_h;                    // 行高(每页可以不同:标题页矮一点)
    int top_y;                    // 第一行的 y
    // 窗口内容由 set_list 时记下,滚动时重新填行。标题/提示是字面量,存指针即可。
    const char *title_text;
    const char *hint_text;
    const char *const *labels;
    const char *const *values_src;
} lime_list_t;

typedef struct {
    lime_image_t art;                 // 画面区画布

    // 列表控件只有一份,5 个列表页共用(省 ~23KB LVGL 内存池):切页时按当前页的
    // 数据重画。每页的行数据仍然分开存在 lists[] 里。
    struct _lv_obj_t *list_root;
    struct _lv_obj_t *list_title;
    struct _lv_obj_t *list_hint;
    struct _lv_obj_t *list_labels[LIME_UI_MAX_ROWS];
    struct _lv_obj_t *list_values[LIME_UI_MAX_ROWS];

    // 正文带:一块纯色半透明蓝(不再分段做渐变 —— 分段会在屏幕上留下横向接缝)。
    struct _lv_obj_t *band;
    struct _lv_obj_t *box;            // 正文文本框(透明,压在带子上)
    struct _lv_obj_t *name_plate;     // 说话人名牌
    struct _lv_obj_t *name_text;
    struct _lv_obj_t *body;           // 正文
    struct _lv_obj_t *progress;       // 画面区左上角:章节
    struct _lv_obj_t *page_hint;      // 画面区右下角:第几页
    struct _lv_obj_t *battery;        // 画面区右上角:电量
    struct _lv_obj_t *auto_hint;      // 自动阅读指示

    struct _lv_obj_t *choice_box;     // 选项(盖在画面区上)
    struct _lv_obj_t *choice_rows[LIME_UI_MAX_CHOICES];
    struct _lv_obj_t *choice_texts[LIME_UI_MAX_CHOICES];
    int choice_count;
    int choice_selected;
    int choice_top_y;                 // 选项块起始 y(按条数居中)

    lime_list_t lists[LIME_LIST_COUNT];

    struct _lv_obj_t *page_warning;
    struct _lv_obj_t *page_title;
    struct _lv_obj_t *page_menu;
    struct _lv_obj_t *page_settings;
    struct _lv_obj_t *page_slots;
    struct _lv_obj_t *page_chapters;
    struct _lv_obj_t *page_gallery;
    struct _lv_obj_t *page_about;
    struct _lv_obj_t *page_ending;

    struct _lv_obj_t *gallery_info;   // 鉴赏页:第几张/共几张

    struct _lv_obj_t *notice;         // 瞬时提示(保存成功等)
    struct _lv_obj_t *notice_text;

    struct _lv_obj_t *warning_body;
    struct _lv_obj_t *warning_hint;
    struct _lv_obj_t *ending_kicker;
    struct _lv_obj_t *ending_name;
    struct _lv_obj_t *ending_hint;
    struct _lv_obj_t *about_body;

    const lv_font_t *font_cjk;        // 16px 中文子集(正文/名字/菜单)
    const lv_font_t *font_tiny;       // Montserrat 14(浮层数字)

    lime_page_t page_current;
    bool auto_on;                     // 自动阅读指示灯

    lv_timer_t *typewriter;
    char typing_target[LIME_TEXT_BUFFER];   // 本页完整文本(稳定缓冲)
    char typing_shown[LIME_TEXT_BUFFER];    // 已显示部分
    uint32_t typing_index;
    uint32_t typing_total;
    uint32_t typing_step_ms;
    bool typing_active;
} lime_ui_t;

// 建界面(需持有 LVGL 锁)。画面区缓冲与立绘/遮罩缓冲由调用方静态提供。
bool lime_ui_create(lime_ui_t *ui, uint16_t *art_pixels, uint8_t *sprite_buffer,
                    uint32_t sprite_capacity, uint8_t *mask_buffer, uint32_t mask_capacity,
                    const lv_font_t *font_cjk);

void lime_ui_show_page(lime_ui_t *ui, lime_page_t page);

// 画面区:背景 + 立绘(asset 下标,<0 = 不画)。标题页/鉴赏页用 backdrop 只铺背景。
bool lime_ui_set_art(lime_ui_t *ui, const lime_assets_t *assets, int bg_index, int sprite_index);
bool lime_ui_set_backdrop(lime_ui_t *ui, const lime_assets_t *assets, int bg_index);

// 正文页:设置说话人与本页文本,并按 ms_per_char 逐字显示。
void lime_ui_set_text(lime_ui_t *ui, const char *speaker, const char *text, uint32_t ms_per_char);
void lime_ui_finish_typing(lime_ui_t *ui);
bool lime_ui_typing(const lime_ui_t *ui);

// 画面区浮层。chapter 从 1 起,0 = 不显示;battery 用 -1 表示未知。
// 章节显示为 X-X(major-minor,如 10-3);major < 0 表示没有章节信息。
void lime_ui_set_progress(lime_ui_t *ui, int major, int minor, int page, int pages);
void lime_ui_set_battery(lime_ui_t *ui, int percent);
void lime_ui_set_auto(lime_ui_t *ui, bool on);

// 选项:最多 4 条。set_choice_selected 只动高亮。
void lime_ui_show_choices(lime_ui_t *ui, const char *const *texts, int count);
void lime_ui_hide_choices(lime_ui_t *ui);
void lime_ui_set_choice_selected(lime_ui_t *ui, int index);
int lime_ui_choice_selected(const lime_ui_t *ui);

// 列表页:labels 必填(长度 = count),values 可为 NULL。selected 会被夹到合法范围,
// 窗口会自动滚到让光标可见。
void lime_ui_set_list(lime_ui_t *ui, lime_list_id_t id, const char *title, const char *hint,
                      const char *const *labels, const char *const *values, int count,
                      int selected);
void lime_ui_set_list_selected(lime_ui_t *ui, lime_list_id_t id, int selected);
void lime_ui_set_list_value(lime_ui_t *ui, lime_list_id_t id, const char *value, int row);
int lime_ui_list_selected(const lime_ui_t *ui, lime_list_id_t id);

// 结局页 / 警告页 / 关于页 / 鉴赏页信息行。
void lime_ui_set_ending(lime_ui_t *ui, const char *kicker, const char *name, const char *hint);
void lime_ui_set_warning(lime_ui_t *ui, const char *body, const char *hint);
void lime_ui_set_about(lime_ui_t *ui, const char *body);
void lime_ui_set_gallery_info(lime_ui_t *ui, int index, int count);

// 瞬时提示(空串隐去)。它总在最上层。
void lime_ui_notice(lime_ui_t *ui, const char *text);
