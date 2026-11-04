// main/gal/gal_app.h —— 《纸上的魔法使》横屏阅读器状态与接口。
#pragma once

#include "lvgl.h"
#include "paper_game.h"
#include "paper_gfx.h"

#include <stdbool.h>
#include <stdint.h>

#define GAL_HISTORY_MAX PAPER_HISTORY_MAX
#define GAL_TEXT_CAP 2048
#define GAL_LINE_MAX 6
#define GAL_ROWS_VISIBLE 5

typedef struct {
    uint8_t btn;
    uint8_t ev;
} gal_key_t;

typedef enum {
    GAL_PAGE_TITLE = 0,
    GAL_PAGE_READER,
    GAL_PAGE_CHOICE,
    GAL_PAGE_MENU,
    GAL_PAGE_SAVE,
    GAL_PAGE_LOAD,
    GAL_PAGE_SETTINGS,
    GAL_PAGE_HISTORY,
    GAL_PAGE_GALLERY,
    GAL_PAGE_CG,
    GAL_PAGE_CHAPTER,
    GAL_PAGE_ENDING,
    GAL_PAGE_ABOUT,
} gal_page_t;

typedef enum {
    GAL_SPEED_SLOW = 0,
    GAL_SPEED_NORMAL,
    GAL_SPEED_FAST,
    GAL_SPEED_INSTANT,
    GAL_SPEED_COUNT
} gal_speed_t;

typedef struct gal_app_s gal_app_t;

struct gal_app_s {
    paper_game_t game;
    paper_pack_t *pack;

    gal_page_t page;
    gal_page_t back_page;      // 最近一次进入子页面时的来源（兼容旧字段）
    gal_page_t page_stack[6];  // 返回栈：长按逐级回退，不改写更上一级来源
    int page_depth;
    gal_page_t menu_origin;    // 打开菜单时的来源页（选项页/正文页）
    int menu_depth;            // 打开菜单时的返回栈深度
    uint32_t meta_retry_ms;    // 解锁进度落盘重试退避
    int sel;
    int top;
    int menu_sel;
    uint16_t auto_scene;       // 自动存档用：上次落盘时的场景，用于换场景触发

    // 当前这句
    uint16_t cur_bg;
    uint16_t cur_sprites[3];
    uint8_t speaker;
    char text[GAL_TEXT_CAP];
    int page_count;
    int page_index;
    int lines;
    int line_offs[GAL_LINE_MAX + 1];
    int typed;             // 已显示字节数
    bool typing;

    // 选项
    int choice_index;
    int choice_count;
    int choice_sel;        // 选项光标，独立于列表页共用的 sel

    // 鉴赏 / 章节光标
    int cg_group;
    int cg_member;

    // 设置
    gal_speed_t speed;
    bool auto_mode;
    uint32_t auto_accum_ms;
    bool fast_forward;
    uint32_t ff_accum_ms;
    bool suppress_click;   // 长按后忽略随之而来的 CLICK
    bool draw_error;       // 最近一次画面/立绘绘制是否失败

    // 空闲 / 提示
    uint32_t idle_ms;
    char notice[32];
    uint32_t notice_ms;
    bool sleeping;

    // LVGL 对象
    lv_obj_t *root;
    lv_obj_t *img_art;
    lv_obj_t *box;
    lv_obj_t *lbl_name;
    lv_obj_t *lbl_page;
    lv_obj_t *lbl_text;
    lv_obj_t *lbl_hint;
    lv_obj_t *lbl_notice;   // 非正文页的常驻提示行（notice 以前在这类页上无处显示）
    lv_obj_t *rows[GAL_ROWS_VISIBLE];
    lv_obj_t *row_labels[GAL_ROWS_VISIBLE];
    lv_obj_t *header;
};

bool gal_app_init(gal_app_t *app);
void gal_app_key(gal_app_t *app, const gal_key_t *key);
void gal_app_tick(gal_app_t *app, uint32_t elapsed_ms);
uint32_t gal_app_idle_ms(const gal_app_t *app);
void gal_app_flush(gal_app_t *app);
void gal_app_show_sleeping(gal_app_t *app);

#ifdef PAPER_UI_TEST_HOOKS
// 测试钩子：当前页的字节长度。
int gal_app_test_page_len(const gal_app_t *app);
// 最近一次画进画布的标题图下标（host 回归断言用）。
extern uint16_t gal_app_test_title_image;
// 仅供 host 回归：重新载入当前句（打字从头）、直接回标题
void gal_app_test_reload_line(gal_app_t *app);
void gal_app_test_goto_title(gal_app_t *app);
// 画一张指定的背景图（用于注入坏图，验证失败语义）
bool gal_app_test_draw_frame(gal_app_t *app, uint16_t image_index);
// 立绘失败注入：指定 1~3 张立绘做一次合成，返回是否全部成功
bool gal_app_test_draw_sprites(gal_app_t *app, uint16_t a, uint16_t b, uint16_t c);
// 跳到第一处选项（真实 load_step 路径）
bool gal_app_test_goto_choice(gal_app_t *app);
// 走到第 n 处选项（n 从 0 起），沿途用第 0 项推进；返回是否到达
bool gal_app_test_goto_choice_nth(gal_app_t *app, int nth);
#endif
