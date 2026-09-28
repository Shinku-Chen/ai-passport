// main/oc_settings.c —— 见 oc_settings.h。NVS 命名空间 "oc_settings"。
#include "oc_settings.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "oc_settings";
static const char *NS = "oc_settings";

typedef struct {
    uint8_t volume;
    uint8_t mic_gain_db;
    uint8_t brightness;
    uint16_t backlight_timeout_s;
} oc_settings_t;

static oc_settings_t s_cfg;
static bool s_loaded;

static nvs_handle_t open_ns(void)
{
    nvs_handle_t h = 0;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open(%s) 失败: %s", NS, esp_err_to_name(e));
        return 0;
    }
    return h;
}

static uint8_t read_u8(nvs_handle_t h, const char *key, uint8_t def)
{
    uint8_t v = def;
    if (h == 0 || nvs_get_u8(h, key, &v) != ESP_OK) {
        return def;
    }
    return v;
}

static uint16_t read_u16(nvs_handle_t h, const char *key, uint16_t def)
{
    uint16_t v = def;
    if (h == 0 || nvs_get_u16(h, key, &v) != ESP_OK) {
        return def;
    }
    return v;
}

esp_err_t oc_settings_load(void)
{
    // nvs_flash_init 幂等;即使 oc_link 已经初始化过,这里重复调用也无害,
    // 这样本模块不依赖"谁先启动"。
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs = nvs_flash_init();
    }

    nvs_handle_t h = (nvs == ESP_OK) ? open_ns() : 0;
    s_cfg.volume = read_u8(h, "vol", OC_SETTINGS_VOLUME_DEFAULT);
    s_cfg.mic_gain_db = read_u8(h, "mic", OC_SETTINGS_MIC_GAIN_DEFAULT);
    s_cfg.brightness = read_u8(h, "bright", OC_SETTINGS_BRIGHTNESS_DEFAULT);
    s_cfg.backlight_timeout_s = read_u16(h, "bl_to", OC_SETTINGS_BACKLIGHT_TIMEOUT_DEFAULT);
    if (h != 0) {
        nvs_close(h);
    }

    // 值域兜底:NVS 里可能是旧固件写入的越界值。
    if (s_cfg.volume > 100) s_cfg.volume = OC_SETTINGS_VOLUME_DEFAULT;
    if (s_cfg.mic_gain_db > 32) s_cfg.mic_gain_db = OC_SETTINGS_MIC_GAIN_DEFAULT;
    if (s_cfg.brightness > 100) s_cfg.brightness = OC_SETTINGS_BRIGHTNESS_DEFAULT;
    if (s_cfg.backlight_timeout_s == 0) s_cfg.backlight_timeout_s = OC_SETTINGS_BACKLIGHT_TIMEOUT_DEFAULT;

    s_loaded = true;
    ESP_LOGI(TAG, "设置: 音量=%u 麦克风=%udB 亮度=%u 背光超时=%us", s_cfg.volume, s_cfg.mic_gain_db,
             s_cfg.brightness, (unsigned)s_cfg.backlight_timeout_s);
    return ESP_OK;
}

uint8_t oc_settings_volume(void)
{
    return s_loaded ? s_cfg.volume : OC_SETTINGS_VOLUME_DEFAULT;
}

uint8_t oc_settings_mic_gain_db(void)
{
    return s_loaded ? s_cfg.mic_gain_db : OC_SETTINGS_MIC_GAIN_DEFAULT;
}

uint8_t oc_settings_brightness(void)
{
    return s_loaded ? s_cfg.brightness : OC_SETTINGS_BRIGHTNESS_DEFAULT;
}

uint16_t oc_settings_backlight_timeout_s(void)
{
    return s_loaded ? s_cfg.backlight_timeout_s : OC_SETTINGS_BACKLIGHT_TIMEOUT_DEFAULT;
}

esp_err_t oc_settings_set_volume(uint8_t percent)
{
    s_cfg.volume = percent > 100 ? 100 : percent;
    s_loaded = true;
    nvs_handle_t h = open_ns();
    if (h == 0) return ESP_FAIL;
    esp_err_t e = nvs_set_u8(h, "vol", s_cfg.volume);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

esp_err_t oc_settings_set_mic_gain_db(uint8_t db)
{
    s_cfg.mic_gain_db = db > 32 ? 32 : db;
    s_loaded = true;
    nvs_handle_t h = open_ns();
    if (h == 0) return ESP_FAIL;
    esp_err_t e = nvs_set_u8(h, "mic", s_cfg.mic_gain_db);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

esp_err_t oc_settings_set_brightness(uint8_t percent)
{
    s_cfg.brightness = percent > 100 ? 100 : percent;
    s_loaded = true;
    nvs_handle_t h = open_ns();
    if (h == 0) return ESP_FAIL;
    esp_err_t e = nvs_set_u8(h, "bright", s_cfg.brightness);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

esp_err_t oc_settings_set_backlight_timeout_s(uint16_t seconds)
{
    s_cfg.backlight_timeout_s = seconds == 0 ? OC_SETTINGS_BACKLIGHT_TIMEOUT_DEFAULT : seconds;
    s_loaded = true;
    nvs_handle_t h = open_ns();
    if (h == 0) return ESP_FAIL;
    esp_err_t e = nvs_set_u16(h, "bl_to", s_cfg.backlight_timeout_s);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}
