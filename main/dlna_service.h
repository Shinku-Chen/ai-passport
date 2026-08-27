// main/dlna_service.h —— DLNA/UPnP 服务器对外接口。
// 实现三件事:
//   · SSDP 发现(组播 239.255.255.250:1900,应答 M-SEARCH / 周期发送 NOTIFY)
//   · HTTP 服务(esp_http_server):/dlna/description.xml + AVTransport / RenderingControl / ConnectionManager SOAP
// 启动后手机(网易云/QQ 等支持 DLNA 投屏的 App)可发现本设备,并将音频 URL 通过
// SetAVTransportURI 推给我们。
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 启动 DLNA 服务(HTTP server + SSDP)。需先连上 WiFi(拿到同网段 IP)。
// 同一函数可重复调用(幂等)。成功后设备会在局域网广播存在。
esp_err_t dlna_service_start(void);

// 停止 DLNA 服务(HTTP server + SSDP)。用于退出演示页时释放网络资源。
void dlna_service_stop(void);

#ifdef __cplusplus
}
#endif
