// main/dlna_app.h —— DLNA 音频接收器应用入口接口。
// 开机直接进入 DLNA 接收器(无主菜单)。初始化 WiFi、DLNA 协议栈、音频管线、
// 状态机后,把按键回调交给本应用自行导航。
#pragma once

#include "bsp_button.h"

#ifdef __cplusplus
extern "C" {
#endif

// 初始化并启动 DLNA 接收器应用。
// 内部依次初始化 WiFi → DLNA 协议栈 → 音频管线 → 建 UI → 起播放。
void dlna_app_start(void);

// 按键回调(由 main.c 的 bsp_button_init 注册)。运行于 button 组件任务,
// 内部会用 bsp_lvgl_lock() 保护 LVGL,禁止阻塞做重活。
void dlna_app_on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user);

// 停止并释放资源(退出前调用)。
void dlna_app_stop(void);

#ifdef __cplusplus
}
#endif
