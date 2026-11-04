// main/gal_app.h —— 应用状态机:页面切换、按键语义、自动/快进、存档与解锁。
//
// 按键语义(三键):
//   正文页   UP 短按 = 下一句(打字中先整页显示);UP 长按 = 快进(松开停)
//            DOWN 短按 = 历史文本
//            OK 短按/长按 = 游戏内菜单
//   选项页   UP/DOWN 选择,OK 确认
//   列表页   UP/DOWN 选择,OK 执行,长按 OK 返回
//   历史/章节 UP/DOWN 滚动,OK 返回
//   鉴赏     UP/DOWN 选格(跨页自动翻页),OK 全屏浏览,长按 OK 返回
//   CG 浏览  UP/DOWN 上一张/下一张,OK 返回
//   结局页   OK 回到标题
#pragma once

#include "bsp_button.h"
#include "gal_pack.h"
#include "gal_store.h"
#include "gal_ui.h"

#include <stdbool.h>
#include <stdint.h>

#define GAL_HISTORY_MAX 64

typedef struct {
    uint8_t btn;    // bsp_btn_t
    uint8_t ev;     // bsp_btn_ev_t
} gal_key_t;

typedef struct {
    uint16_t *art_px;
    uint16_t *strip_px;
    uint8_t *scratch;
    uint32_t scratch_size;
} gal_app_buffers_t;

typedef struct {
    gal_pack_t pack;
    gal_play_t play;
    gal_unlock_t unlock;
    gal_settings_t settings;
    gal_ui_t ui;

    gal_page_t page;
    gal_page_t return_page;

    int title_sel;
    int menu_sel;
    int slots_sel;
    int settings_sel;
    bool slots_saving;
    bool confirm_active;
    bool choice_pending;
    bool menu_from_choice;
    bool suppress_next_click;
    bool render_invalid;
    bool unlock_dirty;
    uint32_t unlock_try_ms;
    uint32_t unlock_backoff_ms;
    int confirm_sel;
    int confirm_slot;
    int browse_sel;
    int gallery_page;
    int gallery_sel;

    uint16_t save_pc;          // 当前停点(用于存档/读档)
    uint16_t line_id;
    uint16_t line_speaker;
    uint16_t line_page;
    uint16_t line_pages;
    char page_text[GAL_TEXT_BUFFER];

    uint16_t history[GAL_HISTORY_MAX];
    int history_count;

    uint16_t choice_group;
    int choice_sel;
    int choice_count;
    uint8_t choice_enabled[5];
    uint16_t choice_option[5];

    uint16_t rendered_bg, rendered_cg;
    uint16_t rendered_spr[3];

    bool fast_forward;
    bool auto_mode;
    bool ff_held;
    uint32_t ff_accum_ms;
    uint32_t auto_accum_ms;
    uint32_t now_ms;
    uint32_t tick_ms;

    bool transition_active;
    uint32_t transition_until;
    bool ending_active;
    bool started;
    bool have_save;
    int battery;
    uint32_t battery_accum_ms;
    uint32_t idle_ms;
    uint32_t autosave_accum_ms;
} gal_app_t;

bool gal_app_init(gal_app_t *app, const uint8_t *pack_data, uint32_t pack_size,
                  const gal_app_buffers_t *buffers);
void gal_app_key(gal_app_t *app, const gal_key_t *key);
void gal_app_tick(gal_app_t *app, uint32_t elapsed_ms);
uint32_t gal_app_idle_ms(const gal_app_t *app);
void gal_app_flush(gal_app_t *app);
void gal_app_show_sleeping(gal_app_t *app);
