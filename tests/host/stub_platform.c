// tests/host/stub_platform.c —— 宿主适配：堆（可注入失败）、时间、内存 NVS。
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t s_limit;
static size_t s_used;
static size_t s_peak;

void host_heap_set_limit(size_t max_single_alloc) { s_limit = max_single_alloc; }
size_t host_heap_peak(void) { return s_peak; }

void *heap_caps_malloc(size_t size, uint32_t caps)
{
    (void)caps;
    if (s_limit && size > s_limit) {
        return NULL;
    }
    void *p = malloc(size);
    if (p) {
        s_used += size;
        if (s_used > s_peak) {
            s_peak = s_used;
        }
    }
    return p;
}

void heap_caps_free(void *p)
{
    if (p) {
        free(p);
    }
}

size_t heap_caps_get_free_size(uint32_t caps)
{
    (void)caps;
    return 300000;
}

size_t heap_caps_get_largest_free_block(uint32_t caps)
{
    (void)caps;
    return 200000;
}

static int64_t s_now_us;
int64_t esp_timer_get_time(void) { return s_now_us += 1000000; }

// ---------------------------------------------------------------- 内存 NVS
#define MAX_ENTRIES 24
#define MAX_KEY 16
typedef struct {
    char key[MAX_KEY];
    uint8_t *data;
    size_t len;
    int used;
} entry_t;
static entry_t s_entries[MAX_ENTRIES];
static int s_inited;
static int s_fail;   // 1 = set_blob/commit 失败（模拟 NVS 满/坏）

void host_nvs_set_fail(int fail) { s_fail = fail; }

const char *esp_err_to_name(esp_err_t err) { (void)err; return "stub"; }

esp_err_t nvs_flash_init(void)
{
    s_inited = 1;
    return ESP_OK;
}

esp_err_t nvs_flash_erase(void)
{
    memset(s_entries, 0, sizeof(s_entries));
    return ESP_OK;
}

esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *out)
{
    (void)mode;
    if (!s_inited) {
        return 0x1101;
    }
    if (strcmp(ns, "paper") != 0) {
        return 0x1101;
    }
    *out = 1;
    return ESP_OK;
}

void nvs_close(nvs_handle_t h) { (void)h; }

static entry_t *find(const char *key)
{
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (s_entries[i].used && strcmp(s_entries[i].key, key) == 0) {
            return &s_entries[i];
        }
    }
    return NULL;
}

esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *v, size_t len)
{
    (void)h;
    if (s_fail) {
        return 0x1103;   // ESP_ERR_NVS_NOT_ENOUGH_SPACE
    }
    entry_t *e = find(key);
    if (!e) {
        for (int i = 0; i < MAX_ENTRIES && !e; i++) {
            if (!s_entries[i].used) {
                e = &s_entries[i];
            }
        }
    }
    if (!e) {
        return 0x1103;
    }
    free(e->data);
    e->data = malloc(len);
    memcpy(e->data, v, len);
    e->len = len;
    e->used = 1;
    snprintf(e->key, MAX_KEY, "%s", key);
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *v, size_t *len)
{
    (void)h;
    entry_t *e = find(key);
    if (!e) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (*len < e->len) {
        *len = e->len;
        return 0x1104;   // ESP_ERR_NVS_INVALID_LENGTH
    }
    memcpy(v, e->data, e->len);
    *len = e->len;
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    (void)h;
    entry_t *e = find(key);
    if (e) {
        free(e->data);
        memset(e, 0, sizeof(*e));
    }
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t h)
{
    (void)h;
    return s_fail ? 0x1103 : ESP_OK;
}
