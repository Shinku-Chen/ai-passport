// tests/host/stub_platform.c —— 宿主适配：堆（可注入失败）、时间、内存 NVS（可注入故障）。
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

static size_t s_free = 300000;
static int s_allow_alloc = -1;   // -1 = 不限；>=0 = 只允许成功这么多次分配
static size_t s_fail_size;       // >0 = 这个字节数的分配一律失败

void host_heap_set_limit(size_t max_single_alloc) { s_limit = max_single_alloc; }
void host_heap_set_free(size_t free_bytes) { s_free = free_bytes; }
void host_heap_fail_after(int allow) { s_allow_alloc = allow; }
void host_heap_fail_size(size_t bytes) { s_fail_size = bytes; }
size_t host_heap_peak(void) { return s_peak; }

void *heap_caps_malloc(size_t size, uint32_t caps)
{
    (void)caps;
    if (s_limit && size > s_limit) {
        return NULL;
    }
    if (s_fail_size && size == s_fail_size) {
        return NULL;
    }
    if (s_allow_alloc == 0) {
        return NULL;
    }
    if (s_allow_alloc > 0) {
        s_allow_alloc--;
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

void heap_caps_free(void *p) { free(p); }

size_t heap_caps_get_free_size(uint32_t caps)
{
    (void)caps;
    return s_free;
}

size_t heap_caps_get_largest_free_block(uint32_t caps)
{
    (void)caps;
    return 200000;
}

static int64_t s_now_us = 1000000;

int64_t esp_timer_get_time(void) { return s_now_us; }
void host_timer_advance_us(int64_t us) { s_now_us += us; }

// ---------------------------------------------------------------- 内存 NVS
#define MAX_ENTRIES 96
#define MAX_NS 8
#define MAX_KEY 20
typedef struct {
    char ns[MAX_NS];
    char key[MAX_KEY];
    uint8_t *data;
    size_t len;
    int used;
} entry_t;
typedef struct {
    char ns[MAX_NS];
    int open;
    // 单次提交的回滚快照
    entry_t *pending;
    uint8_t *old;
    size_t old_len;
    int old_used;
} handle_t;
static entry_t s_entries[MAX_ENTRIES];
static handle_t s_handles[8];
static int s_inited;
static int s_fail;
static int s_allow_init = 1;

const char *esp_err_to_name(esp_err_t err) { (void)err; return "stub"; }

void host_nvs_reset(void)
{
    for (int i = 0; i < MAX_ENTRIES; i++) {
        free(s_entries[i].data);
    }
    memset(s_entries, 0, sizeof(s_entries));
    memset(s_handles, 0, sizeof(s_handles));
    s_inited = 1;
    s_fail = HOST_NVS_OK;
    s_allow_init = 1;
    s_now_us = 1000000;
}

void host_nvs_set_fail(int mode) { s_fail = mode; }
void host_nvs_set_init_ok(int ok) { s_allow_init = ok; }

esp_err_t nvs_flash_init(void)
{
    if (!s_allow_init) {
        return 0x1105;
    }
    s_inited = 1;
    return ESP_OK;
}

esp_err_t nvs_flash_erase(void)
{
    for (int i = 0; i < MAX_ENTRIES; i++) {
        free(s_entries[i].data);
    }
    memset(s_entries, 0, sizeof(s_entries));
    return ESP_OK;
}

esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *out)
{
    (void)mode;
    if (!s_inited || s_fail == HOST_NVS_OPEN_FAIL) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (strlen(ns) >= MAX_NS) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    for (int i = 0; i < 8; i++) {
        if (!s_handles[i].open) {
            s_handles[i].open = 1;
            s_handles[i].pending = NULL;
            s_handles[i].old = NULL;
            s_handles[i].old_len = 0;
            s_handles[i].old_used = 0;
            snprintf(s_handles[i].ns, MAX_NS, "%s", ns);
            *out = (nvs_handle_t)(i + 1);
            return ESP_OK;
        }
    }
    return ESP_ERR_NVS_NOT_FOUND;
}

void nvs_close(nvs_handle_t h)
{
    handle_t *hd = h && h <= 8 ? &s_handles[h - 1] : NULL;
    if (hd) {
        hd->open = 0;
    }
}

static entry_t *find_ns(const char *ns, const char *key)
{
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (s_entries[i].used && strcmp(s_entries[i].ns, ns) == 0 &&
            strcmp(s_entries[i].key, key) == 0) {
            return &s_entries[i];
        }
    }
    return NULL;
}

static entry_t *find_any(const char *ns, const char *key)
{
    entry_t *e = find_ns(ns, key);
    if (e) {
        return e;
    }
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (!s_entries[i].used) {
            s_entries[i].used = 1;
            snprintf(s_entries[i].ns, MAX_NS, "%s", ns);
            snprintf(s_entries[i].key, MAX_KEY, "%s", key);
            return &s_entries[i];
        }
    }
    return NULL;
}

esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *v, size_t len)
{
    handle_t *hd = h && h <= 8 ? &s_handles[h - 1] : NULL;
    if (!hd || !hd->open) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (s_fail == HOST_NVS_FAIL_WRITE) {
        return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    }
    entry_t *e = find_any(hd->ns, key);
    if (!e) {
        return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    }
    hd->pending = e;
    free(hd->old);
    hd->old = e->data;
    hd->old_len = e->len;
    hd->old_used = e->used;
    e->data = malloc(len ? len : 1);
    memcpy(e->data, v, len);
    e->len = len;
    e->used = 1;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t h)
{
    handle_t *hd = h && h <= 8 ? &s_handles[h - 1] : NULL;
    if (!hd || !hd->open) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (s_fail == HOST_NVS_FAIL_WRITE) {
        if (hd->pending) {
            free(hd->pending->data);
            hd->pending->data = hd->old;
            hd->pending->len = hd->old_len;
            hd->pending->used = hd->old_used;
        }
        hd->old = NULL;
        hd->pending = NULL;
        return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    }
    free(hd->old);
    hd->old = NULL;
    hd->pending = NULL;
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *v, size_t *len)
{
    handle_t *hd = h && h <= 8 ? &s_handles[h - 1] : NULL;
    if (!hd || !hd->open) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    entry_t *e = find_ns(hd->ns, key);
    if (!e) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (v == NULL) {
        *len = e->len;
        return ESP_OK;
    }
    if (*len < e->len) {
        *len = e->len;
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(v, e->data, e->len);
    *len = e->len;
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    handle_t *hd = h && h <= 8 ? &s_handles[h - 1] : NULL;
    if (!hd || !hd->open) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    entry_t *e = find_ns(hd->ns, key);
    if (e) {
        free(e->data);
        memset(e, 0, sizeof(*e));
    }
    return ESP_OK;
}

// ---- 测试注入（直接按 ns/key 操作）
int host_nvs_put(const char *ns, const char *key, const void *data, size_t len)
{
    if (strlen(ns) >= MAX_NS || strlen(key) >= MAX_KEY) {
        return 0;
    }
    entry_t *e = find_any(ns, key);
    if (!e) {
        return 0;
    }
    free(e->data);
    e->data = malloc(len ? len : 1);
    memcpy(e->data, data, len);
    e->len = len;
    e->used = 1;
    snprintf(e->ns, MAX_NS, "%s", ns);
    snprintf(e->key, MAX_KEY, "%s", key);
    return 1;
}

int host_nvs_get(const char *ns, const char *key, void *data, size_t cap, size_t *out_len)
{
    entry_t *e = find_ns(ns, key);
    if (!e || e->len > cap) {
        return 0;
    }
    memcpy(data, e->data, e->len);
    if (out_len) {
        *out_len = e->len;
    }
    return 1;
}

int host_nvs_count(const char *ns)
{
    int n = 0;
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (s_entries[i].used && strcmp(s_entries[i].ns, ns) == 0) {
            n++;
        }
    }
    return n;
}

void host_nvs_corrupt(const char *ns, const char *key, unsigned seed)
{
    entry_t *e = find_ns(ns, key);
    if (!e) {
        return;
    }
    unsigned x = seed * 2654435761u + 12345u;
    for (size_t i = 0; i < e->len; i++) {
        x = x * 1103515245u + 12345u;
        e->data[i] = (uint8_t)(x >> 16);
    }
}
