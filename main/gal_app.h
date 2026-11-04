// main/gal_app.h —— 应用状态机(标题/正文/选项/菜单/存读档/鉴赏/章节/手册/结局)。
#pragma once

#include "gal_image.h"
#include "gal_pack.h"
#include "gal_save.h"
#include "gal_store.h"
#include "gal_text.h"
#include "gal_ui.h"
#include "gal_unlock.h"
#include "gal_vm.h"

#include <stdbool.h>
#include <stdint.h>

#define GAL_HISTORY_MAX 32
#define GAL_SLOTS_PER_PAGE 4
#define GAL_BOOK_UTF8_MAX 1232   // 419 UTF-16 units worst-case = 1,257 B + NUL

typedef enum {
    APP_PAGE_TITLE = 0,
    APP_PAGE_GAME,
    APP_PAGE_MENU,
    APP_PAGE_CHOICE,
    APP_PAGE_HISTORY,
    APP_PAGE_SAVE,
    APP_PAGE_LOAD,
    APP_PAGE_SETTINGS,
    APP_PAGE_GALLERY,
    APP_PAGE_CG,
    APP_PAGE_ROUTES,
    APP_PAGE_CHAPTERS,
    APP_PAGE_BOOK,
    APP_PAGE_BOOK_DETAIL,
    APP_PAGE_ENDING,
    APP_PAGE_ABOUT,
    APP_PAGE_ERROR,
    APP_PAGE_CONFIRM,
} app_page_t;

typedef struct {
    uint8_t btn;
    uint8_t ev;
} gal_key_t;

typedef struct {
    uint32_t text_off;
    uint16_t text_len;
    uint16_t speaker;
} gal_hist_t;

typedef struct {
    uint8_t state;      // 0 空/NOT_FOUND, 1 有效, 2 损坏/CRC, 3 I/O 读取失败
    char title[48];
    char preview[52];
} gal_slot_summary_t;

typedef struct {
    const gal_pack_t *pack;
    gal_vm_t vm;
    // 可重用的读档工作区:解码失败不触碰 vm,也不再为每次槽位摘要 malloc 5,112 B。
    // load_vm and the longest book text are never live at the same time:
    // share one scratch union to keep the no-PSRAM static budget bounded.
    union {
        gal_vm_t load_vm;
        char book_text[GAL_BOOK_UTF8_MAX];
    } scratch;
    gal_ui_t ui;
    gal_image_t image;
    gal_store_t store;
    gal_settings_t settings;
    gal_unlock_t unlock;
    uint16_t progress;
    bool progress_dirty;
    uint32_t progress_retry_ms;

    app_page_t page;
    app_page_t page_source;       // 长 OK 返回来源
    app_page_t menu_source;       // 菜单返回到 GAME/CHOICE
    app_page_t save_source;
    app_page_t load_source;
    app_page_t settings_source;
    app_page_t gallery_source;
    app_page_t book_source;

    int title_sel;
    int menu_sel;
    int choice_sel;
    int list_sel;
    int route_sel;
    int chapter_sel;
    int book_sel;
    int book_page;
    uint8_t book_pages;
    int gallery_sel;
    int cg_sel;
    int cg_variant;
    int history_sel;
    int settings_sel;
    int confirm_sel;
    int slot_sel;
    int about_page;
    int ending_page;

    bool objection_mode;
    bool auto_enabled;
    uint32_t auto_accum_ms;
    bool ff_held;
    uint32_t ff_accum_ms;
    uint32_t text_accum_ms;
    uint32_t unlock_retry_ms;
    uint32_t autosave_retry_ms;
    uint32_t last_now_ms;
    uint32_t last_tick_ms;
    uint32_t art_bg;
    uint32_t art_bodies;
    bool art_ok;
    uint8_t key_action[3];
    uint8_t key_long[3];
    uint32_t last_auto_root;
    bool autosave_due;

    bool vm_started;
    bool frame_valid;
    bool auto_valid;
    bool manual_boundary;
    bool error_image;
    int pending_page;
    int pending_choice;
    bool pending_page_valid;
    bool pending_choice_valid;
    bool pending_objection_valid;
    // 阅读 checkpoint 与当前展示页解耦:进入手册/历史/设置/读档时仍保留。
    uint8_t checkpoint_mode;    // 0=正文, 1=普通待选, 2=反驳/link
    uint8_t checkpoint_page;
    uint8_t checkpoint_choice;
    bool ui_ready;
    uint8_t save_scene_mode;
    uint8_t save_page_idx;
    int save_choice_row;
    gal_store_status_t auto_status;
    gal_frame_t frame;
    uint8_t text_page;
    uint8_t text_pages;
    uint16_t text_chapter;
    uint16_t text_cursor;
    char text_utf8[640];
    char page_text[768];
    char detail_page[768];
    char shown_text[768];
    int typing_index;
    int typing_total;
    bool typing_active;

    gal_hist_t history[GAL_HISTORY_MAX];
    int history_count;

    char choice_texts[GAL_MAX_ROWS][176];
    gal_ui_row_t choice_rows[GAL_MAX_ROWS];
    int choice_count;

    gal_ui_row_t list_rows[GAL_UI_LIST_ROWS];
    char list_text[GAL_UI_LIST_ROWS][256];
    gal_slot_summary_t slots[GAL_SLOT_COUNT];
    bool slots_loaded;

    uint8_t save_blob[GAL_SAVE_MAX];
    char notice[96];
    uint32_t notice_until_ms;
    int battery_percent;
    bool image_dirty;
    uint32_t image_retry_ms;
    char error[128];
    bool boot_notice;
} gal_app_t;

bool gal_app_init(gal_app_t *app, const gal_pack_t *pack,
                  uint16_t *art_px, uint16_t *strip_px, uint8_t *scratch, uint32_t scratch_size);
void gal_app_key(gal_app_t *app, const gal_key_t *key);
void gal_app_tick(gal_app_t *app, uint32_t now_ms);
void gal_app_set_battery(gal_app_t *app, int percent);
// 睡前/切后台:强制写自动档、全局解锁与进度(不碰屏幕)。
void gal_app_flush(gal_app_t *app);
const char *gal_app_page_name(app_page_t page);
