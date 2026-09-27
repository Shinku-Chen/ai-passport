// main/tsxx_ui.h —— 《天使☆騒々 RE-BOOT!》阅读器的 LVGL 界面层。
//
// 版面(竖屏 240x320,画布与屏幕 1:1,文字层画在原生分辨率上):
//   整屏        美术画布 240x320:立绘原尺寸合成;背景/事件图在包里是 180x240,
//               合成时按最近邻放大铺满(见 tsxx_image.h)
//   y 188..212   说话人名牌(带子上方左侧),右侧同行显示 自动 / 快进 状态
//   y 214..319   正文带:半透明深夜蓝底,顶边一条 2px 香槟金细线;正文 5 行 x 13 全角字
//   左上角       当前章节标签(章节页/正文页)
//   右上角       电量百分比(-1 时不画数字,只留空位)
// 选项、结局覆盖层、列表页(菜单 / 存读档 / 章节 / 设置 / 关于)都是叠在这层之上的整屏页面。
//
// 本模块只"把给定的内容画出来",不决定剧情走向 —— 状态机在 tsxx_app.c。
#pragma once

#include "tsxx_image.h"
#include "tsxx_model.h"
#include "tsxx_pack.h"

#include "lvgl.h"   // lv_font_t / lv_obj_t:界面层本身就依赖 LVGL

#include <stdbool.h>
#include <stdint.h>

#define TSXX_UI_W 240
#define TSXX_UI_H 320
// 正文带顶边:画布/屏幕同坐标,214 起盖住画面区下沿。立绘整身存到画布底边,
// 下半身就由这条带子压住(与源工程的版式一致)。
#define TSXX_BAND_Y 214
#define TSXX_BAND_H (TSXX_UI_H - TSXX_BAND_Y)
// 正文排版:16px 字体、20px 行高;一行 13 个全角字(26 个半角单位),
// 正文标签宽 224px,留 16px 余量给半角字符,避免 LVGL 自己折行把分屏算错。
#define TSXX_LINE_H 20
#define TSXX_TEXT_LINES 5
#define TSXX_UNITS_PER_LINE 26
#define TSXX_BODY_X 8
#define TSXX_BODY_W (TSXX_UI_W - 2 * TSXX_BODY_X)
// 说话人名牌与状态标签所在的一行(正文带上方)。
#define TSXX_TAG_Y (TSXX_BAND_Y - 26)
// 列表页最多显示几行(行高 26 + 间距 6)。
#define TSXX_UI_MAX_ROWS 7
// 选项点最多几个(资源包里最多 5 个)。
#define TSXX_CHOICE_MAX 5

typedef enum {
    TSXX_PAGE_WARNING = 0,   // 首次开机的同人移植提示(读到底才放行)
    TSXX_PAGE_TITLE,         // 标题页:背景画 + 纵向菜单
    TSXX_PAGE_GAME,          // 正文页
    TSXX_PAGE_MENU,          // 阅读菜单
    TSXX_PAGE_SAVE,          // 保存进度
    TSXX_PAGE_LOAD,          // 读取存档
    TSXX_PAGE_CHAPTERS,      // 章节跳转
    TSXX_PAGE_SETTINGS,      // 系统设置
    TSXX_PAGE_ABOUT,         // 关于本作
    TSXX_PAGE_COUNT,
} tsxx_page_id_t;

typedef enum {
    TSXX_LIST_TITLE = 0,
    TSXX_LIST_MENU,
    TSXX_LIST_SAVE,
    TSXX_LIST_LOAD,
    TSXX_LIST_CHAPTERS,
    TSXX_LIST_SETTINGS,
    TSXX_LIST_COUNT,
} tsxx_list_id_t;

// 通用"行列表":一行 = 左侧文字 + 右侧取值(可空)。调用方只传当前要显示的那几行,
// 需要窗口滑动的页面(章节页 45 条)自己算好窗口再传进来。
typedef struct {
    struct _lv_obj_t *page;
    struct _lv_obj_t *header;
    struct _lv_obj_t *rule;
    struct _lv_obj_t *counter;      // 页眉右侧的"3/45"
    struct _lv_obj_t *hint;
    struct _lv_obj_t *rows[TSXX_UI_MAX_ROWS];
    struct _lv_obj_t *labels[TSXX_UI_MAX_ROWS];
    struct _lv_obj_t *values[TSXX_UI_MAX_ROWS];
    int count;
    int selected;
} tsxx_list_t;

// 可滚动正文页(警告页 / 关于页):三键设备没有触摸滑屏,滚动完全由按键驱动。
typedef struct {
    struct _lv_obj_t *page;
    struct _lv_obj_t *scroll;   // 滚动容器
    struct _lv_obj_t *body;     // LV_LABEL_LONG_WRAP 的正文标签
    struct _lv_obj_t *hint;     // 底部提示
} tsxx_scroll_t;

typedef struct {
    tsxx_art_t art;                 // 画面区画布(美术层)

    struct _lv_obj_t *band;           // 正文带(半透明深夜蓝)
    struct _lv_obj_t *band_rule;      // 带顶边的香槟金细线
    struct _lv_obj_t *body;           // 正文
    struct _lv_obj_t *name_tag;       // 说话人名牌
    struct _lv_obj_t *name_text;
    struct _lv_obj_t *chapter;        // 左上角:当前章节
    struct _lv_obj_t *battery;        // 右上角:电量
    struct _lv_obj_t *mode;           // 正文带上方右侧:自动 / 快进

    struct _lv_obj_t *choice_box;     // 选项(盖在画面区上)
    struct _lv_obj_t *choice_rows[TSXX_CHOICE_MAX];
    struct _lv_obj_t *choice_texts[TSXX_CHOICE_MAX];
    int choice_count;
    int choice_selected;

    struct _lv_obj_t *title_big;
    struct _lv_obj_t *title_sub;
    struct _lv_obj_t *title_hint;

    struct _lv_obj_t *ending;         // 结局/休眠覆盖层(整屏)
    struct _lv_obj_t *ending_title;
    struct _lv_obj_t *ending_hint;
    bool ending_visible;

    struct _lv_obj_t *notice;         // 瞬时提示(保存成功等)
    struct _lv_obj_t *notice_text;

    tsxx_list_t lists[TSXX_LIST_COUNT];
    tsxx_scroll_t warning;
    tsxx_scroll_t about;

    const lv_font_t *font_cjk;        // 16px 中文子集(正文 / 名字 / 菜单)
    const lv_font_t *font_num;        // Montserrat 14(纯数字浮层)

    tsxx_page_id_t page_current;

    // 打字机:由 tsxx_app 的 tick 驱动(不在 LVGL 定时器里跑,避免和按键/刷新抢锁)。
    char typing_target[TSXX_TEXT_BUFFER];
    char typing_shown[TSXX_TEXT_BUFFER];
    uint32_t typing_index;
    uint32_t typing_total;
    uint32_t typing_accum_ms;
    uint32_t typing_step_ms;
    bool typing_active;

    bool auto_on;
    bool fast_on;
} tsxx_ui_t;

// 建界面(需持有 LVGL 锁)。art_pixels 由调用方静态提供并保证生命周期。
bool tsxx_ui_create(tsxx_ui_t *ui, uint16_t *art_pixels, const lv_font_t *font_cjk);

// 切换页面:隐去其他页的容器、按页面决定浮层显隐。不改变画布内容。
void tsxx_ui_show_page(tsxx_ui_t *ui, tsxx_page_id_t page);

// 画面区:背景 + 事件图 + 立绘(cg 可为 NULL;sprite 为 TSXX_NONE8 表示本页没有立绘)。
bool tsxx_ui_set_art(tsxx_ui_t *ui, const tsxx_pack_t *pack, uint8_t bg, const tsxx_cg_t *cg,
                     uint8_t sprite);
void tsxx_ui_clear_art(tsxx_ui_t *ui);

// 正文页:设置说话人与本屏文本,并按 ms_per_char 逐字显示(0 = 立即全显)。
void tsxx_ui_set_text(tsxx_ui_t *ui, const char *speaker, const char *text,
                      uint32_t ms_per_char);
// 由 app 的 tick 调用:推进打字机。elapsed_ms 是距上次调用的毫秒数。
void tsxx_ui_typing_tick(tsxx_ui_t *ui, uint32_t elapsed_ms, uint32_t ms_per_char);
void tsxx_ui_finish_typing(tsxx_ui_t *ui);
bool tsxx_ui_typing(const tsxx_ui_t *ui);

// 浮层:章节标签(空串 = 不显示)、电量(-1 = 不画数字)。
void tsxx_ui_set_chapter(tsxx_ui_t *ui, const char *label);
void tsxx_ui_set_battery(tsxx_ui_t *ui, int percent);
// 自动阅读 / 快进 状态指示(正文带上方右侧)。
void tsxx_ui_set_mode(tsxx_ui_t *ui, bool auto_on, bool fast_on);

// 选项:显示时正文带让位(玩家此时只需要看选项)。labels 需有 count 项,count <= TSXX_CHOICE_MAX。
void tsxx_ui_show_choices(tsxx_ui_t *ui, const char *const *labels, int count);
void tsxx_ui_hide_choices(tsxx_ui_t *ui);
void tsxx_ui_set_choice_selected(tsxx_ui_t *ui, int index);
int tsxx_ui_choice_selected(const tsxx_ui_t *ui);

// 结局 / 休眠覆盖层:整屏半透明底 + 一行大字 + 一行提示。
void tsxx_ui_ending_overlay(tsxx_ui_t *ui, const char *title, const char *hint, bool visible);

// 标题页文字(标题 / 副标题 / 底部提示)。
void tsxx_ui_set_title(tsxx_ui_t *ui, const char *title, const char *subtitle,
                       const char *hint);

// 列表页:labels 必填,values 可为 NULL。count <= TSXX_UI_MAX_ROWS,selected 会被夹紧。
// counter 是页眉右侧的位置提示("3/45"),可为 NULL。
void tsxx_ui_set_list(tsxx_ui_t *ui, tsxx_list_id_t id, const char *title, const char *hint,
                      const char *counter, const char *const *labels, const char *const *values,
                      int count, int selected);
void tsxx_ui_set_list_selected(tsxx_ui_t *ui, tsxx_list_id_t id, int selected);
void tsxx_ui_set_list_value(tsxx_ui_t *ui, tsxx_list_id_t id, int row, const char *value);

// 警告页 / 关于页:设置正文与提示(body 传 NULL 时保留现有正文),并把滚动复位到顶部。
void tsxx_ui_set_warning(tsxx_ui_t *ui, const char *body, const char *hint);
void tsxx_ui_set_about(tsxx_ui_t *ui, const char *body, const char *hint);
// 按像素滚动(delta > 0 向下)。返回是否已经滚到底。
bool tsxx_ui_scroll_by(tsxx_ui_t *ui, tsxx_page_id_t page, int delta);
// 当前是否已滚到底(警告页的放行条件)。
bool tsxx_ui_scrolled_to_bottom(tsxx_ui_t *ui, tsxx_page_id_t page);

// 瞬时提示(空串隐去)。它总在最上层,任何页面都能用。
void tsxx_ui_notice(tsxx_ui_t *ui, const char *text);
