// main/atri_save.h —— 存档槽与设置的 NVS 持久化。
//
// 五个手动槽(0..4)+ 一个自动槽(ATRI_AUTO_SLOT)。自动槽在每次换场景时写入,
// 标题页的"继续阅读"读它;手动槽在存档页写入,长按确定删除。
#pragma once

#include "atri_model.h"

#include <stdbool.h>
#include <stdint.h>

#define ATRI_SAVE_SLOTS 5
#define ATRI_AUTO_SLOT 5   // 自动槽,只读(玩家不能手动覆盖/删除)

typedef struct {
    uint8_t text_speed;     // 0 慢 / 1 中 / 2 快
    uint8_t seen_warning;   // 版权与同人移植提示是否已确认
    uint8_t seen_happy;     // 已达成圆满结局
    uint8_t seen_bad;       // 已达成悲剧结局
    uint8_t seen_true;      // 已达成真正的结局
} atri_settings_t;

void atri_settings_default(atri_settings_t *settings);

bool atri_save_init(void);
bool atri_settings_load(atri_settings_t *out);
bool atri_settings_store(const atri_settings_t *settings);

bool atri_slot_load(uint8_t slot, atri_save_t *out);
bool atri_slot_store(uint8_t slot, const atri_save_t *save);
bool atri_slot_clear(uint8_t slot);

void atri_save_from_player(const atri_player_t *player, atri_save_t *out);
