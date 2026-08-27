// main/autopower.h —— 全局「无操作自动深睡断电」。
//
// 行为:整机任意外设/页面,只要 2 分钟无按键活动,即关闭背光并进入深睡,
//       由 GPIO0(按键 ADC 节点,外部 10k 上拉,常态高)低电平触发唤醒。
//
// 设计约束(见 AGENTS.md 与硬件指南):
//   - C3 无 EXT0/EXT1;深睡 GPIO 唤醒仅限 RTC GPIO(GPIO0~5)。GPIO0 是板级按键
//     ADC 节点,按下拉低,故用 ESP_GPIO_WAKEUP_GPIO_LOW 低电平唤醒。
//   - 板子已声明「外部上拉 10k」,常态高、按下低,低电平唤醒可靠;入睡前把
//     GPIO0 重配为数字输入,靠外部上拉维持高,避免误唤醒。
//   - 只使用定时器唤醒会给「周期重启」而不是「关机」,故此处仅用 GPIO 唤醒。
#pragma once

#include <stdbool.h>

// 初始化全局自动关机监控(创建周期检查定时器)。返回 false 表示初始化失败(如定时器创建失败)。
bool autopower_init(void);

// 在任何按键活动时调用,刷新「最后活动时间」,重置关机倒计时。
// 必须可重复、轻量、非阻塞。可在 button 任务回调里调用。
void autopower_notify_activity(void);

// 立即进入深睡(关闭背光 → 使能 GPIO0 低电平唤醒 → esp_deep_sleep_start)。
// 本函数不返回(esp_deep_sleep_start 标记 noreturn);仅定时检查超时后调用。
void autopower_sleep_now(void);
