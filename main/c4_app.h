// main/c4_app.h —— 四子棋控制器:状态机 + 事件分发 + 电脑对手 worker。
//
// 线程模型:按键回调和 AI worker 都不碰 LVGL;所有事件都汇进 main.c 的输入队列,
// 由输入任务调用 c4_app_handle_event()。该函数自己持 bsp_lvgl_lock(),因此界面
// 只有一个改写者,不需要额外的互斥。
#pragma once

#include "bsp_button.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    C4_EVENT_KEY = 0,     // 按键事件
    C4_EVENT_AI_MOVE,     // AI worker 算完了(只能由 c4_app 自己产生)
} c4_event_kind_t;

typedef struct {
    c4_event_kind_t kind;
    bsp_btn_t btn;
    bsp_btn_ev_t ev;
    int col;              // C4_EVENT_AI_MOVE:选定的列
    uint8_t generation;   // C4_EVENT_AI_MOVE:发请求时的对局代号,用于丢弃过期结果
} c4_event_t;

// 建 AI worker。input_queue 是输入任务的事件队列:worker 算完后把结果塞回它。
// 失败时游戏仍可进入,但电脑不会走棋(界面会显示 AI OFFLINE)。
esp_err_t c4_app_start_worker(QueueHandle_t input_queue);

// 初始化模型并载入标题屏。须在 c4_ui_build() 之后、持 LVGL 锁时调用一次。
void c4_app_init(void);

// 处理一个事件;由输入任务调用(非 LVGL 任务)。任何事件都会重新计时空闲。
void c4_app_handle_event(const c4_event_t *event);

// 输入任务的循环周期:无论收到事件还是等队列超时,每轮都会调用一次
// c4_app_idle_tick()。联机的重传计时依赖这个稳定的推进节奏,不要随意调大。
#define C4_APP_TICK_MS 100u

// 输入任务每轮调用一次,传入距离上次调用的实际毫秒数;同时驱动联机收包、
// 重传与联机屏刷新。返回 true 表示空闲已超时,调用方应立刻调用 c4_app_enter_sleep()。
// 空闲阈值:设置屏/联机屏 60s,对局中 180s,联机对阵 600s(睡着等于断线)。
bool c4_app_idle_tick(uint32_t elapsed_ms);

// 空闲休眠:停外设 → 熄屏 → deep sleep(任意按键唤醒,唤醒即重启应用)。
// 唤醒源配不上时不会睡下去,直接返回;正常路径不返回。
void c4_app_enter_sleep(void);

// 启动时调用一次。返回 true = 本次启动来自 deep sleep(唤醒),false = 冷启动/复位。
// 两者在串口上都是“重新开始”,靠 RTC 内存里的记号区分,便于确认唤醒真的生效。
bool c4_app_take_sleep_magic(void);
