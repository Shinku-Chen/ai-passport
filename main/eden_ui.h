// main/eden_ui.h —— eden* 横屏(320x240)界面层。
//
// 版面(与 main/eden_image.h 一致):
//   画面区  x 28..291, y 0..164  :264x165 原生画面(LVGL 画布)
//   正文带  y 165..239           :74px 半透明面板,16px 档 3 行 / 20px 档 3 行
//   名牌                         :正文带上沿外侧、左对齐;旁白不显示
//   章节 / 电量                  :正文带右上角、左上角
//
// 本文件只负责"把给定内容画出来",页面切换与按键语义在 eden_app.c。
// 与框架骨架的不同:横屏、真图画面(画布直读)、CG 鉴赏按页翻而不是固定 8 格、
// 章节列表可滚动、选项页保留但没有内容会触发(eden 原作是纯剧情)。
#pragma once

#include "lvgl.h"
#include "eden_image.h"   // EDEN_CG_BAND_W/H、EDEN_CG_BANDS

#include <stdbool.h>
#include <stdint.h>

#define EDEN_UI_W 320
#define EDEN_UI_H 240

// 正文带:画面 165 高之后到底部。
#define EDEN_BAND_Y 168   // 画布 168 行：面板从画布下沿开始，0 压画面
#define EDEN_BAND_H (EDEN_UI_H - EDEN_BAND_Y)
#define EDEN_TEXT_X 12
#define EDEN_TEXT_W (EDEN_UI_W - 2 * EDEN_TEXT_X)
#define EDEN_LINE_H_SMALL 20
#define EDEN_LINE_H_LARGE 24
#define EDEN_TEXT_LINES 3
// 每行能放多少"单位"(半角 1 / 全角 2):16px 档 18 字 = 36 单位,20px 档 14 字 = 28 单位。
#define EDEN_UNITS_SMALL 36
#define EDEN_UNITS_LARGE 28
// 单句最长字节数(源剧本最长一句 < 400B,这里给足余量)。
#define EDEN_TEXT_CAPACITY 1024
// 一页最多几行列表(章节 21 章需要滚动,不做上限裁剪)。
#define EDEN_ROWS_MAX 32
// CG 缩略图:每页 2 行 × 3 列。
#define EDEN_CG_COLS 3
#define EDEN_CG_ROWS 2
#define EDEN_CG_PER_PAGE (EDEN_CG_COLS * EDEN_CG_ROWS)

typedef enum {
    EDEN_PAGE_START = 0,   // 启动页(徽标 + 提示)
    EDEN_PAGE_TITLE,       // 标题页(菜单)
    EDEN_PAGE_GAME,        // 对话页
    EDEN_PAGE_CHOICE,      // 选项(eden 原作没有选项,页面保留)
    EDEN_PAGE_MENU,        // 暂停菜单
    EDEN_PAGE_SAVE,        // 存档
    EDEN_PAGE_LOAD,        // 读档
    EDEN_PAGE_SETTINGS,    // 设置
    EDEN_PAGE_HISTORY,     // 历史
    EDEN_PAGE_GALLERY,     // 鉴赏室入口
    EDEN_PAGE_CG_LIST,     // CG 缩略图分页
    EDEN_PAGE_CG,          // CG 全屏
    EDEN_PAGE_CHAPTER,     // 章节鉴赏
    EDEN_PAGE_TRANSITION,  // 章节过场
    EDEN_PAGE_ENDING,      // 结局
    EDEN_PAGE_COUNT,
} eden_page_t;

typedef enum {
    EDEN_LIST_TITLE = 0,
    EDEN_LIST_MENU,
    EDEN_LIST_SAVE,
    EDEN_LIST_LOAD,
    EDEN_LIST_SETTINGS,
    EDEN_LIST_HISTORY,
    EDEN_LIST_GALLERY,
    EDEN_LIST_CHAPTER,
    EDEN_LIST_CG,
    EDEN_LIST_CHOICE,
    EDEN_LIST_COUNT,
} eden_list_t;

typedef struct {
    lv_obj_t *page;
    lv_obj_t *rows[EDEN_ROWS_MAX];
    bool disabled[EDEN_ROWS_MAX];
    int count;
    int selected;
    int row_h;
    int top;
    int inset;
    lv_obj_t *view;   // 可滚动容器
} eden_list_ui_t;

typedef struct {
    lv_obj_t *pages[EDEN_PAGE_COUNT];
    lv_obj_t *bg;                 // 全屏底色(渐变)
    lv_obj_t *canvas_slot;        // 对话页的画面槽(由 app 把画布挂进来)
    lv_obj_t *title_slot;         // 标题页的画面槽(同一个画布切页时搬进来)
    lv_obj_t *cg_slot;
    lv_obj_t *scene_canvas;       // app 挂进来的画布(切换页面时在槽之间搬)

    lv_obj_t *brand;              // 启动页/标题页的徽标图
    lv_obj_t *brand_title;
    lv_obj_t *brand_sub;

    lv_obj_t *panel;              // 正文带
    lv_obj_t *name;               // 名牌
    lv_obj_t *page_label;         // 页码（面板内右上，独立 label）
    lv_obj_t *text;               // 正文
    lv_obj_t *chapter;            // 章节
    lv_obj_t *battery;            // 电量
    lv_obj_t *notice;             // 顶部提示
    lv_obj_t *mode_auto;          // "自动"

    lv_obj_t *titles[EDEN_PAGE_COUNT];   // 各页页头
    lv_obj_t *footers[EDEN_PAGE_COUNT];  // 各页页脚
    lv_obj_t *dim;                       // 菜单/弹窗压暗层

    lv_obj_t *cg_tiles[EDEN_CG_PER_PAGE];
    lv_obj_t *cg_labels[EDEN_CG_PER_PAGE];
    lv_obj_t *cg_index;           // 保留:CG 页页码文字
    lv_obj_t *cg_name;
    lv_obj_t *cg_caption;

    lv_obj_t *transition_eyebrow;
    lv_obj_t *transition_title;
    lv_obj_t *transition_sub;
    lv_obj_t *ending_eyebrow;
    lv_obj_t *ending_title;
    lv_obj_t *ending_body;

    lv_obj_t *modal_box;
    lv_obj_t *modal_title;
    lv_obj_t *modal_body;

    eden_list_ui_t lists[EDEN_LIST_COUNT];

    const lv_font_t *font_small;
    const lv_font_t *font_large;

    // 打字机
    lv_timer_t *typing_timer;
    char typing_target[EDEN_TEXT_CAPACITY];
    char typing_shown[EDEN_TEXT_CAPACITY];
    uint32_t typing_index;
    uint32_t typing_total;
    uint32_t typing_step_ms;
    bool typing_active;
} eden_ui_t;

// 建界面(需持有 LVGL 锁)。canvas_slot 的父对象是对话页。
bool eden_ui_create(eden_ui_t *ui, const lv_font_t *font_small, const lv_font_t *font_large);

// 把 app 建好的画布挂到对话页(会在画面位置摆放)。
void eden_ui_attach_canvas(eden_ui_t *ui, lv_obj_t *canvas);

void eden_ui_show_page(eden_ui_t *ui, eden_page_t page);
eden_page_t eden_ui_page(const eden_ui_t *ui);

void eden_ui_set_brand(eden_ui_t *ui, const char *title, const char *subtitle);
void eden_ui_set_brand_image(eden_ui_t *ui, const void *dsc);

// 正文:speaker 为空串 = 旁白;ms_per_char = 0 表示直接整段显示。
void eden_ui_set_dialogue(eden_ui_t *ui, const char *speaker, const char *page_text,
                          uint32_t ms_per_char);
// 页码：独立 label，面板内右上；pages<=1 时隐藏。
void eden_ui_set_page(eden_ui_t *ui, int page, int pages);
void eden_ui_show_full_text(eden_ui_t *ui);
bool eden_ui_typing(const eden_ui_t *ui);

void eden_ui_set_choice(eden_ui_t *ui, const char *prompt, const char *const *labels,
                        int count, int selected);

void eden_ui_set_notice(eden_ui_t *ui, const char *text);
void eden_ui_set_chapter(eden_ui_t *ui, const char *text);
void eden_ui_set_battery(eden_ui_t *ui, int percent);
void eden_ui_set_mode(eden_ui_t *ui, bool auto_play);

void eden_ui_set_font_large(eden_ui_t *ui, bool large);
void eden_ui_set_panel_opacity(eden_ui_t *ui, uint8_t percent);

void eden_ui_set_rows(eden_ui_t *ui, eden_list_t list, const char *const *labels,
                      const char *const *values, int count, int selected);
void eden_ui_set_row_value(eden_ui_t *ui, eden_list_t list, int row, const char *value);
void eden_ui_set_row_enabled(eden_ui_t *ui, eden_list_t list, int row, bool enabled);
void eden_ui_select_row(eden_ui_t *ui, eden_list_t list, int selected);

void eden_ui_set_page_title(eden_ui_t *ui, eden_page_t page, const char *text);
void eden_ui_set_footer(eden_ui_t *ui, eden_page_t page, const char *text);

// CG 鉴赏:一页最多 6 格;labels 可为 NULL(未解锁显示问号)。
void eden_ui_set_cg_grid(eden_ui_t *ui, const void *const *thumbs, const char *const *labels,
                         const bool *unlocked, int count, int selected);
void eden_ui_set_cg_page_info(eden_ui_t *ui, const char *text);
// CG 分带：只刷某一条带（画完一带立刻 invalidate+flush）。
// 把 4 条带图像绑到同一块 320x60 缓冲（分带复用）。
// 对话页分带：只刷某一条带（画完一带立刻 invalidate + flush）。


void eden_ui_set_cg(eden_ui_t *ui, const char *name, const char *caption);

void eden_ui_set_transition(eden_ui_t *ui, const char *eyebrow, const char *title,
                            const char *subtitle);
void eden_ui_set_ending(eden_ui_t *ui, const char *eyebrow, const char *title, const char *body);

void eden_ui_scroll(eden_ui_t *ui, eden_page_t page, int dy);

// 单按钮提示弹窗(存档覆盖确认、槽位为空等)。
void eden_ui_show_modal(eden_ui_t *ui, const char *title, const char *body);
void eden_ui_hide_modal(eden_ui_t *ui);
