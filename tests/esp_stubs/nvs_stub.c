#include "nvs.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_KEYS 64
#define MAX_VAL 4096
// Contract double, not a physical-flash claim: set_blob stages a pending value;
// commit publishes it atomically. A failed commit leaves the previous committed
// value visible. Real ESP-IDF NVS/power-loss behavior is still NOT RUN.
typedef struct {
    char key[24];
    uint8_t data[MAX_VAL];
    size_t len;
    uint8_t pending[MAX_VAL];
    size_t pending_len;
    int pending_valid;
    int used;
} kv_t;
static kv_t s_kv[MAX_KEYS];
static bool s_fail_set, s_fail_commit, s_fail_open, s_fail_get;
static int s_open_depth;

void nvs_stub_reset(void)
{
    memset(s_kv, 0, sizeof(s_kv));
    s_fail_set = s_fail_commit = s_fail_open = s_fail_get = false;
    s_open_depth = 0;
}
void nvs_stub_fail_set(bool fail) { s_fail_set = fail; }
void nvs_stub_fail_commit(bool fail) { s_fail_commit = fail; }
void nvs_stub_fail_open(bool fail) { s_fail_open = fail; }
void nvs_stub_fail_get(bool fail) { s_fail_get = fail; }
int nvs_stub_key_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_KEYS; ++i) if (s_kv[i].used) n++;
    return n;
}
size_t nvs_stub_key_size(const char *key)
{
    for (int i = 0; i < MAX_KEYS; ++i)
        if (s_kv[i].used && strcmp(s_kv[i].key, key) == 0) return s_kv[i].len;
    return 0;
}

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out)
{
    (void)name; (void)mode;
    if (s_fail_open) return ESP_FAIL;
    if (out) *out = 1;
    s_open_depth++;
    return ESP_OK;
}
void nvs_close(nvs_handle_t h) { (void)h; if (s_open_depth) s_open_depth--; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len)
{
    (void)h;
    if (s_fail_get) return ESP_FAIL;
    for (int i = 0; i < MAX_KEYS; ++i) {
        if (!s_kv[i].used || strcmp(s_kv[i].key, key) != 0) continue;
        if (!len) return ESP_ERR_INVALID_ARG;
        if (!out) { *len = s_kv[i].len; return ESP_OK; }
        if (*len < s_kv[i].len) { *len = s_kv[i].len; return ESP_ERR_NVS_INVALID_LENGTH; }
        memcpy(out, s_kv[i].data, s_kv[i].len);
        *len = s_kv[i].len;
        return ESP_OK;
    }
    return ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *data, size_t len)
{
    (void)h;
    if (s_fail_set) return ESP_FAIL;
    if (len > MAX_VAL) return ESP_ERR_INVALID_ARG;
    int slot = -1;
    for (int i = 0; i < MAX_KEYS; ++i) {
        if (s_kv[i].used && strcmp(s_kv[i].key, key) == 0) { slot = i; break; }
        if (!s_kv[i].used && slot < 0) slot = i;
    }
    if (slot < 0) return ESP_ERR_NO_MEM;
    if (!s_kv[slot].used) snprintf(s_kv[slot].key, sizeof(s_kv[slot].key), "%s", key);
    memcpy(s_kv[slot].pending, data, len);
    s_kv[slot].pending_len = len;
    s_kv[slot].pending_valid = 1;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h)
{
    (void)h;
    if (s_fail_commit) return ESP_FAIL;
    for (int i = 0; i < MAX_KEYS; ++i) {
        if (!s_kv[i].pending_valid) continue;
        memcpy(s_kv[i].data, s_kv[i].pending, s_kv[i].pending_len);
        s_kv[i].len = s_kv[i].pending_len;
        s_kv[i].used = 1;
        s_kv[i].pending_valid = 0;
    }
    return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    (void)h;
    for (int i = 0; i < MAX_KEYS; ++i) {
        if (s_kv[i].used && strcmp(s_kv[i].key, key) == 0) { s_kv[i].used = 0; return ESP_OK; }
    }
    return ESP_ERR_NVS_NOT_FOUND;
}
