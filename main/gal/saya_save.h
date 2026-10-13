// main/saya_save.h —— 存档与设置的 NVS 持久化。
//
// 存档内容很小(章节/场景/对白/立绘 + 选择历史),所以不占用文件系统,直接放 NVS。
// 5 个手动存档位 + 1 个自动续读位(每次换场景时写入,深睡唤醒后能接上)。
#pragma once

#include "saya_model.h"

#include <stdbool.h>
#include <stdint.h>

#define SAYA_SAVE_SLOTS 5

// 对话框透明度（百分比，越大越透明）：与框架规范一致，默认 30%（= 70% 不透明），
// 可调 10%–55%、步进 5%。
#define SAYA_TRANSPARENCY_MIN     10
#define SAYA_TRANSPARENCY_MAX     55
#define SAYA_TRANSPARENCY_STEP    5
#define SAYA_TRANSPARENCY_DEFAULT 30

typedef struct {
    uint8_t text_speed;         // 0=慢 1=中 2=快 3=瞬间
    uint8_t font_large;         // 0=16px 1=20px
    uint8_t auto_play;          // 自动阅读开关
    uint8_t seen_warning;       // 已确认过内容警告（保留字段：不再使用，不改变 NVS 布局）
    uint8_t panel_transparency; // 对话框透明度百分比
} saya_settings_t;

void saya_settings_default(saya_settings_t *settings);

// 透明度百分比 -> LVGL 不透明度（0..255）。
static inline uint8_t saya_panel_opa(uint8_t transparency)
{
    if (transparency < SAYA_TRANSPARENCY_MIN) transparency = SAYA_TRANSPARENCY_MIN;
    if (transparency > SAYA_TRANSPARENCY_MAX) transparency = SAYA_TRANSPARENCY_MAX;
    return (uint8_t)(255u - (transparency * 255u + 50u) / 100u);
}

// 循环调节：每次 +5%，到 55% 回到 10%。
static inline uint8_t saya_transparency_next(uint8_t current)
{
    if (current >= SAYA_TRANSPARENCY_MAX) return SAYA_TRANSPARENCY_MIN;
    const uint16_t next = (uint16_t)(current + SAYA_TRANSPARENCY_STEP);
    return (uint8_t)(next > SAYA_TRANSPARENCY_MAX ? SAYA_TRANSPARENCY_MAX : next);
}

// 重置展示类设置（保留阅读位置、自动存档与操作历史）。
static inline void saya_settings_reset_display(saya_settings_t *settings)
{
    if (!settings) return;
    settings->text_speed = 1;
    settings->font_large = 0;
    settings->auto_play = 0;
    settings->panel_transparency = SAYA_TRANSPARENCY_DEFAULT;
}

// 打开 NVS 命名空间。nvs_flash_init() 由 main 负责。
bool saya_save_init(void);

bool saya_settings_load(saya_settings_t *out);
bool saya_settings_store(const saya_settings_t *settings);

bool saya_slot_load(uint8_t slot, saya_save_t *out);
bool saya_slot_store(uint8_t slot, const saya_save_t *save);
bool saya_slot_clear(uint8_t slot);

bool saya_auto_load(saya_save_t *out);
bool saya_auto_store(const saya_save_t *save);

// 由玩家状态构造存档。
void saya_save_from_player(const saya_player_t *player, saya_save_t *out);
