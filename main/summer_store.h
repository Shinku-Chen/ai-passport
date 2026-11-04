// main/summer_store.h —— NVS 持久化：设置、30 个手动档、独立自动档、周目进度。
//
// 失败语义：NVS 初始化失败只降级（available=false），绝不 erase；写入必须
// nvs_set_blob 与 nvs_commit 都成功才返回 true，失败时调用方的旧 RAM 档位
// 与预览保持不变。
#pragma once

#include "summer_model.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SUMMER_SLOT_COUNT 30
#define SUMMER_SETTINGS_VERSION 1u
#define SUMMER_PROGRESS_KEY "prog"
#define SUMMER_AUTO_KEY "auto"
#define SUMMER_SETTINGS_KEY "cfg"

typedef struct {
    bool available;
    bool settings_loaded;
    uint8_t text_speed;     // 0 = 即时；否则毫秒/字
    uint8_t auto_speed;     // 0 慢 / 1 标准 / 2 快
    uint8_t panel_opacity;  // 0..4（18/30/42/55/70%）
    summer_progress_t progress;
    bool progress_dirty;
    uint8_t progress_retries;
} summer_store_t;

// 打开 NVS 并读取设置/进度。NVS 不可用时返回 false，但 store 仍可安全使用。
bool summer_store_init(summer_store_t *store);
bool summer_store_available(const summer_store_t *store);

// 设置：失败保留 RAM 值并返回 false。
bool summer_store_save_settings(summer_store_t *store);
void summer_store_default_settings(summer_store_t *store);

// 周目进度：失败保持 dirty=true，调用方可稍后重试。
bool summer_store_flush_progress(summer_store_t *store);
void summer_store_mark_progress_dirty(summer_store_t *store);

// 手动档 0..29。写入成功才返回 true。
bool summer_store_write_slot(summer_store_t *store, int index, const uint8_t *data, size_t len);
bool summer_store_read_slot(const summer_store_t *store, int index, uint8_t *buf, size_t cap,
                            size_t *out_len);
bool summer_store_slot_exists(const summer_store_t *store, int index);

// 独立自动档。
bool summer_store_write_auto(summer_store_t *store, const uint8_t *data, size_t len);
bool summer_store_read_auto(const summer_store_t *store, uint8_t *buf, size_t cap, size_t *out_len);

// 测试/诊断：当前 NVS 是否可用、最近一次写入是否成功。
bool summer_store_last_write_ok(const summer_store_t *store);
