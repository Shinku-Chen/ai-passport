// main/eden_app.h —— eden* 应用状态机(页面切换、按键语义、存读档、解锁)。
//
// 按键(与 README 的说明一致):
//   正文页    UP 短按 = 下一句(打字中先全显);UP 长按 = 快进(松手停);
//             DOWN 短按 = 历史;OK 短按 = 暂停菜单;OK 长按 = 暂停菜单
//   列表页    UP/DOWN 移动,OK 确认,OK 长按返回
//   设置页    UP/DOWN 移动,OK 切换,OK 长按返回
// 状态机不认识 LVGL 之外的硬件:按键从 main 的输入任务串行送进来,
// 界面读写都在持有 bsp_lvgl_lock() 时进行。
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t btn;   // bsp_btn_t
    uint8_t ev;    // bsp_btn_ev_t
} eden_key_t;

// 建界面并进入启动页(内部会拿 LVGL 锁)。失败返回 false。
bool eden_app_init(void);

// 用调用方提供的资源包初始化(宿主机测试用;固件里 eden_app_init 就是传 EMBED 包)。
bool eden_app_init_with_pack(const uint8_t *blob, uint32_t size);

// 处理一个按键事件(由输入任务调用)。
void eden_app_key(const eden_key_t *key);

// 周期回调:自动阅读与快进的时间推进(由输入任务每 50ms 调一次)。
void eden_app_tick(uint32_t now_ms);

// 空闲毫秒数(供 main 的调暗/熄屏/deep sleep 策略使用)。
uint32_t eden_app_idle_ms(void);

// 把进度落盘(deep sleep 前调用)。
void eden_app_flush(void);

#ifdef EDEN_APP_HOST_TEST
// 仅供 host 回归：最近一次画进标题画布的帧号（0xFFFF = 还没画过）。
extern uint16_t eden_app_test_title_frame;
// 仅供 host 回归：阅读页几何硬指标。
bool eden_app_test_reader_geometry(void);
// 仅供 host 回归：设置行单行且够宽；姓名不被切、不压面板。
bool eden_app_test_settings_rows(void);
bool eden_app_test_name_label(void);
// 仅供 host 回归：当前是否仍在逐字显示（快进时必须为 false）。
bool eden_app_test_typing(void);
#endif
