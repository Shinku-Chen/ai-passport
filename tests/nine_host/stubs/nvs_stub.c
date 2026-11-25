#include "nvs.h"

#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#define MAX_NS 4
#define MAX_ITEMS 80
#define MAX_BLOB 4096

typedef struct {
    char ns[20];
    char key[16];
    uint8_t data[MAX_BLOB];
    size_t len;
    bool used;
} item_t;

static item_t s_items[MAX_ITEMS];
static char s_namespaces[MAX_NS][20];
static int s_ns_count;

const char *esp_err_to_name(esp_err_t err)
{
    (void)err;
    return "stub";
}

void nvs_stub_reset(void)
{
    memset(s_items, 0, sizeof(s_items));
    memset(s_namespaces, 0, sizeof(s_namespaces));
    s_ns_count = 0;
}

int nvs_stub_set_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_ITEMS; ++i) if (s_items[i].used) ++n;
    return n;
}

esp_err_t nvs_stub_store(const char *ns, const char *key, const void *data, size_t len)
{
    if (!ns || !key || !data || len > MAX_BLOB) return ESP_FAIL;
    item_t *it = NULL;
    for (int i = 0; i < MAX_ITEMS; ++i) {
        if (s_items[i].used && !strcmp(s_items[i].ns, ns) && !strcmp(s_items[i].key, key)) {
            it = &s_items[i];
            break;
        }
    }
    if (!it) for (int i = 0; i < MAX_ITEMS; ++i) if (!s_items[i].used) { it = &s_items[i]; break; }
    if (!it) return ESP_FAIL;
    memset(it, 0, sizeof(*it));
    it->used = true;
    snprintf(it->ns, sizeof(it->ns), "%s", ns);
    snprintf(it->key, sizeof(it->key), "%s", key);
    memcpy(it->data, data, len);
    it->len = len;
    return ESP_OK;
}

esp_err_t nvs_open(const char *namespace_name, int mode, nvs_handle_t *out)
{
    (void)mode;
    if (!namespace_name || !out) return ESP_FAIL;
    for (int i = 0; i < s_ns_count; ++i) {
        if (!strcmp(s_namespaces[i], namespace_name)) { *out = i; return ESP_OK; }
    }
    if (s_ns_count >= MAX_NS) return ESP_FAIL;
    snprintf(s_namespaces[s_ns_count], sizeof(s_namespaces[s_ns_count]), "%s", namespace_name);
    *out = s_ns_count++;
    return ESP_OK;
}

static item_t *find(nvs_handle_t h, const char *key)
{
    if (h < 0 || h >= s_ns_count || !key) return NULL;
    for (int i = 0; i < MAX_ITEMS; ++i) {
        if (s_items[i].used && !strcmp(s_items[i].ns, s_namespaces[h]) &&
            !strcmp(s_items[i].key, key)) return &s_items[i];
    }
    return NULL;
}

esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *length)
{
    item_t *it = find(h, key);
    if (!it) return ESP_ERR_NVS_NOT_FOUND;
    if (!out || !length) return ESP_FAIL;
    if (*length < it->len) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out, it->data, it->len);
    *length = it->len;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *data, size_t length)
{
    if (h < 0 || h >= s_ns_count) return ESP_FAIL;
    return nvs_stub_store(s_namespaces[h], key, data, length);
}

esp_err_t nvs_commit(nvs_handle_t h)
{
    (void)h;
    return ESP_OK;
}

void nvs_close(nvs_handle_t h)
{
    (void)h;
}
