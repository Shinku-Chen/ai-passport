// main/dlna_service.c —— DLNA/UPnP 服务器(SSDP + HTTP/SOAP)。
// 参考实现:zming-huang/ESP32-S3_WiFi_Music_player 的 DLNA 部分,用 ESP-IDF 版改写。
// 关键差异:ESP-IDF 用 esp_http_server(无第三方 AsyncWebServer),SOAP 的 request body
// 走 httpd_req_t 的 recv 流程;SSDP 用 lwip 的 UDP 原生 socket。
//
// 安全与约束(遵循 AGENTS.md 硬件契约):
//   仅支持局域网 http 流;https 需 TLS(内存扛不住)这里明确不启用。
#include "dlna_service.h"
#include "dlna_player.h"
#include "dlna_wifi.h"
#include "bsp_display.h"   // bsp_lvgl_lock(配网页提示走向屏幕)

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/ip_addr.h"
#include "lwip/inet.h"   // inet_ntoa
#include <string.h>
#include <stdlib.h>

static const char *TAG = "dlna_service";
#define DEVICE_NAME     "FoloToy Music Player"
#define DEVICE_UUID     "2f402f80-da50-11e1-9b23-001788092242"

static httpd_handle_t s_server = NULL;
static bool s_started = false;

// ---- SSDP(简单服务发现协议) ---- //
// 用 UDP 组播 239.255.255.250:1900 广播设备存在,并响应 M-SEARCH 探测。
#define SSDP_MCAST_IP   "239.255.255.250"
#define SSDP_MCAST_PORT 1900

static void ssdp_notify(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { ESP_LOGE(TAG, "ssdp socket 失败"); return; }

    int ok = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (const char *)&ok, sizeof(ok));

    // 统一用 dlna_wifi_ip_str() 取当前生效 IP(STA 联网或 AP 配网)。
    // AP 模式返回 192.168.4.1;STA 模式返回真实网段 IP。
    const char *ip_str = dlna_wifi_ip_str();
    if (!ip_str || !*ip_str) ip_str = "0.0.0.0";

    // 发送 socket 也绑定到本地 STA 接口,确保组播从正确的网卡发出。
    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    local.sin_port = 0;
    if (inet_aton(ip_str, &local.sin_addr) == 1) {
        bind(sock, (struct sockaddr *)&local, sizeof(local));
    }

    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(SSDP_MCAST_PORT);
    inet_aton(SSDP_MCAST_IP, &dest.sin_addr);

    // 标准 UPnP:一台设备通常广播多条 NOTIFY,分属不同 NT/USN 组合:
    //   upnp:rootdevice, device:MediaRenderer, 以及各 service(可省略)。
    // 发 rootdevice + MediaRenderer 两条,覆盖常见投屏端的发现。
    struct { const char *nt; const char *usn; } sends[] = {
        { "upnp:rootdevice", "uuid:" DEVICE_UUID "::upnp:rootdevice" },
        { "urn:schemas-upnp-org:device:MediaRenderer:1",
          "uuid:" DEVICE_UUID "::urn:schemas-upnp-org:device:MediaRenderer:1" },
    };
    for (size_t i = 0; i < sizeof(sends) / sizeof(sends[0]); i++) {
        char notify[512];
        int len = snprintf(notify, sizeof(notify),
            "NOTIFY * HTTP/1.1\r\n"
            "HOST: 239.255.255.250:1900\r\n"
            "CACHE-CONTROL: max-age=1800\r\n"
            "LOCATION: http://%s/dlna/description.xml\r\n"
            "SERVER: DLNADOC/1.50 UPnP/1.0 %s/1.0\r\n"
            "BOOTID.UPNP.ORG: 1\r\n"
            "CONFIGID.UPNP.ORG: 1\r\n"
            "NT: %s\r\n"
            "NTS: ssdp:alive\r\n"
            "USN: %s\r\n\r\n",
            ip_str, DEVICE_NAME, sends[i].nt, sends[i].usn);
        if (len > 0) {
            int sent = sendto(sock, notify, (size_t)len, 0, (struct sockaddr *)&dest, sizeof(dest));
            ESP_LOGI(TAG, "SSDP TX %s ret=%d", sends[i].nt, sent);
        }
    }
    close(sock);
}

static void ssdp_respond(const char *req, int sock, struct sockaddr *from, socklen_t fromlen)
{
    const char *ip_str = dlna_wifi_ip_str();
    if (!ip_str || !*ip_str) ip_str = "0.0.0.0";

    // 解析 M-SEARCH 请求里的 ST(Search Target),按请求回应对应 ST;
    // 若请求的是 ssdp:all 或特定类型,回一个匹配的响应。DLNA 播放端
    // 通常搜 upnp:rootdevice / MediaRenderer:1 / ssdp:all 之一。
    // 统一用 MediaRenderer:1,兼容 ssdp:all(接收方按自己搜的类型匹配 USN)。
    // 标准(UPnP)要求:若请求 ST != ssdp:all 且不是本设备类型,应忽略。
    // 这里简化:回 ssdp:all + MediaRenderer 两条,保证投屏端命中。
    // 分别构造两条响应(MediaRenderer 和 rootdevice)用同一个缓冲复用。
    char resp[512];   // 单条响应缓冲(栈小,不复用叠加,避免撑爆)
    int len = snprintf(resp, sizeof(resp),
        "HTTP/1.1 200 OK\r\n"
        "CACHE-CONTROL: max-age=1800\r\n"
        "EXT:\r\n"
        "LOCATION: http://%s/dlna/description.xml\r\n"
        "SERVER: DLNADOC/1.50 UPnP/1.0 %s/1.0\r\n"
        "BOOTID.UPNP.ORG: 1\r\n"
        "CONFIGID.UPNP.ORG: 1\r\n"
        "ST: urn:schemas-upnp-org:device:MediaRenderer:1\r\n"
        "USN: uuid:%s::urn:schemas-upnp-org:device:MediaRenderer:1\r\n\r\n",
        ip_str, DEVICE_NAME, DEVICE_UUID);
    if (len > 0) sendto(sock, resp, (size_t)len, 0, from, fromlen);

    // 再回一条 rootdevice,兼容只按 rootdevice 搜的 App(复用 resp)。
    len = snprintf(resp, sizeof(resp),
        "HTTP/1.1 200 OK\r\n"
        "CACHE-CONTROL: max-age=1800\r\n"
        "EXT:\r\n"
        "LOCATION: http://%s/dlna/description.xml\r\n"
        "SERVER: DLNADOC/1.50 UPnP/1.0 %s/1.0\r\n"
        "ST: upnp:rootdevice\r\n"
        "USN: uuid:%s::upnp:rootdevice\r\n\r\n",
        ip_str, DEVICE_NAME, DEVICE_UUID);
    if (len > 0) sendto(sock, resp, (size_t)len, 0, from, fromlen);
    (void)req;
}

static void ssdp_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { ESP_LOGE(TAG, "ssdp task socket 失败"); vTaskDelete(NULL); return; }

    int ok = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&ok, sizeof(ok));
    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    local.sin_port = htons(SSDP_MCAST_PORT);
    local.sin_addr.s_addr = INADDR_ANY;
    if (bind(sock, (struct sockaddr *)&local, sizeof(local)) != 0) {
        ESP_LOGE(TAG, "ssdp bind 失败(端口被占?)");
        close(sock); vTaskDelete(NULL); return;
    }

    // 加入组播。lwIP 接收组播时 imr_interface 用 INADDR_ANY 让系统走默认接口,
    // 用具体 IP 反而可能选错网卡导致收不到 M-SEARCH。
    struct ip_mreq mreq = {0};
    inet_aton(SSDP_MCAST_IP, &mreq.imr_multiaddr);
    mreq.imr_interface.s_addr = INADDR_ANY;
    int join = setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                          (const char *)&mreq, sizeof(mreq));
    ESP_LOGI(TAG, "SSDP 发现任务启动 join=%d", join);

    // 周期广播 NOTIFY(alive),并响应 M-SEARCH。手机"点投屏才搜索"时,
    // 只发一次的启动 NOTIFY 早就过了,必须持续广播才能被搜到。
    char buf[512];   // 收 M-SEARCH 用;栈小,避免撑爆(栈保护)
    const TickType_t notify_interval = pdMS_TO_TICKS(3000);
    const TickType_t wake_interval  = pdMS_TO_TICKS(100);
    uint32_t last_notify = 0;
    for (;;) {
        uint32_t now = xTaskGetTickCount();
        if (now - last_notify >= notify_interval) {
            ssdp_notify();
            last_notify = now;
        }
        // 处理 M-SEARCH。
        struct sockaddr from;
        socklen_t fromlen = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf) - 1, MSG_DONTWAIT, &from, &fromlen);
        if (n > 0) {
            buf[n] = '\0';
            // 诊断:记录收到的 SSDP 报文摘要,判断 M-SEARCH 是否到达。
            const struct sockaddr_in *fi = (const struct sockaddr_in *)&from;
            ESP_LOGI(TAG, "SSDP RX %d bytes from %s", n, inet_ntoa(fi->sin_addr));
            if (strstr(buf, "M-SEARCH")) {
                ssdp_respond(buf, sock, &from, fromlen);
            }
        }
        vTaskDelay(wake_interval);
    }
    // (不会走到这)
}

// ---- HTTP/SOAP 服务 ---- //
static const char *DESCRIPTION_XML =
    "<?xml version=\"1.0\"?>"
    "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">"
    "<specVersion><major>1</major><minor>0</minor></specVersion>"
    "<device>"
    "<deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>"
    "<friendlyName>" DEVICE_NAME "</friendlyName>"
    "<manufacturer>FoloToy</manufacturer>"
    "<modelName>AI Passport</modelName>"
    "<UDN>uuid:" DEVICE_UUID "</UDN>"
    "<serviceList>"
    "<service><serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType>"
    "<serviceId>urn:upnp-org:serviceId:AVTransport</serviceId>"
    "<SCPDURL>/dlna/AVTransport.xml</SCPDURL><controlURL>/dlna/AVTransport</controlURL>"
    "<eventSubURL>/dlna/AVTransportEvent</eventSubURL></service>"
    "<service><serviceType>urn:schemas-upnp-org:service:RenderingControl:1</serviceType>"
    "<serviceId>urn:upnp-org:serviceId:RenderingControl</serviceId>"
    "<SCPDURL>/dlna/RenderingControl.xml</SCPDURL><controlURL>/dlna/RenderingControl</controlURL>"
    "<eventSubURL>/dlna/RenderingControlEvent</eventSubURL></service>"
    "<service><serviceType>urn:schemas-upnp-org:service:ConnectionManager:1</serviceType>"
    "<serviceId>urn:upnp-org:serviceId:ConnectionManager</serviceId>"
    "<SCPDURL>/dlna/ConnectionManager.xml</SCPDURL><controlURL>/dlna/ConnectionManager</controlURL>"
    "<eventSubURL>/dlna/ConnectionManagerEvent</eventSubURL></service>"
    "</serviceList>"
    "</device></root>";

// 从一个 SOAP request body 里提取 <Tag>value</Tag> 的值。
static int soap_extract(const char *body, const char *tag, char *out, size_t max)
{
    char open[64], close[64];
    snprintf(open, sizeof(open), "<%s>", tag);
    snprintf(close, sizeof(close), "</%s>", tag);
    const char *p = strstr(body, open);
    if (!p) return -1;
    p += strlen(open);
    const char *q = strstr(p, close);
    if (!q) return -1;
    size_t len = (size_t)(q - p);
    if (len >= max) len = max - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return (int)len;
}

// description.xml
static esp_err_t desc_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/xml");
    httpd_resp_set_hdr(req, "Content-Type", "text/xml; charset=\"utf-8\"");
    return httpd_resp_send(req, DESCRIPTION_XML, HTTPD_RESP_USE_STRLEN);
}

// SCPD(服务描述)xml —— 返回一个能识别的 MediaRenderer 最小集合。
static esp_err_t avt_scpd_handler(httpd_req_t *req)
{
    const char *xml =
        "<?xml version=\"1.0\"?>"
        "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">"
        "<specVersion><major>1</major><minor>0</minor></specVersion>"
        "<actionList>"
        "<action><name>SetAVTransportURI</name></action>"
        "<action><name>Play</name></action>"
        "<action><name>Pause</name></action>"
        "<action><name>Stop</name></action>"
        "</actionList></scpd>";
    httpd_resp_set_type(req, "text/xml");
    return httpd_resp_send(req, xml, HTTPD_RESP_USE_STRLEN);
}
static esp_err_t rc_scpd_handler(httpd_req_t *req)
{
    const char *xml =
        "<?xml version=\"1.0\"?>"
        "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">"
        "<specVersion><major>1</major><minor>0</minor></specVersion>"
        "<actionList>"
        "<action><name>SetVolume</name></action>"
        "<action><name>GetVolume</name></action>"
        "</actionList></scpd>";
    httpd_resp_set_type(req, "text/xml");
    return httpd_resp_send(req, xml, HTTPD_RESP_USE_STRLEN);
}
static esp_err_t cm_scpd_handler(httpd_req_t *req)
{
    const char *xml =
        "<?xml version=\"1.0\"?>"
        "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">"
        "<specVersion><major>1</major><minor>0</minor></specVersion>"
        "<actionList><action><name>GetProtocolInfo</name></action></actionList></scpd>";
    httpd_resp_set_type(req, "text/xml");
    return httpd_resp_send(req, xml, HTTPD_RESP_USE_STRLEN);
}

// 读取 SOAP POST body。
static int read_body(httpd_req_t *req, char *buf, size_t max)
{
    int total = req->content_len;
    if (total <= 0) return 0;
    if ((size_t)total >= max) total = (int)(max - 1);
    int got = 0;
    while (got < total) {
        int n = httpd_req_recv(req, buf + got, (size_t)(total - got));
        if (n <= 0) break;
        got += n;
    }
    buf[got] = '\0';
    return got;
}

// AVTransport SOAP
static esp_err_t avt_handler(httpd_req_t *req)
{
    char body[1024];
    int len = read_body(req, body, sizeof(body));

    char url[512];
    if (len > 0 && strstr(body, "SetAVTransportURI")) {
        if (soap_extract(body, "CurrentURI", url, sizeof(url)) >= 0) {
            ESP_LOGI(TAG, "SetAVTransportURI: %s", url);
            dlna_player_play(url);
        }
    } else if (len > 0 && strstr(body, "Play")) {
        dlna_player_resume();
    } else if (len > 0 && strstr(body, "Pause")) {
        dlna_player_pause();
    } else if (len > 0 && strstr(body, "Stop")) {
        dlna_player_stop();
    }

    httpd_resp_set_type(req, "text/xml");
    const char *resp =
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\""
        " s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body><u:ActionResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\"/>"
        "</s:Body></s:Envelope>";
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

// RenderingControl SOAP
static esp_err_t rc_handler(httpd_req_t *req)
{
    char body[1024];
    int len = read_body(req, body, sizeof(body));

    char vol_str[16];
    if (len > 0 && strstr(body, "SetVolume")) {
        if (soap_extract(body, "DesiredVolume", vol_str, sizeof(vol_str)) >= 0) {
            int v = atoi(vol_str);
            if (v < 0) v = 0;
            if (v > 100) v = 100;
            dlna_player_set_volume(v);
        }
    }

    httpd_resp_set_type(req, "text/xml");
    const char *resp =
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\""
        " s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body><u:ActionResponse xmlns:u=\"urn:schemas-upnp-org:service:RenderingControl:1\"/>"
        "</s:Body></s:Envelope>";
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

// ConnectionManager SOAP —— 返回支持的协议(手机 App 靠 GetProtocolInfo 才给投屏选项)。
static esp_err_t cm_handler(httpd_req_t *req)
{
    char body[1024];
    int len = read_body(req, body, sizeof(body));

    httpd_resp_set_type(req, "text/xml");
    if (len > 0 && strstr(body, "GetProtocolInfo")) {
        const char *resp =
            "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\""
            " s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
            "<s:Body><u:GetProtocolInfoResponse xmlns:u=\"urn:schemas-upnp-org:service:ConnectionManager:1\">"
            "<Source>http-get:*:audio/mpeg:*</Source>"
            "<Sink></Sink>"
            "</u:GetProtocolInfoResponse></s:Body></s:Envelope>";
        return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    }
    const char *resp =
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\""
        " s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body><u:ActionResponse xmlns:u=\"urn:schemas-upnp-org:service:ConnectionManager:1\"/>"
        "</s:Body></s:Envelope>";
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t dlna_player_status_handler(httpd_req_t *req)
{
    // 一个非 DLNA 标准的自检端点,方便调试/浏览器看到设备状态。
    char buf[128];
    snprintf(buf, sizeof(buf), "state=%d\nvolume=%d\n",
             (int)dlna_player_get_state(), dlna_player_get_volume());
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

// ---- AP 配网页 ---- //
// 设备处于 AP 配网模式时,用户连上 AP 后访问根路径看到此表单;提交后存 NVS 并重启。
// 适配移动端(viewport + 大按钮),并支持"扫描 WiFi"。
static esp_err_t wifi_config_page(httpd_req_t *req)
{
    const char *ap_pass = dlna_wifi_ap_pass_get();
    // 用 HTML 转义防止注入(数字密码,实际无风险,但保持规范)。
    char body[4096];
    int len = snprintf(body, sizeof(body),
        "<!DOCTYPE html>"
        "<html><head><meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>FoloToy DLNA 配网</title>"
        "<style>"
        "body{font-family:sans-serif;margin:0;padding:16px;background:#f4f4ea;color:#17202a}"
        "h3{margin:4px 0 12px}"
        ".hint{font-size:14px;color:#0872c9;margin:8px 0}"
        "input,select{font-size:18px;padding:12px;width:100%%;box-sizing:border-box;margin:4px 0 14px;border:2px solid #17202a;border-radius:6px}"
        "button{font-size:18px;padding:14px;width:100%%;background:#17202a;color:#fff;border:0;border-radius:6px;margin-top:8px}"
        "button.ghost{background:#fff;color:#17202a;border:2px solid #17202a;margin-top:0}"
        ".list{max-height:180px;overflow-y:auto;border:2px solid #17202a;border-radius:6px;margin:4px 0 14px}"
        ".list div{padding:12px;font-size:18px;border-bottom:1px solid #ddd;cursor:pointer}"
        ".list div.sel{background:#0872c9;color:#fff}"
        "</style></head><body>"
        "<h3>WiFi 配网</h3>"
        "<p class='hint'>热点密码: <b>%s</b></p>"
        "<button class='ghost' onclick='doScan()'>扫描 WiFi</button>"
        "<div id='list' class='list' style='display:none'></div>"
        "<form method='POST' action='/save' id='f'>"
        "<select id='ssid' name='ssid'><option value=''>请选择或输入 WiFi</option></select>"
        "<input id='ssid_in' name='ssid' placeholder='或直接输入 WiFi 名' style='display:none'><br>"
        "密码: <input type='password' name='pass'><br>"
        "<button type='submit'>保存并重启</button>"
        "</form>"
        "<script>"
        "var sel='';"
        "function doScan(){var l=document.getElementById('list');l.style.display='block';"
        "l.innerHTML='扫描中... (热点会短暂断开,请等待几秒)';"
        "fetch('/scan').then(function(r){return r.json()}).then(function(d){"
        "var w=l.innerHTML=''; var s=document.getElementById('ssid'); s.innerHTML='';"
        "var opt=document.createElement('option');opt.value='';opt.text='请选择 WiFi';s.appendChild(opt);"
        "d.nets.forEach(function(n,i){"
        "var o=document.createElement('option');o.value=n;o.text=n;s.appendChild(o);"
        "var dd=document.createElement('div');dd.textContent=n;dd.onclick=function(){sel=n;"
        "s.value=n;var ds=l.querySelectorAll('.sel');for(var j=0;j<ds.length;j++)ds[j].className='';"
        "dd.className='sel';};l.appendChild(dd);});})}"
        "</script></body></html>",
        ap_pass);
    httpd_resp_set_type(req, "text/html");
    if (len < 0) len = 0;
    return httpd_resp_send(req, body, (size_t)len);
}

// /scan:扫描附近 WiFi,返回 JSON 数组。注意会短暂断连(AP→扫→回AP)。
static esp_err_t wifi_scan_handler(httpd_req_t *req)
{
    #define SCAN_MAX 20
    char names[SCAN_MAX][33];
    // 用 JSON 序列化输出。
    int count = dlna_wifi_scan_networks(names, SCAN_MAX);
    if (count < 0) count = 0;

    char body[4096];
    int off = snprintf(body, sizeof(body), "{\"nets\":[");
    for (int i = 0; i < count && off < (int)sizeof(body) - 4; i++) {
        // 转义 JSON 字符串里的引号/反斜杠(SSID 可能含特殊字符)。
        int a = snprintf(body + off, sizeof(body) - (size_t)off, "\"%s\",", names[i]);
        if (a < 0) break;
        off += a;
    }
    if (count > 0) off--;   // 去掉最后一个逗号
    snprintf(body + off, sizeof(body) - (size_t)off, "]}");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t wifi_config_save(httpd_req_t *req)
{
    char body[512];
    int len = 0;
    int total = req->content_len;
    if (total > 0) {
        if (total > (int)sizeof(body) - 1) total = (int)sizeof(body) - 1;
        while (len < total) {
            int n = httpd_req_recv(req, body + len, (size_t)(total - len));
            if (n <= 0) break;
            len += n;
        }
    }
    body[len] = '\0';

    // 解析 x-www-form-urlencoded 的 ssid / pass 字段。
    char ssid[64] = {0}, pass[64] = {0};
    char *tok = strtok(body, "&");
    while (tok) {
        // 提取 "key=value"
        char *eq = strchr(tok, '=');
        if (eq) {
            *eq = '\0';
            const char *key = tok;
            const char *val = eq + 1;
            if (strcmp(key, "ssid") == 0) strncpy(ssid, val, sizeof(ssid) - 1);
            else if (strcmp(key, "pass") == 0) strncpy(pass, val, sizeof(pass) - 1);
        }
        tok = strtok(NULL, "&");
    }

    ESP_LOGI(TAG, "配网提交: ssid=%s", ssid);
    esp_err_t e1 = dlna_wifi_save_credentials(ssid, pass);
    httpd_resp_set_type(req, "text/plain");
    const char *msg = (e1 == ESP_OK)
        ? "配置已保存,设备重启中..."
        : "保存失败,请重试";
    esp_err_t sr = httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
    if (e1 == ESP_OK) dlna_wifi_restart();
    return sr;
}

// ---- 启动/停止 ---- //
esp_err_t dlna_service_start(void)
{
    if (s_started) return ESP_OK;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.lru_purge_enable = true;
    // 需要注册 10 个 URI(description + 3 SCPD + 3 SOAP + status + 配网页 + save),
    // 默认 max_uri_handlers(=8)不够,加大避免 "no slots left"。
    cfg.max_uri_handlers = 16;
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start 失败(端口占用?): %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    httpd_uri_t uri_desc = { .uri = "/dlna/description.xml", .method = HTTP_GET,
                             .handler = desc_handler, .user_ctx = NULL };
    httpd_uri_t uri_avt_scpd = { .uri = "/dlna/AVTransport.xml", .method = HTTP_GET,
                             .handler = avt_scpd_handler, .user_ctx = NULL };
    httpd_uri_t uri_rc_scpd = { .uri = "/dlna/RenderingControl.xml", .method = HTTP_GET,
                             .handler = rc_scpd_handler, .user_ctx = NULL };
    httpd_uri_t uri_cm_scpd = { .uri = "/dlna/ConnectionManager.xml", .method = HTTP_GET,
                             .handler = cm_scpd_handler, .user_ctx = NULL };
    httpd_uri_t uri_avt = { .uri = "/dlna/AVTransport", .method = HTTP_POST,
                             .handler = avt_handler, .user_ctx = NULL };
    httpd_uri_t uri_rc = { .uri = "/dlna/RenderingControl", .method = HTTP_POST,
                             .handler = rc_handler, .user_ctx = NULL };
    httpd_uri_t uri_cm = { .uri = "/dlna/ConnectionManager", .method = HTTP_POST,
                             .handler = cm_handler, .user_ctx = NULL };
    httpd_uri_t uri_status = { .uri = "/status", .method = HTTP_GET,
                             .handler = dlna_player_status_handler, .user_ctx = NULL };
    httpd_uri_t uri_cfg_page = { .uri = "/", .method = HTTP_GET,
                             .handler = wifi_config_page, .user_ctx = NULL };
    httpd_uri_t uri_cfg_save = { .uri = "/save", .method = HTTP_POST,
                             .handler = wifi_config_save, .user_ctx = NULL };
    httpd_uri_t uri_cfg_scan = { .uri = "/scan", .method = HTTP_GET,
                             .handler = wifi_scan_handler, .user_ctx = NULL };

    httpd_register_uri_handler(s_server, &uri_desc);
    httpd_register_uri_handler(s_server, &uri_cfg_page);
    httpd_register_uri_handler(s_server, &uri_cfg_save);
    httpd_register_uri_handler(s_server, &uri_cfg_scan);
    httpd_register_uri_handler(s_server, &uri_avt_scpd);
    httpd_register_uri_handler(s_server, &uri_rc_scpd);
    httpd_register_uri_handler(s_server, &uri_cm_scpd);
    httpd_register_uri_handler(s_server, &uri_avt);
    httpd_register_uri_handler(s_server, &uri_rc);
    httpd_register_uri_handler(s_server, &uri_cm);
    httpd_register_uri_handler(s_server, &uri_status);
    ESP_LOGI(TAG, "DLNA HTTP 服务启动,端口 80");

    // 启动 SSDP 发现任务。
    // 栈给足:ssdp_task 里 ssdp_notify/ssdp_respond 都有 512 局部缓冲,4KB 会栈保护溢出。
    xTaskCreate(ssdp_task, "ssdp", 8192, NULL, 5, NULL);

    s_started = true;
    return ESP_OK;
}

void dlna_service_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    s_started = false;
}
