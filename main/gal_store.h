// main/gal_store.h —— NVS 持久化(存档槽、自动槽、全局解锁、进度、设置)。
// NVS 失败只降级为“本次不落盘”,绝不 nvs_flash_erase。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GAL_SLOT_COUNT 30
#define GAL_SLOT_AUTO 30
#define GAL_SETTINGS_VERSION 1u

typedef enum {
    GAL_STORE_OK = 0,
    GAL_STORE_NOT_FOUND,
    GAL_STORE_IO_ERROR,
    GAL_STORE_CORRUPT,
} gal_store_status_t;

typedef struct {
    bool ready;          // NVS 命名空间可用
    uint32_t writes;     // 成功写入次数
    uint32_t failures;   // 失败次数(测试/提示用)
} gal_store_t;

typedef struct {
    uint16_t version;
    uint16_t text_speed;   // 0 = 即时, 1 = 逐字
    uint16_t auto_speed;   // 0 慢 / 1 标准 / 2 快
    uint16_t panel_opa;    // 0..3(保留;当前 UI 固定使用 2)
} gal_settings_t;

bool gal_store_init(gal_store_t *store);
void gal_settings_defaults(gal_settings_t *s);
bool gal_store_load_settings(gal_store_t *store, gal_settings_t *s);
bool gal_store_save_settings(gal_store_t *store, const gal_settings_t *s);

// slot: 0..29 手动栏位, GAL_SLOT_AUTO 自动栏位。
// 读取状态区分:空槽/NOT_FOUND、I/O 失败、记录损坏(长度等)、OK。
// 兼容包装 load_slot 只在 OK 时返回 true;UI 应使用 read_slot 保留失败原因。
gal_store_status_t gal_store_read_slot(gal_store_t *store, int slot,
                                       uint8_t *buf, size_t cap, size_t *len);
bool gal_store_load_slot(gal_store_t *store, int slot, uint8_t *buf, size_t cap, size_t *len);
bool gal_store_save_slot(gal_store_t *store, int slot, const uint8_t *buf, size_t len);

bool gal_store_load_unlock(gal_store_t *store, uint8_t *buf, size_t cap, size_t *len);
bool gal_store_save_unlock(gal_store_t *store, const uint8_t *buf, size_t len);
bool gal_store_load_progress(gal_store_t *store, uint16_t *progress);
bool gal_store_save_progress(gal_store_t *store, uint16_t progress);
