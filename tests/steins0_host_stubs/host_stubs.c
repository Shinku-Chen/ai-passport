// Host NVS/battery stubs for the Steins;Gate 0 application regressions.
#include "nvs.h"
#include "lvgl.h"
#include "bsp_battery.h"

#include <stdio.h>
#include <string.h>

#define MAX_ENTRIES 320
#define MAX_BLOB 2048

typedef struct {
    char ns[16];
    char key[16];
    uint8_t data[MAX_BLOB];
    size_t len;
    int used;
} entry_t;

static entry_t s_entries[MAX_ENTRIES];
int host_nvs_commit_fail;
int host_nvs_open_fail;
int host_nvs_set_fail;
int host_nvs_get_fail;
int host_nvs_writes;
static int s_battery = 88;

void host_nvs_reset(void)
{
    memset(s_entries, 0, sizeof(s_entries));
    host_nvs_commit_fail = 0;
    host_nvs_open_fail = 0;
    host_nvs_set_fail = 0;
    host_nvs_get_fail = 0;
    host_nvs_writes = 0;
    s_battery = 88;
}

void host_set_battery(int percent) { s_battery = percent; }
int bsp_battery_soc(void) { return s_battery; }

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

esp_err_t nvs_open(const char *name, nvs_open_mode_t open_mode, nvs_handle_t *out_handle)
{
    (void)open_mode;
    if (host_nvs_open_fail) return ESP_FAIL;
    entry_t *marker = find(name, "\x01open", 1);
    if (!marker) return ESP_FAIL;
    *out_handle = (nvs_handle_t)((marker - s_entries) + 1);
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle) { (void)handle; }

static entry_t *entry_from_handle(nvs_handle_t handle)
{
    if (handle == 0 || handle > MAX_ENTRIES) return NULL;
    return &s_entries[handle - 1];
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length)
{
    if (host_nvs_set_fail) return ESP_FAIL;
    entry_t *marker = entry_from_handle(handle);
    if (!marker || length > MAX_BLOB) return ESP_FAIL;
    entry_t *entry = find(marker->ns, key, 1);
    if (!entry) return ESP_FAIL;
    memcpy(entry->data, value, length);
    entry->len = length;
    ++host_nvs_writes;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    return host_nvs_commit_fail ? ESP_FAIL : ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value, size_t *length)
{
    if (host_nvs_get_fail) return ESP_FAIL;
    entry_t *marker = entry_from_handle(handle);
    if (!marker || !length) return ESP_FAIL;
    entry_t *entry = find(marker->ns, key, 0);
    if (!entry) return ESP_ERR_NVS_NOT_FOUND;
    if (!out_value) {
        *length = entry->len;
        return ESP_OK;
    }
    if (*length < entry->len) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out_value, entry->data, entry->len);
    *length = entry->len;
    return ESP_OK;
}

const lv_font_t steins0_font16 = {0};
