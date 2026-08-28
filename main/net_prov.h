// main/net_prov.h —— SoftAP 配网 Web 服务。
// 设备开启配网热点后,手机浏览器访问热点 IP(http://192.168.4.1)填 SSID/密码,
// 服务把凭证写入 NVS,随后由调用方重启并转 STA 联网。
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 启动配网 Web 服务(httpd,绑定 192.168.4.1:80)。
// 提供 GET /(配网表单) 和 POST /wifi(提交 SSID/密码)。
esp_err_t net_prov_start(void);

// 停止配网服务(配网成功后调用)。
void net_prov_stop(void);

#ifdef __cplusplus
}
#endif
