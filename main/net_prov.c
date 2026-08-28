// main/net_prov.c —— SoftAP 配网 Web 服务实现。
// 提供配网表单,POST 后把 SSID/密码写入 NVS(经 bsp_wifi_save_credentials)。
#include "net_prov.h"
#include "bsp_wifi.h"
#include "dns_server.h"

#include "esp_log.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "net_prov";

static httpd_handle_t s_server;
static dns_server_handle_t s_dns;

// 简单的 url_decode(把 %XX 转字符,+ 转空格)。
static void url_decode(char *dst, size_t dst_sz, const char *src)
{
    size_t i = 0;
    while (*src && i < dst_sz - 1) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], 0 };
            dst[i++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = 0;
}

// 解析 urlencoded body,提取 key 的值。
static const char *form_value(const char *body, const char *key, char *out, size_t out_sz)
{
    size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        size_t seg_len = amp ? (size_t)(amp - p) : strlen(p);
        if (seg_len > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *val = p + klen + 1;
            size_t vlen = amp ? (size_t)(amp - val) : strlen(val);
            if (vlen >= out_sz) vlen = out_sz - 1;
            char tmp[128];
            strlcpy(tmp, val, sizeof(tmp));
            tmp[vlen] = 0;
            url_decode(out, out_sz, tmp);
            return out;
        }
        p = amp ? amp + 1 : NULL;
    }
    return NULL;
}

// GET / —— 配网表单。
static esp_err_t handler_index(httpd_req_t *req)
{
    // 开机已扫描的 WiFi 列表,预填进 <select>(不提供页面内扫描按钮)。
    bsp_wifi_ap_t aps[16];
    int n = bsp_wifi_get_scan_results(aps, 16);

    static char html[4096];
    char opts[2560] = {0};
    int used = 0;
    for (int i = 0; i < n && used < (int)sizeof(opts) - 1; i++) {
        int w = snprintf(opts + used, sizeof(opts) - used,
                         "<option value='%s'>%s (%d)</option>",
                         aps[i].ssid, aps[i].ssid, aps[i].rssi);
        if (w < 0 || w >= (int)sizeof(opts) - used) break;
        used += w;
    }
    if (n <= 0) {
        // 若扫描无结果,提供手动输入框兜底。
        snprintf(opts, sizeof(opts),
                 "<input type='text' name='ssid' placeholder='手动输入 SSID'><br>");
    }

    int w = snprintf(html, sizeof(html),
        "<html><head><meta charset='utf-8'><meta name='viewport' "
        "content='width=device-width,initial-scale=1'>"
        "<title>AI Passport 配网</title></head><body>"
        "<h2>AI Passport WiFi 配网</h2>"
        "<form method='post' action='/wifi'>"
        "WiFi: <select name='ssid'>%s</select><br>"
        "密码: <input name='pass' type='password'><br>"
        "<input type='submit' value='连接'>"
        "</form></body></html>", opts);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, html, w < 0 ? 0 : (size_t)w);
    return ESP_OK;
}

// POST /wifi —— 接收 SSID/密码,写 NVS,回成功页。
static esp_err_t handler_connect(httpd_req_t *req)
{
    char body[512] = {0};
    int total = 0;
    read_body:
    if (req->content_len > 0) {
        int n = httpd_req_recv(req, body + total, sizeof(body) - total - 1);
        if (n > 0) {
            total += n;
            if (total < (int)sizeof(body) - 1) goto read_body;
        }
    }
    body[total] = 0;

    char ssid[33] = {0}, pass[65] = {0};
    if (!form_value(body, "ssid", ssid, sizeof(ssid)) || !ssid[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID is required");
        return ESP_OK;
    }
    form_value(body, "pass", pass, sizeof(pass));

    esp_err_t err = bsp_wifi_save_credentials(ssid, pass);
    ESP_LOGI(TAG, "配网提交: ssid=%s err=%s", ssid, esp_err_to_name(err));

    const char *html = err == ESP_OK ?
        "<html><body><h2>保存成功,设备正在重启连接...</h2></body></html>" :
        "<html><body><h2>保存失败,请重试</h2></body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, html, strlen(html));

    // 保存成功:延迟重启以转 STA。由调用方决定是否立即重启;这里简短 delay。
    if (err == ESP_OK) {
        // 通知调用方/重启由 dlna_app 层处理,这里仅延后一点时间给响应送达。
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
    return ESP_OK;
}

// HTTP 404 错误 handler —— 把所有未匹配请求 303 重定向到 "/" 配网页,
// 触发手机(Android/iOS/Windows)的 captive portal 自动弹出认证页面。
static esp_err_t http_404_handler(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    // iOS 需要响应体才能识别 captive portal,仅重定向不够。
    httpd_resp_send(req, "Redirect to captive portal", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t net_prov_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 8;
    // DLNA 的 custom_dlna 已起 httpd(端口 8080,默认 ctrl_port)。
    // 配网服务须用不同 server_port 与 ctrl_port,且少占 socket,避免冲突。
    cfg.server_port = 80;
    cfg.ctrl_port = 32769;
    cfg.max_open_sockets = 4;
    // C3 单核:显式绑 core0,避免 tskNO_AFFINITY 触发 xTaskCreatePinnedToCore 断言。
    cfg.core_id = 0;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd 启动失败: %s (open=%d)", esp_err_to_name(err),
                 cfg.max_open_sockets);
        return ESP_FAIL;
    }

    httpd_uri_t uri_index = {
        .uri = "/",            .method = HTTP_GET,  .handler = handler_index,
        .user_ctx = NULL,
    };
    httpd_uri_t uri_connect = {
        .uri = "/wifi",        .method = HTTP_POST, .handler = handler_connect,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &uri_index);
    httpd_register_uri_handler(s_server, &uri_connect);
    // 所有未匹配请求重定向到配网页 → 触发手机 captive portal 自动弹出
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, http_404_handler);

    ESP_LOGI(TAG, "配网 Web 服务运行于 http://%s/", bsp_wifi_get_ap_ip());

    // 启动 DNS 拦截服务器:把所有 A 类查询应答为软AP IP(192.168.4.1),
    // 配合上方的 404 重定向,让手机连上热点后自动弹出配网页(captive portal)。
    dns_server_config_t dns_cfg = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    s_dns = start_dns_server(&dns_cfg);
    if (s_dns) {
        ESP_LOGI(TAG, "DNS captive portal 已启动(拦截所有查询→192.168.4.1)");
    } else {
        ESP_LOGW(TAG, "dns_server 启动失败");
    }
    return ESP_OK;
}

void net_prov_stop(void)
{
    if (s_dns) {
        stop_dns_server(s_dns);
        s_dns = NULL;
    }
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}
