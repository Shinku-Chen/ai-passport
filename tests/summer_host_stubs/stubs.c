#include "bsp_battery.h"
#include "bsp_display.h"
#include "esp_err.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <stdio.h>
#include <string.h>

int stub_lock_depth;

bool bsp_lvgl_lock(int timeout_ms)
{
    (void)timeout_ms;
    stub_lock_depth++;
    return true;
}

void bsp_lvgl_unlock(void)
{
    if (stub_lock_depth > 0) stub_lock_depth--;
}

int bsp_battery_soc(void) { return 76; }
const char *esp_err_to_name(esp_err_t err) { (void)err; return "stub"; }
esp_err_t nvs_flash_init(void) { return ESP_OK; }

#define MAX_KEYS 64
#define MAX_VALUE 8192
typedef struct {
    char key[16];
    size_t len;
    uint8_t data[MAX_VALUE];
} kv_t;
static kv_t s_kv[MAX_KEYS];
static int s_kv_count;
static int s_available = 1;
static int s_fail_set;
static int s_fail_commit;
static int s_fail_get;

void stub_nvs_reset(void)
{
    memset(s_kv, 0, sizeof(s_kv));
    s_kv_count = 0;
    s_available = 1;
    s_fail_set = 0;
    s_fail_commit = 0;
    s_fail_get = 0;
}
void stub_nvs_set_available(int available) { s_available = available; }
void stub_nvs_set_fail_set(int fail) { s_fail_set = fail; }
void stub_nvs_set_fail_commit(int fail) { s_fail_commit = fail; }
void stub_nvs_set_fail_get(int fail) { s_fail_get = fail; }
int stub_nvs_key_count(void) { return s_kv_count; }

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out)
{
    (void)name;
    (void)mode;
    if (!s_available || !out) return ESP_ERR_INVALID_STATE;
    *out = 1;
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *len)
{
    (void)handle;
    if (!s_available || !key || !len) return ESP_ERR_INVALID_ARG;
    if (s_fail_get) return ESP_ERR_INVALID_STATE;
    for (int i = 0; i < s_kv_count; ++i) {
        if (strcmp(s_kv[i].key, key) == 0) {
            if (!out) {
                *len = s_kv[i].len;
                return ESP_OK;
            }
            if (*len < s_kv[i].len) return ESP_ERR_NVS_INVALID_LENGTH;
            memcpy(out, s_kv[i].data, s_kv[i].len);
            *len = s_kv[i].len;
            return ESP_OK;
        }
    }
    return ESP_ERR_INVALID_ARG;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t len)
{
    (void)handle;
    if (!s_available || !key || !data || len > MAX_VALUE || s_fail_set) {
        return ESP_ERR_INVALID_STATE;
    }
    for (int i = 0; i < s_kv_count; ++i) {
        if (strcmp(s_kv[i].key, key) == 0) {
            memcpy(s_kv[i].data, data, len);
            s_kv[i].len = len;
            return ESP_OK;
        }
    }
    if (s_kv_count >= MAX_KEYS) return ESP_ERR_INVALID_STATE;
    snprintf(s_kv[s_kv_count].key, sizeof(s_kv[s_kv_count].key), "%s", key);
    memcpy(s_kv[s_kv_count].data, data, len);
    s_kv[s_kv_count].len = len;
    s_kv_count++;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    return s_fail_commit ? ESP_ERR_INVALID_STATE : ESP_OK;
}

void nvs_close(nvs_handle_t handle) { (void)handle; }
