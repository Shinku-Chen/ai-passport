// main/gal/gal_save.h —— 骨架的存档槽位(3 个,NVS 持久化)。
//
// 骨架不存真正的剧本数据:每个槽位记录事件下标与所在章节名,足以驱动
// 存档页、读档页、覆盖确认与空状态弹窗。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define GAL_SAVE_SLOTS 3
#define GAL_SAVE_CHAPTER_LEN 28

typedef struct {
    uint8_t occupied;
    uint16_t event_index;                  // 事件下标
    uint16_t page_index;                   // 事件内的页
    char chapter[GAL_SAVE_CHAPTER_LEN];    // 章节名(槽位里显示)
    uint8_t reserved;
} gal_slot_t;

typedef struct {
    gal_slot_t slots[GAL_SAVE_SLOTS];
} gal_save_t;

// NVS 准备(失败时存档只在本次运行内有效)。
bool gal_save_init(void);

// 读取全部槽位;没有记录时清空并返回 false。
bool gal_save_load(gal_save_t *save);

// 写回全部槽位。
bool gal_save_store(const gal_save_t *save);

// 清空内存中的槽位。
void gal_save_clear(gal_save_t *save);

// 是否至少有一个存档。
bool gal_save_any(const gal_save_t *save);

// 最近一次存档的槽位下标(没有存档时返回 -1)。
int gal_save_latest(const gal_save_t *save);
