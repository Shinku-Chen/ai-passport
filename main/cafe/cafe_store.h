// main/cafe/cafe_store.h —— 设置 / 3 手动槽 / 自动进度 / 永久解锁 的 NVS 持久化。
#pragma once

#include "cafe_vm.h"

#include <stdbool.h>
#include <stdint.h>

#define CAFE_SAVE_SLOTS 3
#define CAFE_CHAPTER_LEN 48
#define CAFE_CG_SEEN_BYTES 16
#define CAFE_TITLE_BG_COUNT 8

typedef struct {
    uint8_t text_speed;    // 0 慢, 1 中, 2 快, 3 瞬间
    bool font_large;
    bool auto_play;
    uint8_t panel_opacity;
    uint8_t title_bg;
    bool sound_on;         // 标题曲/OP/音乐鉴赏总开关
    uint8_t volume;        // 0..100
} cafe_settings_t;

typedef struct {
    bool occupied;
    uint16_t node;
    uint16_t index;
    uint32_t progress;
    uint8_t flags[CAFE_FLAG_MAX];
    uint8_t progression;
    uint8_t last_complete;
    char chapter[CAFE_CHAPTER_LEN];
} cafe_save_t;

typedef struct {
    uint32_t progress;
    uint8_t cg_seen[CAFE_CG_SEEN_BYTES];
    uint8_t last_cleared;
} cafe_unlock_t;

void cafe_store_defaults(cafe_settings_t *settings);

bool cafe_store_init(void);
bool cafe_store_load_settings(cafe_settings_t *out);
bool cafe_store_save_settings(const cafe_settings_t *settings);

bool cafe_store_load_slot(int slot, cafe_save_t *out);
bool cafe_store_save_slot(int slot, const cafe_save_t *slot_data);
bool cafe_store_load_auto(cafe_save_t *out);
bool cafe_store_save_auto(const cafe_save_t *slot_data);
bool cafe_store_clear_auto(void);
bool cafe_store_any_slot(void);

bool cafe_store_load_unlock(cafe_unlock_t *out);
bool cafe_store_save_unlock(const cafe_unlock_t *unlock);
