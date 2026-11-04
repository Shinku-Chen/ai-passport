// main/steins_app.h —— 《命运石之门》阅读器的应用状态机。
//
// 按键语义(三键:UP / DOWN / OK):
//   标题页      上/下 选菜单,短 OK 进入,长 OK 无动作
//   正文页      短 OK / 短 UP 推进(打字中先全显);长 UP 快进,松手停;
//               短 DOWN 历史;长 DOWN 自动阅读开关;长 OK 游戏内菜单
//   待选/列表   上/下 移动,短 OK 执行,长 OK 返回来源
//   确认弹窗    上/下 选确定/取消,短 OK 执行,长 OK 取消
//   鉴赏 CG     上/下 切换差分,长 OK 返回
//
// 关键约束:按键只认 CLICK / LONG / RELEASE 三种事件(PRESS 不触发任何动作),
// 所以长按之后的抬手不会再多走一步,长按也不会吞掉下一次独立短按。
#pragma once

#include "steins_model.h"
#include "steins_ui.h"
#include "steins_mobile_pack.h"

#include <stdbool.h>
#include <stdint.h>

#define STEINS_SLOTS 30
#define STEINS_HISTORY_MAX 32
#define STEINS_NAV_MAX 8
#define STEINS_CHOICE_MAX 8
#define STEINS_LIST_TEXT 128

// 选择点正文(邮件)缓冲:实测最长 509 B,留到 640 B。
#define STEINS_CHOICE_TEXT 640
#define STEINS_MAIL_LINES 6

#define STEINS_BATTERY_NONE (-1)

typedef enum {
    STEINS_SLOT_EMPTY = 0,
    STEINS_SLOT_VALID,
    STEINS_SLOT_CORRUPT,
    STEINS_SLOT_IO_ERROR,
} steins_slot_state_t;

typedef struct {
    uint8_t btn;   // bsp_btn_t
    uint8_t ev;    // bsp_btn_ev_t
} steins_key_t;

typedef struct {
    steins_page_t page;
    int16_t sel;
    uint8_t kind;
} steins_nav_t;

typedef struct {
    steins_pack_t pack;
    steins_ui_t ui;
    uint16_t canvas[STEINS_IMAGE_PIXELS];

    // 读到的位置 + 手机/邮件完整 checkpoint(v2)
    steins_pos_t pos;
    steins_checkpoint_t chk;
    steins_mobile_t mobile;
    steins_mobile_pack_ctx_t mobile_ctx;
    steins_mobile_content_t mobile_content;
    void *mobile_user;
    bool mobile_ready;
    bool storage_ok;
    bool reading;
    bool checkpoint_dirty;
    uint32_t checkpoint_ms;
    bool migrated_v1;

    // 当前帧
    uint32_t frame_index;
    uint16_t view_index;
    uint16_t drawn_view;       // 已成功画出的画面(==0xFFFF 表示画布无效)
    bool scene_ok;
    steins_image_error_t scene_error;
    char text[STEINS_TEXT_BYTES + 64];
    char speaker[96];
    char date[64];
    char title[160];
    steins_text_page_t page_data;
    int page_index;
    int page_count;
    int shown;                 // 打字机已显示的字符数
    uint32_t type_ms;
    uint32_t auto_ms;
    uint32_t ff_ms;
    bool auto_mode;
    bool fast_forward;
    bool pending_advance;      // 自动/快进待推进

    // 选择
    uint16_t choice_record;
    uint8_t choice_kind;
    uint8_t choice_count;
    int choice_sel;
    // 邮件/选择标题:实测源里最长的选择点正文 509 B,这里留到 640 B。
    // 超过一屏时先进邮件阅读页(分页),正文必须能全部读到再回到选项。
    char choice_heading[STEINS_CHOICE_TEXT];
    char choice_tag[64];
    char choice_rows[STEINS_CHOICE_MAX][STEINS_CHOICE_TEXT];
    int mail_page;
    int mail_pages;
    steins_text_page_t mail_data;
    bool mail_pending_choice;

    // 手机/邮件页面状态（checkpoint 是唯一持久源）
    int phone_sel;
    uint16_t phone_mail;
    bool phone_mail_sent;
    bool phone_mail_mode;
    int phone_mail_page;
    int phone_mail_pages;
    // 与选择点邮件正文复用同一个 page 工作区；不同时显示，不另分配 4 KiB。
    uint16_t phone_clip_node;
    uint32_t phone_clip_frame;
    uint32_t phone_clip_count;
    uint16_t phone_reply_ids[16];
    uint8_t phone_reply_count;
    bool phone_presentation_pending;
    bool phone_return_to_game;
    char phone_title[160];
    char phone_body[STEINS_TEXT_BYTES + 64];

    // 页面栈
    steins_page_t page;
    steins_nav_t nav[STEINS_NAV_MAX];
    int nav_depth;
    int sel;

    // 列表内容(共享缓冲,渲染时填)
    char list_title[24];
    char list_hint[STEINS_LIST_TEXT];
    char list_main[STEINS_UI_LIST_ROWS][STEINS_LIST_TEXT];
    char list_sub[STEINS_UI_LIST_ROWS][64];
    const char *list_main_ptr[STEINS_UI_LIST_ROWS];
    const char *list_sub_ptr[STEINS_UI_LIST_ROWS];
    int list_count;
    bool list_story_font;

    // 标题菜单:按"有没有自动存档"动态拼行,行动作用于分发
    const char *title_rows[STEINS_UI_TITLE_ROWS];
    uint8_t title_action[STEINS_UI_TITLE_ROWS];
    int title_count;

    // 章节标题卡(源在章节开头插入一张标题 CG)
    uint16_t chapter_card_asset;
    bool chapter_card_active;
    uint32_t chapter_card_ms;

    // 图鉴
    int gallery_sel;
    int gallery_variant;
    uint16_t cg_asset;
    bool cg_valid;

    // 结局
    char ending_name[64];

    // 存档 / 进度
    steins_slot_state_t slot_state[STEINS_SLOTS];
    steins_unlock_t unlock;
    bool unlock_dirty;
    uint32_t save_ms;
    uint32_t progress_ms;
    bool progress_dirty;
    char notice[40];
    uint32_t notice_ms;

    // 设置
    uint8_t text_speed;   // 0..2
    uint8_t auto_speed;   // 0..2
    uint8_t box_opacity;  // 0..2
    uint8_t font_size;    // 16 / 20(20 未随包提供时固定 16)

    // 历史
    char history[STEINS_HISTORY_MAX][STEINS_LIST_TEXT];
    int history_count;
    int history_sel;

    int battery_percent;
    uint32_t battery_ms;
    uint32_t idle_ms;

    uint8_t pending_action;   // 确认弹窗确认后执行的动作
    int pending_slot;
    int confirm_sel;
    char confirm_text[64];
} steins_app_t;

bool steins_app_init(steins_app_t *app);
void steins_app_key(steins_app_t *app, const steins_key_t *key);
void steins_app_tick(steins_app_t *app, uint32_t elapsed_ms);
uint32_t steins_app_idle_ms(const steins_app_t *app);
void steins_app_flush(steins_app_t *app);
void steins_app_show_sleeping(steins_app_t *app);
