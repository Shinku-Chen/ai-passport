// main/cafe/cafe_app.h —— 横屏阅读器状态机（三键：OK / UP / DOWN）。
#pragma once

#include <cafe_image.h>
#include "cafe_input.h"
#include "cafe_store.h"
#include <cafe_ui.h>
#include "cafe_vm.h"

#include <stdbool.h>
#include <stdint.h>

#define CAFE_HISTORY_ROWS 16
#define CAFE_CHOICE_MAX 4

typedef struct {
    cafe_ui_t *ui;
    cafe_bands_t *bands;
    cafe_vm_t vm;
    cafe_input_t input;
    cafe_settings_t settings;
    cafe_save_t slots[CAFE_SAVE_SLOTS];
    cafe_unlock_t unlock;

    cafe_page_t page;
    cafe_page_t return_page;   // sub-page return destination
    cafe_page_t menu_source;   // GAME or CHOICE that opened the menu
    uint32_t now_ms;
    uint32_t last_activity_ms;
    uint32_t notice_ms;
    uint32_t auto_accum_ms;
    uint32_t ff_start_ms;
    uint32_t ff_steps;
    uint32_t ff_total_us;
    uint32_t ff_max_us;
    uint32_t ff_scene_start;
    uint32_t ff_verify_start;
    uint32_t save_accum_ms;
    bool auto_mode;
    bool fast_forward;
    bool progress_dirty;
    bool unlock_dirty;
    bool settings_dirty;
    bool art_dirty;
    bool resource_fault;       // 资源读取/CRC/解码失败，阻塞推进与存档
    bool title_art_failed;     // 返回标题时不再重试已失败的标题立绘
    uint32_t progress_checkpoint;  // 最近一次成功显示时的永久进度，故障时回滚
    uint8_t fault_kind;        // 0 none, 1 reading, 2 title, 3 CG
    char fault_message[96];

    int title_sel;
    int menu_sel;
    int save_sel;
    int load_sel;
    int settings_sel;
    int history_sel;
    int gallery_sel;
    int cg_sel;
    int cg_variant;
    int recall_sel;
    int music_sel;
    int chapter_sel;
    int chapter_page;
    int route_sel;
    int choice_sel;
    int modal_sel;
    int modal_action;
    bool modal_open;
    bool chapter_routes;
    bool preview_pending;
    bool op_pending;
    bool op_gallery;       // OP was opened from Gallery, not story
    bool op_auto_resume;
    uint8_t audio_scene;       // 0 none, 1 title/menu, 2 OP, 3 music preview
    uint16_t op_frame;
    uint32_t op_start_ms;
    uint32_t op_elapsed_ms;
    uint32_t fault_op_elapsed_ms;
    cafe_page_t fault_page;
    uint8_t fault_audio_scene;
    bool fault_op_pending;
    bool fault_op_gallery;
    bool fault_op_auto_resume;
    uint16_t fault_op_frame;
    uint8_t fault_swallow_mask;
    uint16_t preview_node;
    uint16_t ending_bit;

    uint16_t current_frame;
    uint16_t frame_page;
    uint16_t frame_pages;
    char current_speaker[64];
    char current_text[1024];
    char current_chapter[96];
    char history[CAFE_HISTORY_ROWS][256];
    int history_count;
    int history_head;

    char choice_labels[CAFE_CHOICE_MAX][96];
    int choice_count;
    uint32_t last_art_hash;
    bool last_art_valid;   // hash 0 is a valid scene only when this is true
    int battery;
    int title_bg_resolved;
} cafe_app_t;

bool cafe_app_init(cafe_app_t *app, cafe_ui_t *ui, cafe_bands_t *bands);
void cafe_app_key(cafe_app_t *app, uint8_t key, bool pressed, uint32_t now_ms);
// event_ms is sampled at the button callback; process_ms is when the input task
// actually applies it. Keeping them separate prevents a queued short release
// from being reclassified as a long press after a render/audio/lock delay.
void cafe_app_key_at(cafe_app_t *app, uint8_t key, bool pressed,
                     uint32_t event_ms, uint32_t process_ms);
void cafe_app_tick(cafe_app_t *app, uint32_t now_ms, uint32_t elapsed_ms);
// Cancel an incomplete key cycle after an input-queue overflow. Does not move
// the VM, page, cursor, save state or business progress; only stops FF/auto.
void cafe_app_cancel_input(cafe_app_t *app);
uint32_t cafe_app_idle_ms(const cafe_app_t *app);
void cafe_app_flush(cafe_app_t *app);
void cafe_app_set_battery(cafe_app_t *app, int percent);
