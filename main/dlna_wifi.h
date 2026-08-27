// main/dlna_wifi.h —— DLNA 应用所需的 WiFi 连接 + 配网。
// 与 main/demo_wifi.c(只扫描)不同,这里做"连上已知 SSID,连接失败启动 AP 配网"。
// 凭证断电保存在 NVS。
#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 连接结果。
typedef enum {
    DLNA_WIFI_OK = 0,     // 已作为 STA 连接成功
    DLNA_WIFI_FAIL,       // 已重试仍无法连接(未开 AP)
    DLNA_WIFI_AP,         // 已知 SSID 连不上 → 已转 AP 配网模式
} dlna_wifi_result_t;

// 初始化 netif + 事件循环(NVS/netif 准备)。调用一次。
esp_err_t dlna_wifi_init(void);

// 连接到已知 SSID(从 NVS 读,若没有则直接开 AP)。
// 阻塞直到结果确定(成功 / 失败 / 转 AP)。
dlna_wifi_result_t dlna_wifi_connect(void);

// 启动 softAP 配网热点。用于无凭证或 STA 连不上时,让用户连 AP 后配网。
esp_err_t dlna_wifi_start_ap(void);

// 保存凭证到 NVS。配网页提交成功后调用,之后可调用 dlna_wifi_restart()。
esp_err_t dlna_wifi_save_credentials(const char *ssid, const char *pass);

// 清除已保存的网络凭证(NVS)。用于"长按 OK 重置网络"——清掉后重启会回到 AP 配网。
esp_err_t dlna_wifi_clear_credentials(void);

// 保存凭证后重启设备(使新凭证生效)。带 1s 延迟让用户看到提示。
void dlna_wifi_restart(void);

// 查询当前是否处于 AP 配网模式。
bool dlna_wifi_is_ap_mode(void);

// 取 AP 配网热点的 SSID 与密码(写入调用方缓冲;非 AP 模式返回空)。
void dlna_wifi_ap_credentials(char *ssid, size_t ssid_max,
                              char *pass, size_t pass_max);

// 取当前热点密码(供配网页显示);非 AP 模式返回空串。
const char *dlna_wifi_ap_pass_get(void);

// 扫描附近 WiFi(供配网页)。
// 注意:AP 模式下扫描会短暂切到 STA 再切回 AP(断连几秒)。结果写入调用方,
// 每个 SSID 最多 name_max 字节。返回扫描到的数量(负数=错误)。
int dlna_wifi_scan_networks(char names[][33], int max_networks);

// 开机时预扫一次附近 WiFi 并缓存,供配网页直接展示(不必等切 AP 后再在线扫)。
// 在尝试连接 / 开热点之前调用效果最好(此时射频还没被 AP 占用)。
void dlna_wifi_prefetch_networks(void);

// 读取缓存的预扫 WiFi 列表(配网页用)。返回数量,若未预扫或列表空返回 0。
int dlna_wifi_get_cached_networks(char names[][33], int max_networks);

// 查询是否已连上 STA。
bool dlna_wifi_is_connected(void);

// 获取当前生效的 IP(STA 或 AP 网段)字符串。
const char *dlna_wifi_ip_str(void);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
}
#endif
