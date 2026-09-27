// main/sanoba_save.h —— 存档与设置的 NVS 持久化。
//
// 存档内容很小(章节/场景/对白/背景/立绘 + 选择历史),所以不占用文件系统,直接放
// NVS。5 个手动存档位 + 1 个自动续读位(每次换场景时写入,深睡唤醒后能接上)。
#pragma once

#include "sanoba_model.h"

#include <stdbool.h>
#include <stdint.h>

#define SANOBA_SAVE_SLOTS 5

typedef struct {
    uint8_t text_speed;   // 0=慢 1=中 2=快
    uint8_t font_large;   // 0=16px 1=20px
    uint8_t seen_tips;    // 已看过首次启动的操作提示
} sanoba_settings_t;

void sanoba_settings_default(sanoba_settings_t *settings);

// 打开 NVS 命名空间。nvs_flash_init() 由 main 负责。
bool sanoba_save_init(void);

bool sanoba_settings_load(sanoba_settings_t *out);
bool sanoba_settings_store(const sanoba_settings_t *settings);

bool sanoba_slot_load(uint8_t slot, sanoba_save_t *out);
bool sanoba_slot_store(uint8_t slot, const sanoba_save_t *save);
bool sanoba_slot_clear(uint8_t slot);

bool sanoba_auto_load(sanoba_save_t *out);
bool sanoba_auto_store(const sanoba_save_t *save);
// 删除自动续读位(通关后标题页的"继续阅读"不该再指向已走完的进度)。
bool sanoba_auto_clear(void);
