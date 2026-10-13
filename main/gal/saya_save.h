// main/saya_save.h —— 存档与设置的 NVS 持久化。
//
// 存档内容很小(章节/场景/对白/立绘 + 选择历史),所以不占用文件系统,直接放 NVS。
// 5 个手动存档位 + 1 个自动续读位(每次换场景时写入,深睡唤醒后能接上)。
#pragma once

#include "saya_model.h"

#include <stdbool.h>
#include <stdint.h>

#define SAYA_SAVE_SLOTS 5

typedef struct {
    uint8_t text_speed;     // 0=慢 1=中 2=快
    uint8_t font_large;     // 0=16px 1=20px
    uint8_t seen_warning;   // 已确认过内容警告
} saya_settings_t;

void saya_settings_default(saya_settings_t *settings);

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
