#include <stdio.h>
#include "nvs.h"
#include <string.h>
#define NS_MAX 4
#define ITEM_MAX 64
#define BLOB_MAX 2048
typedef struct { char ns[24]; char key[16]; unsigned char data[BLOB_MAX]; size_t len; int used; } item_t;
static item_t items[ITEM_MAX];
static char names[NS_MAX][24];
static int ns_count;
static int fail_set;
const char *esp_err_to_name(esp_err_t e) { (void)e; return "nvs"; }
void nvs_stub_reset(void) { memset(items, 0, sizeof(items)); memset(names, 0, sizeof(names)); ns_count = 0; fail_set = 0; }
void nvs_stub_fail_next_set(void) { fail_set = 1; }
size_t nvs_stub_count(void) { size_t n = 0; for (int i = 0; i < ITEM_MAX; ++i) n += items[i].used ? 1u : 0u; return n; }
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *out) {
    (void)mode;
    if (!name || !out) return ESP_FAIL;
    for (int i = 0; i < ns_count; ++i) if (!strcmp(names[i], name)) { *out = i; return ESP_OK; }
    if (ns_count >= NS_MAX) return ESP_FAIL;
    snprintf(names[ns_count], sizeof(names[ns_count]), "%s", name);
    *out = ns_count++;
    return ESP_OK;
}
static item_t *find(nvs_handle_t h, const char *key) {
    if (h < 0 || h >= ns_count || !key) return NULL;
    for (int i = 0; i < ITEM_MAX; ++i)
        if (items[i].used && !strcmp(items[i].ns, names[h]) && !strcmp(items[i].key, key)) return &items[i];
    return NULL;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len) {
    item_t *it = find(h, key);
    if (!it) return ESP_ERR_NVS_NOT_FOUND;
    if (!out || !len) return ESP_FAIL;
    if (*len < it->len) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out, it->data, it->len); *len = it->len; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *data, size_t len) {
    if (fail_set) { fail_set = 0; return ESP_FAIL; }
    if (h < 0 || h >= ns_count || !key || !data || len > BLOB_MAX) return ESP_FAIL;
    item_t *it = find(h, key);
    if (!it) for (int i = 0; i < ITEM_MAX; ++i) if (!items[i].used) { it = &items[i]; break; }
    if (!it) return ESP_FAIL;
    memset(it, 0, sizeof(*it)); it->used = 1;
    snprintf(it->ns, sizeof(it->ns), "%s", names[h]); snprintf(it->key, sizeof(it->key), "%s", key);
    memcpy(it->data, data, len); it->len = len; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }
