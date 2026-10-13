// main/gal/gal_ui.h —— 框架骨架的 LVGL 界面层（竖屏 240x320）。
//
// 版面(框架规范,与原型 240x320 竖屏一致):
//   画面区      y 0..209   :程序绘制的渐变背景
//   名牌        对话框面板上沿外侧、左对齐
//   对话框面板  y 210..319 :正文(贴底、可调透明度)
// 全屏页面盖住整屏;启动页、标题页、对话页与过场、结局共用同一层背景。
//
// 本模块只负责“把给定的内容画出来”,页面切换与按键语义在 gal_app.c。
#pragma once

#include "gal_content.h"
#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>

#define GAL_UI_W 240
#define GAL_UI_H 320

// 对话框面板:贴底 110px(框架竖屏规范:16px 档 5 行 / 20px 档 4 行)。
#define GAL_PANEL_H 110
#define GAL_PANEL_Y (GAL_UI_H - GAL_PANEL_H)
#define GAL_TEXT_X 10
#define GAL_TEXT_W (GAL_UI_W - 2 * GAL_TEXT_X)
#define GAL_LINE_H_SMALL 20
#define GAL_LINE_H_LARGE 24
#define GAL_TEXT_LINES_SMALL 5
#define GAL_TEXT_LINES_LARGE 4
#define GAL_LIST_MAX_ROWS 16
#define GAL_TEXT_CAPACITY 1024
#define GAL_CG_TILES 8
#define GAL_PAGE_MODAL GAL_PAGE_COUNT   // 弹窗层:不在页面枚举里

// 全部逻辑屏(与原型 docs/design/galgame-ui-wireframes/prototype.html 对应)。
typedef enum {
    GAL_PAGE_START = 0,     // 启动画面
    GAL_PAGE_TITLE,         // 标题页
    GAL_PAGE_GAME,          // 对话页
    GAL_PAGE_CHOICE,        // 选项分支
    GAL_PAGE_MENU,          // 游戏内菜单
    GAL_PAGE_SAVE,          // 存档页
    GAL_PAGE_LOAD,          // 读档页
    GAL_PAGE_SETTINGS,      // 设置页
    GAL_PAGE_HISTORY,       // 历史文本
    GAL_PAGE_GALLERY,       // 鉴赏室(分类入口)
    GAL_PAGE_CG_LIST,       // CG 鉴赏(缩略图)
    GAL_PAGE_RECALL,        // CG 回想(重看已看过的场景)
    GAL_PAGE_MUSIC,         // 音乐鉴赏
    GAL_PAGE_CHAPTER,       // 剧情鉴赏(章节 / 路线)
    GAL_PAGE_TRANSITION,    // 章节过场
    GAL_PAGE_ENDING,        // 结局
    GAL_PAGE_ABOUT,         // 关于
    GAL_PAGE_CG,            // CG 全屏浏览
    GAL_PAGE_COUNT,
} gal_page_t;

// 叠加在页面上的弹窗(存档覆盖确认 / 无存档空状态)。
typedef enum {
    GAL_MODAL_NONE = 0,
    GAL_MODAL_CONFIRM,
    GAL_MODAL_EMPTY,
} gal_modal_t;

// 可复用行列表:每屏一个,行是懒创建的。
typedef enum {
    GAL_LIST_START = 0,   // 启动页的“开始游戏”
    GAL_LIST_TITLE,       // 标题页菜单
    GAL_LIST_CHOICE,      // 选项分支
    GAL_LIST_MENU,        // 游戏内菜单
    GAL_LIST_SAVE,        // 存档槽位
    GAL_LIST_LOAD,        // 读档槽位
    GAL_LIST_SETTINGS,    // 设置项
    GAL_LIST_HISTORY,     // 历史文本
    GAL_LIST_GALLERY,     // 鉴赏室分类
    GAL_LIST_RECALL,      // CG 回想
    GAL_LIST_MUSIC,       // 音乐鉴赏
    GAL_LIST_CHAPTER,     // 章节 / 路线
    GAL_LIST_ENDING,      // 结局后续入口
    GAL_LIST_MODAL,       // 弹窗按钮
    GAL_LIST_COUNT,
} gal_list_t;

typedef struct {
    lv_obj_t *page;
    lv_obj_t *box;                          // 可选的外层框(菜单/弹窗)
    lv_obj_t *title;
    lv_obj_t *rows[GAL_LIST_MAX_ROWS];
    lv_obj_t *values[GAL_LIST_MAX_ROWS];
    bool disabled[GAL_LIST_MAX_ROWS];
    int count;
    int selected;
    int row_h;
    int top;
    int inset;                              // 左右留白
    int width;                              // 容器宽度(行宽 = width - 2*inset)
    bool wrap;                              // 行文字自动换行(历史条目)
    const lv_font_t *font;
} gal_list_ui_t;

typedef struct {
    const char *speaker;
    const char *text;
} gal_history_row_t;

typedef struct {
    lv_obj_t *bg;             // 全屏渐变背景

    lv_obj_t *pages[GAL_PAGE_COUNT + 1];   // 各页面根对象(末尾多一个是弹窗层)

    lv_obj_t *brand_title;    // 启动页/标题页上的作品名
    lv_obj_t *brand_sub;      // 副标题

    lv_obj_t *panel;          // 对话框面板
    lv_obj_t *name;           // 说话人名牌(面板上沿外侧)
    lv_obj_t *text;           // 正文
    lv_obj_t *chapter;        // 左上角章节
    lv_obj_t *battery;        // 右上角电量
    lv_obj_t *notice;         // 顶部提示(存读档等)
    lv_obj_t *mode_auto;      // “自动”指示(快进不另显示文字)

    lv_obj_t *titles[GAL_PAGE_COUNT];   // 各页页头标题
    lv_obj_t *choice_box;               // 选项框
    lv_obj_t *menu_box;                 // 游戏内菜单框
    lv_obj_t *dim;                      // 菜单/弹窗的背景压暗

    lv_obj_t *save_preview;             // 存档页的槽位预览
    lv_obj_t *footers[GAL_PAGE_COUNT];  // 各页页脚文字
    lv_obj_t *chapter_info;             // 章节说明
    lv_obj_t *action_hint;              // 页面底部的说明文字

    lv_obj_t *transition_eyebrow;       // 过场:CHAPTER 02
    lv_obj_t *transition_title;
    lv_obj_t *transition_sub;

    lv_obj_t *ending_eyebrow;           // 结局:TRUE END
    lv_obj_t *ending_title;
    lv_obj_t *ending_body;

    lv_obj_t *about_logo;
    lv_obj_t *about_logo_sub;
    lv_obj_t *about_view;               // 关于正文的滚动容器
    lv_obj_t *about_text;

    lv_obj_t *cg_tiles[GAL_CG_TILES];   // 鉴赏室缩略图
    lv_obj_t *cg_tiles_label[GAL_CG_TILES];
    lv_obj_t *cg_index;                 // CG 浏览:CG 01 / 08
    lv_obj_t *cg_name;
    lv_obj_t *cg_caption;

    lv_obj_t *modal_box;
    lv_obj_t *modal_title;
    lv_obj_t *modal_body;
    lv_obj_t *modal_mark;

    gal_list_ui_t lists[GAL_LIST_COUNT];

    const lv_font_t *font_small;   // 16px 子集
    const lv_font_t *font_large;   // 20px 子集

    lv_timer_t *typewriter_timer;
    char typing_target[GAL_TEXT_CAPACITY];
    char typing_shown[GAL_TEXT_CAPACITY];
    uint32_t typing_index;
    uint32_t typing_total;
    uint32_t typing_step_ms;
    bool typing_active;
} gal_ui_t;

// 建界面(需持有 LVGL 锁)。
bool gal_ui_create(gal_ui_t *ui, const lv_font_t *font_small, const lv_font_t *font_large);

void gal_ui_show_page(gal_ui_t *ui, gal_page_t page);

// 启动页/标题页上的作品名与副标题。
void gal_ui_set_brand(gal_ui_t *ui, const char *title, const char *subtitle);

// 正文页与选项页。
void gal_ui_set_text(gal_ui_t *ui, const char *speaker, const char *page_text,
                     uint32_t ms_per_char);
void gal_ui_show_full_text(gal_ui_t *ui);
bool gal_ui_typing(const gal_ui_t *ui);
void gal_ui_set_choice(gal_ui_t *ui, const char *prompt, const gal_option_t *options,
                       int count, int selected);

// 自动阅读指示(快进只在视觉上变化,不显示文字)。
void gal_ui_set_mode(gal_ui_t *ui, bool auto_play);

// 状态栏与提示。
void gal_ui_set_chapter(gal_ui_t *ui, const char *text);
void gal_ui_set_battery(gal_ui_t *ui, int percent);
void gal_ui_set_notice(gal_ui_t *ui, const char *text);   // NULL/空串 = 清除

// 文字大小与对话框透明度。
void gal_ui_set_font_size(gal_ui_t *ui, bool large);
void gal_ui_set_panel_transparency(gal_ui_t *ui, uint8_t percent);

// 行列表(标题菜单、选项、槽位、设置、历史、章节、结局入口)。
void gal_ui_set_rows(gal_ui_t *ui, gal_list_t list, const char *const *labels,
                     const char *const *values, int count, int selected);
void gal_ui_set_row_value(gal_ui_t *ui, gal_list_t list, int row, const char *value);
void gal_ui_select_row(gal_ui_t *ui, gal_list_t list, int selected);
void gal_ui_set_row_enabled(gal_ui_t *ui, gal_list_t list, int row, bool enabled);
void gal_ui_set_page_title(gal_ui_t *ui, gal_page_t page, const char *title);

// 存档页与章节页的说明文字。
void gal_ui_set_save_preview(gal_ui_t *ui, const char *text);
void gal_ui_set_footer(gal_ui_t *ui, gal_page_t page, const char *text);
void gal_ui_set_chapter_info(gal_ui_t *ui, const char *text);
void gal_ui_set_action_hint(gal_ui_t *ui, const char *text);

// 鉴赏室与 CG 浏览。
void gal_ui_set_gallery(gal_ui_t *ui, const gal_cg_t *items, int count, int selected);
void gal_ui_set_cg(gal_ui_t *ui, const char *index_text, const char *name, const char *caption);

// 章节过场与结局。
void gal_ui_set_transition(gal_ui_t *ui, const char *eyebrow, const char *title,
                           const char *subtitle);
void gal_ui_set_ending(gal_ui_t *ui, const char *eyebrow, const char *title, const char *body);

// 关于页。
void gal_ui_set_about(gal_ui_t *ui, const char *logo, const char *logo_sub, const char *text);
void gal_ui_scroll(gal_ui_t *ui, gal_page_t page, int dy);

// 弹窗(叠加在存档 / 读档页上)。
void gal_ui_show_modal(gal_ui_t *ui, gal_modal_t kind, const char *title, const char *body,
                       const char *const *actions, int count, int selected);
void gal_ui_hide_modal(gal_ui_t *ui);
