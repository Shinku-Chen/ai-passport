// main/voice_app.h —— 音效钥匙扣应用入口接口。
// 产品化形态: 开机直接进入音效钥匙扣, 无主菜单; 按键回调交给本应用自行导航。
#pragma once

#include "bsp_button.h"

#ifdef __cplusplus
extern "C" {
#endif

// 初始化音效钥匙扣应用: 挂载 littlefs 数据分区、建首个界面、启动播放任务。
// 返回 ESP_OK 成功; 其余为失败(如 littlefs 挂载失败)。
void voice_app_start(void);

// 按键回调(由 main.c 的 bsp_button_init 注册)。运行于 button 组件任务,
// 内部会用 bsp_lvgl_lock() 保护 LVGL 访问, 禁止阻塞做重活。
void voice_app_on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user);

// 停止播放任务并释放资源(退出前调用, 防止悬挂访问 UI 的任务)。
void voice_app_stop(void);

#ifdef __cplusplus
}
#endif
