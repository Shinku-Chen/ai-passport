// main/gal_store.h —— 设置与存档的 NVS 持久化(带版本与 CRC)。
#pragma once

#include "gal_model.h"

#include <stdbool.h>
#include <stdint.h>

#define GAL_SLOT_AUTO 3
#define GAL_SAVE_PREVIEW 40

typedef struct {
    uint8_t version;
    uint8_t text_speed;   // 0 慢 / 1 标准 / 2 快
    uint8_t font_large;   // 0 = 16px, 1 = 20px
    uint8_t auto_delay;   // 0 短 / 1 中 / 2 长
    uint8_t panel_opa;    // 0..3 -> 55/70/85/95 %
} gal_settings_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t pc;          // 当前停点(台词/选项)对应的事件下标
    uint16_t chapter;
    uint16_t bg;
    uint16_t cg;
    uint16_t spr[3];
    int16_t vars[GAL_MAX_VARS];
    gal_unlock_t unlock;
    char preview[GAL_SAVE_PREVIEW];
    uint32_t stamp;
    uint32_t crc;
} gal_save_t;

// 打开 NVS 命名空间;失败返回 false(此时所有读写都返回 false,但不崩)。
bool gal_store_init(void);

void gal_settings_defaults(gal_settings_t *s);
bool gal_store_load_settings(gal_settings_t *s);
bool gal_store_save_settings(const gal_settings_t *s);

bool gal_store_load_slot(int slot, gal_save_t *save);
bool gal_store_save_slot(int slot, const gal_save_t *save);
bool gal_store_erase_slot(int slot);
bool gal_store_any_slot(void);

// 全局解锁记录(独立于存档槽):鉴赏/章节/结局的"已解锁"跨存档与重启保持。
bool gal_store_load_unlock(gal_unlock_t *unlock);
bool gal_store_save_unlock(const gal_unlock_t *unlock);
