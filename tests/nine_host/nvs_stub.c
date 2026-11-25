#include "nvs.h"
#include <stdio.h>
#include <string.h>
#define MAX_NAMESPACES 4
#define MAX_ITEMS 16
#define MAX_BLOB 4096
typedef struct {
    char ns[24], key[16];
    unsigned char data[MAX_BLOB];
    size_t len;
    int used;
} item_t;
static item_t items[MAX_ITEMS];
static char namespaces[MAX_NAMESPACES][24];
static int ns_count;
const char *esp_err_to_name(esp_err_t err) { (void)err; return "stub"; }
esp_err_t nvs_open(const char *namespace_name, int mode, nvs_handle_t *out) {
    (void)mode;
    if (!namespace_name || !out) return ESP_FAIL;
    for (int i = 0; i < ns_count; ++i)
        if (strcmp(namespaces[i], namespace_name) == 0) { *out = i; return ESP_OK; }
    if (ns_count >= MAX_NAMESPACES) return ESP_FAIL;
    snprintf(namespaces[ns_count], sizeof(namespaces[ns_count]), "%s", namespace_name);
    *out = ns_count++;
    return ESP_OK;
}
static item_t *find(nvs_handle_t h, const char *key) {
    if (h < 0 || h >= ns_count || !key) return NULL;
    for (int i = 0; i < MAX_ITEMS; ++i)
        if (items[i].used && strcmp(items[i].ns, namespaces[h]) == 0 &&
            strcmp(items[i].key, key) == 0) return &items[i];
    return NULL;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *length) {
    item_t *it = find(h, key);
    if (!it) return ESP_ERR_NVS_NOT_FOUND;
    if (!length) return ESP_FAIL;
    if (!out) { *length = it->len; return ESP_OK; }
    if (*length < it->len) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out, it->data, it->len);
    *length = it->len;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *data, size_t length) {
    if (h < 0 || h >= ns_count || !key || !data || length > MAX_BLOB) return ESP_FAIL;
    item_t *it = find(h, key);
    if (!it) for (int i = 0; i < MAX_ITEMS; ++i) if (!items[i].used) { it = &items[i]; break; }
    if (!it) return ESP_FAIL;
    memset(it, 0, sizeof(*it));
    it->used = 1;
    snprintf(it->ns, sizeof(it->ns), "%s", namespaces[h]);
    snprintf(it->key, sizeof(it->key), "%s", key);
    memcpy(it->data, data, length);
    it->len = length;
    return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *out) {
    item_t *it = find(h, key);
    if (!it) return ESP_ERR_NVS_NOT_FOUND;
    if (!out || it->len != 1) return ESP_FAIL;
    *out = it->data[0];
    return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t value) {
    return nvs_set_blob(h, key, &value, 1);
}
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key) {
    item_t *it = find(h, key);
    if (!it) return ESP_ERR_NVS_NOT_FOUND;
    memset(it, 0, sizeof(*it));
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }
int nvs_stub_mutate(const char *key, size_t offset, uint8_t xor_value) {
    for (int i = 0; i < MAX_ITEMS; ++i) {
        if (items[i].used && strcmp(items[i].key, key) == 0 && offset < items[i].len) {
            items[i].data[offset] ^= xor_value;
            return 1;
        }
    }
    return 0;
}
