// main/gal/gal_save.c —— 存档槽位的 NVS 持久化实现。
//
// 落盘格式显式按字段排列（不直接写结构体），避免不同架构的填充差异：
//   v1: 4 字节头 + 3 × {occupied:u8, event:u16, page:u16, chapter[28], reserved:u8}
//   v2: 在每个槽位末尾增加 flags:u32（旧骨架的周目标记）
//   v3: 增加 run_mask:u8（本局实际走过的章节，结局资格）
//   v4: 增加 preview:u8（1 = 本局从章节/回想试读开始，永不授正式结局）
#include "gal_save.h"

#include "esp_log.h"
#include "nvs.h"

#include <string.h>

static const char *TAG = "gal_save";

#define GAL_SAVE_NAMESPACE "galsave"
#define GAL_SAVE_KEY "slots"
#define GAL_SAVE_MAGIC 0x53u
#define GAL_SAVE_VERSION 4u

typedef struct {
    uint8_t magic;
    uint8_t version;
    uint8_t reserved[2];
    uint8_t occupied[GAL_SAVE_SLOTS];
    uint8_t run_mask[GAL_SAVE_SLOTS];
    uint8_t preview[GAL_SAVE_SLOTS];
    uint16_t event_index[GAL_SAVE_SLOTS];
    uint16_t page_index[GAL_SAVE_SLOTS];
    uint32_t flags[GAL_SAVE_SLOTS];
    char chapter[GAL_SAVE_SLOTS][GAL_SAVE_CHAPTER_LEN];
} gal_save_blob_t;

typedef struct {
    uint8_t occupied;
    uint16_t event_index;
    uint16_t page_index;
    char chapter[GAL_SAVE_CHAPTER_LEN];
    uint8_t reserved;
} gal_slot_v1_t;

typedef struct {
    uint8_t magic;
    uint8_t version;
    uint8_t reserved[2];
    gal_slot_v1_t slots[GAL_SAVE_SLOTS];
} gal_save_blob_v1_t;

typedef struct {
    uint8_t occupied;
    uint16_t event_index;
    uint16_t page_index;
    char chapter[GAL_SAVE_CHAPTER_LEN];
    uint8_t reserved;
    uint32_t flags;
} gal_slot_v2_t;

typedef struct {
    uint8_t magic;
    uint8_t version;
    uint8_t reserved[2];
    gal_slot_v2_t slots[GAL_SAVE_SLOTS];
} gal_save_blob_v2_t;

typedef struct {
    uint8_t magic;
    uint8_t version;
    uint8_t reserved[2];
    uint8_t occupied[GAL_SAVE_SLOTS];
    uint8_t run_mask[GAL_SAVE_SLOTS];
    uint16_t event_index[GAL_SAVE_SLOTS];
    uint16_t page_index[GAL_SAVE_SLOTS];
    uint32_t flags[GAL_SAVE_SLOTS];
    char chapter[GAL_SAVE_SLOTS][GAL_SAVE_CHAPTER_LEN];
} gal_save_blob_v3_t;

static nvs_handle_t s_handle;
static bool s_ready;

bool gal_save_init(void)
{
    if (s_ready) return true;
    const esp_err_t err = nvs_open(GAL_SAVE_NAMESPACE, NVS_READWRITE, &s_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 打开失败: %s", esp_err_to_name(err));
        return false;
    }
    s_ready = true;
    return true;
}

void gal_save_clear(gal_save_t *save)
{
    if (!save) return;
    memset(save, 0, sizeof(*save));
}

bool gal_save_load(gal_save_t *save)
{
    if (!save) return false;
    gal_save_clear(save);
    if (!s_ready) return false;

    // 兼容 v1 / v2 / v3 三种长度里最大的一个。
    uint8_t raw[sizeof(gal_save_blob_v2_t)] = { 0 };
    size_t len = sizeof(raw);
    const esp_err_t err = nvs_get_blob(s_handle, GAL_SAVE_KEY, raw, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) return false;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "读存档失败: %s", esp_err_to_name(err));
        return false;
    }
    if (len == sizeof(gal_save_blob_v1_t) && raw[0] == GAL_SAVE_MAGIC && raw[1] == 1u) {
        gal_save_blob_v1_t old;
        memcpy(&old, raw, sizeof(old));
        for (int i = 0; i < GAL_SAVE_SLOTS; ++i) {
            save->slots[i].occupied = old.slots[i].occupied;
            save->slots[i].event_index = old.slots[i].event_index;
            save->slots[i].page_index = old.slots[i].page_index;
            memcpy(save->slots[i].chapter, old.slots[i].chapter,
                   sizeof(save->slots[i].chapter));
            save->slots[i].flags = 0;
            save->slots[i].run_mask = 0;
            save->slots[i].preview = 1;   // 旧记录保守处理：不视为正式周目
        }
        return gal_save_any(save);
    }
    if (len == sizeof(gal_save_blob_v2_t) && raw[0] == GAL_SAVE_MAGIC && raw[1] == 2u) {
        gal_save_blob_v2_t old;
        memcpy(&old, raw, sizeof(old));
        for (int i = 0; i < GAL_SAVE_SLOTS; ++i) {
            save->slots[i].occupied = old.slots[i].occupied;
            save->slots[i].event_index = old.slots[i].event_index;
            save->slots[i].page_index = old.slots[i].page_index;
            memcpy(save->slots[i].chapter, old.slots[i].chapter,
                   sizeof(save->slots[i].chapter));
            save->slots[i].flags = old.slots[i].flags;
            save->slots[i].run_mask = 0;
            save->slots[i].preview = 1;
        }
        return gal_save_any(save);
    }
    if (len == sizeof(gal_save_blob_v3_t) && raw[0] == GAL_SAVE_MAGIC && raw[1] == 3u) {
        gal_save_blob_v3_t old;
        memcpy(&old, raw, sizeof(old));
        for (int i = 0; i < GAL_SAVE_SLOTS; ++i) {
            save->slots[i].occupied = old.occupied[i];
            save->slots[i].run_mask = old.run_mask[i];
            save->slots[i].event_index = old.event_index[i];
            save->slots[i].page_index = old.page_index[i];
            save->slots[i].flags = old.flags[i];
            memcpy(save->slots[i].chapter, old.chapter[i], sizeof(save->slots[i].chapter));
            save->slots[i].preview = 1;
        }
        return gal_save_any(save);
    }
    if (len != sizeof(gal_save_blob_t) || raw[0] != GAL_SAVE_MAGIC ||
        raw[1] != GAL_SAVE_VERSION) {
        ESP_LOGW(TAG, "存档格式不认识(长度 %u),忽略", (unsigned)len);
        return false;
    }
    gal_save_blob_t blob;
    memcpy(&blob, raw, sizeof(blob));
    for (int i = 0; i < GAL_SAVE_SLOTS; ++i) {
        save->slots[i].occupied = blob.occupied[i];
        save->slots[i].event_index = blob.event_index[i];
        save->slots[i].page_index = blob.page_index[i];
        save->slots[i].flags = blob.flags[i];
        save->slots[i].run_mask = blob.run_mask[i];
        save->slots[i].preview = blob.preview[i];
        memcpy(save->slots[i].chapter, blob.chapter[i], sizeof(save->slots[i].chapter));
    }
    return gal_save_any(save);
}

bool gal_save_store(const gal_save_t *save)
{
    if (!save || !s_ready) return false;
    gal_save_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.magic = GAL_SAVE_MAGIC;
    blob.version = GAL_SAVE_VERSION;
    for (int i = 0; i < GAL_SAVE_SLOTS; ++i) {
        blob.occupied[i] = save->slots[i].occupied;
        blob.run_mask[i] = save->slots[i].run_mask;
        blob.preview[i] = save->slots[i].preview;
        blob.event_index[i] = save->slots[i].event_index;
        blob.page_index[i] = save->slots[i].page_index;
        blob.flags[i] = save->slots[i].flags;
        memcpy(blob.chapter[i], save->slots[i].chapter, GAL_SAVE_CHAPTER_LEN);
    }
    const esp_err_t err = nvs_set_blob(s_handle, GAL_SAVE_KEY, &blob, sizeof(blob));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写存档失败: %s", esp_err_to_name(err));
        return false;
    }
    return nvs_commit(s_handle) == ESP_OK;
}

bool gal_save_any(const gal_save_t *save)
{
    if (!save) return false;
    for (int i = 0; i < GAL_SAVE_SLOTS; ++i) {
        if (save->slots[i].occupied) return true;
    }
    return false;
}

int gal_save_latest(const gal_save_t *save)
{
    if (!save) return -1;
    // 槽位下标即写入顺序:取最后一个已占用的槽位。
    for (int i = GAL_SAVE_SLOTS - 1; i >= 0; --i) {
        if (save->slots[i].occupied) return i;
    }
    return -1;
}
