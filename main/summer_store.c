// main/summer_store.c —— NVS 存档/设置实现。
#include "summer_store.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "summer_store";
static const char *NS = "summer";

static bool last_write_ok;
static bool slot_exists_state[SUMMER_SLOT_COUNT];

static void slot_key(int index, char out[8])
{
    snprintf(out, 8, "s%02d", index);
}

bool summer_store_available(const summer_store_t *store)
{
    return store && store->available;
}

bool summer_store_last_write_ok(const summer_store_t *store)
{
    (void)store;
    return last_write_ok;
}

void summer_store_default_settings(summer_store_t *store)
{
    if (!store) return;
    store->text_speed = 30;
    store->auto_speed = 1;
    store->panel_opacity = 2;
    store->settings_loaded = true;
}

static bool read_blob(nvs_handle_t handle, const char *key, uint8_t *buf, size_t cap,
                      size_t *out_len)
{
    size_t len = cap;
    const esp_err_t err = nvs_get_blob(handle, key, buf, &len);
    if (err != ESP_OK) return false;
    if (out_len) *out_len = len;
    return true;
}

static bool write_blob(nvs_handle_t handle, const char *key, const uint8_t *data, size_t len)
{
    const esp_err_t set_err = nvs_set_blob(handle, key, data, len);
    if (set_err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_set_blob %s 失败: %s", key, esp_err_to_name(set_err));
        return false;
    }
    const esp_err_t commit_err = nvs_commit(handle);
    if (commit_err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_commit %s 失败: %s", key, esp_err_to_name(commit_err));
        return false;
    }
    return true;
}

static bool settings_encode(const summer_store_t *store, uint8_t *out, size_t *len)
{
    out[0] = SUMMER_SETTINGS_VERSION;
    out[1] = store->text_speed;
    out[2] = store->auto_speed;
    out[3] = store->panel_opacity;
    const uint32_t crc = summer_crc32(out, 4);
    memcpy(out + 4, &crc, 4);
    *len = 8;
    return true;
}

static bool settings_decode(summer_store_t *store, const uint8_t *data, size_t len)
{
    if (!store || !data || len != 8 || data[0] != SUMMER_SETTINGS_VERSION) return false;
    uint32_t crc = 0;
    memcpy(&crc, data + 4, 4);
    if (crc != summer_crc32(data, 4)) return false;
    store->text_speed = data[1] == 0 ? 0 : 30;
    store->auto_speed = data[2] <= 2 ? data[2] : 1;
    store->panel_opacity = data[3] <= 4 ? data[3] : 2;
    store->settings_loaded = true;
    return true;
}

bool summer_store_init(summer_store_t *store)
{
    if (!store) return false;
    memset(store, 0, sizeof(*store));
    summer_store_default_settings(store);
    summer_progress_init(&store->progress);
    memset(slot_exists_state, 0, sizeof(slot_exists_state));
    last_write_ok = false;

    nvs_handle_t handle;
    const esp_err_t err = nvs_open(NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS 不可用(%s)，存档只读降级，不 erase", esp_err_to_name(err));
        store->available = false;
        return false;
    }
    store->available = true;

    uint8_t buf[512];
    size_t len = 0;
    if (read_blob(handle, SUMMER_SETTINGS_KEY, buf, sizeof(buf), &len)) {
        (void)settings_decode(store, buf, len);
    }
    if (read_blob(handle, SUMMER_PROGRESS_KEY, buf, sizeof(buf), &len)) {
        if (!summer_progress_decode(&store->progress, buf, len)) {
            ESP_LOGW(TAG, "周目进度校验失败，使用默认值（不覆盖旧档）");
            summer_progress_init(&store->progress);
        }
    }
    for (int i = 0; i < SUMMER_SLOT_COUNT; ++i) {
        char key[8];
        slot_key(i, key);
        size_t slot_len = 0;
        slot_exists_state[i] = nvs_get_blob(handle, key, NULL, &slot_len) == ESP_OK &&
                               slot_len > 0;
    }
    nvs_close(handle);
    return true;
}

bool summer_store_save_settings(summer_store_t *store)
{
    if (!store) return false;
    uint8_t buf[8];
    size_t len = 0;
    settings_encode(store, buf, &len);
    if (!store->available) {
        last_write_ok = false;
        return false;
    }
    nvs_handle_t handle;
    if (nvs_open(NS, NVS_READWRITE, &handle) != ESP_OK) {
        last_write_ok = false;
        return false;
    }
    last_write_ok = write_blob(handle, SUMMER_SETTINGS_KEY, buf, len);
    nvs_close(handle);
    return last_write_ok;
}

void summer_store_mark_progress_dirty(summer_store_t *store)
{
    if (store) store->progress.dirty = true;
}

bool summer_store_flush_progress(summer_store_t *store)
{
    if (!store) return false;
    if (!store->progress.dirty) return true;
    if (!store->available) {
        store->progress_retries++;
        return false;
    }
    uint8_t buf[96];
    size_t len = 0;
    if (!summer_progress_encode(&store->progress, buf, sizeof(buf), &len)) {
        return false;
    }
    nvs_handle_t handle;
    if (nvs_open(NS, NVS_READWRITE, &handle) != ESP_OK) {
        store->progress_retries++;
        return false;
    }
    const bool ok = write_blob(handle, SUMMER_PROGRESS_KEY, buf, len);
    nvs_close(handle);
    if (ok) {
        store->progress.dirty = false;
        store->progress_retries = 0;
    } else {
        store->progress_retries++;
    }
    return ok;
}

bool summer_store_write_slot(summer_store_t *store, int index, const uint8_t *data, size_t len)
{
    last_write_ok = false;
    if (!store || index < 0 || index >= SUMMER_SLOT_COUNT || !data || len == 0) return false;
    if (!store->available) return false;
    nvs_handle_t handle;
    if (nvs_open(NS, NVS_READWRITE, &handle) != ESP_OK) return false;
    char key[8];
    slot_key(index, key);
    const bool ok = write_blob(handle, key, data, len);
    nvs_close(handle);
    last_write_ok = ok;
    if (ok) slot_exists_state[index] = true;
    return ok;
}

bool summer_store_read_slot(const summer_store_t *store, int index, uint8_t *buf, size_t cap,
                            size_t *out_len)
{
    if (!store || index < 0 || index >= SUMMER_SLOT_COUNT || !buf || cap == 0) return false;
    if (!store->available) return false;
    nvs_handle_t handle;
    if (nvs_open(NS, NVS_READONLY, &handle) != ESP_OK) return false;
    char key[8];
    slot_key(index, key);
    const bool ok = read_blob(handle, key, buf, cap, out_len);
    nvs_close(handle);
    return ok;
}

bool summer_store_slot_exists(const summer_store_t *store, int index)
{
    if (!store || index < 0 || index >= SUMMER_SLOT_COUNT) return false;
    return slot_exists_state[index];
}

bool summer_store_write_auto(summer_store_t *store, const uint8_t *data, size_t len)
{
    last_write_ok = false;
    if (!store || !data || len == 0 || !store->available) return false;
    nvs_handle_t handle;
    if (nvs_open(NS, NVS_READWRITE, &handle) != ESP_OK) return false;
    const bool ok = write_blob(handle, SUMMER_AUTO_KEY, data, len);
    nvs_close(handle);
    last_write_ok = ok;
    return ok;
}

bool summer_store_read_auto(const summer_store_t *store, uint8_t *buf, size_t cap, size_t *out_len)
{
    if (!store || !buf || cap == 0 || !store->available) return false;
    nvs_handle_t handle;
    if (nvs_open(NS, NVS_READONLY, &handle) != ESP_OK) return false;
    const bool ok = read_blob(handle, SUMMER_AUTO_KEY, buf, cap, out_len);
    nvs_close(handle);
    return ok;
}
