// main/gal/gal_settings.h —— 框架骨架的展示设置（NVS 持久化）。
//
// 只存四项:文字速度 / 字号 / 自动阅读 / 对话框透明度。全部是展示类设置,
// 与作品内容无关。透明度与框架规范一致:10–55%,默认 30%。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define GAL_TRANSPARENCY_MIN     10
#define GAL_TRANSPARENCY_MAX     55
#define GAL_TRANSPARENCY_STEP    5
#define GAL_TRANSPARENCY_DEFAULT 30

typedef struct {
    uint8_t text_speed;         // 0=慢 1=中 2=快 3=瞬间
    uint8_t font_large;         // 0=16px 1=20px
    uint8_t auto_play;          // 自动阅读开关
    uint8_t panel_transparency; // 对话框透明度百分比
} gal_settings_t;

void gal_settings_default(gal_settings_t *settings);
// 重置展示类设置(速度/字号/自动/透明度),不动其它状态。
void gal_settings_reset(gal_settings_t *settings);

// 透明度百分比 -> LVGL 不透明度(0..255)。
uint8_t gal_panel_opa(uint8_t transparency);
// 循环调节:每次 +5%,到 55% 回到 10%。
uint8_t gal_transparency_next(uint8_t current);

// 打开 NVS 命名空间(nvs_flash_init 由 main 负责)。
bool gal_settings_init(void);
bool gal_settings_load(gal_settings_t *out);
bool gal_settings_store(const gal_settings_t *settings);
