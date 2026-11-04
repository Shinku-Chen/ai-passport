#include <stdlib.h>
// tests/app_stubs/stubs.c —— NVS / JPEG / BSP / 时间的替身实现。
//
// 三处失败注入点是本轮回归的核心:
//   eden_stub_nvs_fail_set / _fail_commit   -> 存档写入失败(真 NVS 适配器路径)
//   eden_stub_jpeg_fail_next / _fail_all    -> 画面解码失败(真 eden_image_draw 路径)
//   eden_stub_button_soc                    -> 电量显示
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "jpeg_decoder.h"
#include "lvgl.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------- 时间
static int64_t s_now_us;

int64_t esp_timer_get_time(void)
{
    return s_now_us;
}

void eden_stub_advance_us(int64_t delta)
{
    s_now_us += delta;
}

// ---------------------------------------------------------------- BSP
static int s_landscape;
static int s_backlight = 100;

void bsp_display_backlight(uint8_t percent)
{
    s_backlight = percent;
}

int eden_stub_backlight(void)
{
    return s_backlight;
}

bool bsp_lvgl_lock(int timeout_ms)
{
    (void)timeout_ms;
    eden_stub_set_lock_depth(eden_stub_lock_depth() + 1);
    return true;
}

void bsp_lvgl_unlock(void)
{
    const int depth = eden_stub_lock_depth();
    eden_stub_set_lock_depth(depth > 0 ? depth - 1 : 0);
}

esp_err_t bsp_lvgl_set_landscape(bool landscape)
{
    s_landscape = landscape ? 1 : 0;
    return ESP_OK;
}

int eden_stub_landscape(void)
{
    return s_landscape;
}

static int s_battery = 88;

int bsp_battery_soc(void)
{
    return s_battery;
}

void eden_stub_set_battery(int percent)
{
    s_battery = percent;
}

esp_err_t bsp_button_init(bsp_btn_cb_t cb, void *user)
{
    (void)cb;
    (void)user;
    return ESP_OK;
}

esp_err_t bsp_button_prepare_deep_sleep(int *level)
{
    if (level != NULL) {
        *level = 1;
    }
    return ESP_OK;
}

int bsp_button_read_mv(void)
{
    return 3300;
}

const char *esp_err_to_name(esp_err_t err)
{
    switch (err) {
    case ESP_OK: return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    case ESP_ERR_NVS_NO_FREE_PAGES: return "ESP_ERR_NVS_NO_FREE_PAGES";
    case ESP_ERR_NVS_NEW_VERSION_FOUND: return "ESP_ERR_NVS_NEW_VERSION_FOUND";
    default: return "ESP_ERR";
    }
}

// ---------------------------------------------------------------- NVS
// 语义与真 NVS 对齐:**set 只进待提交区,commit 成功才落到已提交区**。
// 这样"commit 失败"才真的等于"flash 里还是旧记录",而不是 RAM 里已经被覆盖。
#define NVS_MAX_ENTRIES 12
#define NVS_MAX_BLOB 1024

typedef struct {
    char ns[16];
    char key[16];
    uint8_t data[NVS_MAX_BLOB];          // 已提交(可读)
    size_t size;
    bool present;
    uint8_t pending[NVS_MAX_BLOB];       // 待提交
    size_t pending_size;
    bool pending_valid;
} nvs_entry_t;

static nvs_entry_t s_nvs[NVS_MAX_ENTRIES];
static int s_nvs_count;
static bool s_nvs_fail_set;
static bool s_nvs_fail_commit;
static bool s_nvs_init_fails;
static esp_err_t s_nvs_init_error = ESP_FAIL;
static int s_commit_attempts;
static int s_commit_success;
static int s_set_calls;

static const char *nvs_ns_of(nvs_handle_t handle);

esp_err_t nvs_flash_init(void)
{
    return s_nvs_init_fails ? s_nvs_init_error : ESP_OK;
}

esp_err_t nvs_flash_erase(void)
{
    // 门禁里禁止调用它;真被调用就记一笔,便于测试断言"没有整片擦除"。
    return ESP_OK;
}

static nvs_entry_t *nvs_find(const char *ns, const char *key, bool create)
{
    for (int index = 0; index < s_nvs_count; index++) {
        if (strcmp(s_nvs[index].ns, ns) == 0 && strcmp(s_nvs[index].key, key) == 0) {
            return &s_nvs[index];
        }
    }
    if (!create || s_nvs_count >= NVS_MAX_ENTRIES) {
        return NULL;
    }
    nvs_entry_t *entry = &s_nvs[s_nvs_count++];
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->ns, sizeof(entry->ns), "%s", ns);
    snprintf(entry->key, sizeof(entry->key), "%s", key);
    return entry;
}

esp_err_t nvs_open(const char *name, nvs_open_mode_t open_mode, nvs_handle_t *out_handle)
{
    (void)open_mode;
    if (out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (nvs_find(name, "", true) == NULL) {
        return ESP_ERR_NO_MEM;
    }
    for (int index = 0; index < s_nvs_count; index++) {
        if (strcmp(s_nvs[index].ns, name) == 0) {
            *out_handle = (nvs_handle_t)(index + 1);
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

void nvs_close(nvs_handle_t handle)
{
    // 未提交的改动随关闭丢弃(与真 NVS 一致)。
    const char *ns = nvs_ns_of(handle);
    for (int index = 0; index < s_nvs_count; index++) {
        if (strcmp(s_nvs[index].ns, ns) == 0) {
            s_nvs[index].pending_valid = false;
        }
    }
}

static const char *nvs_ns_of(nvs_handle_t handle)
{
    return s_nvs[handle - 1].ns;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value, size_t *length)
{
    nvs_entry_t *entry = nvs_find(nvs_ns_of(handle), key, false);
    if (entry == NULL || !entry->present) {
        return ESP_FAIL;   // 未提交的改动对读不可见
    }
    if (length == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (*length < entry->size) {
        *length = entry->size;
        return ESP_ERR_NO_MEM;
    }
    memcpy(out_value, entry->data, entry->size);
    *length = entry->size;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length)
{
    s_set_calls++;
    if (s_nvs_fail_set) {
        return ESP_FAIL;
    }
    nvs_entry_t *entry = nvs_find(nvs_ns_of(handle), key, true);
    if (entry == NULL || length > NVS_MAX_BLOB) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(entry->pending, value, length);
    entry->pending_size = length;
    entry->pending_valid = true;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    s_commit_attempts++;
    if (s_nvs_fail_commit) {
        // 提交失败:待提交区丢弃,已提交区(flash 里的旧记录)保持不变。
        const char *ns = nvs_ns_of(handle);
        for (int index = 0; index < s_nvs_count; index++) {
            if (strcmp(s_nvs[index].ns, ns) == 0) {
                s_nvs[index].pending_valid = false;
            }
        }
        return ESP_FAIL;
    }
    // 按命名空间提交:NVS 的句柄按命名空间作用,不按条目下标。
    const char *ns = nvs_ns_of(handle);
    int applied = 0;
    for (int index = 0; index < s_nvs_count; index++) {
        if (strcmp(s_nvs[index].ns, ns) == 0 && s_nvs[index].pending_valid) {
            memcpy(s_nvs[index].data, s_nvs[index].pending, s_nvs[index].pending_size);
            s_nvs[index].size = s_nvs[index].pending_size;
            s_nvs[index].present = true;
            s_nvs[index].pending_valid = false;
            applied++;
        }
    }
    if (applied > 0) {
        s_commit_success++;
        return ESP_OK;
    }
    return ESP_OK;
}

// 测试接口:注入 / 观测 / 直接塞入"已在 flash 里"的旧记录
void eden_stub_nvs_reset(void)
{
    memset(s_nvs, 0, sizeof(s_nvs));
    s_nvs_count = 0;
    s_nvs_fail_set = false;
    s_nvs_fail_commit = false;
    s_nvs_init_fails = false;
    s_commit_attempts = 0;
    s_commit_success = 0;
    s_set_calls = 0;
}

void eden_stub_nvs_fail(bool fail_set, bool fail_commit)
{
    s_nvs_fail_set = fail_set;
    s_nvs_fail_commit = fail_commit;
}

void eden_stub_nvs_fail_init(esp_err_t error)
{
    s_nvs_init_fails = true;
    s_nvs_init_error = error;
}

int eden_stub_nvs_commit_attempts(void)
{
    return s_commit_attempts;
}

int eden_stub_nvs_commit_success(void)
{
    return s_commit_success;
}

int eden_stub_nvs_set_calls(void)
{
    return s_set_calls;
}

bool eden_stub_nvs_present(const char *ns, const char *key)
{
    nvs_entry_t *entry = nvs_find(ns, key, false);
    return entry != NULL && entry->present;
}

bool eden_stub_nvs_read(const char *ns, const char *key, void *out, size_t size)
{
    nvs_entry_t *entry = nvs_find(ns, key, false);
    if (entry == NULL || !entry->present || entry->size != size) {
        return false;
    }
    memcpy(out, entry->data, size);
    return true;
}

bool eden_stub_nvs_write_raw(const char *ns, const char *key, const void *data, size_t size)
{
    nvs_entry_t *entry = nvs_find(ns, key, true);
    if (entry == NULL || size > NVS_MAX_BLOB) {
        return false;
    }
    memcpy(entry->data, data, size);
    entry->size = size;
    entry->present = true;
    entry->pending_valid = false;
    return true;
}

// ---------------------------------------------------------------- JPEG
static bool s_jpeg_fail_all;
static uint32_t s_jpeg_fail_len;
static int s_jpeg_calls;

void eden_stub_jpeg_reset(void)
{
    s_jpeg_fail_all = false;
    s_jpeg_fail_len = 0;
    s_jpeg_calls = 0;
}

// fail_len != 0 时,只让"长度等于该值"的图失败(用于只坏某一帧)。
void eden_stub_jpeg_fail(bool all, uint32_t fail_len)
{
    s_jpeg_fail_all = all;
    s_jpeg_fail_len = fail_len;
}

int eden_stub_jpeg_calls(void)
{
    return s_jpeg_calls;
}

// 合成包里的"JPEG"就是一段标记字节:头 4 字节放宽高,便于替身返回正确尺寸。
static void read_header(const uint8_t *indata, uint16_t *w, uint16_t *h)
{
    *w = (uint16_t)(indata[0] | (indata[1] << 8));
    *h = (uint16_t)(indata[2] | (indata[3] << 8));
}

esp_err_t esp_jpeg_get_image_info(esp_jpeg_image_cfg_t *cfg, esp_jpeg_image_output_t *img)
{
    if (cfg == NULL || img == NULL || cfg->indata == NULL || cfg->indata_size < 4) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_jpeg_fail_all) {
        return ESP_FAIL;
    }
    uint16_t w = 0;
    uint16_t h = 0;
    read_header(cfg->indata, &w, &h);
    img->width = w;
    img->height = h;
    img->output_len = (size_t)w * h * 2;
    return ESP_OK;
}

esp_err_t esp_jpeg_decode(esp_jpeg_image_cfg_t *cfg, esp_jpeg_image_output_t *img)
{
    s_jpeg_calls++;
    if (cfg == NULL || img == NULL || cfg->outbuf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_jpeg_fail_all || (s_jpeg_fail_len != 0 && cfg->indata_size == s_jpeg_fail_len)) {
        return ESP_FAIL;
    }
    uint16_t w = 0;
    uint16_t h = 0;
    read_header(cfg->indata, &w, &h);
    if (cfg->outbuf_size < (uint32_t)w * h * 2) {
        return ESP_ERR_NO_MEM;
    }
    // 填充可区分的像素:高字节=宽度低 8 位,低字节=高度低 8 位,便于断言画布内容。
    uint16_t *pixels = (uint16_t *)cfg->outbuf;
    for (uint32_t index = 0; index < (uint32_t)w * h; index++) {
        pixels[index] = (uint16_t)(((w & 0xFF) << 8) | (h & 0xFF));
    }
    img->width = w;
    img->height = h;
    img->output_len = (size_t)w * h * 2;
    return ESP_OK;
}

// ---------------------------------------------------------------- 堆桩
void *heap_caps_malloc(size_t size, uint32_t caps)
{
    (void)caps;
    return malloc(size);
}
void heap_caps_free(void *ptr) { free(ptr); }
size_t heap_caps_get_free_size(uint32_t caps) { (void)caps; return 300000; }
size_t heap_caps_get_largest_free_block(uint32_t caps) { (void)caps; return 200000; }

// 宿主桩：分带刷新的延时。
void vTaskDelay(uint32_t ticks) { (void)ticks; }
