// components/bsp/include/bsp_wifi.h
// FoloToy AI Passport 的 WiFi STA 联网模块。
// 固定 SSID 直连 + 预留 SoftAP 配网接口。所有网络回调都在 event loop 任务里,
// 回调里禁止阻塞或做重活(如 lvgl 操作),需要处理 UI 时派到应用任务。
#pragma once

#include "esp_err.h"
#include "esp_netif.h"
#include "esp_wifi_types.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 连接状态,供应用层/UI 显示。
typedef enum {
    BSP_WIFI_IDLE = 0,        // 未开始
    BSP_WIFI_CONNECTING,      // 正在连接
    BSP_WIFI_CONNECTED,       // 已连上(拿到 IP)
    BSP_WIFI_DISCONNECTED,    // 断开(可重试)
    BSP_WIFI_FAILED,          // 失败(凭证错误/不可达)
} bsp_wifi_state_t;

// 配网/连接回调。connected=true 表示已拿到 IP;state 提供细粒度状态。
typedef void (*bsp_wifi_evt_cb_t)(bsp_wifi_state_t state, void *user);

// 配网(连接)配置。SSID/密码为空串时表示"使用 NVS 默认值"。
typedef struct {
    const char *ssid;      // 要连接的 AP 名
    const char *password;  // 密码,开放网络传 NULL
    uint32_t    max_retry; // 最大重试次数(0 = 只尝试一次)
} bsp_wifi_config_t;

// 初始化 WiFi(STA),进入固定 SSID 连接流程。
// config 传 NULL 时使用 NVS 保存的 SSID/密码。
// 重复调用是幂等的;首次会初始化 netif/event loop/wifi 驱动。
esp_err_t bsp_wifi_init(const bsp_wifi_config_t *config);

// 注册连接状态回调(可多次调用,续接在同一 event loop 线程)。
void bsp_wifi_set_evt_cb(bsp_wifi_evt_cb_t cb, void *user);

// 获取当前连接状态。
bsp_wifi_state_t bsp_wifi_get_state(void);

// 获取当前 IP 地址字符串(未连上时为空串)。
const char *bsp_wifi_get_ip_str(void);

// 是否已连接上 AP(拿到 IP)。
bool bsp_wifi_is_connected(void);

// 获取当前连接的 SSID 名(未连/空则返回空串)。
const char *bsp_wifi_get_ssid(void);

// 获取网关 IP 字符串(未连上时为空串)。
const char *bsp_wifi_get_gateway(void);

// 获取当前连接 AP 的信号强度(dBm);未连上返回 0。
int bsp_wifi_get_current_rssi(void);

// 手动触发重连(断开后由应用调用)。
esp_err_t bsp_wifi_reconnect(void);

// 停止并释放 WiFi。
void bsp_wifi_deinit(void);

// ---- 扫描接口 ----

// 扫描单个 SSID 信息。
typedef struct {
    char ssid[32];    // SSID 名
    int  rssi;        // 信号强度 (dBm)
} bsp_wifi_ap_t;

// 扫描附近 2.4GHz AP。结果存到内部缓冲,供配网页预填列表。
// 返回扫描到的 AP 数量;失败返回负值。
int bsp_wifi_scan(void);

// 获取最近一次扫描的结果。返回结果数量,结果写入 ap 数组(按 rssi 降序)。
// 返回 -1 表示尚未扫描。
int bsp_wifi_get_scan_results(bsp_wifi_ap_t *ap, int max_count);

// ---- SoftAP 配网接口 ----

// 是否已保存过配网凭证(NVS 里有 SSID)。
bool bsp_wifi_has_credentials(void);

// 保存配网凭证到 NVS(配网成功后由应用调用)。失败返回 esp_err。
esp_err_t bsp_wifi_save_credentials(const char *ssid, const char *password);

// 开启 SoftAP 配网热点(APSTA 模式)。设备开热点后手机浏览器访问
// 热点 IP(默认 192.168.4.1)填 SSID/密码。ssid 缺省用 "AI-Passport-Prov"。
// 返回热点 IP 字符串(经 bsp_wifi_get_ap_ip 获取)。
esp_err_t bsp_wifi_start_ap(const char *ssid, const char *password);

// 获取 SoftAP 热点 IP 字符串(未开启时为空)。
const char *bsp_wifi_get_ap_ip(void);

// 关闭 SoftAP,转为纯 STA(配网完成后调用,随后 bsp_wifi_init 会连 STA)。
esp_err_t bsp_wifi_stop_ap(void);

// 清除已保存的配网凭证(长按 OK 清除配置后重启用)。
void bsp_wifi_clear_credentials(void);

// 手动设置 STA 凭证(覆盖 NVS 读取值,用于配网后立即连接)。
void bsp_wifi_set_credentials(const char *ssid, const char *password);

// 阻塞等待 STA 连接结果。ssid/pass 提供则用它们连接,否则用 bsp_wifi_init
// 已解析的凭证(NVS)。timeout_ms 内拿到 IP 返回 ESP_OK;超时/失败返回 ESP_ERR_TIMEOUT
// 或其他错误。用于开机「尝试连已存 WiFi」,让上层决定连不上就开配网。
esp_err_t bsp_wifi_connect_sta(const char *ssid, const char *password,
                               uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
