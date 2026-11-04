// main/steins0_app.h —— 应用状态机:标题/正文/选项/菜单/存读档/章节/鉴赏/历史/结局。
#pragma once

#include "steins0_engine.h"
#include "steins0_pack.h"
#include "steins0_text.h"
#include "steins0_ui.h"

#include <stdbool.h>
#include <stdint.h>

#define SG0_SLOT_COUNT 30
#define SG0_HISTORY_COUNT 8
#define SG0_HISTORY_TEXT 240
#define SG0_CHUNK_BUFFER 4608
#define SG0_NOTICE_BYTES 64
#define SG0_MAX_STARTS 104
#define SG0_PHONE_THREAD_MAX 320
#define SG0_PHONE_KEY_MAX 64
#define SG0_STAMP_PIXELS 96 * 96

typedef struct {
    uint8_t btn;
    uint8_t ev;
} steins0_key_t;

typedef struct {
    uint8_t speed;      // 0 慢, 1 普通, 2 快, 3 即时
    uint8_t auto_wait;  // 0 慢, 1 普通, 2 快
} steins0_settings_t;

typedef enum {
    SG0_SLOT_EMPTY = 0,
    SG0_SLOT_VALID,
    SG0_SLOT_IO,
    SG0_SLOT_CORRUPT,
} sg0_slot_state_t;

typedef enum {
    SG0_PHONE_HOME = 0,
    SG0_PHONE_INBOX,
    SG0_PHONE_SENT,
    SG0_PHONE_CONTACTS,
    SG0_PHONE_CALLS,
    SG0_PHONE_REPLY,
    SG0_PHONE_THREAD,
} sg0_phone_view_t;

typedef enum {
    SG0_LIST_MENU = 0,
    SG0_LIST_SLOTS_SAVE,
    SG0_LIST_SLOTS_LOAD,
    SG0_LIST_SETTINGS,
    SG0_LIST_ROUTES,
    SG0_LIST_STARTS,
    SG0_LIST_GALLERY,
    SG0_LIST_CLEAR,
} sg0_list_kind_t;

typedef enum {
    SG0_CONFIRM_NONE = 0,
    SG0_CONFIRM_OVERWRITE,
    SG0_CONFIRM_TITLE,
} sg0_confirm_t;

typedef struct {
    char speaker[24];
    char text[SG0_HISTORY_TEXT];
} sg0_history_t;

// 列表返回栈:列表套列表时记住上一层(深度 <= 4),不靠单个 list_return 猜层级。
#define SG0_LIST_STACK 4

typedef struct {
    uint16_t base;   // RINE message base / catalog id width
    uint32_t date;   // actual DATE value (e.g. 101123), not a uint16 index
    uint8_t sender;
    uint8_t sent;
} sg0_phone_key_t;

typedef struct {
    sg0_pack_t pack;
    bool pack_ok;
    steins0_ui_t ui;
    uint16_t pixels[SG0_SCREEN_W * SG0_SCREEN_H];
    uint16_t stamp_pixels[SG0_STAMP_PIXELS];
    const lv_font_t *font;

    steins0_settings_t settings;
    steins0_page_t page;
    steins0_page_t page_return;
    sg0_list_kind_t list_kind;
    steins0_page_t list_return;
    // Nested-list return chain: {parent kind, parent return page} per level.
    sg0_list_kind_t list_kind_stack[SG0_LIST_STACK];
    steins0_page_t list_ret_stack[SG0_LIST_STACK];
    uint8_t list_depth;
    int list_sel;
    int list_win;
    int route_group;
    int cg_group;
    int cg_variant;
    int title_sel;
    int menu_sel;
    int settings_sel;
    int slot_sel;
    int confirm_sel;
    uint8_t card_pending;
    uint32_t card_ms;
    uint8_t last_card;
    sg0_confirm_t confirm_action;
    int confirm_slot;
    steins0_page_t confirm_return;

    sg0_state_t state;
    bool game_started;
    uint8_t chunk_buf[SG0_CHUNK_BUFFER];
    uint32_t chunk_raw_len;
    sg0_chunk_t chunk;
    sg0_frame_t frame;
    const char *raw_text;
    uint16_t raw_text_len;
    uint16_t view;
    bool scene_ok;
    uint16_t last_view;
    bool force_scene;
    char op_text[SG0_TEXT_BUFFER];
    sg0_pages_t pages;
    uint8_t page_index;
    char line_buf[3][192];
    uint8_t line_count;
    int choice_sel;
    uint8_t last_history_node;
    uint16_t last_history_cursor;
    bool history_this_op;

    bool typing;
    uint32_t type_accum;
    uint32_t type_pos;
    bool fast_forward;
    bool auto_mode;
    uint32_t ff_accum;
    uint32_t auto_accum;
    bool swallow_click;

    int battery;
    uint32_t battery_accum;
    uint32_t idle_ms;
    uint32_t notice_ms;
    uint32_t autosave_accum;
    bool nvs_ok;
    bool unlock_dirty;
    uint32_t unlock_retry_accum;
    uint8_t unlock_retry;
    bool title_art_ok;

    uint16_t start_order[SG0_MAX_STARTS];   // 按源 script 排序的 START 下标
    uint8_t start_order_count;
    sg0_unlock_t unlock;
    sg0_save_meta_t autosave;
    bool auto_valid;
    uint8_t auto_state;
    sg0_save_meta_t slots[SG0_SLOT_COUNT];
    bool slot_valid[SG0_SLOT_COUNT];
    uint8_t slot_state[SG0_SLOT_COUNT];

    sg0_history_t history[SG0_HISTORY_COUNT];
    int history_count;
    int history_next;

    char row_text[SG0_LIST_ROWS][96];
    char row_value[SG0_LIST_ROWS][40];
    const char *row_ptrs[SG0_LIST_ROWS];
    const char *value_ptrs[SG0_LIST_ROWS];
    char phone_text[SG0_LIST_ROWS][96];
    char phone_value[SG0_LIST_ROWS][40];
    const char *phone_ptrs[SG0_LIST_ROWS];
    const char *phone_value_ptrs[SG0_LIST_ROWS];
    char phone_title[64];
    char phone_sub[64];
    char phone_page_label[24];
    char phone_footer[96];
    sg0_phone_view_t phone_view;
    int phone_sel;
    int phone_win;
    int phone_page;
    uint16_t phone_thread_refs[SG0_PHONE_THREAD_MAX];
    uint8_t phone_thread_sub[SG0_PHONE_THREAD_MAX];
    uint16_t phone_thread_count;
    uint16_t phone_thread_part;
    uint16_t phone_thread_base;
    uint32_t phone_thread_date;   // actual six-digit source date value
    uint8_t phone_thread_sender;
    uint8_t phone_thread_sent;
    uint16_t phone_reply;
    sg0_phone_key_t phone_thread_keys[SG0_PHONE_KEY_MAX];
    char choice_text[2][96];
    const char *choice_ptrs[2];
    char title_text[6][24];
    const char *title_ptrs[6];
    char chapter_line[96];
    char cg_caption[48];
    char confirm_prompt[64];
    char confirm_yes[8];
    char confirm_no[8];
    char notice[SG0_NOTICE_BYTES];
    char history_buf[2048];
} steins0_app_t;

bool steins0_app_init(steins0_app_t *app, const uint8_t *pack_data, uint32_t pack_size);
void steins0_app_key(steins0_app_t *app, const steins0_key_t *key);
void steins0_app_tick(steins0_app_t *app, uint32_t elapsed_ms);
uint32_t steins0_app_idle_ms(const steins0_app_t *app);
void steins0_app_flush(steins0_app_t *app);
void steins0_app_show_sleeping(steins0_app_t *app);

// 宿主测试/诊断:当前可见的正文页行(不复制进 UI 的临时缓冲)。
uint8_t steins0_app_line_count(const steins0_app_t *app);
uint8_t steins0_app_page_index(const steins0_app_t *app);
