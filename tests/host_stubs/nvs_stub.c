// tests/host_stubs/nvs_stub.c —— 内存 NVS 假实现。
#include "nvs.h"

#include <stdio.h>
#include <string.h>

#define MAX_KEYS 64
#define MAX_BLOB 512

typedef struct {
    char key[16];
    size_t len;
    uint8_t data[MAX_BLOB];
    int used;
} entry_t;

static entry_t s_entries[MAX_KEYS];
static int s_writable = 1;
static int s_fail_commit = 0;

void nvs_stub_reset(void)
{
    memset(s_entries, 0, sizeof(s_entries));
    s_writable = 1;
    s_fail_commit = 0;
}

void nvs_stub_set_writable(int writable) { s_writable = writable ? 1 : 0; }
void nvs_stub_set_fail_commit(int fail) { s_fail_commit = fail ? 1 : 0; }

int nvs_stub_key_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_KEYS; ++i) n += s_entries[i].used ? 1 : 0;
    return n;
}

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out)
{
    (void)name;
    if (!s_writable && mode == NVS_READWRITE) return ESP_FAIL;
    if (out) *out = 1;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle) { (void)handle; }

static entry_t *find(const char *key)
{
    for (int i = 0; i < MAX_KEYS; ++i) {
        if (s_entries[i].used && strcmp(s_entries[i].key, key) == 0) return &s_entries[i];
    }
    return NULL;
}

int nvs_stub_read_count;

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *len)
{
    (void)handle;
    nvs_stub_read_count++;
    entry_t *e = find(key);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    if (*len < e->len) {
        *len = e->len;
        return ESP_FAIL;
    }
    memcpy(out, e->data, e->len);
    *len = e->len;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t len)
{
    (void)handle;
    if (!s_writable) return ESP_FAIL;
    if (len > MAX_BLOB) return ESP_FAIL;
    entry_t *e = find(key);
    if (!e) {
        for (int i = 0; i < MAX_KEYS; ++i) {
            if (!s_entries[i].used) {
                e = &s_entries[i];
                break;
            }
        }
    }
    if (!e) return ESP_FAIL;
    memset(e, 0, sizeof(*e));
    snprintf(e->key, sizeof(e->key), "%s", key);
    memcpy(e->data, data, len);
    e->len = len;
    e->used = 1;
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    (void)handle;
    entry_t *e = find(key);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    memset(e, 0, sizeof(*e));
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    return s_fail_commit ? ESP_FAIL : ESP_OK;
}
