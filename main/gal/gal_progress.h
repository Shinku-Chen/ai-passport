// main/gal/gal_progress.h —— 阅读进度(自动保存的一条记录)。
//
// 与手动存档槽位(gal_save)相互独立:
//   进度 = 每次推进剧情自动更新,「继续游戏」直接从这里接上;
//   存档 = 玩家在存档页手动写入的 3 个节点。
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t valid;            // 0 = 还没有任何进度
    uint16_t event_index;
    uint16_t page_index;
} gal_progress_t;

// NVS 准备(失败时进度只在本次运行内有效)。
bool gal_progress_init(void);

// 读取进度;没有记录时清空并返回 false。
bool gal_progress_load(gal_progress_t *out);

// 写回进度。
bool gal_progress_store(const gal_progress_t *in);

// 清空内存中的进度(开始新游戏时用)。
void gal_progress_clear(gal_progress_t *out);
