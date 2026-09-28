// main/oc_ui.h —— 对讲机自研界面:对讲屏(状态 + 对话)/ 历史滚动 / 设置页 / 配对码 / 背光超时。
//
// 这是派生应用自己的界面(不是 demo 菜单,也不用 ui_pixel 外壳):布局、配色与交互
// 都按对讲机需求重新设计,只复用 BSP 的显示/背光接口与 LVGL 常规控件。
//
// 线程:所有函数都会自己加 bsp_lvgl_lock(),可以在任意任务里调用。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    OC_UI_STATE_IDLE = 0,   // 未连接
    OC_UI_STATE_CONNECTING, // 已连接,等待加密/订阅
    OC_UI_STATE_PAIRING,    // 正在配对(屏幕显示 6 位密码)
    OC_UI_STATE_READY,      // 就绪,可以按住说话
    OC_UI_STATE_RECORDING,  // 录音中
    OC_UI_STATE_SENDING,    // 已发送,等回复
    OC_UI_STATE_RECEIVING,  // 收到内容
} oc_ui_state_t;

// 建屏并显示对讲屏。device_name 显示在状态栏(可为 NULL)。
esp_err_t oc_ui_init(const char *device_name);

// 状态栏状态;detail 可空(用状态默认文案)。
void oc_ui_set_state(oc_ui_state_t state, const char *detail);

// 底部提示行(操作指引/错误提示)。
void oc_ui_set_hint(const char *text);

// 电量百分比;-1 表示读取不可用(此时不画数字,也不画 0%)。
void oc_ui_set_battery(int percent);

// App 同步的墙钟(秒)。设备无网络时钟,只用于状态栏显示。
void oc_ui_set_time(int64_t epoch_seconds);

// 追加一条对话(role: 'U' 用户 / 'A' 助手 / 'R' 系统提示)。
void oc_ui_append(char role, const char *text);

// 清空对话区。
void oc_ui_clear_conversation(void);

// 上下翻历史:direction > 0 往前(更旧),< 0 往后(更新)。
void oc_ui_scroll(int direction);

// 配对码面板:显示/隐藏(配对完成、断开时隐藏)。
void oc_ui_show_pairing(uint32_t passkey);
void oc_ui_hide_pairing(void);

// 有输入或新消息:点亮背光并重置闲置计时(背光超时后第一次按键也走这里)。
void oc_ui_note_activity(void);

// 亮度设置变化后立即套用(不重置闲置计时)。
void oc_ui_apply_brightness(void);

// 设置页(内容由调用方给:对讲机菜单/设备信息/亮度由 oc_app 组装)。
void oc_ui_settings_open(const char *title, const char *body, const char *hint);
void oc_ui_settings_update(const char *body);
void oc_ui_settings_close(void);
bool oc_ui_settings_active(void);
