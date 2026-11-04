// main/steins_ui.h —— 320x240 横屏界面(作品专属,不复用硬件自检 demo 的视觉壳)。
//
// 版面(与 main/steins_model.h 的排版常量一致):
//   y=0..180   艺术画布 320x180(源 432x243 画布等比缩放;正文页底部 30px 被对白带盖住)
//   y=150..240 不透明对白带:说话人 19px + 三行正文(行高 19px)
//   y=0..20    顶部状态条压在画面上:左日期、右电量
// 其余页(标题/选择/列表/CG/结局/确认)各自占满 320x240。
#pragma once

#include "steins_image.h"
#include "steins_model.h"

#include <stdbool.h>
#include <stdint.h>

#define STEINS_UI_W 320
#define STEINS_UI_H 240
#define STEINS_UI_LIST_ROWS 5
#define STEINS_UI_CHOICE_ROWS 4
#define STEINS_UI_TITLE_ROWS 7          // 菜单项上限（app 侧数组）
#define STEINS_UI_TITLE_VISIBLE 4        // 单列可见行数（窗口跟随光标）
#define STEINS_UI_TITLE_TEXT 48
#define STEINS_UI_ABOUT_H 200            // ABOUT 可滚动区域高度（y=30..230）
#define STEINS_UI_LINE_H 18

typedef enum {
    STEINS_PAGE_TITLE = 0,
    STEINS_PAGE_GAME,
    STEINS_PAGE_CHOICE,
    STEINS_PAGE_MENU,
    STEINS_PAGE_SAVES,
    STEINS_PAGE_HISTORY,
    STEINS_PAGE_SETTINGS,
    STEINS_PAGE_CHAPTERS,
    STEINS_PAGE_GALLERY,
    STEINS_PAGE_CG,
    STEINS_PAGE_ENDING,
    STEINS_PAGE_ABOUT,
    STEINS_PAGE_MAIL,      // 邮件正文阅读页(选择点正文超过一屏时先看正文)
    STEINS_PAGE_PHONE_HOME,
    STEINS_PAGE_PHONE_INBOX,
    STEINS_PAGE_PHONE_SENT,
    STEINS_PAGE_PHONE_CALLS,
    STEINS_PAGE_PHONE_CONTACTS,
    STEINS_PAGE_PHONE_REPLY,
    STEINS_PAGE_PHONE_CLIP,
    STEINS_PAGE_CONFIRM,
    STEINS_PAGE_COUNT
} steins_page_t;

typedef struct {
    struct _lv_obj_t *root;
    const struct _lv_font_t *font_story;
    const struct _lv_font_t *font_ui;
    const struct _lv_font_t *font_ui_lg;
    steins_image_t image;

    struct _lv_obj_t *pages[STEINS_PAGE_COUNT];

    // 标题页
    struct _lv_obj_t *title_name;
    struct _lv_obj_t *title_worldline;
    struct _lv_obj_t *title_rows[STEINS_UI_TITLE_ROWS];
    char title_all[STEINS_UI_TITLE_ROWS][STEINS_UI_TITLE_TEXT];  // 整份菜单缓存
    int title_all_count;
    char title_text[STEINS_UI_TITLE_VISIBLE][STEINS_UI_TITLE_TEXT];
    int title_row_count;
    int title_first;                     // 窗口首项下标
    int title_sel_shown;                 // 上一次高亮行（只重绘变动行）
    int list_sel_shown;                  // 列表页上一次高亮行

    // 正文页
    struct _lv_obj_t *game_band;
    struct _lv_obj_t *game_date;
    struct _lv_obj_t *game_battery;
    struct _lv_obj_t *game_name;
    struct _lv_obj_t *game_text;
    struct _lv_obj_t *game_status;
    struct _lv_obj_t *game_page;
    struct _lv_obj_t *game_hint;

    // 选择页
    struct _lv_obj_t *choice_tag;
    struct _lv_obj_t *choice_heading;
    struct _lv_obj_t *choice_rows[STEINS_UI_CHOICE_ROWS];
    struct _lv_obj_t *choice_hint;

    // 通用列表页(菜单/存读档/历史/设置/章节/鉴赏/关于)
    struct _lv_obj_t *list_title;
    struct _lv_obj_t *list_rows[STEINS_UI_LIST_ROWS];
    struct _lv_obj_t *list_subs[STEINS_UI_LIST_ROWS];
    struct _lv_obj_t *list_hint;
    struct _lv_obj_t *about_body;    // ABOUT 页专用多行正文（WRAP）

    // 邮件正文页
    struct _lv_obj_t *mail_title;
    struct _lv_obj_t *mail_text;
    struct _lv_obj_t *mail_page;
    struct _lv_obj_t *mail_hint;

    // CG 页
    struct _lv_obj_t *cg_caption;
    struct _lv_obj_t *cg_hint;

    // 结局页
    struct _lv_obj_t *ending_title;
    struct _lv_obj_t *ending_body;
    struct _lv_obj_t *ending_hint;

    // 确认弹窗
    struct _lv_obj_t *confirm_text;
    struct _lv_obj_t *confirm_hint;

    steins_page_t page;
} steins_ui_t;

bool steins_ui_create(steins_ui_t *ui, struct _lv_obj_t *root, uint16_t *canvas_pixels,
                      const struct _lv_font_t *font_story,
                      const struct _lv_font_t *font_ui,
                      const struct _lv_font_t *font_ui_lg);

// 只显示某一页(其余全部隐藏)。CG/标题页时画布移到居中位置。
void steins_ui_show(steins_ui_t *ui, steins_page_t page);

void steins_ui_title(steins_ui_t *ui, const char *name, const char *worldline,
                     const char *const *rows, int row_count, int selected);
// 只挪高亮（上下键）：不重设别的行文本、不动位置。窗口需要滚动时用缓存的
// title_text 重填可见行——不再回头找 app 重新拼整页。
void steins_ui_title_select(steins_ui_t *ui, int selected);
// ABOUT 正文按行滚动（delta>0 向下）。返回真正的位移量。
int steins_ui_about_scroll(steins_ui_t *ui, int delta_lines);
// 对话框浓度:0 半透 1 中 2 不透明。
void steins_ui_set_box_opacity(steins_ui_t *ui, int level);
// failed=true 时将收起正文、只留提示行（图片错误）；两者互斥，不重叠。
void steins_ui_game(steins_ui_t *ui, const char *date, const char *battery,
                    const char *speaker, const char *text, const char *status,
                    const char *page, const char *hint, bool failed);
void steins_ui_choice(steins_ui_t *ui, const char *tag, const char *heading,
                      const char *const *rows, int row_count, int selected,
                      const char *hint);
// ABOUT 页：标题 + 多行正文（与列表行互斥）。
void steins_ui_about(steins_ui_t *ui, const char *title, const char *body);
void steins_ui_list(steins_ui_t *ui, const char *title, const char *const *rows,
                    const char *const *subs, int row_count, int selected,
                    const char *hint);
// 行内显式指定字体:历史页要按正文原字形显示。
void steins_ui_list_font(steins_ui_t *ui, bool story_font);
// 邮件正文页:六行正文 + 页码;按键由 app 处理(上下翻页,确定回到选项)。
void steins_ui_mail(steins_ui_t *ui, const char *title, const char *text,
                    const char *page, const char *hint);
void steins_ui_cg(steins_ui_t *ui, const char *caption, const char *hint);
void steins_ui_ending(steins_ui_t *ui, const char *title, const char *body,
                      const char *hint);
void steins_ui_confirm(steins_ui_t *ui, const char *text, const char *hint);

// 测试与调用方用的可见性检查。
bool steins_ui_page_visible(const steins_ui_t *ui, steins_page_t page);
steins_image_t *steins_ui_image(steins_ui_t *ui);
