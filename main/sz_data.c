// main/sz_data.c — 字卡数据只读表 + NVS 认识标记。
#include "sz_data.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "sz_data";

#define SZ_NVS_NAMESPACE "sz_cards"
#define SZ_NVS_KEY_KNOWN "known"

// 认识标记内存副本(0/1)。分配失败或 NVS 不可用时该指针为 NULL,功能降级。
static uint8_t *s_known = NULL;
static nvs_handle_t s_nvs = 0;

esp_err_t sz_data_init(void)
{
    // nvs_flash_init 幂等(已在别处调过也没关系)
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS 初始化失败(%s),认识标记不保存", esp_err_to_name(err));
        return err;
    }

    err = nvs_open(SZ_NVS_NAMESPACE, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "打开 NVS 命名空间失败(%s)", esp_err_to_name(err));
        s_nvs = 0;
        return err;
    }

    s_known = calloc(sz_card_count, 1);   // 212 字节
    if (!s_known) {
        ESP_LOGW(TAG, "认识标记内存分配失败,降级不保存");
        return ESP_ERR_NO_MEM;
    }

    size_t len = sz_card_count;
    err = nvs_get_blob(s_nvs, SZ_NVS_KEY_KNOWN, s_known, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "首次启动,认识标记初始化为全 0");
        memset(s_known, 0, sz_card_count);
        err = ESP_OK;
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "读取认识标记失败(%s)", esp_err_to_name(err));
        memset(s_known, 0, sz_card_count);
        err = ESP_OK;
    } else if (len != sz_card_count) {
        // 长度不匹配(可能换了字表),重置
        memset(s_known, 0, sz_card_count);
        nvs_set_blob(s_nvs, SZ_NVS_KEY_KNOWN, s_known, sz_card_count);
    }

    ESP_LOGI(TAG, "字卡 %u 张,已认识 %u", (unsigned)sz_card_count,
             (unsigned)sz_known_count());
    return err;
}

bool sz_is_known(uint16_t idx)
{
    if (!s_known || idx >= sz_card_count) return false;
    return s_known[idx] != 0;
}

void sz_set_known(uint16_t idx, bool is_known)
{
    if (!s_known || idx >= sz_card_count) return;
    s_known[idx] = is_known ? 1 : 0;
    if (s_nvs) {
        nvs_set_blob(s_nvs, SZ_NVS_KEY_KNOWN, s_known, sz_card_count);
        nvs_commit(s_nvs);
    }
}

uint16_t sz_known_count(void)
{
    if (!s_known) return 0;
    uint16_t n = 0;
    for (uint16_t i = 0; i < sz_card_count; i++) {
        if (s_known[i]) n++;
    }
    return n;
}
