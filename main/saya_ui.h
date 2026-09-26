// main/saya_ui.h —— 《沙耶之歌》的 LVGL 界面层。
//
// 版面(横屏 320x240):
//   y 0..149    画面区: 一张 RGB565 画布(背景 + 立绘);标题页复用同一张画布
//   y 126..149  说话人名字:画面区左下角、文本框正上方的半透明标签
//   y 150..231  文本框: 正文(最后一行与屏幕底部留 SAYA_BOX_BOTTOM_MARGIN 的边距)
// 全屏页面(菜单、警告、设置、存读档、结局、关于)盖住整个屏幕。
//
// 本模块只负责"把给定的内容画出来",不决定剧情走向 —— 状态机在 saya_app.c。
#pragma once

#include "saya_image.h"
#include "saya_model.h"
#include "saya_pack.h"

#include "lvgl.h"   // lv_font_t / lv_obj_t:界面层本身就依赖 LVGL

#include <stdbool.h>
#include <stdint.h>

#define SAYA_UI_W 320
#define SAYA_UI_H 240
// 文本框:底部留 SAYA_BOX_BOTTOM_MARGIN 的边距,最后一行不贴屏幕边缘。
#define SAYA_BOX_BOTTOM_MARGIN 8
#define SAYA_BOX_H 82
#define SAYA_BOX_Y (SAYA_UI_H - SAYA_BOX_BOTTOM_MARGIN - SAYA_BOX_H)
// 说话人名字放在画面区左下角、文本框正上方(不再占用文本框内部空间)。
#define SAYA_NAME_Y (SAYA_BOX_Y - 24)
#define SAYA_UI_MAX_ROWS 6
// 正文行高/行数:必须与 tools/saya_font.py 的 EXTRA_LEADING(字号+3)和
// main/saya_app.c 的 layout_for() 一致,否则最后一行会被裁掉或留下大片空白。
#define SAYA_LINE_H 19
#define SAYA_TEXT_LINES 4

typedef enum {
    SAYA_PAGE_WARNING = 0,
    SAYA_PAGE_TITLE,
    SAYA_PAGE_GAME,
    SAYA_PAGE_MENU,
    SAYA_PAGE_SETTINGS,
    SAYA_PAGE_SLOTS,
    SAYA_PAGE_ENDING,
    SAYA_PAGE_ABOUT,
} saya_page_t;

// 通用"行列表":一行 = 左侧文字 + 右侧取值(可空)。
typedef struct {
    struct _lv_obj_t *page;
    struct _lv_obj_t *title;
    struct _lv_obj_t *rows[SAYA_UI_MAX_ROWS];
    struct _lv_obj_t *values[SAYA_UI_MAX_ROWS];
    int count;
    int selected;
    int row_h;
    int top;
    const lv_font_t *font;   // 懒创建的行要用它,由 list_apply_font 维护
} saya_list_t;

typedef struct {
    saya_image_t art;                 // 画面区画布

    struct _lv_obj_t *box;            // 正文文本框
    struct _lv_obj_t *name;
    struct _lv_obj_t *text;
    struct _lv_obj_t *battery;        // 画面区右上角电量
    struct _lv_obj_t *chapter;        // 画面区左上角章节提示

    struct _lv_obj_t *choice_box;     // 选项页(盖在文本框上)
    struct _lv_obj_t *choice_rows[2];
    struct _lv_obj_t *choice_texts[2];
    int choice_count;
    int choice_selected;

    saya_list_t title_menu;
    saya_list_t menu;
    saya_list_t settings_menu;
    saya_list_t slots_menu;
    saya_list_t about_menu;

    struct _lv_obj_t *page_warning;
    struct _lv_obj_t *page_title;
    struct _lv_obj_t *page_game;
    struct _lv_obj_t *page_menu;
    struct _lv_obj_t *page_settings;
    struct _lv_obj_t *page_slots;
    struct _lv_obj_t *page_ending;
    struct _lv_obj_t *page_about;
    struct _lv_obj_t *ending_title;
    struct _lv_obj_t *ending_name;
    struct _lv_obj_t *ending_hint;
    struct _lv_obj_t *warning_view;    // 警告正文的可滚动容器(读到底才能继续)
    struct _lv_obj_t *warning_text;
    struct _lv_obj_t *warning_hint;
    char warning_tail[64];            // 读完后的提示文案(由 set_warning 传入)
    struct _lv_obj_t *about_view;     // 关于正文的可滚动容器(没地方显示时用上/下滚)
    struct _lv_obj_t *about_text;
    struct _lv_obj_t *about_hint;     // 内容超出时显示的“上/下 滚动”提示
    struct _lv_obj_t *slots_title;
    struct _lv_obj_t *slots_hint;

    const lv_font_t *font_small;      // 16px 子集(字号"小")
    const lv_font_t *font_large;      // 20px 子集(字号"大")

    lv_timer_t *typewriter_timer;
    char typing_target[SAYA_TEXT_BUFFER];    // 本页完整文本(稳定缓冲)
    char typing_shown[SAYA_TEXT_BUFFER];     // 已显示部分
    uint32_t typing_index;
    uint32_t typing_total;
    uint32_t typing_step_ms;
    bool typing_active;
} saya_ui_t;

// 建界面(需持有 LVGL 锁)。画面区/立绘缓冲由调用方静态提供并保证生命周期。
bool saya_ui_create(saya_ui_t *ui, uint16_t *art_pixels, uint8_t *sprite_scratch,
                    uint32_t sprite_scratch_size, const lv_font_t *font_small,
                    const lv_font_t *font_large);

void saya_ui_show_page(saya_ui_t *ui, saya_page_t page);

// 标题页:把标题画面画进画面区(与正文页共用同一张画布)。
bool saya_ui_set_title_art(saya_ui_t *ui, const saya_pack_t *pack);

// 正文页
bool saya_ui_set_scene(saya_ui_t *ui, const saya_pack_t *pack, uint16_t bg, uint16_t fg);
void saya_ui_set_text(saya_ui_t *ui, const char *speaker, const char *page_text,
                      uint32_t ms_per_char);
void saya_ui_show_full_text(saya_ui_t *ui);
bool saya_ui_typing(const saya_ui_t *ui);
void saya_ui_set_chapter(saya_ui_t *ui, const char *text);

// 文字大小:切换正文/名字所用字号,并返回该字号对应的分页参数。
void saya_ui_set_font_size(saya_ui_t *ui, bool large);

// 选项
void saya_ui_show_choices(saya_ui_t *ui, const char *first, const char *second);
void saya_ui_hide_choices(saya_ui_t *ui);
void saya_ui_set_choice_selected(saya_ui_t *ui, int index);
int saya_ui_choice_selected(const saya_ui_t *ui);

// 列表页
void saya_ui_set_title_rows(saya_ui_t *ui, const char *const *labels, int count, int selected);
void saya_ui_set_menu_rows(saya_ui_t *ui, const char *const *labels, int count, int selected);
void saya_ui_set_about_rows(saya_ui_t *ui, const char *const *labels, int count, int selected);
void saya_ui_set_settings_rows(saya_ui_t *ui, const char *const *labels,
                               const char *const *values, int count, int selected);
void saya_ui_set_setting_value(saya_ui_t *ui, int row, const char *value);
void saya_ui_set_settings_selected(saya_ui_t *ui, int index);
void saya_ui_slots_setup(saya_ui_t *ui, const char *title, const char *hint,
                         const char *const *labels, const char *const *values, int count);
void saya_ui_set_slots_selected(saya_ui_t *ui, int index);
void saya_ui_set_ending(saya_ui_t *ui, const char *name, const char *hint);
void saya_ui_set_warning(saya_ui_t *ui, const char *text, const char *hint);
// 警告页正文滚动(短按一行、长按四行由调用方决定步长);滚到底后提示才会变成"可继续"。
void saya_ui_warning_scroll(saya_ui_t *ui, int dy);
// 正文是否已读到底(已滚到底返回 true,未读完会保持"滚动阅读"提示)。
bool saya_ui_warning_ready(saya_ui_t *ui);
// 未读完时按确定 / 长按上、下:整屏翻页;dir<0 往上,dir>0 往下。
void saya_ui_warning_page(saya_ui_t *ui, int dir);
void saya_ui_set_about_text(saya_ui_t *ui, const char *text);
// 滚动关于正文:dy > 0 看后面的内容(按“下”),dy < 0 回看(按“上”)。越界自动夹紧。
void saya_ui_about_scroll(saya_ui_t *ui, int dy);
void saya_ui_set_battery(saya_ui_t *ui, int percent);
