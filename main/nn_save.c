// main/nn_save.c —— NVS 持久化：5 个手动存档 + 1 个自动存档 + 观看记录 + 设置。
//
// 关键设计（与“绝不动别人的数据”一致）：
//   * NVS 初始化失败（NO_FREE_PAGES / NEW_VERSION_FOUND / 其它错误）时**绝不擦除**。
//     NVS 分区是整机共享的，`nvs_flash_erase()` 会连带删掉别的应用/旧游戏留下的
//     namespace。这里只做**降级**：本次运行照常玩，存档/设置写不进去，并向界面报
//     “存档不可用”。要修复请由人在菜单里清空，或由维护者显式决定重建 NVS。
//   * 存档记录走 nn_save_codec 的定长编码（魔数 + 版本 + 载荷长度 + 强制 NUL 结尾），
//     坏档在读取时被丢弃，而不是拿乱码去恢复剧情。
#include "nn_save.h"

#include "nn_save_codec.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "nn_save";
static const char *NS = "nn";

static nn_store_state_t s_state = NN_STORE_UNAVAILABLE;
static const char *s_reason = "尚未初始化";
static uint16_t s_cg_mask;
static uint8_t s_end_mask;

nn_store_state_t nn_store_init(void)
{
    // 注意：这里不调用 nvs_flash_erase()。NVS 是整机共享分区，擦除会带走其它
    // namespace；损坏时只降级并在日志里说清楚。
    const esp_err_t err = nvs_flash_init();
    if (err == ESP_OK) {
        s_state = NN_STORE_OK;
        s_reason = "ok";
    } else if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
        s_state = NN_STORE_BROKEN;
        s_reason = "NVS 分区没有空闲页（需要人工处理，未自动擦除）";
    } else if (err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        s_state = NN_STORE_BROKEN;
        s_reason = "NVS 是新版本格式（需要人工处理，未自动擦除）";
    } else {
        s_state = NN_STORE_UNAVAILABLE;
        s_reason = esp_err_to_name(err);
    }
    if (s_state != NN_STORE_OK) {
        ESP_LOGE(TAG, "NVS 不可用(%s): %s —— 本次运行可继续阅读，但存档与设置不会保留",
                 esp_err_to_name(err), s_reason);
        return s_state;
    }

    // 解锁进度：两个小整数，读不到就当 0（不影响可玩性）。
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_u16(h, "cg", &s_cg_mask);
        uint8_t e = 0;
        if (nvs_get_u8(h, "end", &e) == ESP_OK) s_end_mask = e;
        nvs_close(h);
    }
    ESP_LOGI(TAG, "NVS 就绪：解锁位 cg=0x%04x end=0x%02x", (unsigned)s_cg_mask, (unsigned)s_end_mask);
    return NN_STORE_OK;
}

nn_store_state_t nn_store_state(void)
{
    return s_state;
}

const char *nn_store_reason(void)
{
    return s_reason;
}

bool nn_store_available(void)
{
    return s_state == NN_STORE_OK;
}

static bool open_rw(nvs_handle_t *h)
{
    if (s_state != NN_STORE_OK) return false;
    const esp_err_t err = nvs_open(NS, NVS_READWRITE, h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "打开 NVS 失败: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static void slot_key(uint8_t slot, char *out, size_t n)
{
    if (slot == NN_SLOT_AUTO) {
        snprintf(out, n, "auto");
    } else {
        snprintf(out, n, "slot%u", (unsigned)slot);
    }
}

bool nn_save_write(uint8_t slot, const nn_save_t *save)
{
    if (!save || slot >= NN_SLOT_COUNT) return false;
    nvs_handle_t h;
    if (!open_rw(&h)) return false;
    uint8_t rec[NN_SAVE_RECORD_BYTES];
    size_t len = 0;
    if (!nn_save_encode(save, rec, sizeof(rec), &len)) {
        nvs_close(h);
        return false;
    }
    char key[12];
    slot_key(slot, key, sizeof(key));
    esp_err_t err = nvs_set_blob(h, key, rec, len);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGW(TAG, "写存档 %s 失败: %s", key, esp_err_to_name(err));
    return err == ESP_OK;
}

bool nn_save_read(uint8_t slot, nn_save_t *out)
{
    if (!out || slot >= NN_SLOT_COUNT) return false;
    if (s_state != NN_STORE_OK) return false;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t rec[NN_SAVE_RECORD_BYTES + 8];   // 多留一点，长度不符时能识别出来
    size_t len = sizeof(rec);
    char key[12];
    slot_key(slot, key, sizeof(key));
    const esp_err_t err = nvs_get_blob(h, key, &rec, &len);
    nvs_close(h);
    if (err != ESP_OK) return false;
    if (!nn_save_decode(rec, len, out)) {
        ESP_LOGW(TAG, "丢弃不兼容/损坏的存档 %s (len=%u)", key, (unsigned)len);
        return false;
    }
    return true;
}

bool nn_save_exists(uint8_t slot)
{
    nn_save_t tmp;
    return nn_save_read(slot, &tmp);
}

void nn_save_erase(uint8_t slot)
{
    if (slot >= NN_SLOT_COUNT) return;
    nvs_handle_t h;
    if (!open_rw(&h)) return;
    char key[12];
    slot_key(slot, key, sizeof(key));
    (void)nvs_erase_key(h, key);
    (void)nvs_commit(h);
    nvs_close(h);
}

uint16_t nn_unlock_cg_mask(void) { return s_cg_mask; }
uint8_t nn_unlock_ending_mask(void) { return s_end_mask; }

static void persist_unlocks(void)
{
    nvs_handle_t h;
    if (!open_rw(&h)) return;
    (void)nvs_set_u16(h, "cg", s_cg_mask);
    (void)nvs_set_u8(h, "end", s_end_mask);
    (void)nvs_commit(h);
    nvs_close(h);
}

void nn_unlock_cg(uint8_t cg_index)
{
    if (cg_index >= 16) return;
    const uint16_t bit = (uint16_t)(1u << cg_index);
    if (s_cg_mask & bit) return;
    s_cg_mask |= bit;
    persist_unlocks();
}

void nn_unlock_ending(uint8_t ending_id)
{
    if (ending_id >= 8) return;
    const uint8_t bit = (uint8_t)(1u << ending_id);
    if (s_end_mask & bit) return;
    s_end_mask |= bit;
    persist_unlocks();
}

void nn_unlock_clear(void)
{
    s_cg_mask = 0;
    s_end_mask = 0;
    persist_unlocks();
}

void nn_settings_load(nn_settings_t *out)
{
    if (!out) return;
    out->text_speed = 1;
    out->font_large = 0;
    out->auto_play = 0;
    out->panel_opa = 70;
    if (s_state != NN_STORE_OK) return;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v = 0;
    if (nvs_get_u8(h, "speed", &v) == ESP_OK && v <= 3) out->text_speed = v;
    if (nvs_get_u8(h, "bigfont", &v) == ESP_OK && v <= 1) out->font_large = v;
    if (nvs_get_u8(h, "auto", &v) == ESP_OK && v <= 1) out->auto_play = v;
    if (nvs_get_u8(h, "opa", &v) == ESP_OK && v >= 10 && v <= 70) out->panel_opa = v;
    nvs_close(h);
}

void nn_settings_store(const nn_settings_t *in)
{
    if (!in) return;
    nvs_handle_t h;
    if (!open_rw(&h)) return;
    (void)nvs_set_u8(h, "speed", in->text_speed);
    (void)nvs_set_u8(h, "bigfont", in->font_large);
    (void)nvs_set_u8(h, "auto", in->auto_play);
    (void)nvs_set_u8(h, "opa", in->panel_opa);
    (void)nvs_commit(h);
    nvs_close(h);
}
