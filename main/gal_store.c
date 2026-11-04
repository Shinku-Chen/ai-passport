// main/gal_store.c —— NVS 持久化实现。
#include "gal_store.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "gal_store";
#define NS_NAME "manosaba"

static bool s_ready;
static gal_store_t *s_store;

static const char *slot_key(int slot)
{
    static char buf[8];
    if (slot == GAL_SLOT_AUTO) return "auto";
    snprintf(buf, sizeof(buf), "s%02d", slot);
    return buf;
}

bool gal_store_init(gal_store_t *store)
{
    if (!store) return false;
    memset(store, 0, sizeof(*store));
    s_store = store;
    nvs_handle_t h;
    const esp_err_t err = nvs_open(NS_NAME, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        store->ready = false;
        s_ready = false;
        ESP_LOGW(TAG, "NVS 命名空间不可用: %s", esp_err_to_name(err));
        return false;
    }
    nvs_close(h);
    store->ready = true;
    s_ready = true;
    return true;
}

static gal_store_status_t read_key_status(const char *key, uint8_t *buf, size_t cap,
                                           size_t *len)
{
    if (len) *len = 0;
    if (!s_ready || !buf || !len || cap == 0) return GAL_STORE_IO_ERROR;
    nvs_handle_t h;
    if (nvs_open(NS_NAME, NVS_READONLY, &h) != ESP_OK) return GAL_STORE_IO_ERROR;
    size_t need = 0;
    esp_err_t err = nvs_get_blob(h, key, NULL, &need);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(h);
        return GAL_STORE_NOT_FOUND;
    }
    if (err != ESP_OK) {
        nvs_close(h);
        return GAL_STORE_IO_ERROR;
    }
    if (need == 0 || need > cap) {
        nvs_close(h);
        return GAL_STORE_CORRUPT;
    }
    size_t got = need;
    err = nvs_get_blob(h, key, buf, &got);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return GAL_STORE_NOT_FOUND;
    if (err != ESP_OK) return err == ESP_ERR_NVS_INVALID_LENGTH ? GAL_STORE_CORRUPT
                                                                 : GAL_STORE_IO_ERROR;
    if (got == 0 || got != need || got > cap) return GAL_STORE_CORRUPT;
    *len = got;
    return GAL_STORE_OK;
}

static bool read_key(const char *key, uint8_t *buf, size_t cap, size_t *len)
{
    return read_key_status(key, buf, cap, len) == GAL_STORE_OK;
}

static bool write_key(const char *key, const uint8_t *buf, size_t len)
{
    if (s_store) s_store->writes++;
    if (!s_ready || !buf || len == 0) {
        if (s_store) s_store->failures++;
        return false;
    }
    nvs_handle_t h;
    if (nvs_open(NS_NAME, NVS_READWRITE, &h) != ESP_OK) {
        if (s_store) s_store->failures++;
        return false;
    }
    esp_err_t err = nvs_set_blob(h, key, buf, len);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        if (s_store) s_store->failures++;
        ESP_LOGW(TAG, "写入 %s 失败: %s", key, esp_err_to_name(err));
        return false;
    }
    return true;
}

void gal_settings_defaults(gal_settings_t *s)
{
    if (!s) return;
    s->version = GAL_SETTINGS_VERSION;
    s->text_speed = 1;
    s->auto_speed = 1;
    s->panel_opa = 2;
}

bool gal_store_load_settings(gal_store_t *store, gal_settings_t *s)
{
    (void)store;
    if (!s) return false;
    gal_settings_defaults(s);
    uint8_t buf[sizeof(gal_settings_t)];
    size_t len = 0;
    if (!read_key("set", buf, sizeof(buf), &len) || len != sizeof(*s)) return false;
    gal_settings_t tmp;
    memcpy(&tmp, buf, sizeof(tmp));
    if (tmp.version != GAL_SETTINGS_VERSION || tmp.text_speed > 1 ||
        tmp.auto_speed > 2 || tmp.panel_opa > 3) {
        return false;
    }
    *s = tmp;
    return true;
}

bool gal_store_save_settings(gal_store_t *store, const gal_settings_t *s)
{
    (void)store;
    if (!s) return false;
    return write_key("set", (const uint8_t *)s, sizeof(*s));
}

gal_store_status_t gal_store_read_slot(gal_store_t *store, int slot,
                                       uint8_t *buf, size_t cap, size_t *len)
{
    (void)store;
    if (slot < 0 || slot > GAL_SLOT_AUTO) return GAL_STORE_IO_ERROR;
    return read_key_status(slot_key(slot), buf, cap, len);
}

bool gal_store_load_slot(gal_store_t *store, int slot, uint8_t *buf, size_t cap, size_t *len)
{
    return gal_store_read_slot(store, slot, buf, cap, len) == GAL_STORE_OK;
}

bool gal_store_save_slot(gal_store_t *store, int slot, const uint8_t *buf, size_t len)
{
    (void)store;
    if (slot < 0 || slot > GAL_SLOT_AUTO) return false;
    return write_key(slot_key(slot), buf, len);
}

bool gal_store_load_unlock(gal_store_t *store, uint8_t *buf, size_t cap, size_t *len)
{
    (void)store;
    return read_key("unlock", buf, cap, len);
}

bool gal_store_save_unlock(gal_store_t *store, const uint8_t *buf, size_t len)
{
    (void)store;
    return write_key("unlock", buf, len);
}

bool gal_store_load_progress(gal_store_t *store, uint16_t *progress)
{
    (void)store;
    uint8_t buf[2];
    size_t len = 0;
    if (!progress || !read_key("prog", buf, sizeof(buf), &len) || len != 2) return false;
    *progress = (uint16_t)(buf[0] | (buf[1] << 8));
    return true;
}

bool gal_store_save_progress(gal_store_t *store, uint16_t progress)
{
    (void)store;
    const uint8_t buf[2] = { (uint8_t)progress, (uint8_t)(progress >> 8) };
    return write_key("prog", buf, sizeof(buf));
}
