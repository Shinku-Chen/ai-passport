#include "nvs.h"
#include <stdbool.h>
#include <string.h>

#define MAX_KEYS 8
#define MAX_VALUE 512

typedef struct {
    char key[16];
    int is_u8;
    size_t len;
    uint8_t data[MAX_VALUE];
} fake_entry_t;

static fake_entry_t s_committed[MAX_KEYS];
static int s_committed_count;
static fake_entry_t s_staged[MAX_KEYS];
static int s_staged_count;
static bool s_staged_valid;
static int s_fail_set;
static int s_fail_commit;

const char *esp_err_to_name(int error) { (void)error; return "error"; }

static fake_entry_t *find_in(fake_entry_t *entries, int count, const char *key)
{
    for (int i = 0; i < count; ++i) {
        if (strcmp(entries[i].key, key) == 0) return &entries[i];
    }
    return NULL;
}

static void stage_begin(void)
{
    if (s_staged_valid) return;
    memcpy(s_staged, s_committed, sizeof(s_staged));
    s_staged_count = s_committed_count;
    s_staged_valid = true;
}

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out)
{
    (void)name; (void)mode; if (out) *out = 1; return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *len)
{
    (void)handle;
    fake_entry_t *e = find_in(s_committed, s_committed_count, key);
    if (!e || e->is_u8) return ESP_ERR_NVS_NOT_FOUND;
    if (!len || !out || *len < e->len) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out, e->data, e->len);
    *len = e->len;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t len)
{
    (void)handle;
    if (s_fail_set || len > MAX_VALUE) return ESP_FAIL;
    stage_begin();
    fake_entry_t *e = find_in(s_staged, s_staged_count, key);
    if (!e) {
        if (s_staged_count >= MAX_KEYS) return ESP_FAIL;
        e = &s_staged[s_staged_count++];
        memset(e, 0, sizeof(*e));
        strncpy(e->key, key, sizeof(e->key) - 1);
    }
    e->is_u8 = 0;
    e->len = len;
    memcpy(e->data, data, len);
    return ESP_OK;
}

esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out)
{
    (void)handle;
    fake_entry_t *e = find_in(s_committed, s_committed_count, key);
    if (!e || !e->is_u8 || !out) return ESP_ERR_NVS_NOT_FOUND;
    *out = e->data[0];
    return ESP_OK;
}

esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    (void)handle;
    if (s_fail_set) return ESP_FAIL;
    stage_begin();
    fake_entry_t *e = find_in(s_staged, s_staged_count, key);
    if (!e) {
        if (s_staged_count >= MAX_KEYS) return ESP_FAIL;
        e = &s_staged[s_staged_count++];
        memset(e, 0, sizeof(*e));
        strncpy(e->key, key, sizeof(e->key) - 1);
    }
    e->is_u8 = 1;
    e->len = 1;
    e->data[0] = value;
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    (void)handle;
    stage_begin();
    fake_entry_t *e = find_in(s_staged, s_staged_count, key);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    const int idx = (int)(e - s_staged);
    memmove(&s_staged[idx], &s_staged[idx + 1],
            (size_t)(s_staged_count - idx - 1) * sizeof(fake_entry_t));
    --s_staged_count;
    memset(&s_staged[s_staged_count], 0, sizeof(fake_entry_t));
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    if (s_fail_commit) return ESP_FAIL;
    if (s_staged_valid) {
        memcpy(s_committed, s_staged, sizeof(s_committed));
        s_committed_count = s_staged_count;
        s_staged_valid = false;
    }
    return ESP_OK;
}

void fake_nvs_reset(void)
{
    memset(s_committed, 0, sizeof(s_committed));
    memset(s_staged, 0, sizeof(s_staged));
    s_committed_count = 0;
    s_staged_count = 0;
    s_staged_valid = false;
    s_fail_set = 0;
    s_fail_commit = 0;
}

void fake_nvs_fail_set(int fail) { s_fail_set = fail; }
void fake_nvs_fail_commit(int fail) { s_fail_commit = fail; }

int fake_nvs_has(const char *key)
{
    return find_in(s_committed, s_committed_count, key) != NULL;
}

