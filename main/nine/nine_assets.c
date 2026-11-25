#include "nine_assets.h"
#include "nine_crc.h"

#include <string.h>

#ifndef NINE_ASSETS_HOST
#include "esp_log.h"
#include "esp_partition.h"
static const char *TAG = "nine_assets";
static esp_partition_mmap_handle_t s_handle;
static const esp_partition_t *s_partition;
#endif

#define CONTAINER_MAGIC "NINECNT\0"
#define CONTAINER_VERSION 1u
#define CONTAINER_HEADER 64u
#define CONTAINER_CRC_OFFSET 32u

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

bool nine_assets_parse(const uint8_t *data, size_t size, nine_assets_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->error = "资源包缺失";
    if (!data || size < CONTAINER_HEADER) {
        out->error = "资源包过短";
        return false;
    }
    if (memcmp(data, CONTAINER_MAGIC, 8) || rd32(data + 8) != CONTAINER_VERSION) {
        out->error = "资源包版本不认识";
        return false;
    }
    const uint32_t total = rd32(data + 12);
    const uint32_t script_off = rd32(data + 16);
    const uint32_t script_size = rd32(data + 20);
    const uint32_t images_off = rd32(data + 24);
    const uint32_t images_size = rd32(data + 28);
    if (total != size) {
        out->error = "资源包长度不符";
        return false;
    }
    if (script_off < CONTAINER_HEADER || script_size == 0 ||
        script_off > total || script_size > total - script_off ||
        images_off < CONTAINER_HEADER || images_size == 0 ||
        images_off > total || images_size > total - images_off ||
        (uint64_t)script_off + script_size > images_off) {
        out->error = "资源包布局错误";
        return false;
    }
    if (nine_crc32_zero(data, size, CONTAINER_CRC_OFFSET, 4) != rd32(data + CONTAINER_CRC_OFFSET)) {
        out->error = "资源包校验失败";
        return false;
    }
    if (memcmp(data + script_off, "NINESC1\0", 8) ||
        memcmp(data + images_off, "NINEIM1\0", 8)) {
        out->error = "剧情/图像包标识错误";
        return false;
    }
    out->container = data;
    out->container_size = size;
    out->script = data + script_off;
    out->script_size = script_size;
    out->images = data + images_off;
    out->images_size = images_size;
    out->error = NULL;
    return true;
}

#ifndef NINE_ASSETS_HOST
bool nine_assets_map(nine_assets_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->error = "找不到资源分区";
    if (s_handle) {
        out->error = "资源分区已映射";
        return false;
    }
    s_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x06, "assets");
    if (!s_partition) s_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "assets");
    if (!s_partition || s_partition->size < CONTAINER_HEADER) {
        out->error = "找不到资源分区";
        return false;
    }
    uint8_t header[CONTAINER_HEADER];
    if (esp_partition_read(s_partition, 0, header, sizeof(header)) != ESP_OK) {
        out->error = "资源分区读取失败";
        return false;
    }
    const uint32_t total = rd32(header + 12);
    if (total < CONTAINER_HEADER || total > s_partition->size) {
        out->error = "资源分区长度不符";
        return false;
    }
    const void *mapped = NULL;
    if (esp_partition_mmap(s_partition, 0, total, ESP_PARTITION_MMAP_DATA,
                           &mapped, &s_handle) != ESP_OK || !mapped) {
        s_handle = 0;
        out->error = "资源分区映射失败";
        return false;
    }
    if (!nine_assets_parse((const uint8_t *)mapped, total, out)) {
        ESP_LOGE(TAG, "资源包校验失败: %s", out->error);
        esp_partition_munmap(s_handle);
        s_handle = 0;
        memset(out, 0, sizeof(*out));
        out->error = "资源包校验失败";
        return false;
    }
    ESP_LOGI(TAG, "资源包 %u 字节已映射", (unsigned)total);
    return true;
}

void nine_assets_unmap(nine_assets_t *out)
{
    if (s_handle) {
        esp_partition_munmap(s_handle);
        s_handle = 0;
    }
    s_partition = NULL;
    if (out) memset(out, 0, sizeof(*out));
}
#endif
