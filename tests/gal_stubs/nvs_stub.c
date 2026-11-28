#include "nvs.h"
#include <stdio.h>
#include <string.h>
#define MAX_NS 4
#define MAX_ITEMS 8
#define MAX_BLOB 256
typedef struct { char ns[24], key[16]; unsigned char data[MAX_BLOB]; size_t len; int used; } item_t;
static item_t items[MAX_ITEMS];
static char namespaces[MAX_NS][24];
static int ns_count;
const char *esp_err_to_name(esp_err_t err) { (void)err; return "stub"; }
esp_err_t nvs_open(const char *namespace_name, int mode, nvs_handle_t *out) {
    (void)mode;
    if (!namespace_name || !out) return ESP_FAIL;
    for (int i = 0; i < ns_count; ++i) if (strcmp(namespaces[i], namespace_name) == 0) { *out = i; return ESP_OK; }
    if (ns_count >= MAX_NS) return ESP_FAIL;
    snprintf(namespaces[ns_count], sizeof(namespaces[ns_count]), "%s", namespace_name);
    *out = ns_count++;
    return ESP_OK;
}
static item_t *find(nvs_handle_t h, const char *key) {
    if (h < 0 || h >= ns_count || !key) return NULL;
    for (int i = 0; i < MAX_ITEMS; ++i)
        if (items[i].used && strcmp(items[i].ns, namespaces[h]) == 0 && strcmp(items[i].key, key) == 0) return &items[i];
    return NULL;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *length) {
    item_t *it = find(h, key);
    if (!it) return ESP_ERR_NVS_NOT_FOUND;
    if (!out || !length) return ESP_FAIL;
    if (*length < it->len) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out, it->data, it->len); *length = it->len; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *data, size_t length) {
    if (h < 0 || h >= ns_count || !key || !data || length > MAX_BLOB) return ESP_FAIL;
    item_t *it = find(h, key);
    if (!it) { for (int i = 0; i < MAX_ITEMS; ++i) if (!items[i].used) { it = &items[i]; break; } }
    if (!it) return ESP_FAIL;
    memset(it, 0, sizeof(*it)); it->used = 1;
    snprintf(it->ns, sizeof(it->ns), "%s", namespaces[h]); snprintf(it->key, sizeof(it->key), "%s", key);
    memcpy(it->data, data, length); it->len = length; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }
