// main/tsxx_save.h —— 存档槽与设置的 NVS 持久化。
//
// 五个手动槽(0..4)+ 一个自动槽(TSXX_AUTO_SLOT)。自动槽在正文推进时按节流写入
// (见 tsxx_app.c 的 TSXX_AUTOSAVE_MS),标题页的"继续阅读"读它;手动槽只在存档页写,
// 长按确定删除。自动槽不能手动覆盖,也不能删除 —— 它是"上次读到哪"的唯一记录。
//
// 存档内容就是 tsxx_model.h 的 tsxx_save_t(页号 + 屏号),由 tsxx_save_encode/decode
// 编解码,所以格式变化只会影响这两个纯逻辑函数。设置另存一包。
#pragma once

#include "tsxx_model.h"

#include <stdbool.h>
#include <stdint.h>

#define TSXX_SAVE_SLOTS 5
#define TSXX_AUTO_SLOT 5   // 自动槽,只读(玩家不能手动覆盖/删除)

typedef struct {
    uint8_t text_speed;    // 0 慢 / 1 中 / 2 快
    uint8_t auto_play;     // 自动阅读开关(下次进入正文时的初始状态)
    uint8_t seen_warning;  // 版权与同人移植提示是否已确认
} tsxx_settings_t;

void tsxx_settings_default(tsxx_settings_t *settings);

// 打开 NVS 命名空间。失败返回 false:本次运行不保存进度,阅读本身不受影响。
bool tsxx_save_init(void);

bool tsxx_settings_load(tsxx_settings_t *out);
bool tsxx_settings_store(const tsxx_settings_t *settings);

// slot: 0..TSXX_SAVE_SLOTS-1 为手动槽;TSXX_AUTO_SLOT 为自动槽。越界返回 false。
bool tsxx_slot_load(uint8_t slot, tsxx_save_t *out);
bool tsxx_slot_store(uint8_t slot, const tsxx_save_t *save);
bool tsxx_slot_clear(uint8_t slot);

// 把当前阅读位置装进存档结构。player 为空或存档结构为空时不改动。
void tsxx_save_from_player(const tsxx_player_t *player, tsxx_save_t *out);
