// main/gal/gal_app.h —— 《PARQUET》横屏（320x240）阅读器界面状态。
//
// 按键语义（三键）：
//   标题/列表   UP/DOWN 移动光标，OK 进入/确认，长按 OK 返回
//   正文        UP 推进（打字中先全显），长按 UP 快进（松手停）
//               DOWN 历史，长按 DOWN 自动阅读开关
//               OK 打开游戏内菜单，长按 OK 同样打开菜单
//   待选        UP/DOWN 移动，OK 确认，长按 OK 打开菜单
// 所有 LVGL 写入都在 bsp_lvgl_lock 下；按键回调只入队（见 main.c）。
#pragma once

#include "parquet_game.h"
#include "parquet_gfx.h"

#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>

#define PQ_HISTORY_VISIBLE 5
#define PQ_LIST_ROWS 5
#define PQ_TITLE_ROWS 4

typedef struct {
    uint8_t btn;   // bsp_btn_t
    uint8_t ev;    // bsp_btn_ev_t
} gal_key_t;

typedef enum {
    PQ_PAGE_TITLE = 0,
    PQ_PAGE_READER,
    PQ_PAGE_CHOICE,
    PQ_PAGE_MENU,
    PQ_PAGE_SAVE,
    PQ_PAGE_LOAD,
    PQ_PAGE_HISTORY,
    PQ_PAGE_SETTINGS,
    PQ_PAGE_GALLERY,
    PQ_PAGE_CG,
    PQ_PAGE_ROUTES,
    PQ_PAGE_CHAPTERS,
    PQ_PAGE_ENDING,
    PQ_PAGE_ABOUT,
    PQ_PAGE_OVERWRITE,
    PQ_PAGE_ERROR,
} pq_page_t;

typedef struct {
    // 数据与存储
    pq_game_t game;
    pq_settings_t settings;

    // 页面
    pq_page_t page;
    pq_page_t page_stack[8];
    int page_depth;
    pq_page_t menu_return;      // 菜单返回的页面
    int page_refresh;           // 每页独立光标
    int title_sel;
    int menu_sel;
    int list_sel;               // 通用列表光标（滑动窗口）
    int list_top;
    int route_sel;
    int chapter_sel;
    int gallery_sel;
    int cg_group;
    int cg_member;
    int settings_sel;
    int overwrite_slot;
    int save_sel;
    int load_sel;

    // 正文
    char text[PQ_TEXT_MAX];
    int page_index;
    int page_count;
    int page_start[8];
    int page_len[8];
    int typed;
    uint32_t type_accum_ms;   // 逐字打字的时间余数（50ms 输入节拍下累积到 speed_ms）
    bool typing;
    bool auto_mode;
    bool auto_paused;
    bool fast_forward;
    uint32_t auto_accum_ms;
    uint32_t ff_accum_ms;

    // 当前视觉（text op 快照）
    uint16_t bg;
    uint16_t bodies[3];
    uint16_t cutin;
    uint8_t view;
    uint8_t cutin_opacity;
    uint16_t speaker;
    uint8_t tag;
    uint32_t shown_op;
    bool draw_error;
    uint16_t visual_fp;    // 当前 visual 下标（同值则跳过重建/解码）
    bool reader_built;     // 阅读页对象是否已建（快进时复用）

    // 结局 / 错误
    uint16_t ending_index;
    char error_text[64];

    // LVGL 对象
    lv_obj_t *root;
    lv_obj_t *img_art;      // 画布第 0 条带（y=0）
    lv_obj_t *img_art2;     // 第 1 条带（y=120）；单块回退模式下为 NULL
    lv_obj_t *box;
    lv_obj_t *lbl_speaker;
    lv_obj_t *lbl_tag;
    lv_obj_t *lbl_page;
    lv_obj_t *lbl_text;
    lv_obj_t *lbl_hint;
    lv_obj_t *header;
    lv_obj_t *rows[PQ_LIST_ROWS];
    lv_obj_t *row_labels[PQ_LIST_ROWS];

    // 运行状态
    uint32_t meta_retry_ms;
    uint32_t auto_retry_ms;
    bool auto_dirty;          // 自动档写失败，tick 里退避重试
    uint32_t notice_ms;
    uint32_t idle_ms;
    int battery_percent;
    uint32_t battery_accum_ms;
    uint16_t auto_scene;
    bool suppress_click;
    bool sleeping;
    char notice[32];
} gal_app_t;

bool gal_app_init(gal_app_t *app);
void gal_app_key(gal_app_t *app, const gal_key_t *key);
void gal_app_tick(gal_app_t *app, uint32_t elapsed_ms);
uint32_t gal_app_idle_ms(const gal_app_t *app);
void gal_app_flush(gal_app_t *app);
void gal_app_show_sleeping(gal_app_t *app);

#ifdef PQ_UI_TEST_HOOKS
// 测试钩子：当前页的字节长度。
int gal_app_test_page_len(const gal_app_t *app);
// 测试钩子：章节自动续播（末章返回 false）。
void gal_app_test_start_chapter(gal_app_t *app, uint16_t scene);
bool gal_app_test_auto_next_chapter(gal_app_t *app);
// 测试钩子：直接重画一帧（立绘底边断言用）。
bool gal_app_test_draw_scene(gal_app_t *app);
// 测试钩子：draw_scene 调用次数（验证快进时同背景不重复解码）。
int gal_app_test_draw_scene_count(void);
void gal_app_test_reset_draw_scene_count(void);
// 宿主测试入口：直接驱动生产 UI 状态机（不模拟触摸/按键队列）。
void gal_app_test_goto_title(gal_app_t *app);
bool gal_app_test_goto_choice(gal_app_t *app);
bool gal_app_test_advance_to_scene(gal_app_t *app, uint16_t start_scene,
                                  uint16_t target_scene);
bool gal_app_test_reload_line(gal_app_t *app);
bool gal_app_test_render_scene(gal_app_t *app, uint16_t bg, uint16_t a, uint16_t b,
                               uint16_t c, uint16_t cutin);
void gal_app_test_set_page(gal_app_t *app, pq_page_t page);
#endif
