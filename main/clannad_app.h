// main/clannad_app.h —— 应用状态机：标题 / 正文 / 选项 / 菜单 / 存读档 / 设置 /
// 历史 / 鉴赏 / 章节路线 / 结局。按键语义见 main.c 头注释。
#pragma once

#include "clannad_ui.h"
#include "clannad_vm.h"

#include <stdbool.h>
#include <stdint.h>

#define CLAN_SAVE_SLOTS 30
#define CLAN_HISTORY 12
#define CLAN_SNAPSHOTS 2
#define CLAN_TEXT_CAP 2048
#define CLAN_NOTICE_MS 2600u

typedef struct {
    uint8_t btn;
    uint8_t ev;
} clannad_key_t;

typedef enum {
    CLAN_SLOT_EMPTY = 0,
    CLAN_SLOT_VALID,
    CLAN_SLOT_CORRUPT,
    CLAN_SLOT_IO_ERROR,
} clannad_slot_state_t;

typedef struct {
    uint8_t speed;        // 0 slow, 1 normal, 2 fast, 3 instant
    uint8_t auto_speed;   // 0 slow, 1 normal, 2 fast
    uint8_t auto_enabled; // 仅本次运行；启动默认关闭
} clannad_settings_t;

typedef enum {
    CLAN_PAGE_TITLE = 0,
    CLAN_PAGE_GAME,
    CLAN_PAGE_CHOICE,
    CLAN_PAGE_MENU,
    CLAN_PAGE_SAVE,
    CLAN_PAGE_LOAD,
    CLAN_PAGE_SETTINGS,
    CLAN_PAGE_HISTORY,
    CLAN_PAGE_GALLERY,
    CLAN_PAGE_CG,
    CLAN_PAGE_ROUTES,
    CLAN_PAGE_CHAPTERS,
    CLAN_PAGE_ABOUT,
    CLAN_PAGE_CONFIRM,
    CLAN_PAGE_ENDING,
    CLAN_PAGE_ERROR,
} clannad_page_t;

typedef struct {
    clannad_pack_t pack;
    clannad_vm_t vm;
    clannad_ui_t ui;
    // Reusable list-row scratch owned by the app (1,040 B) instead of a stack
    // array in refresh_page/show_ending. The input task's stack is the scarce
    // resource; this is counted in BSS by tools/clannad_mem_budget.py.
    clannad_ui_row_t rows[CLAN_UI_LIST_ROWS];
    uint16_t canvas[CLAN_IMG_W * CLAN_IMG_H];
    uint8_t raw[28672];

    clannad_page_t page;
    clannad_page_t return_page;      // 子页面（存档/设置/历史…）的返回来源
    clannad_page_t menu_return;      // 阅读菜单返回的正文/待选页
    clannad_settings_t settings;
    bool storage_ok;

    int title_sel;
    int menu_sel;
    int choice_sel;
    int choice_top;
    int list_sel;
    int list_top;
    int gallery_group;
    int gallery_variant;
    int route_sel;
    int chapter_sel;
    int slot_sel;
    int confirm_sel;
    int slot_action;      // 0 load, 1 save
    int pending_slot;

    clannad_frame_t frame;
    char text[CLAN_TEXT_CAP];
    uint16_t text_len;
    uint16_t page_offsets[CLAN_TEXT_MAX_PAGES + 1];
    int page_count;
    int page_index;
    char line[CLAN_TEXT_LINES][160];
    char speaker[64];
    char page_mark[16];

    uint16_t shown_bytes;
    uint32_t type_ms;
    bool typing;
    bool auto_mode;
    bool auto_armed;
    uint32_t auto_ms;
    bool fast_forward;
    uint8_t long_btn;

    uint8_t choice_count;
    char choice_text[CLAN_VM_CHOICES][CLAN_VM_CHOICE_TEXT];
    int32_t choice_value[CLAN_VM_CHOICES];

    struct {
        char speaker[24];
        char text[200];
    } history[CLAN_HISTORY];
    int history_count;
    int history_head;

    uint8_t snapshot[CLAN_SNAPSHOTS][CLAN_VM_SAVE_RAW_MAX];
    uint16_t snapshot_len[CLAN_SNAPSHOTS];
    uint16_t snapshot_page[CLAN_SNAPSHOTS];
    uint16_t snapshot_choice[CLAN_SNAPSHOTS];
    int snapshot_count;
    int snapshot_head;

    clannad_save_info_t slot_info[CLAN_SAVE_SLOTS];
    clannad_slot_state_t slot_state[CLAN_SAVE_SLOTS];
    bool slot_ok[CLAN_SAVE_SLOTS];
    bool auto_dirty;
    uint32_t auto_retry_ms;

    uint8_t progress[CLAN_VM_PROGRESS_MAX];
    uint16_t progress_len;
    bool progress_valid;
    bool progress_dirty;
    uint32_t progress_ms;
    int32_t light_count;
    bool after_unlocked;
    bool true_ending;

    bool game_active;
    uint32_t ff_ms;
    char notice[64];
    uint32_t notice_ms;
    uint32_t idle_ms;
    uint32_t tick_ms;
    uint32_t auto_save_acc;
    uint16_t last_scene;
    int battery;

    // 当前画面缓存（避免每页重复解码）
    uint16_t cache_bg;
    uint16_t cache_bodies[3];
    uint8_t cache_count;
    bool cache_valid;
    bool image_failed;
    bool title_art_for_ending;
} clannad_app_t;

bool clannad_app_init(clannad_app_t *app, const uint8_t *pack_data, uint32_t pack_size,
                      char *why, size_t cap);
void clannad_app_key(clannad_app_t *app, const clannad_key_t *key);
void clannad_app_tick(clannad_app_t *app, uint32_t elapsed_ms);
uint32_t clannad_app_idle_ms(const clannad_app_t *app);
void clannad_app_flush(clannad_app_t *app);
void clannad_app_show_sleeping(clannad_app_t *app);

// 宿主回归辅助：复制当前可见的 UI 行（不暴露内部结构细节）。
int clannad_app_ui_rows(const clannad_app_t *app, char out[][96], int max);
