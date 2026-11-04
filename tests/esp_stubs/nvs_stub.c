// tests/esp_stubs/nvs_stub.c —— 内存版 NVS + 故障注入(宿主机测试)。
//
// 语义尽量贴近 NVS:nvs_set_blob / nvs_erase_key 只是"暂存",nvs_commit 成功后才
// 真正生效;commit 失败时旧值保持不变。这样测试才能验证"commit 失败 = 没保存"。
#include "nvs.h"

#include "esp_err.h"
#include "nvs_flash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ENTRIES 32
#define MAX_BLOB 512
#define MAX_NAME 32

typedef struct {
    char key[16];
    uint8_t data[MAX_BLOB];
    size_t len;
    int used;
} entry_t;

static entry_t g_committed[MAX_ENTRIES];
static int g_committed_count;

static entry_t g_pending[MAX_ENTRIES];
static int g_pending_count;
static int g_pending_erase[MAX_ENTRIES];

static int g_fail_open;
static int g_fail_set;
static int g_fail_commit;
static int g_fail_get;

static entry_t *find_in(entry_t *table, int count, const char *key)
{
    for (int i = 0; i < count; ++i) {
        if (strcmp(table[i].key, key) == 0) return &table[i];
    }
    return NULL;
}

void nvs_stub_reset(void)
{
    memset(g_committed, 0, sizeof(g_committed));
    memset(g_pending, 0, sizeof(g_pending));
    memset(g_pending_erase, 0, sizeof(g_pending_erase));
    g_committed_count = g_pending_count = 0;
    g_fail_open = g_fail_set = g_fail_commit = g_fail_get = 0;
}

void nvs_stub_fail_open(int on) { g_fail_open = on; }
void nvs_stub_fail_set(int on) { g_fail_set = on; }
void nvs_stub_fail_commit(int on) { g_fail_commit = on; }
void nvs_stub_fail_get(int on) { g_fail_get = on; }

int nvs_stub_count(void) { return g_committed_count; }

// 直接写入已提交区(绕过暂存与 CRC 重算),用于构造"旧版本/损坏"存档。
int nvs_stub_put(const char *key, const void *data, size_t len)
{
    if (!key || len > MAX_BLOB) return 0;
    entry_t *e = find_in(g_committed, g_committed_count, key);
    if (!e) {
        if (g_committed_count >= MAX_ENTRIES) return 0;
        e = &g_committed[g_committed_count++];
        snprintf(e->key, sizeof(e->key), "%s", key);
    }
    memcpy(e->data, data, len);
    e->len = len;
    e->used = 1;
    return 1;
}

int nvs_stub_corrupt(const char *ns, const char *key, size_t offset)
{
    (void)ns;
    entry_t *e = find_in(g_committed, g_committed_count, key);
    if (!e || offset >= e->len) return 0;
    e->data[offset] ^= 0xFFu;
    return 1;
}

esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_flash_erase(void) { nvs_stub_reset(); return ESP_OK; }

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out)
{
    (void)mode;
    if (g_fail_open) return ESP_FAIL;
    if (!name || !out) return ESP_FAIL;
    *out = 1;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle) { (void)handle; }

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *len)
{
    (void)handle;
    if (g_fail_get) return ESP_FAIL;
    if (!out || !len) return ESP_FAIL;
    entry_t *e = find_in(g_committed, g_committed_count, key);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    if (*len < e->len) {
        *len = e->len;
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(out, e->data, e->len);
    *len = e->len;
    return ESP_OK;
}

static int pending_slot(const char *key, int create)
{
    for (int i = 0; i < g_pending_count; ++i) {
        if (strcmp(g_pending[i].key, key) == 0) return i;
    }
    if (!create || g_pending_count >= MAX_ENTRIES) return -1;
    const int idx = g_pending_count++;
    memset(&g_pending[idx], 0, sizeof(g_pending[idx]));
    snprintf(g_pending[idx].key, sizeof(g_pending[idx].key), "%s", key);
    g_pending_erase[idx] = 0;
    return idx;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length)
{
    (void)handle;
    if (g_fail_set) return ESP_FAIL;
    if (!key || !value || length > MAX_BLOB) return ESP_FAIL;
    const int idx = pending_slot(key, 1);
    if (idx < 0) return ESP_FAIL;
    memcpy(g_pending[idx].data, value, length);
    g_pending[idx].len = length;
    g_pending[idx].used = 1;
    g_pending_erase[idx] = 0;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    if (g_fail_commit) {
        g_pending_count = 0;                // 暂存内容丢弃,旧值保持
        return ESP_FAIL;
    }
    for (int i = 0; i < g_pending_count; ++i) {
        const char *key = g_pending[i].key;
        if (g_pending_erase[i]) {
            entry_t *e = find_in(g_committed, g_committed_count, key);
            if (e) {
                const int idx = (int)(e - g_committed);
                g_committed[idx] = g_committed[--g_committed_count];
            }
            continue;
        }
        entry_t *e = find_in(g_committed, g_committed_count, key);
        if (!e) {
            if (g_committed_count >= MAX_ENTRIES) return ESP_FAIL;
            e = &g_committed[g_committed_count++];
            snprintf(e->key, sizeof(e->key), "%s", key);
        }
        memcpy(e->data, g_pending[i].data, g_pending[i].len);
        e->len = g_pending[i].len;
        e->used = 1;
    }
    g_pending_count = 0;
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    (void)handle;
    const int idx = pending_slot(key, 1);
    if (idx < 0) return ESP_FAIL;
    g_pending_erase[idx] = 1;
    g_pending[idx].used = 0;
    return ESP_OK;
}

uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *buf, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        crc ^= buf[i];
        for (int b = 0; b < 8; ++b) {
            const uint32_t mask = (uint32_t)(-(int32_t)(crc & 1u));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}
