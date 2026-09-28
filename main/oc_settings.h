// main/oc_settings.h —— 对讲机本地设置(音量/麦克风增益/亮度/背光超时)的 NVS 持久化。
//
// 音量与麦克风增益由 App 下发(CONTROL audio),但设备要记住:重连后 App 未必立刻再发一次,
// 而设备重启后也必须回到用户上次听感。亮度保留设备本地调节(设置页),同样持久化。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define OC_SETTINGS_VOLUME_DEFAULT 80U
#define OC_SETTINGS_MIC_GAIN_DEFAULT 30U   // 与 BSP 打开 codec 时的旧硬编码值一致
#define OC_SETTINGS_BRIGHTNESS_DEFAULT 100U
#define OC_SETTINGS_BACKLIGHT_TIMEOUT_DEFAULT 60U   // 秒;闲置多久关背光

// 从 NVS 读取全部设置(缺失或非法时用默认值并回写)。可重复调用。
esp_err_t oc_settings_load(void);

uint8_t oc_settings_volume(void);
uint8_t oc_settings_mic_gain_db(void);
uint8_t oc_settings_brightness(void);
uint16_t oc_settings_backlight_timeout_s(void);

// 以下 setter 立即持久化并返回是否写成功(NVS 写失败不影响本次生效)。
esp_err_t oc_settings_set_volume(uint8_t percent);
esp_err_t oc_settings_set_mic_gain_db(uint8_t db);
esp_err_t oc_settings_set_brightness(uint8_t percent);
esp_err_t oc_settings_set_backlight_timeout_s(uint16_t seconds);
