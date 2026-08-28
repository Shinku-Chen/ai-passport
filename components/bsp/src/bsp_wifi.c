// components/bsp/src/bsp_wifi.c —— WiFi STA 联网实现。
// 设计要点:
//   - 复用 demo_radio 的 nvs/netif 准备逻辑(避免重复初始化)。
//   - 固定 SSID 直连;凭证缺省时从 NVS 读,方便后续换成 SoftAP 配网写入。
//   - 事件回调跑在默认 event loop,只更新状态缓存/通知,绝不阻塞。
#include "bsp_wifi.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "nvs_flash.h"
#include "esp_wifi_types.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "bsp_wifi";

// NVS 保存凭证的键名(供后续配网写入)。
#define BSP_WIFI_NVS_NAMESPACE   "bsp_wifi"
#define BSP_WIFI_NVS_KEY_SSID    "ssid"
#define BSP_WIFI_NVS_KEY_PASS    "pass"

static bool s_wifi_initialized;
static bool s_wifi_started;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static char s_ip_str[16];
static char s_ap_ip_str[16];
static char s_ssid[33];
static char s_pass[65];
static volatile bsp_wifi_state_t s_state = BSP_WIFI_IDLE;
static bsp_wifi_evt_cb_t s_evt_cb;
static void *s_evt_user;

static void set_state(bsp_wifi_state_t st)
{
    if (s_state != st) {
        s_state = st;
        if (s_evt_cb) s_evt_cb(st, s_evt_user);
    }
}

// 从 NVS 读默认凭证;没有则返回 ESP_ERR_NOT_FOUND(不阻塞,调用方可用空配置)。
static esp_err_t load_credentials(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(BSP_WIFI_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    size_t len = sizeof(s_ssid);
    err = nvs_get_str(handle, BSP_WIFI_NVS_KEY_SSID, s_ssid, &len);
    if (err != ESP_OK) { nvs_close(handle); return err; }

    len = sizeof(s_pass);
    err = nvs_get_str(handle, BSP_WIFI_NVS_KEY_PASS, s_pass, &len);
    nvs_close(handle);
    return err;
}

// 用传入配置或 NVS 凭证填充 s_ssid/s_pass。
static void resolve_credentials(const bsp_wifi_config_t *config)
{
    s_ssid[0] = '\0';
    s_pass[0] = '\0';

    if (config && config->ssid && config->ssid[0]) {
        strlcpy(s_ssid, config->ssid, sizeof(s_ssid));
        if (config->password) strlcpy(s_pass, config->password, sizeof(s_pass));
        return;
    }
    if (load_credentials() != ESP_OK) {
        ESP_LOGW(TAG, "无可用凭证,保持未连接;请通过配网接口写入");
    }
}

// ---- WIFI_EVENT 处理 ----
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;

    switch (id) {
    case WIFI_EVENT_STA_START:
        if (s_ssid[0]) {
            ESP_LOGI(TAG, "开始连接 %s", s_ssid);
            set_state(BSP_WIFI_CONNECTING);
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "未配置 SSID,跳过连接");
            set_state(BSP_WIFI_FAILED);
        }
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        ESP_LOGW(TAG, "Wi-Fi 断开: %s", esp_err_to_name((esp_err_t)data));
        set_state(BSP_WIFI_DISCONNECTED);
        // 短暂延迟后重连(200ms,避免协议风暴)。已成功过则尽量重连。
        esp_wifi_connect();
        break;
    default:
        break;
    }
}

// ---- IP_EVENT 处理 ----
static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&evt->ip_info.ip));
        ESP_LOGI(TAG, "已获 IP: %s", s_ip_str);
        set_state(BSP_WIFI_CONNECTED);
    }
}

esp_err_t bsp_wifi_init(const bsp_wifi_config_t *config)
{
    resolve_credentials(config);

    // NVS / netif / event loop —— 复用广播层现有逻辑。
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // 不自动擦除(避免破坏用户数据),返回失败让应用决定。
        ESP_LOGE(TAG, "NVS 需要擦除,未自动处理: %s", esp_err_to_name(err));
        return err;
    }
    if (err != ESP_OK) return err;

    err = esp_netif_init();
    if (err != ESP_OK) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    if (s_wifi_initialized) {
        // 已初始化过:仅切状态 + 重连。
        if (s_wifi_started) esp_wifi_start();
        return ESP_OK;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (!s_sta_netif) return ESP_ERR_NO_MEM;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) return err;
    s_wifi_initialized = true;

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              on_wifi_event, NULL, NULL);
    if (err != ESP_OK) return err;
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              on_ip_event, NULL, NULL);
    if (err != ESP_OK) return err;

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) return err;

    // 触发配网:让 STA 连上 s_ssid。
    wifi_config_t wifi_cfg = {0};
    strlcpy((char *)wifi_cfg.sta.ssid, s_ssid, sizeof(wifi_cfg.sta.ssid));
    if (s_pass[0]) strlcpy((char *)wifi_cfg.sta.password, s_pass, sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;  // 最低允许开放网络
    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (err != ESP_OK) return err;

    err = esp_wifi_start();
    if (err != ESP_OK) return err;
    s_wifi_started = true;

    set_state(BSP_WIFI_CONNECTING);
    return ESP_OK;
}

void bsp_wifi_set_evt_cb(bsp_wifi_evt_cb_t cb, void *user)
{
    s_evt_cb = cb;
    s_evt_user = user;
}

bsp_wifi_state_t bsp_wifi_get_state(void)
{
    return s_state;
}

const char *bsp_wifi_get_ip_str(void)
{
    return s_ip_str;
}

esp_err_t bsp_wifi_reconnect(void)
{
    if (!s_wifi_started) return ESP_ERR_INVALID_STATE;
    set_state(BSP_WIFI_CONNECTING);
    return esp_wifi_connect();
}

void bsp_wifi_deinit(void)
{
    if (s_wifi_started) {
        esp_wifi_stop();
        s_wifi_started = false;
    }
    if (s_wifi_initialized) {
        esp_wifi_deinit();
        s_wifi_initialized = false;
    }
    if (s_sta_netif) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }
    if (s_ap_netif) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }
    set_state(BSP_WIFI_IDLE);
}

/* ─────────────── SoftAP 配网 ─────────────── */

bool bsp_wifi_has_credentials(void)
{
    nvs_handle_t handle;
    if (nvs_open(BSP_WIFI_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    nvs_close(handle);
    // 二次确认:ssid 非空。
    char tmp[33] = {0};
    size_t len = sizeof(tmp);
    if (nvs_open(BSP_WIFI_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;
    bool ok = (nvs_get_str(handle, BSP_WIFI_NVS_KEY_SSID, tmp, &len) == ESP_OK && tmp[0]);
    nvs_close(handle);
    return ok;
}

esp_err_t bsp_wifi_save_credentials(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(BSP_WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_str(handle, BSP_WIFI_NVS_KEY_SSID, ssid);
    if (err == ESP_OK) err = nvs_set_str(handle, BSP_WIFI_NVS_KEY_PASS,
                                         password ? password : "");
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

const char *bsp_wifi_get_ap_ip(void)
{
    return s_ap_ip_str;
}

esp_err_t bsp_wifi_start_ap(const char *ssid, const char *password)
{
    if (s_wifi_initialized && s_wifi_started) {
        // 已启动过:切到 APSTA。
    }
    esp_wifi_set_mode(WIFI_MODE_APSTA);

    // 创建 AP netif(避免重复)。
    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }
    if (!s_ap_netif) return ESP_ERR_NO_MEM;

    wifi_config_t ap_cfg = {0};
    strlcpy((char *)ap_cfg.ap.ssid, ssid ? ssid : "AI-Passport-Prov",
            sizeof(ap_cfg.ap.ssid));
    if (password && password[0]) {
        const char *p = password;
        size_t len;
        if ((len = strlen(password)) >= 8) {
            strlcpy((char *)ap_cfg.ap.password, p, sizeof(ap_cfg.ap.password));
            ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
        } else {
            ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
        }
    } else {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.channel = 6;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) return err;
    err = esp_wifi_start();
    if (err != ESP_OK) return err;
    s_wifi_started = true;

    // 软AP IP 默认 192.168.4.1(由 esp_netif 分配)。
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_ap_netif, &ip) == ESP_OK) {
        snprintf(s_ap_ip_str, sizeof(s_ap_ip_str), IPSTR, IP2STR(&ip.ip));
    } else {
        strlcpy(s_ap_ip_str, "192.168.4.1", sizeof(s_ap_ip_str));
    }
    ESP_LOGI(TAG, "SoftAP 启动: ssid=%s 密码=%s IP=%s(手机浏览器访问配网)",
             ap_cfg.ap.ssid,
             ap_cfg.ap.authmode == WIFI_AUTH_OPEN ? "(开放)" : "***",
             s_ap_ip_str);
    return ESP_OK;
}

esp_err_t bsp_wifi_stop_ap(void)
{
    // 转回纯 STA 模式,重启后走 bsp_wifi_init 联网。
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) return err;
    if (s_ap_netif) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }
    s_ap_ip_str[0] = '\0';
    ESP_LOGI(TAG, "SoftAP 已关闭,转纯 STA");
    return ESP_OK;
}
