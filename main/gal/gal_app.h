// main/gal/gal_app.h —— 框架骨架的应用状态机。
//
// 按键语义(框架规范,三键):
//   启动页     短按确定进入标题页
//   标题菜单   上/下 移动光标,确定 进入,长按确定 返回
//   对话页     短按(确定/上/下)= 下一句(打字中先全显);长按确定=设置面板;
//              长按上=快进,松手停;长按下=自动阅读开关(任意键停)
//   选项/菜单  上/下 移动,确定 执行,长按确定 返回
//   列表页     上/下 移动,确定 执行(存档/读档/鉴赏室/章节/结局),长按确定 返回
//   关于页     上/下 滚动正文,长按确定 返回
#pragma once

#include "gal_save.h"
#include "gal_settings.h"
#include "gal_ui.h"

#include <stdbool.h>
#include <stdint.h>

#define GAL_HISTORY_MAX 16

typedef struct {
    uint8_t btn;   // bsp_btn_t
    uint8_t ev;    // bsp_btn_ev_t
} gal_key_t;

typedef struct {
    gal_ui_t ui;
    gal_settings_t settings;
    gal_save_t save;

    gal_page_t page;
    gal_page_t return_page;         // 子页面返回的目标

    gal_modal_t modal;              // 当前弹窗(叠加在存档 / 读档页)
    gal_page_t modal_page;          // 弹窗所属页面
    int modal_sel;
    int modal_action;               // 确认后执行的动作(见 gal_app.c)

    int start_sel;
    int title_sel;
    int menu_sel;
    int settings_sel;
    int save_sel;
    int load_sel;
    int history_sel;
    int gallery_sel;
    int chapter_sel;
    int ending_sel;
    int choice_sel;

    int event_index;                // 当前事件
    int page_index;                 // 当前事件内的页
    int page_count;                 // 当前事件的页数
    int shown_line;                 // 已显示的行(避免从子页面返回时重新打字)
    int shown_page;

    bool fast_forward;
    uint32_t ff_accum_ms;
    bool auto_mode;
    uint32_t auto_accum_ms;

    const gal_line_t *history[GAL_HISTORY_MAX];   // 只记指针,文本在内容层
    int history_count;

    int battery_percent;
    uint32_t battery_accum_ms;
    uint32_t idle_ms;
    char notice[24];
    uint32_t notice_ms;
} gal_app_t;

// 初始化:读设置与存档、建界面、进入启动页。
bool gal_app_init(gal_app_t *app);

// 处理一次按键(在输入任务里调用,内部自行加 LVGL 锁)。
void gal_app_key(gal_app_t *app, const gal_key_t *key);

// 周期性 tick:快进/自动推进、提示超时、电量刷新。
void gal_app_tick(gal_app_t *app, uint32_t elapsed_ms);

// 空闲时长(毫秒),供 main 决定变暗/深睡。
uint32_t gal_app_idle_ms(const gal_app_t *app);

// 深睡前在屏幕上显示提示(唤醒后设备会重启)。
void gal_app_show_sleeping(gal_app_t *app);
