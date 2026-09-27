// main/limelight_save.h —— 存档与设置的 NVS 持久化。
//
// 存档内容很小(id + 页码 + 选择历史),直接放 NVS,不占文件系统:
// 5 个手动存档位 + 1 个自动续读位(每次换图时写入,深睡唤醒后能接上)。
// 设置也只有几项:文字速度、自动阅读间隔、是否画立绘、首次提示是否看过。
#pragma once

#include "limelight_model.h"

#include <stdbool.h>
#include <stdint.h>

#define LIME_SAVE_SLOTS 5

typedef struct {
    uint8_t text_speed;     // 0 = 慢 1 = 中 2 = 快
    uint8_t auto_delay;     // 0 = 短 1 = 中 2 = 长
    uint8_t show_sprite;    // 0 = 不画立绘(只看文字)
    uint8_t seen_tips;      // 首次启动的操作提示已看过
} lime_settings_t;

void lime_settings_default(lime_settings_t *settings);

// 打开 NVS 命名空间(nvs_flash_init() 由 main 负责)。成功后可重复调用。
bool lime_save_init(void);

bool lime_settings_load(lime_settings_t *out);
bool lime_settings_store(const lime_settings_t *settings);

bool lime_slot_load(uint8_t slot, lime_save_t *out);
bool lime_slot_store(uint8_t slot, const lime_save_t *save);
bool lime_slot_clear(uint8_t slot);

bool lime_auto_load(lime_save_t *out);
bool lime_auto_store(const lime_save_t *save);
// 删除自动续读位(通关后标题页的"继续阅读"不该再指向已走完的进度)。
bool lime_auto_clear(void);
