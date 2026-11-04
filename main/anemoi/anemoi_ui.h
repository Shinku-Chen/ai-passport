// main/anemoi/anemoi_ui.h —— 320x240 横屏界面模型（纯 C，不依赖 LVGL）。
//
// 生产渲染器 anemoi_render.c 只负责把这份模型画成 LVGL 对象；宿主测试直接断言
// 模型里的可见性、行文本、坐标与选中项，不需要造整个 LVGL。这样“页面是否真的
// 显示、行文字是什么、坐标在哪”都是可回归的数据，而不是隐藏在某次绘制里。
#pragma once

#include "anemoi_pack.h"
#include "anemoi_script.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANEM_VIEW_W 320
#define ANEM_VIEW_H 240
#define ANEM_TEXT_Y 180
#define ANEM_TEXT_H 60
#define ANEM_ROW_MAX 10
#define ANEM_ROW_TEXT 96
#define ANEM_LINE_MAX 4
#define ANEM_LINE_TEXT 72
#define ANEM_HEADER_TEXT 80
#define ANEM_NOTICE_TEXT 56

enum {
    ANEM_SCR_TITLE = 0,
    ANEM_SCR_READ,
    ANEM_SCR_CHOICE,
    ANEM_SCR_MENU,
    ANEM_SCR_SAVE,
    ANEM_SCR_LOAD,
    ANEM_SCR_CHAPTERS,
    ANEM_SCR_GALLERY,
    ANEM_SCR_GALLERY_MENU,
    ANEM_SCR_HISTORY,
    ANEM_SCR_SETTINGS,
    ANEM_SCR_ABOUT,
    ANEM_SCR_CG,
    ANEM_SCR_END,
    ANEM_SCR_BOUNDARY,
    ANEM_SCR_ERROR,
    ANEM_SCR_COUNT,
};

enum {
    ANEM_VIEW_TEXT_ROW = 0,
    ANEM_VIEW_TEXT_HEADER,
    ANEM_VIEW_TEXT_HINT,
    ANEM_VIEW_TEXT_TITLE,
    ANEM_VIEW_TEXT_SPEAKER,
    ANEM_VIEW_TEXT_LINE,
};

typedef struct {
    char text[ANEM_ROW_TEXT];
    int16_t x, y, w, h;
    uint8_t kind;
    bool selected;
    bool enabled;
} anem_view_row_t;

typedef struct {
    uint8_t screen;
    bool show_art;
    bool show_panel;          // 列表/对话框面板
    bool fullscreen_art;      // CG / 标题画面
    uint16_t art_image;       // 画布上要画的背景/CG（ANEM_NONE16 = 不重画）
    uint8_t sprite_count;
    anem_sprite_t sprite[ANEM_BODY_MAX];
    char header[ANEM_HEADER_TEXT];
    char notice[ANEM_NOTICE_TEXT];
    uint8_t notice_kind;      // 0 无，1 信息，2 错误
    uint8_t row_count;
    anem_view_row_t row[ANEM_ROW_MAX];
    uint16_t sel;
    uint16_t page;
    uint16_t pages;
    uint8_t line_count;
    char line[ANEM_LINE_MAX][ANEM_LINE_TEXT];
    char speaker[ANEM_ROW_TEXT];
    bool typing;
} anem_view_t;

// 清空模型。
void anem_view_clear(anem_view_t *view);

// 行布局：x = 8, w = 304, h = 20, 从 y0 起。
void anem_view_add_row(anem_view_t *view, int y, uint8_t kind, const char *text, bool selected,
                       bool enabled);

// 文本切片：按 UTF-8 字符切分（不切断多字节字符）。返回写入字节数。
size_t anem_utf8_slice(const char *text, uint16_t from_char, uint16_t max_chars, char *out,
                       size_t cap);
uint16_t anem_utf8_length(const char *text);
// 计算一页能放多少字符（按每行字符数上限分行）。
uint16_t anem_text_pages(const char *text, uint16_t chars_per_line, uint16_t lines);
// 取第 page 页的文本（按行折行成 lines 行，逗号写入 out）。
uint8_t anem_text_page(const char *text, uint16_t chars_per_line, uint16_t lines, uint16_t page,
                       char out[ANEM_LINE_MAX][ANEM_LINE_TEXT]);

#ifdef __cplusplus
}
#endif
