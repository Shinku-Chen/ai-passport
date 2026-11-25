#include "nine_store.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "nine_store";
#define NAMESPACE "nine"
#define REC_VERSION 1u
#define REC_HEADER 32u
#define SLOT_META 104u
#define SLOT_MAGIC 0x3156534Eu   /* "NSV1" */
#define AUTO_MAGIC 0x3156504Eu   /* "NPV1" */
#define PERM_MAGIC 0x31504D4Eu   /* "NPM1" */
#define REC_MAX 1232u

static nvs_handle_t s_handle;
static bool s_ready;
// Store calls are serialized by the reader; one bounded scratch avoids 1.2 KB
// of stack in every load/write path.
static uint8_t s_raw[NINE_SLOT_MAX];

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static void wr32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static uint32_t crc32_bytes(const uint8_t *data, size_t size)
{
    uint32_t crc = 0xFFFFFFFFu;
    while (size--) {
        crc ^= *data++;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

bool nine_store_init(void)
{
    if (s_ready) return true;
    const esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &s_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 打开失败: %s", esp_err_to_name(err));
        return false;
    }
    s_ready = true;
    return true;
}

static void copy_preview(char *dst, size_t cap, const char *src)
{
    if (!dst || !cap) return;
    size_t length = src ? strlen(src) : 0;
    if (length >= cap) length = cap - 1;
    memcpy(dst, src ? src : "", length);
    dst[length] = 0;
}

static void key_for(char *key, size_t cap, const char *prefix, int slot)
{
    snprintf(key, cap, "%s%d", prefix, slot);
}

static nine_store_status_t read_record(const char *key, uint8_t *out, size_t cap,
                                       size_t *size)
{
    if (!s_ready) return NINE_STORE_IO;
    size_t length = 0;
    esp_err_t err = nvs_get_blob(s_handle, key, NULL, &length);
    if (err == ESP_ERR_NVS_NOT_FOUND) return NINE_STORE_MISSING;
    if (err != ESP_OK || !length || length > cap || length > NINE_SLOT_MAX)
        return NINE_STORE_IO;
    err = nvs_get_blob(s_handle, key, out, &length);
    if (err != ESP_OK) return NINE_STORE_IO;
    *size = length;
    return NINE_STORE_OK;
}

static nine_store_status_t write_record(const char *key, const uint8_t *data, size_t size)
{
    if (!s_ready || !data || !size || size > NINE_SLOT_MAX) return NINE_STORE_IO;
    if (nvs_set_blob(s_handle, key, data, size) != ESP_OK) return NINE_STORE_IO;
    return nvs_commit(s_handle) == ESP_OK ? NINE_STORE_OK : NINE_STORE_IO;
}

static nine_store_status_t verify_record(const uint8_t *raw, size_t size, uint32_t magic,
                                         uint32_t identity)
{
    if (!raw || size < REC_HEADER + 4 || rd32(raw) != magic ||
        rd32(raw + 4) != REC_VERSION || rd32(raw + 12) != size)
        return NINE_STORE_FORMAT;
    if (rd32(raw + 8) != identity) return NINE_STORE_IDENTITY;
    if (crc32_bytes(raw, size - 4) != rd32(raw + size - 4)) return NINE_STORE_CRC;
    return NINE_STORE_OK;
}

nine_store_status_t nine_store_load_slots(uint32_t identity,
                                          nine_slot_t slots[NINE_SLOT_COUNT])
{
    if (!slots) return NINE_STORE_IO;
    memset(slots, 0, sizeof(nine_slot_t) * NINE_SLOT_COUNT);
    if (!s_ready) return NINE_STORE_IO;
    nine_store_status_t overall = NINE_STORE_MISSING;
    for (int i = 0; i < NINE_SLOT_COUNT; ++i) {
        char key[8];
        key_for(key, sizeof(key), "s", i);
        uint8_t *raw = s_raw;
        size_t size = 0;
        nine_store_status_t status = read_record(key, raw, NINE_SLOT_MAX, &size);
        if (status != NINE_STORE_OK) {
            if (status != NINE_STORE_MISSING) {
                overall = status;
                /* The key exists but its record is unusable. Mark it occupied
                   with zero size so the UI can show "存档损坏" instead of
                   presenting a silently empty slot. */
                slots[i].occupied = true;
                slots[i].vm_size = 0;
                copy_preview(slots[i].preview, sizeof(slots[i].preview),
                             "存档损坏");
            }
            continue;
        }
        if (overall == NINE_STORE_MISSING) overall = NINE_STORE_OK;
        status = verify_record(raw, size, SLOT_MAGIC, identity);
        if (status != NINE_STORE_OK) {
            overall = status;
            slots[i].occupied = true;
            slots[i].vm_size = 0;
            copy_preview(slots[i].preview, sizeof(slots[i].preview), "存档损坏");
            continue;
        }
        const uint32_t vm_size = rd32(raw + 20);
        if (vm_size == 0 || vm_size > NINE_SAVE_CAPACITY ||
            size != REC_HEADER + SLOT_META + vm_size + 4) {
            overall = NINE_STORE_FORMAT;
            continue;
        }
        nine_slot_t *slot = &slots[i];
        slot->occupied = true;
        slot->page = (uint16_t)(raw[16] | raw[17] << 8);
        slot->chapter = (uint16_t)(raw[18] | raw[19] << 8);
        memcpy(slot->preview, raw + REC_HEADER, NINE_SLOT_PREVIEW);
        slot->preview[NINE_SLOT_PREVIEW] = '\0';
        slot->vm_size = vm_size;
        memcpy(slot->vm, raw + REC_HEADER + SLOT_META, vm_size);
    }
    return overall;
}

nine_store_status_t nine_store_write_slot(uint32_t identity, int slot,
                                          const nine_slot_t *value)
{
    if (!s_ready || !value || slot < 0 || slot >= NINE_SLOT_COUNT ||
        !value->vm_size || value->vm_size > NINE_SAVE_CAPACITY) return NINE_STORE_IO;
    const size_t total = REC_HEADER + SLOT_META + value->vm_size + 4;
    uint8_t *raw = s_raw;
    if (total > NINE_SLOT_MAX) return NINE_STORE_IO;
    memset(raw, 0, total);
    wr32(raw, SLOT_MAGIC);
    wr32(raw + 4, REC_VERSION);
    wr32(raw + 8, identity);
    wr32(raw + 12, (uint32_t)total);
    raw[16] = (uint8_t)value->page;
    raw[17] = (uint8_t)(value->page >> 8);
    raw[18] = (uint8_t)value->chapter;
    raw[19] = (uint8_t)(value->chapter >> 8);
    wr32(raw + 20, (uint32_t)value->vm_size);
    memcpy(raw + REC_HEADER, value->preview, NINE_SLOT_PREVIEW);
    memcpy(raw + REC_HEADER + SLOT_META, value->vm, value->vm_size);
    wr32(raw + total - 4, crc32_bytes(raw, total - 4));
    char key[8];
    key_for(key, sizeof(key), "s", slot);
    return write_record(key, raw, total);
}

nine_store_status_t nine_store_erase_slot(int slot)
{
    if (!s_ready || slot < 0 || slot >= NINE_SLOT_COUNT) return NINE_STORE_IO;
    char key[8];
    key_for(key, sizeof(key), "s", slot);
    const esp_err_t err = nvs_erase_key(s_handle, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) return NINE_STORE_MISSING;
    if (err != ESP_OK) return NINE_STORE_IO;
    return nvs_commit(s_handle) == ESP_OK ? NINE_STORE_OK : NINE_STORE_IO;
}

nine_store_status_t nine_store_load_progress(uint32_t identity, uint8_t *data,
                                             size_t cap, size_t *size, uint16_t *page)
{
    uint8_t *raw = s_raw;
    size_t length = 0;
    nine_store_status_t status = read_record("auto", raw, NINE_SLOT_MAX, &length);
    if (status != NINE_STORE_OK) return status;
    status = verify_record(raw, length, AUTO_MAGIC, identity);
    if (status != NINE_STORE_OK) return status;
    if (length < REC_HEADER + 4) return NINE_STORE_FORMAT;
    const size_t body = length - REC_HEADER - 4;
    if (body > cap) return NINE_STORE_FORMAT;
    if (data) memcpy(data, raw + REC_HEADER, body);
    if (size) *size = body;
    if (page) *page = (uint16_t)(raw[16] | raw[17] << 8);
    return NINE_STORE_OK;
}

nine_store_status_t nine_store_write_progress(uint32_t identity, const uint8_t *data,
                                              size_t size, uint16_t page)
{
    if (!data || !size || size > NINE_SAVE_CAPACITY) return NINE_STORE_IO;
    const size_t total = REC_HEADER + size + 4;
    uint8_t *raw = s_raw;
    if (total > NINE_SLOT_MAX) return NINE_STORE_IO;
    memset(raw, 0, total);
    wr32(raw, AUTO_MAGIC);
    wr32(raw + 4, REC_VERSION);
    wr32(raw + 8, identity);
    wr32(raw + 12, (uint32_t)total);
    raw[16] = (uint8_t)page;
    raw[17] = (uint8_t)(page >> 8);
    memcpy(raw + REC_HEADER, data, size);
    wr32(raw + total - 4, crc32_bytes(raw, total - 4));
    return write_record("auto", raw, total);
}

nine_store_status_t nine_store_load_perm(uint32_t identity, uint8_t *data, size_t cap,
                                         size_t *size)
{
    uint8_t *raw = s_raw;
    size_t length = 0;
    nine_store_status_t status = read_record("perm", raw, NINE_SLOT_MAX, &length);
    if (status != NINE_STORE_OK) return status;
    status = verify_record(raw, length, PERM_MAGIC, identity);
    if (status != NINE_STORE_OK) return status;
    const size_t body = length - REC_HEADER - 4;
    if (body > cap) return NINE_STORE_FORMAT;
    if (data) memcpy(data, raw + REC_HEADER, body);
    if (size) *size = body;
    return NINE_STORE_OK;
}

nine_store_status_t nine_store_write_perm(uint32_t identity, const uint8_t *data,
                                          size_t size)
{
    if (!data || !size || size > NINE_SAVE_CAPACITY) return NINE_STORE_IO;
    const size_t total = REC_HEADER + size + 4;
    uint8_t *raw = s_raw;
    if (total > NINE_SLOT_MAX) return NINE_STORE_IO;
    memset(raw, 0, total);
    wr32(raw, PERM_MAGIC);
    wr32(raw + 4, REC_VERSION);
    wr32(raw + 8, identity);
    wr32(raw + 12, (uint32_t)total);
    memcpy(raw + REC_HEADER, data, size);
    wr32(raw + total - 4, crc32_bytes(raw, total - 4));
    return write_record("perm", raw, total);
}

nine_store_status_t nine_store_load_title_bg(uint8_t *index)
{
    if (!s_ready || !index) return NINE_STORE_IO;
    const esp_err_t err = nvs_get_u8(s_handle, "bg", index);
    if (err == ESP_ERR_NVS_NOT_FOUND) return NINE_STORE_MISSING;
    if (err != ESP_OK || *index > 2) return NINE_STORE_FORMAT;
    return NINE_STORE_OK;
}

nine_store_status_t nine_store_write_title_bg(uint8_t index)
{
    if (!s_ready || index > 2) return NINE_STORE_IO;
    if (nvs_set_u8(s_handle, "bg", index) != ESP_OK) return NINE_STORE_IO;
    return nvs_commit(s_handle) == ESP_OK ? NINE_STORE_OK : NINE_STORE_IO;
}
