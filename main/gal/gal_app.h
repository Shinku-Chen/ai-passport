// main/gal/gal_app.h —— 框架骨架的应用状态机。
//
// 按键语义(框架规范,三键):
//   启动页   短按确定进入标题页
//   标题/列表 上/下 移动光标(到顶/到底停住),确定 进入/确认,长按确定 返回
//   正文页   短按(确定/上/下)= 下一句(打字中先全显);长按确定=设置面板;
//            长按上=快进,松手停;长按下=自动阅读开关(任意键停)
//   设置页   上/下 移动,确定 切换/执行,长按确定 返回
//   关于页   上/下 滚动正文,确定 返回
#pragma once

#include "gal_settings.h"
#include "gal_ui.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t btn;   // bsp_btn_t
    uint8_t ev;    // bsp_btn_ev_t
} gal_key_t;

typedef struct {
    gal_ui_t ui;
    gal_settings_t settings;

    gal_page_t page;
    gal_page_t settings_return;
    int start_sel;
    int title_sel;
    int settings_sel;
    bool settings_from_reader;   // 设置面板由正文页(长按确定)打开

    int line_index;   // 当前句
    int page_index;   // 当前页
    int page_count;   // 当前句的总页数

    bool fast_forward;
    uint32_t ff_accum_ms;
    bool auto_mode;
    uint32_t auto_accum_ms;

    int battery_percent;
    uint32_t battery_accum_ms;
    uint32_t idle_ms;
    char notice[24];
    uint32_t notice_ms;
} gal_app_t;

// 初始化:读设置、建界面、进入启动页。
bool gal_app_init(gal_app_t *app);

// 处理一次按键(在输入任务里调用,内部自行加 LVGL 锁)。
void gal_app_key(gal_app_t *app, const gal_key_t *key);

// 周期性 tick:快进/自动推进、提示超时、电量刷新。
void gal_app_tick(gal_app_t *app, uint32_t elapsed_ms);

// 空闲时长(毫秒),供 main 决定变暗/深睡。
uint32_t gal_app_idle_ms(const gal_app_t *app);

// 深睡前在屏幕上显示提示(唤醒后设备会重启)。
void gal_app_show_sleeping(gal_app_t *app);
