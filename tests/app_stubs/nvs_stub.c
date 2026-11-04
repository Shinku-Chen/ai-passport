#include "nvs.h"

#include <stdio.h>
#include <string.h>

#define MAX_ENTRIES 128
#define MAX_HANDLES 8
#define MAX_DATA 4096

typedef struct {
    int used;
    char ns[20];
    char key[20];
    uint8_t data[MAX_DATA];
    size_t len;
} entry_t;

static entry_t s_entries[MAX_ENTRIES];
static char s_handles[MAX_HANDLES][20];
int nvs_stub_fail_write;
int nvs_stub_fail_read;
int nvs_stub_fail_open;

static entry_t *find(const char *ns, const char *key, int create)
{
    for (int i = 0; i < MAX_ENTRIES; ++i) {
        if (s_entries[i].used && strcmp(s_entries[i].ns, ns) == 0 &&
            strcmp(s_entries[i].key, key) == 0) {
            return &s_entries[i];
        }
    }
    if (!create) return NULL;
    for (int i = 0; i < MAX_ENTRIES; ++i) {
        if (!s_entries[i].used) {
            s_entries[i].used = 1;
            snprintf(s_entries[i].ns, sizeof(s_entries[i].ns), "%s", ns);
            snprintf(s_entries[i].key, sizeof(s_entries[i].key), "%s", key);
            s_entries[i].len = 0;
            return &s_entries[i];
        }
    }
    return NULL;
}

static const char *handle_ns(nvs_handle_t handle)
{
    if (handle <= 0 || handle > MAX_HANDLES) return "";
    return s_handles[handle - 1];
}

esp_err_t nvs_open(const char *name, int open_mode, nvs_handle_t *out_handle)
{
    (void)open_mode;
    if (nvs_stub_fail_open) return ESP_FAIL;
    for (int i = 0; i < MAX_HANDLES; ++i) {
        if (s_handles[i][0] && strcmp(s_handles[i], name) == 0) {
            if (out_handle) *out_handle = i + 1;
            return ESP_OK;
        }
    }
    for (int i = 0; i < MAX_HANDLES; ++i) {
        if (!s_handles[i][0]) {
            snprintf(s_handles[i], sizeof(s_handles[i]), "%s", name);
            if (out_handle) *out_handle = i + 1;
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

void nvs_close(nvs_handle_t handle) { (void)handle; }

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value,
                       size_t length)
{
    if (nvs_stub_fail_write) return ESP_FAIL;
    if (!value || length > MAX_DATA) return ESP_FAIL;
    entry_t *e = find(handle_ns(handle), key, 1);
    if (!e) return ESP_FAIL;
    memcpy(e->data, value, length);
    e->len = length;
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value,
                       size_t *length)
{
    if (nvs_stub_fail_read) return ESP_FAIL;
    entry_t *e = find(handle_ns(handle), key, 0);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    if (!length) return ESP_FAIL;
    if (!out_value) {
        *length = e->len;
        return ESP_OK;
    }
    if (*length < e->len) return ESP_ERR_INVALID_SIZE;
    memcpy(out_value, e->data, e->len);
    *length = e->len;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    return nvs_stub_fail_write ? ESP_FAIL : ESP_OK;
}

const char *esp_err_to_name(esp_err_t err)
{
    return err == ESP_OK ? "ESP_OK" : "ESP_FAIL";
}

void nvs_stub_reset(void)
{
    memset(s_entries, 0, sizeof(s_entries));
    memset(s_handles, 0, sizeof(s_handles));
    nvs_stub_fail_write = 0;
    nvs_stub_fail_read = 0;
    nvs_stub_fail_open = 0;
}

int nvs_stub_count(const char *ns)
{
    int n = 0;
    for (int i = 0; i < MAX_ENTRIES; ++i) {
        if (s_entries[i].used && strcmp(s_entries[i].ns, ns) == 0) n++;
    }
    return n;
}
