// main/oc_app.h —— 对讲机应用入口:把链路、音频、按键、界面串成一个应用。
//
// 数据流:
//   长按 OK → 采集并编码(oc_audio) → AUDIO_OPUS 帧 → oc_link → 手机
//   手机 → TEXT/CONTROL 帧 → oc_link → oc_proto 重组 → 上屏 / 应用设置
//   松开 OK → EVENT turn_end(带帧数与丢帧) → 手机收尾识别
//
// 线程:按键回调与 BLE 事件都只入队,全部逻辑在 oc_app 自己的任务里串行执行。
#pragma once

#include "esp_err.h"

// 固件版本(设备信息页显示;发布时与 release tag 对齐)。
#define OC_APP_VERSION "0.1.0"

// 启动应用:建界面、起链路、起音频、注册按键。成功返回 ESP_OK。
// 需要调用方已完成 bsp_display_init() + bsp_lvgl_init()。
esp_err_t oc_app_start(void);

// 按键入口(由 main.c 的按键回调调用,运行在按键定时器任务里,只入队)。
void oc_app_key(int btn, int ev);
