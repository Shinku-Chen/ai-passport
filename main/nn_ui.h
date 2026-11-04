// main/nn_ui.h —— 横屏阅读器界面（320x240）。
//
// 界面只有几层：一块场景画布（nn_canvas）+ 顶部状态条 + 底部对话框 + 一个可复用
// 的列表/网格浮层。所有页面共用这些对象，切页只改可见性与内容，避免反复
// create/delete。所有函数都要求调用方已经持有 bsp_lvgl_lock()。
#pragma once

#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NN_UI_MAX_ROWS 8

typedef enum {
    NN_PAGE_TITLE = 0,
    NN_PAGE_GAME,
    NN_PAGE_CHOICE,
    NN_PAGE_MENU,
    NN_PAGE_SLOTS,      // 存档 / 读档共用（由 nn_app 决定含义）
    NN_PAGE_SETTINGS,
    NN_PAGE_HISTORY,
    NN_PAGE_GALLERY,
    NN_PAGE_CGVIEW,
    NN_PAGE_ENDING,
    NN_PAGE_ABOUT,
} nn_page_t;

typedef struct {
    nn_page_t page;
    bool font_large;

    lv_obj_t *root;
    lv_obj_t *art;          // 场景画布第 0 条带
    lv_obj_t *art_bands[6]; // 分带图像（每带 240xNN_BAND_H，叠成整屏）
    lv_obj_t *header;       // 左上角：章节 / 场景
    lv_obj_t *battery;      // 右上角：电量
    lv_obj_t *name;         // 说话人名牌（对话框上沿外侧）
    lv_obj_t *panel;        // 对话框
    lv_obj_t *body;         // 正文
    lv_obj_t *hint;         // 对话框右下角按键提示
    lv_obj_t *page_label;   // 页码（面板内右上，独立 label）

    lv_obj_t *overlay;      // 列表浮层
    lv_obj_t *ov_title;
    lv_obj_t *ov_rows[NN_UI_MAX_ROWS];
    lv_obj_t *ov_foot;

    lv_obj_t *grid;         // 鉴赏室缩略图网格
    lv_obj_t *grid_img[6];
    lv_obj_t *grid_cap;

    lv_obj_t *center_name;  // 结局页
    lv_obj_t *center_body;
    lv_obj_t *about_body;

    char text_buf[512];     // 打字机缓冲
} nn_ui_t;

bool nn_ui_create(nn_ui_t *ui);

// 页面切换：只改可见性。
void nn_ui_show(nn_ui_t *ui, nn_page_t page);

// 场景画布重画后调用（让 LVGL 重新取一帧）。
void nn_ui_refresh_art(nn_ui_t *ui);
// 只刷某一条带（分带绘制：画完一带立刻 invalidate+flush）。
void nn_ui_refresh_band(nn_ui_t *ui, int band);

// 正文。shown_chars <= 0 表示整页直接显示；否则只显示前 shown_chars 个字符。
void nn_ui_set_dialogue(nn_ui_t *ui, const char *speaker, const char *text,
                        int shown_bytes);
void nn_ui_set_hint(nn_ui_t *ui, const char *text);
// 页码：独立 label；pages<=1 时隐藏。
void nn_ui_set_page(nn_ui_t *ui, int page, int pages);
void nn_ui_set_header(nn_ui_t *ui, const char *text);
void nn_ui_set_battery(nn_ui_t *ui, int percent);
void nn_ui_set_panel_opa(nn_ui_t *ui, uint8_t percent);
void nn_ui_set_font_large(nn_ui_t *ui, bool large);

// 列表浮层（标题/菜单/选项/存档/设置/关于）。
void nn_ui_set_overlay(nn_ui_t *ui, const char *title, const char *const *rows,
                       const char *const *values, int count, int selected);
void nn_ui_overlay_foot(nn_ui_t *ui, const char *text);

// 浮层不透明度（标题页用低一点的透明度，让标题画透出来）。
void nn_ui_overlay_opa(nn_ui_t *ui, uint8_t percent);

// 紧凑浮层：只占屏幕上半部分，底部对话框继续可见（选项页用）。
void nn_ui_overlay_compact(nn_ui_t *ui, bool compact);
// 标题页：3 列 x 2 行菜单放到画面区下方（画布只有 150 高）。
void nn_ui_overlay_title_layout(nn_ui_t *ui, bool grid);

// 鉴赏室网格：缩略图 + 说明。
void nn_ui_set_gallery(nn_ui_t *ui, const void *const *thumbs, int count, int selected,
                       const char *caption);

// 只改网格下方的说明文字（未解锁提示这类，不动缩略图）。
void nn_ui_gallery_caption(nn_ui_t *ui, const char *caption);

// 结局页。
void nn_ui_set_ending(nn_ui_t *ui, const char *name, const char *text);

// 关于/历史共用的正文块（支持多行）。
void nn_ui_set_about(nn_ui_t *ui, const char *text);

#ifdef __cplusplus
}
#endif
