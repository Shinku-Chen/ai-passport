#include "custom_dlna.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "CUSTOM_DLNA";

/* ── Configuration ── */
#define SSDP_PORT      1900
#define SSDP_MULTICAST "239.255.255.250"

/* Supported audio protocols for ConnectionManager GetProtocolInfo */
#define SUPPORTED_PROTOCOLS \
    "http-get:*:audio/mpeg:*," \
    "http-get:*:audio/mp3:*," \
    "http-get:*:audio/x-mpeg:*," \
    "http-get:*:audio/mp4:*," \
    "http-get:*:audio/ogg:*," \
    "http-get:*:audio/flac:*," \
    "http-get:*:audio/x-flac:*," \
    "http-get:*:audio/wav:*," \
    "http-get:*:audio/x-wav:*," \
    "http-get:*:audio/aac:*," \
    "http-get:*:audio/x-aac:*," \
    "http-get:*:audio/x-m4a:*," \
    "http-get:*:audio/x-ms-wma:*," \
    "http-get:*:audio/L16:*," \
    "http-get:*:audio/vnd.dlna.adts:*," \
    "http-get:*:audio/ape:*," \
    "http-get:*:audio/pcm:*," \
    "http-get:*:audio/opus:*," \
    "http-get:*:audio/*:*"

/* ── Internal state ── */
static const custom_dlna_config_t *s_cfg = NULL;
static httpd_handle_t s_server = NULL;
static char s_uri[2048] = {0};
static char s_metadata[16384] = {0};  /* 当前曲目 DIDL-Lite 元数据（网易云可能 >4KB） */
static char s_next_uri[2048] = {0};   /* 下一曲 URI（SetNextAVTransportURI 设置） */
static char s_next_metadata[2048] = {0};

/* ── 按模式切配置 ── */
static music_source_t s_music_source = MUSIC_SRC_NETEASE;  /* 默认网易云配置 */
static char s_user_agent[128] = {0};  /* 最近一次 SetAVTransportURI 的 User-Agent */

/* GENA 通知互斥锁：pos_notify_task 与 gena_task 并发调用 gena_notify，
 * 同时操作 s_subs[i].client 会 use-after-free 导致 Cache error。 */
static SemaphoreHandle_t s_gena_mutex = NULL;

/* 允许应用层在切歌时更新 URI 和 metadata（cb_next/cb_previous 触发） */
void custom_dlna_update_uri(const char *uri, const char *metadata)
{
    if (uri) strncpy(s_uri, uri, sizeof(s_uri) - 1);
    else s_uri[0] = '\0';
    if (metadata) strncpy(s_metadata, metadata, sizeof(s_metadata) - 1);
    else s_metadata[0] = '\0';
    /* 新曲目上播，清空 next */
    s_next_uri[0] = '\0';
    s_next_metadata[0] = '\0';
}

/* ── 按模式切配置 ── */
void custom_dlna_set_music_source(music_source_t src)
{
    if (src >= MUSIC_SRC_MAX) return;
    if (s_music_source != src) {
        ESP_LOGI(TAG, "Music source: %d → %d", s_music_source, src);
        s_music_source = src;
    }
}

music_source_t custom_dlna_get_music_source(void) { return s_music_source; }

const char* custom_dlna_get_user_agent(void) { return s_user_agent; }
static int  s_volume_cache = 50;   /* cached volume for fast SOAP response */
static int  s_mute_cache   = 0;    /* cached mute for fast SOAP response */
static volatile bool s_ssdp_suppressed = false;  /* MiPlay 活跃时暂停 SSDP */

void custom_dlna_set_ssdp_suppressed(bool suppressed)
{
    s_ssdp_suppressed = suppressed;
    ESP_LOGI(TAG, "SSDP %s", suppressed ? "suppressed (MiPlay active)" : "resumed");
}

/* Forward declaration: SOAP ok stub used by dispatch */
static void soap_ok_stub(httpd_req_t *req, const char *service, const char *action);

/* GENA subscriber list — 持久连接（参考 miair-next session 复用） */
#define MAX_SUBSCRIBERS 4
#define SUB_TIMEOUT_SEC 1800
typedef struct {
    char url[256];
    char sid[96];
    int64_t expiry_time;  /* esp_timer_get_time() + timeout */
    int seq;
} gena_subscriber_t;
static gena_subscriber_t s_subs[MAX_SUBSCRIBERS];
static int  s_sub_count = 0;
static int  s_next_sub_id = 1;  /* unique SID counter */

/* Periodic position notify timer */
static esp_timer_handle_t s_pos_timer = NULL;

/* GENA notify work queue — avoid HTTP I/O in callbacks (e.g. media_task) */
/* Queue passes dynamically allocated char*; gena_task frees after sending. */
#define NOTIFY_QUEUE_LEN 16
typedef struct {
    char *data;
} notify_msg_t;
static QueueHandle_t notify_queue = NULL;

/* Embedded XML files */
extern const uint8_t device_xml_start[]           asm("_binary_device_xml_start");
extern const uint8_t device_xml_end[]             asm("_binary_device_xml_end");
extern const uint8_t avtransport_xml_start[]       asm("_binary_avtransport_xml_start");
extern const uint8_t avtransport_xml_end[]         asm("_binary_avtransport_xml_end");
extern const uint8_t renderingcontrol_xml_start[]  asm("_binary_renderingcontrol_xml_start");
extern const uint8_t renderingcontrol_xml_end[]    asm("_binary_renderingcontrol_xml_end");
extern const uint8_t connectionmanager_xml_start[]  asm("_binary_connectionmanager_xml_start");
extern const uint8_t connectionmanager_xml_end[]    asm("_binary_connectionmanager_xml_end");

/* ── Get local IP ── */
static const char* get_local_ip(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("STA_DEF");
    if (!netif) netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
            static char ipstr[16];
            snprintf(ipstr, sizeof(ipstr), IPSTR, IP2STR(&ip.ip));
            return ipstr;
        }
    }
    return "0.0.0.0";
}

/* ── Get local gateway (for unicast SSDP to hotspot host) ── */
static const char* get_local_gateway(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("STA_DEF");
    if (!netif) netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
            static char gwstr[16];
            snprintf(gwstr, sizeof(gwstr), IPSTR, IP2STR(&ip.gw));
            return gwstr;
        }
    }
    return "0.0.0.0";
}

/* ── Extract action name from SOAPAction header ──
 *   Header format: "urn:schemas-upnp-org:service:AVTransport:1#Play"
 *   Returns pointer to action name (after '#'), or NULL.
 */
static const char *get_soap_action(httpd_req_t *req, char *buf, int buf_sz)
{
    if (httpd_req_get_hdr_value_str(req, "SOAPAction", buf, buf_sz) != ESP_OK) {
        if (httpd_req_get_hdr_value_str(req, "SOAPACTION", buf, buf_sz) != ESP_OK) {
            return NULL;
        }
    }
    char *p = buf;
    while (*p == '"' || *p == ' ') p++;
    char *hash = strchr(p, '#');
    if (hash) p = hash + 1;
    char *end = p + strlen(p) - 1;
    while (end > p && (*end == '"' || *end == ' ')) *end-- = '\0';
    return p;
}

/* ── XML value extraction ── */
/* 解码 XML 实体：&amp; → &，&lt; → <，&gt; >，&quot; → " */

static int xml_get(const char *xml, const char *tag, char *out, int out_max)
{
    /* Try <u:Tag> first (CyberGarage may namespace-prefix), then <Tag> */
    char start[80], end[80];
    const char *p = NULL;
    const char *op = NULL;
    for (int pass = 0; pass < 2 && !op; pass++) {
        if (pass == 0)
            snprintf(start, sizeof(start), "<u:%s>", tag);
        else
            snprintf(start, sizeof(start), "<%s>", tag);
        p = strstr((char *)xml, start);
        if (!p) {
            snprintf(start, sizeof(start), "%s ", tag);
            char tmp[80];
            snprintf(tmp, sizeof(tmp), "<u:%s ", tag);
            const char *pt = strstr((char *)xml, tmp);
            p = pt ? pt : strstr((char *)xml, start);
        }
        /* find closing > of the opening tag */
        const char *gt = p ? strchr(p, '>') : NULL;
        if (gt) op = gt + 1;
        else p = NULL;
    }
    if (!op) return -1;
    /* closing: try </u:Tag> then </Tag> */
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 0) snprintf(end, sizeof(end), "</u:%s", tag);
        else           snprintf(end, sizeof(end), "</%s", tag);
        char *q = strstr((char *)op, end);
        if (q) {
            int len = q - op;
            if (len >= out_max) len = out_max - 1;
            memcpy(out, op, len);
            out[len] = '\0';
            return len;
        }
    }
    return -1;
}

/* ── Parse seek target HH:MM:SS ── */
static int parse_seek_target(const char *body)
{
    char buf[32] = {0};
    if (xml_get(body, "Target", buf, sizeof(buf)) < 0) return -1;
    int h = 0, m = 0, s = 0;
    if (sscanf(buf, "%d:%d:%d", &h, &m, &s) >= 1)
        return h * 3600 + m * 60 + s;
    return atoi(buf);
}

/* ── Format seconds to HH:MM:SS ── */
static void fmt_time(int sec, char *buf, int len)
{
    if (sec < 0) sec = 0;
    snprintf(buf, len, "%02d:%02d:%02d", sec / 3600, (sec / 60) % 60, sec % 60);
}

static void fmt_time_ms(int ms, char *buf, int len)
{
    if (ms < 0) ms = 0;
    int sec = ms / 1000;
    int msec = ms % 1000;
    snprintf(buf, len, "%02d:%02d:%02d.%03d", sec / 3600, (sec / 60) % 60, sec % 60, msec);
}

/* ── SOAP response helpers ── */
static void send_http_resp(httpd_req_t *req, int code, const char *ct,
                           const char *body, int body_len)
{
    if (body_len < 0) body_len = strlen(body);
    httpd_resp_set_status(req, code == 200 ? "200 OK" : "500 Internal Server Error");
    httpd_resp_set_type(req, ct);
    httpd_resp_send(req, body, body_len);
}

static void send_ok(httpd_req_t *req, const char *body)
{
    send_http_resp(req, 200, "text/xml; charset=utf-8", body, -1);
}

static void send_position_info(httpd_req_t *req)
{
    /* 使用毫秒精度回调，输出标准 HH:MM:SS 格式（参考 miair-next） */
    int pos_ms = 0;
    if (s_cfg->get_position_ms) {
        pos_ms = s_cfg->get_position_ms();
    } else if (s_cfg->get_position_sec) {
        pos_ms = s_cfg->get_position_sec() * 1000;
    }
    int dur = s_cfg->get_duration_sec ? s_cfg->get_duration_sec() : 0;

    char rel[16], dur_str[16];
    fmt_time(pos_ms / 1000, rel, sizeof(rel));
    fmt_time(dur, dur_str, sizeof(dur_str));
    /* 诊断：打印实际返回的进度值 */
    ESP_LOGI(TAG, "GetPositionInfo: pos=%dms rel=%s dur=%dms", pos_ms, rel, dur);

    const char *uri = s_cfg->get_uri ? s_cfg->get_uri() : s_uri;
    if (!uri) uri = "";
    /* 返回完整 metadata（歌词/封面关联依赖它，N16R8 内存充足不截断） */
    const char *meta = s_metadata[0] ? s_metadata : "";
    int meta_len = strlen(meta);

    int resp_sz = 1024 + strlen(uri) + meta_len + strlen(rel) + strlen(dur_str);
    char *resp = malloc(resp_sz);
    if (!resp) { httpd_resp_send_500(req); return; }
    snprintf(resp, resp_sz,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\""
        " s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body>"
        "<u:GetPositionInfoResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
        "<Track>1</Track>"
        "<TrackDuration>%s</TrackDuration>"
        "<TrackMetaData>%.*s</TrackMetaData>"
        "<TrackURI>%s</TrackURI>"
        "<RelTime>%s</RelTime>"
        "<AbsTime>%s</AbsTime>"
        "<RelCount>0</RelCount>"
        "<AbsCount>0</AbsCount>"
        "</u:GetPositionInfoResponse>"
        "</s:Body></s:Envelope>",
        dur_str, meta_len, meta, uri, rel, rel);
    send_ok(req, resp);
    free(resp);
}

static void send_transport_info(httpd_req_t *req)
{
    const char *state = DLNA_STATE_STOPPED;
    if (s_cfg->get_transport_state)
        state = s_cfg->get_transport_state();

    char resp[512];
    snprintf(resp, sizeof(resp),
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\""
        " s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body>"
        "<u:GetTransportInfoResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
        "<CurrentTransportState>%s</CurrentTransportState>"
        "<CurrentTransportStatus>OK</CurrentTransportStatus>"
        "<CurrentSpeed>1</CurrentSpeed>"
        "</u:GetTransportInfoResponse>"
        "</s:Body></s:Envelope>", state);
    send_ok(req, resp);
}

static void send_media_info(httpd_req_t *req)
{
    int dur = s_cfg->get_duration_sec ? s_cfg->get_duration_sec() : 0;
    const char *uri = s_cfg->get_uri ? s_cfg->get_uri() : s_uri;
    if (!uri) uri = "";
    const char *meta = s_metadata[0] ? s_metadata : "";

    char dur_str[16];
    fmt_time(dur, dur_str, sizeof(dur_str));

    int resp_sz = 1024 + strlen(uri) + strlen(meta) + strlen(dur_str);
    char *resp = malloc(resp_sz);
    if (!resp) { httpd_resp_send_500(req); return; }
    snprintf(resp, resp_sz,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\""
        " s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body>"
        "<u:GetMediaInfoResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
        "<NrTracks>1</NrTracks>"
        "<MediaDuration>%s</MediaDuration>"
        "<CurrentURI>%s</CurrentURI>"
        "<CurrentURIMetaData>%s</CurrentURIMetaData>"
        "<NextURI></NextURI>"
        "<NextURIMetaData></NextURIMetaData>"
                "<PlayMedium>NONE</PlayMedium>"
        "<RecordMedium>NOT_IMPLEMENTED</RecordMedium>"
        "<WriteStatus>NOT_IMPLEMENTED</WriteStatus>"
        "</u:GetMediaInfoResponse>"
        "</s:Body></s:Envelope>", dur_str, uri, meta);
    send_ok(req, resp);
    free(resp);
}

/* ── GENA notify — 用原始 socket 发送 HTTP NOTIFY（避免 esp_http_client use-after-free） ── */
static void gena_notify(const char *xml_body, const char *service_type)
{
    (void)service_type;
    if (s_gena_mutex) xSemaphoreTake(s_gena_mutex, portMAX_DELAY);
    int64_t now = esp_timer_get_time() / 1000000;
    for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
        if (s_subs[i].url[0] == '\0') continue;
        if (s_subs[i].expiry_time > 0 && now > s_subs[i].expiry_time) {
            ESP_LOGI(TAG, "Subscription expired: SID=%s", s_subs[i].sid);
            s_subs[i].url[0] = '\0';
            continue;
        }

        /* 解析 URL: http://host[:port]/path */
        const char *p = s_subs[i].url;
        if (strncmp(p, "http://", 7) != 0) { ESP_LOGW(TAG, "Bad URL: %s", p); continue; }
        p += 7;
        char host[128] = "";
        int port = 80;
        const char *path = "/";
        int hi = 0;
        while (*p && *p != '/' && *p != ':' && hi < 127) host[hi++] = *p++;
        host[hi] = '\0';
        if (*p == ':') { p++; port = atoi(p); while (*p && *p != '/') p++; }
        if (*p == '/') path = p;

        /* 创建 socket */
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) { ESP_LOGW(TAG, "GENA socket create failed: %d", errno); continue; }
        struct timeval tv = { .tv_sec = 2 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(port) };
        inet_pton(AF_INET, host, &addr.sin_addr);
        if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            ESP_LOGW(TAG, "GENA connect %s:%d failed: %d", host, port, errno);
            close(sock); continue;
        }

        /* 构建 HTTP NOTIFY 请求 */
        char seq_str[16];
        snprintf(seq_str, sizeof(seq_str), "%d", s_subs[i].seq++);
        char req[4096];
        int req_len = snprintf(req, sizeof(req),
            "NOTIFY %s HTTP/1.1\r\n"
            "HOST: %s:%d\r\n"
            "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
            "NT: upnp:event\r\n"
            "NTS: upnp:propchange\r\n"
            "SID: %s\r\n"
            "SEQ: %s\r\n"
            "CONTENT-LENGTH: %d\r\n"
            "\r\n"
            "%s",
            path, host, port, s_subs[i].sid, seq_str,
            (int)strlen(xml_body), xml_body);

        if (req_len > 0 && req_len < (int)sizeof(req)) {
            int sent = send(sock, req, req_len, 0);
            if (sent < 0) {
                ESP_LOGW(TAG, "GENA send to %s:%d failed: %d", host, port, errno);
            }
            /* 读响应 */
            char resp[256];
            int rlen = recv(sock, resp, sizeof(resp) - 1, 0);
            if (rlen > 0) {
                resp[rlen] = '\0';
                /* 检查非 200 响应 */
                if (strstr(resp, "200") == NULL) {
                    ESP_LOGW(TAG, "GENA NOTIFY resp from %s: %.80s", host, resp);
                }
            } else if (rlen < 0) {
                ESP_LOGW(TAG, "GENA recv from %s:%d failed: %d", host, port, errno);
            }
        }
        close(sock);
    }
    if (s_gena_mutex) xSemaphoreGive(s_gena_mutex);
}

/* ── GENA background task ── */
static void gena_task(void *arg)
{
    (void)arg;
    notify_msg_t msg;
    while (1) {
        if (xQueueReceive(notify_queue, &msg, portMAX_DELAY) == pdTRUE) {
            if (msg.data) {
                gena_notify(msg.data, NULL);
                free(msg.data);
            } else {
                /* NULL data = async signal: run full notify in task context */
                custom_dlna_notify_transport_state();
            }
        }
    }
}

/* ── Cleanup expired subscriptions ── */
static void cleanup_stale_subscriptions(void)
{
    int64_t now = esp_timer_get_time() / 1000000;
    int valid = 0;
    for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
        if (s_subs[i].url[0] == '\0') continue;
        if (s_subs[i].expiry_time > 0 && now > s_subs[i].expiry_time) {
            ESP_LOGI(TAG, "Subscription expired: SID=%s", s_subs[i].sid);
            s_subs[i].url[0] = '\0';
        } else {
            valid++;
        }
    }
    if (valid == 0) s_sub_count = 0;
}

/* ── Find subscriber by SID ── */
static int find_sub_by_sid(const char *sid)
{
    for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
        if (s_subs[i].sid[0] != '\0' && strcmp(s_subs[i].sid, sid) == 0)
            return i;
    }
    return -1;
}

/* ── Find free subscriber slot ── */
static int find_free_slot(void)
{
    for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
        if (s_subs[i].url[0] == '\0') return i;
    }
    return -1;
}

void custom_dlna_notify_transport_state(void)
{
    cleanup_stale_subscriptions();

    const char *state = s_cfg->get_transport_state ? s_cfg->get_transport_state() : DLNA_STATE_STOPPED;
    const char *uri = s_uri[0] ? s_uri : "";
    const char *meta = s_metadata[0] ? s_metadata : "";

    /* 动态分配：metadata 可能很大 */
    int meta_len = strlen(meta);
    int buf_size = 2048 + meta_len * 4;  /* XML entity encoding expands ~4x */
    char *buf = malloc(buf_size);
    if (!buf) { ESP_LOGW(TAG, "OOM for AVT notify"); return; }

    /* QQ 音乐需要扩展 LastChange 字段（TransportStatus=OK 等），
     * 网易云 3.4 用简单格式，多了反而不同步
     * 参考 Sparrow 逆向：完整 15 字段模板 */
    int n;
    if (s_music_source == MUSIC_SRC_QQ) {
        /* 格式化时长和位置 */
        int dur_sec = s_cfg->get_duration_sec ? s_cfg->get_duration_sec() : 0;
        int pos_sec = s_cfg->get_position_sec ? s_cfg->get_position_sec() : 0;
        char dur_str[16], pos_str[16];
        fmt_time(dur_sec, dur_str, sizeof(dur_str));
        fmt_time(pos_sec, pos_str, sizeof(pos_str));

        n = snprintf(buf, buf_size,
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
            "<e:propertyset xmlns:e=\"urn:schemas-upnp-org:event-1-0\">"
            "<e:property><LastChange>"
            "&lt;Event xmlns=&quot;urn:schemas-upnp-org:metadata-1-0/AVT/&quot;&gt;"
            "&lt;InstanceID val=&quot;0&quot;&gt;"
            "&lt;TransportState val=&quot;%s&quot;/&gt;"
            "&lt;TransportStatus val=&quot;OK&quot;/&gt;"
            "&lt;TransportPlaySpeed val=&quot;1&quot;/&gt;"
            "&lt;NumberOfTracks val=&quot;1&quot;/&gt;"
            "&lt;CurrentTrack val=&quot;1&quot;/&gt;"
            "&lt;CurrentTrackDuration val=&quot;%s&quot;/&gt;"
            "&lt;CurrentMediaDuration val=&quot;%s&quot;/&gt;"
            "&lt;CurrentTrackMetaData val=&quot;%s&quot;/&gt;"
            "&lt;CurrentTrackURI val=&quot;%s&quot;/&gt;"
            "&lt;AVTransportURI val=&quot;%s&quot;/&gt;"
            "&lt;AVTransportURIMetaData val=&quot;%s&quot;/&gt;"
            "&lt;NextAVTransportURI val=&quot;%s&quot;/&gt;"
            "&lt;NextAVTransportURIMetaData val=&quot;%s&quot;/&gt;"
            "&lt;RelativeTimePosition val=&quot;%s&quot;/&gt;"
            "&lt;AbsoluteTimePosition val=&quot;%s&quot;/&gt;"
            "&lt;CurrentTransportActions val=&quot;PLAY,STOP,PAUSE,SEEK&quot;/&gt;"
            "&lt;/InstanceID&gt;"
            "&lt;/Event&gt;"
            "</LastChange></e:property>"
            "</e:propertyset>",
            state,
            dur_str, dur_str,
            meta, uri, uri, meta,
            s_next_uri, s_next_metadata,
            pos_str, pos_str);
    } else {
        /* 网易云 3.4：简单格式，只通知状态变化 */
        n = snprintf(buf, buf_size,
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
            "<e:propertyset xmlns:e=\"urn:schemas-upnp-org:event-1-0\">"
            "<e:property><LastChange>"
            "&lt;Event xmlns=&quot;urn:schemas-upnp-org:metadata-1-0/AVT/&quot;&gt;"
            "&lt;InstanceID val=&quot;0&quot;&gt;"
            "&lt;TransportState val=&quot;%s&quot;/&gt;"
            "&lt;/InstanceID&gt;"
            "&lt;/Event&gt;"
            "</LastChange></e:property>"
            "</e:propertyset>",
            state);
    }

    if (n <= 0 || n >= buf_size) { free(buf); return; }

    gena_notify(buf, NULL);
    free(buf);
}

/* Async transport notify: sends signal to gena_task which calls the full
 * custom_dlna_notify_transport_state() in its own task context.
 * Safe to call from esp_audio callbacks (won't call esp_audio_duration_get).
 */
void custom_dlna_notify_transport_state_async(void)
{
    notify_msg_t msg;
    msg.data = NULL;  /* NULL signals: just call notify_transport_state */
    if (notify_queue) {
        if (xQueueSend(notify_queue, &msg, 0) != pdTRUE) {
            ESP_LOGW(TAG, "Notify queue full (AVT async)");
        }
    }
}

void custom_dlna_notify_rcs(void)
{
    cleanup_stale_subscriptions();

    int vol = s_cfg->get_volume ? s_cfg->get_volume() : s_volume_cache;
    int mute = s_cfg->get_mute ? s_cfg->get_mute() : s_mute_cache;
    notify_msg_t msg;
    msg.data = malloc(1024);
    if (!msg.data) { ESP_LOGW(TAG, "OOM for RCS notify"); return; }
    snprintf(msg.data, 1024,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<e:propertyset xmlns:e=\"urn:schemas-upnp-org:event-1-0\">"
        "<e:property><Volume>%d</Volume><Mute>%d</Mute></e:property>"
        "</e:propertyset>", vol, mute);
    if (notify_queue) {
        if (xQueueSend(notify_queue, &msg, 0) != pdTRUE) {
            ESP_LOGW(TAG, "Notify queue full (RCS)");
            free(msg.data);
        }
    } else {
        gena_notify(msg.data, NULL);
        free(msg.data);
    }
}

/* ── Periodic position notify: timer gives semaphore, task does the work ── */
static SemaphoreHandle_t s_pos_notify_sem = NULL;

static void periodic_pos_notify_cb(void *arg)
{
    (void)arg;
    if (s_pos_notify_sem) xSemaphoreGive(s_pos_notify_sem);
}

static void pos_notify_task(void *arg)
{
    (void)arg;
    while (1) {
        if (xSemaphoreTake(s_pos_notify_sem, portMAX_DELAY) == pdTRUE) {
            if (!s_cfg->get_transport_state) continue;
            const char *state = s_cfg->get_transport_state();
            if (!state || strcmp(state, DLNA_STATE_PLAYING) != 0) continue;
            custom_dlna_notify_transport_state();
        }
    }
}

/* ── SOAP response builder ── */
static void soap_response(httpd_req_t *req, const char *service, const char *action,
                          const char *params_xml)
{
    char resp[2048];
    int n = snprintf(resp, sizeof(resp),
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\""
        " s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body>"
        "<u:%sResponse xmlns:u=\"urn:schemas-upnp-org:service:%s:1\">"
        "%s"
        "</u:%sResponse>"
        "</s:Body></s:Envelope>",
        action, service, params_xml ? params_xml : "", action);
    if (n > 0 && n < (int)sizeof(resp)) send_ok(req, resp);
    else httpd_resp_send_500(req);
}

/* ── SOAP dispatch: AVTransport ── */
static void handle_avt_control(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 32768) {
        httpd_resp_send_500(req);
        return;
    }
    char *body = malloc(req->content_len + 1);
    if (!body) { httpd_resp_send_500(req); return; }
    int body_len = 0;
    while (body_len < req->content_len) {
        int ret = httpd_req_recv(req, body + body_len, req->content_len - body_len);
        if (ret <= 0) { free(body); httpd_resp_send_500(req); return; }
        body_len += ret;
    }
    body[body_len] = '\0';

    /* Parse action from SOAPAction header (miair-next 方式) */
    char action_buf[128] = {0};
    const char *action = get_soap_action(req, action_buf, sizeof(action_buf));
    ESP_LOGI(TAG, "SOAP action=[%s] len=%d", action ? action : "?", body_len);

    if (!action) {
        ESP_LOGW(TAG, "No SOAPAction header");
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SOAPAction");
        return;
    }

    /* ── SetAVTransportURI ── */
    if (strcmp(action, "SetAVTransportURI") == 0) {
        char uri[2048] = {0};
        /* 记录 User-Agent 用于音乐源检测 */
        httpd_req_get_hdr_value_str(req, "User-Agent", s_user_agent, sizeof(s_user_agent));
        if (xml_get(body, "CurrentURI", uri, sizeof(uri)) >= 0 && uri[0]) {
            snprintf(s_uri, sizeof(s_uri), "%s", uri);
            /* 保存元数据（歌词/封面依赖此数据） */
            xml_get(body, "CurrentURIMetaData", s_metadata, sizeof(s_metadata));
            ESP_LOGI(TAG, "SetAVTransportURI meta len=%d: %.200s",
                     (int)strlen(s_metadata), s_metadata);
            if (s_cfg->on_set_uri) s_cfg->on_set_uri(uri);
            if (s_cfg->on_set_metadata) s_cfg->on_set_metadata(s_metadata);
            ESP_LOGI(TAG, "SetAVTransportURI: %s", s_uri);
            custom_dlna_notify_transport_state_async();
        }
        soap_response(req, "AVTransport", "SetAVTransportURI", NULL);

    /* ── Play ── */
    } else if (strcmp(action, "Play") == 0) {
        ESP_LOGI(TAG, "Play: uri=%s", s_uri);
        if (s_cfg->on_play) s_cfg->on_play();
        soap_response(req, "AVTransport", "Play", "<Speed>1</Speed>");

    /* ── Pause ── */
    } else if (strcmp(action, "Pause") == 0) {
        if (s_cfg->on_pause) s_cfg->on_pause();
        soap_response(req, "AVTransport", "Pause", NULL);

    /* ── Stop ── */
    } else if (strcmp(action, "Stop") == 0) {
        if (s_cfg->on_stop) s_cfg->on_stop();
        soap_response(req, "AVTransport", "Stop", NULL);

    /* ── Seek ── */
    } else if (strcmp(action, "Seek") == 0) {
        int sec = parse_seek_target(body);
        if (sec >= 0 && s_cfg->on_seek) s_cfg->on_seek(sec);
        soap_response(req, "AVTransport", "Seek", NULL);

    /* ── Next / Previous ── */
    } else if (strcmp(action, "Next") == 0) {
        if (s_cfg->on_next) s_cfg->on_next();
        soap_response(req, "AVTransport", "Next", NULL);
    } else if (strcmp(action, "Previous") == 0) {
        if (s_cfg->on_previous) s_cfg->on_previous();
        soap_response(req, "AVTransport", "Previous", NULL);

    /* ── GetPositionInfo ── */
    } else if (strcmp(action, "GetPositionInfo") == 0) {
        send_position_info(req);

    /* ── GetTransportInfo ── */
    } else if (strcmp(action, "GetTransportInfo") == 0) {
        send_transport_info(req);

    /* ── GetMediaInfo ── */
    } else if (strcmp(action, "GetMediaInfo") == 0) {
        send_media_info(req);

    /* ── SetPlayMode (stub) ── */
    } else if (strcmp(action, "SetPlayMode") == 0) {
        soap_response(req, "AVTransport", "SetPlayMode", NULL);

    /* ── SetNextAVTransportURI ── */
    } else if (strcmp(action, "SetNextAVTransportURI") == 0) {
        char next_uri[2048] = {0};
        char next_meta[2048] = {0};
        xml_get(body, "NextURI", next_uri, sizeof(next_uri));
        xml_get(body, "NextURIMetaData", next_meta, sizeof(next_meta));
        snprintf(s_next_uri, sizeof(s_next_uri), "%s", next_uri);
        snprintf(s_next_metadata, sizeof(s_next_metadata), "%s", next_meta);
        if (s_cfg->on_set_next_uri) s_cfg->on_set_next_uri(next_uri, next_meta);
        soap_response(req, "AVTransport", "SetNextAVTransportURI", NULL);

    /* ── GetDeviceCapabilities ── */
    } else if (strcmp(action, "GetDeviceCapabilities") == 0) {
        soap_response(req, "AVTransport", "GetDeviceCapabilities",
            "<PlayMedia>NETWORK</PlayMedia>"
            "<RecMedia>NOT_IMPLEMENTED</RecMedia>"
            "<RecQualityModes>NOT_IMPLEMENTED</RecQualityModes>");

    /* ── GetTransportSettings ── */
    } else if (strcmp(action, "GetTransportSettings") == 0) {
        soap_response(req, "AVTransport", "GetTransportSettings",
            "<PlayMode>NORMAL</PlayMode>"
            "<RecQualityMode>NOT_IMPLEMENTED</RecQualityMode>");

    /* ── GetCurrentTransportActions ── */
    } else if (strcmp(action, "GetCurrentTransportActions") == 0) {
        const char *acts = "Play,Pause,Stop,Seek,Next,Previous";
        const char *st = s_cfg->get_transport_state ? s_cfg->get_transport_state() : "STOPPED";
        if (strcmp(st, "PLAYING") == 0) acts = "Pause,Stop,Seek,Next,Previous";
        else if (strcmp(st, "PAUSED_PLAYBACK") == 0) acts = "Play,Stop,Seek,Next,Previous";
        char params[128];
        snprintf(params, sizeof(params), "<Actions>%s</Actions>", acts);
        soap_response(req, "AVTransport", "GetCurrentTransportActions", params);

    } else {
        ESP_LOGW(TAG, "Unknown AVT action: %s", action);
        soap_response(req, "AVTransport", action, NULL);
    }
    free(body);
}

/* ── AVTransport SOAP handler wrapper (returns esp_err_t for httpd) ── */
static esp_err_t avt_control_handler(httpd_req_t *req)
{
    handle_avt_control(req);
    return ESP_OK;
}

/* ── SOAP stub helper ── */
static void soap_ok_stub(httpd_req_t *req, const char *service, const char *action)
{
    static const char tpl[] =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\""
        " s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body>"
        "<u:%sResponse xmlns:u=\"urn:schemas-upnp-org:service:%s:1\">"
        "</u:%sResponse>"
        "</s:Body></s:Envelope>";
    char body[512];
    snprintf(body, sizeof(body), tpl, action, service, action);
    send_ok(req, body);
}

/* ── SOAP: RenderingControl ── */
static esp_err_t rc_control_handler(httpd_req_t *req)
{
    char body[2048];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len > 0) {
        body[len] = '\0';

        char action_buf[128] = {0};
        const char *action = get_soap_action(req, action_buf, sizeof(action_buf));
        if (!action) return ESP_OK;

        if (strcmp(action, "GetVolume") == 0) {
            int vol = s_cfg->get_volume ? s_cfg->get_volume() : s_volume_cache;
            char params[64];
            snprintf(params, sizeof(params), "<CurrentVolume>%d</CurrentVolume>", vol);
            soap_response(req, "RenderingControl", "GetVolume", params);

        } else if (strcmp(action, "SetVolume") == 0) {
            char buf[16];
            if (xml_get(body, "DesiredVolume", buf, sizeof(buf)) >= 0) {
                s_volume_cache = atoi(buf);
                if (s_cfg->on_set_volume) s_cfg->on_set_volume(s_volume_cache);
            }
            soap_response(req, "RenderingControl", "SetVolume", NULL);

        } else if (strcmp(action, "GetMute") == 0) {
            int mute = s_cfg->get_mute ? s_cfg->get_mute() : s_mute_cache;
            char params[64];
            snprintf(params, sizeof(params), "<CurrentMute>%d</CurrentMute>", mute);
            soap_response(req, "RenderingControl", "GetMute", params);

        } else if (strcmp(action, "SetMute") == 0) {
            char buf[16];
            if (xml_get(body, "DesiredMute", buf, sizeof(buf)) >= 0) {
                s_mute_cache = atoi(buf);
                if (s_cfg->on_set_mute) s_cfg->on_set_mute(s_mute_cache);
            }
            soap_response(req, "RenderingControl", "SetMute", NULL);

        } else if (strcmp(action, "ListPresets") == 0) {
            soap_response(req, "RenderingControl", "ListPresets",
                "<CurrentPresetNameList>FactoryDefaults</CurrentPresetNameList>");

        } else if (strcmp(action, "SelectPreset") == 0) {
            soap_response(req, "RenderingControl", "SelectPreset", NULL);

        } else {
            ESP_LOGW(TAG, "Unknown RC action: %s", action);
            soap_response(req, "RenderingControl", action, NULL);
        }
    }
    return ESP_OK;
}

/* ── SOAP: ConnectionManager ── */
static esp_err_t cm_control_handler(httpd_req_t *req)
{
    char body[2048];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len > 0) {
        body[len] = '\0';
        char action_buf[128] = {0};
        const char *action = get_soap_action(req, action_buf, sizeof(action_buf));
        if (!action) return ESP_OK;

        if (strcmp(action, "GetProtocolInfo") == 0) {
            soap_response(req, "ConnectionManager", "GetProtocolInfo",
                "<Source></Source>"
                "<Sink>" SUPPORTED_PROTOCOLS "</Sink>");
        } else if (strcmp(action, "GetCurrentConnectionIDs") == 0) {
            soap_response(req, "ConnectionManager", "GetCurrentConnectionIDs",
                "<ConnectionIDs>0</ConnectionIDs>");
        } else if (strcmp(action, "GetCurrentConnectionInfo") == 0) {
            soap_response(req, "ConnectionManager", "GetCurrentConnectionInfo",
                "<RcsID>0</RcsID>"
                "<AVTransportID>0</AVTransportID>"
                "<ProtocolInfo></ProtocolInfo>"
                "<PeerConnectionManager></PeerConnectionManager>"
                "<PeerConnectionID>-1</PeerConnectionID>"
                "<Direction>Input</Direction>"
                "<Status>OK</Status>");
        } else {
            ESP_LOGW(TAG, "Unknown CM action: %s", action);
            soap_response(req, "ConnectionManager", action, NULL);
        }
    }
    return ESP_OK;
}

/* ── XML descriptor handler ── */
static esp_err_t xml_handler(httpd_req_t *req)
{
    typedef struct { const uint8_t *start; const uint8_t *end; } xi;
    xi *info = (xi *)req->user_ctx;
    int len = info->end - info->start;
    httpd_resp_set_type(req, "text/xml; charset=utf-8");
    httpd_resp_send(req, (const char *)info->start, len);
    return ESP_OK;
}

/* ── GENA event subscription handler ── */
static esp_err_t event_handler(httpd_req_t *req)
{
    if (req->method == HTTP_SUBSCRIBE) {
        /* Check for SID header (renewal) */
        size_t sid_len = httpd_req_get_hdr_value_len(req, "SID");
        if (sid_len > 0 && sid_len < 63) {
            /* Renewal: update expiry time */
            char sid[64];
            httpd_req_get_hdr_value_str(req, "SID", sid, sizeof(sid));
            int idx = find_sub_by_sid(sid);
            if (idx >= 0) {
                s_subs[idx].expiry_time = esp_timer_get_time() / 1000000 + SUB_TIMEOUT_SEC;
                /* Renewal 也可能带 CALLBACK 头，更新 URL（URL 可能被清理过） */
                size_t cb_len = httpd_req_get_hdr_value_len(req, "CALLBACK");
                if (cb_len > 0 && cb_len < 255) {
                    char cb[256];
                    httpd_req_get_hdr_value_str(req, "CALLBACK", cb, sizeof(cb));
                    char *url = cb;
                    if (url[0] == '<') url++;
                    char *end = strchr(url, '>');
                    if (end) *end = '\0';
                    if (url[0] && strcmp(s_subs[idx].url, url) != 0) {
                        snprintf(s_subs[idx].url, sizeof(s_subs[idx].url), "%s", url);
                        ESP_LOGI(TAG, "SUBSCRIBE URL updated: %s", url);
                    }
                }
                ESP_LOGI(TAG, "SUBSCRIBE renewed: SID=%s (slot %d, url=%s)", sid, idx,
                         s_subs[idx].url[0] ? s_subs[idx].url : "(none)");
                /* Return 200 with SID and TIMEOUT */
                httpd_resp_set_status(req, "200 OK");
                httpd_resp_set_hdr(req, "SID", s_subs[idx].sid);
                char timeout_str[32];
                snprintf(timeout_str, sizeof(timeout_str), "Second-%d", SUB_TIMEOUT_SEC);
                httpd_resp_set_hdr(req, "TIMEOUT", timeout_str);
                httpd_resp_set_hdr(req, "Server", "ESP32-DLNA/1.0 UPnP/1.0");
                httpd_resp_send(req, "", 0);
                return ESP_OK;
            }

            /* 关键修复：SID 是设备固定 UUID（网易云重启后仍 renew 它）。
             * 此时无活跃订阅，但必须接受 renew 并重建订阅，否则
             * 网易云收不到任何事件 → 界面卡在"暂停"。 */
            const char *dev_uuid = (s_cfg && s_cfg->uuid) ? s_cfg->uuid : "8db0797a-f01a-4949-8f59-51188b18180b";
            char fixed_sid[96];
            snprintf(fixed_sid, sizeof(fixed_sid), "uuid:%s", dev_uuid);
            if (strcmp(sid, fixed_sid) == 0) {
                /* 分配一个槽位（保留 CALLBACK 为空，后续 NOTIFY 无法发，但至少
                 * controller 的订阅是"有效"的，且一旦有新 SUBSCRIBE 会更新 URL） */
                int slot = find_free_slot();
                if (slot < 0) {
                    ESP_LOGW(TAG, "SUBSCRIBE rebuild: no free slot");
                    httpd_resp_set_status(req, "500 Internal Server Error");
                    httpd_resp_send(req, "", 0);
                    return ESP_OK;
                }
                snprintf(s_subs[slot].sid, sizeof(s_subs[slot].sid), "%s", fixed_sid);
                s_subs[slot].expiry_time = esp_timer_get_time() / 1000000 + SUB_TIMEOUT_SEC;
                s_subs[slot].seq = 0;
                /* rebuild 时也尝试读 CALLBACK 头 */
                size_t cb_len2 = httpd_req_get_hdr_value_len(req, "CALLBACK");
                if (cb_len2 > 0 && cb_len2 < 255) {
                    char cb2[256];
                    httpd_req_get_hdr_value_str(req, "CALLBACK", cb2, sizeof(cb2));
                    char *url2 = cb2;
                    if (url2[0] == '<') url2++;
                    char *end2 = strchr(url2, '>');
                    if (end2) *end2 = '\0';
                    if (url2[0]) {
                        snprintf(s_subs[slot].url, sizeof(s_subs[slot].url), "%s", url2);
                    }
                }
                ESP_LOGI(TAG, "SUBSCRIBE rebuild fixed SID=%s (slot %d, url=%s)",
                         fixed_sid, slot, s_subs[slot].url[0] ? s_subs[slot].url : "(none)");
                httpd_resp_set_status(req, "200 OK");
                httpd_resp_set_hdr(req, "SID", fixed_sid);
                char timeout_str[32];
                snprintf(timeout_str, sizeof(timeout_str), "Second-%d", SUB_TIMEOUT_SEC);
                httpd_resp_set_hdr(req, "TIMEOUT", timeout_str);
                httpd_resp_set_hdr(req, "Server", "ESP32-DLNA/1.0 UPnP/1.0");
                httpd_resp_send(req, "", 0);
                return ESP_OK;
            }

            ESP_LOGW(TAG, "SUBSCRIBE renew: unknown SID=%s", sid);
            httpd_resp_set_status(req, "412 Precondition Failed");
            httpd_resp_send(req, "", 0);
            return ESP_OK;
        }

        /* New subscription: extract CALLBACK URL */
        size_t cb_len = httpd_req_get_hdr_value_len(req, "CALLBACK");
        if (cb_len > 0 && cb_len < 255) {
            char cb[256];
            httpd_req_get_hdr_value_str(req, "CALLBACK", cb, sizeof(cb));
            char *url = cb;
            if (url[0] == '<') url++;
            char *end = strchr(url, '>');
            if (end) *end = '\0';

            /* Check if already subscribed (same URL) */
            int slot = -1;
            for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
                if (s_subs[i].url[0] != '\0' && strcmp(s_subs[i].url, url) == 0) {
                    slot = i;
                    ESP_LOGI(TAG, "SUBSCRIBE renew by URL: %s (slot %d)", url, i);
                    break;
                }
            }

            /* 优先复用固定 SID 槽（slot 0），填充 URL。
             * 这样后续 renew 固定 SID 时能找到完整订阅（含 URL）。 */
            if (slot < 0) {
                if (s_subs[0].url[0] == '\0') {
                    slot = 0;   /* 固定槽无 URL → 填充 */
                } else {
                    slot = find_free_slot();
                    if (slot < 0) {
                        ESP_LOGW(TAG, "SUBSCRIBE: no free slot for %s", url);
                        httpd_resp_set_status(req, "500 Internal Server Error");
                        httpd_resp_send(req, "No capacity", -1);
                        return ESP_OK;
                    }
                }
            }

            /* Register/update subscription.
             * SID 固定为设备 UUID：网易云只在首次连接时 SUBSCRIBE，
             * 之后一直 renew 同一 SID。若 SID 随重启变化，重启后网易云
             * renew 找不到 → 订阅永久失效 → 收不到任何事件。
             * 固定 SID = 设备重启后 renew 仍能找到。 */
            const char *dev_uuid = (s_cfg && s_cfg->uuid) ? s_cfg->uuid : "8db0797a-f01a-4949-8f59-51188b18180b";
            snprintf(s_subs[slot].sid, sizeof(s_subs[slot].sid),
                     "uuid:%s", dev_uuid);
            snprintf(s_subs[slot].url, sizeof(s_subs[slot].url), "%s", url);
            s_subs[slot].expiry_time = esp_timer_get_time() / 1000000 + SUB_TIMEOUT_SEC;
            s_subs[slot].seq = 0;

            ESP_LOGI(TAG, "SUBSCRIBE new: %s -> SID=%s (slot %d)",
                     url, s_subs[slot].sid, slot);

            /* Response with required headers */
            httpd_resp_set_status(req, "200 OK");
            httpd_resp_set_hdr(req, "SID", s_subs[slot].sid);
            char timeout_str[32];
            snprintf(timeout_str, sizeof(timeout_str), "Second-%d", SUB_TIMEOUT_SEC);
            httpd_resp_set_hdr(req, "TIMEOUT", timeout_str);
            httpd_resp_set_hdr(req, "Server", "ESP32-DLNA/1.0 UPnP/1.0");
            httpd_resp_send(req, "", 0);

            /* Send immediate initial event so app gets current state */
            custom_dlna_notify_transport_state_async();
            custom_dlna_notify_rcs();

            return ESP_OK;
        }

        ESP_LOGW(TAG, "SUBSCRIBE without CALLBACK or SID");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing CALLBACK or SID");
        return ESP_OK;

    } else if (req->method == HTTP_UNSUBSCRIBE) {
        size_t sid_len = httpd_req_get_hdr_value_len(req, "SID");
        if (sid_len > 0 && sid_len < 63) {
            char sid[64];
            httpd_req_get_hdr_value_str(req, "SID", sid, sizeof(sid));
            int idx = find_sub_by_sid(sid);
            if (idx >= 0) {
                ESP_LOGI(TAG, "UNSUBSCRIBE: SID=%s (slot %d)", sid, idx);
                s_subs[idx].url[0] = '\0';
            }
        }
        httpd_resp_set_status(req, "200 OK");
        httpd_resp_send(req, "", 0);
        return ESP_OK;
    }

    /* Read body if present */
    if (req->content_len > 0) {
        size_t rl = req->content_len;
        if (rl > 4096) rl = 4096;
        char *buf = malloc(rl + 1);
        if (buf) {
            int total = 0;
            while (total < (int)rl) {
                int ret = httpd_req_recv(req, buf + total, rl - total);
                if (ret <= 0) break;
                total += ret;
            }
            buf[total] = '\0';
            ESP_LOGD(TAG, "EVENT body: %s", buf);
            free(buf);
        }
    }

    httpd_resp_set_status(req, "200 OK");
    httpd_resp_send(req, "", 0);
    return ESP_OK;
}

/* ── 404 handler ── */
static esp_err_t my_404_handler(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    if (strstr(req->uri, "renderingcontrol")) {
        httpd_resp_set_status(req, "200 OK");
        httpd_resp_set_type(req, "text/xml; charset=utf-8");
        httpd_resp_send(req, "", 0);
        return ESP_OK;
    }
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_send(req, "Not Found", 9);
    return ESP_OK;
}

/* ── Wildcard diagnostic handler: log every request we did NOT explicitly
 *    register. Lets us see what the control point actually asks for
 *    (e.g. /description.xml, /upnp/control/AVTransport w/ different case). */
static esp_err_t wildcard_handler(httpd_req_t *req)
{
    char ua[128] = "";
    httpd_req_get_hdr_value_str(req, "User-Agent", ua, sizeof(ua));
    ESP_LOGW(TAG, "UNMATCHED req: method=%d uri='%s' len=%d UA='%s'",
             (int)req->method, req->uri, req->content_len, ua);
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "not found", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ── SSDP periodic alive notify (keep device visible) ── */
/* ── SSDP byebye: notify network we're leaving ── */
static void ssdp_send_byebye(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return;
    struct sockaddr_in dest = { .sin_family = AF_INET, .sin_port = htons(SSDP_PORT) };
    inet_pton(AF_INET, SSDP_MULTICAST, &dest.sin_addr);
    static const char *svc[] = {
        "upnp:rootdevice",
        "urn:schemas-upnp-org:device:MediaRenderer:1",
        "urn:schemas-upnp-org:service:AVTransport:1",
        "urn:schemas-upnp-org:service:RenderingControl:1",
        "urn:schemas-upnp-org:service:ConnectionManager:1",
    };
    for (int i = 0; i < 5; i++) {
        char msg[512];
        int len = snprintf(msg, sizeof(msg),
            "NOTIFY * HTTP/1.1\r\n"
            "HOST: 239.255.255.250:1900\r\n"
            "NTS: ssdp:byebye\r\n"
            "NT: %s\r\n"
            "USN: uuid:%s::%s\r\n\r\n",
            svc[i], s_cfg->uuid, svc[i]);
        sendto(sock, msg, len, 0, (struct sockaddr *)&dest, sizeof(dest));
    }
    close(sock);
    ESP_LOGI(TAG, "SSDP byebye sent");
}

static void ssdp_alive_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { ESP_LOGE(TAG, "SSDP alive socket failed"); vTaskDelete(NULL); return; }
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port = htons(SSDP_PORT),
    };
    inet_pton(AF_INET, SSDP_MULTICAST, &dest.sin_addr);
    int ttl = 4;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    /* Also send unicast to gateway (phone hotspot doesn't relay multicast well) */
    struct sockaddr_in gw = { .sin_family = AF_INET, .sin_port = htons(SSDP_PORT) };
    const char *gw_str = "0.0.0.0";
    inet_pton(AF_INET, gw_str, &gw.sin_addr);

    int alive_cnt = 0;
    while (1) {
        /* 30 秒 ± 5 秒随机抖动（miair-next 设计） */
        int jitter = (esp_random() % 10000) - 5000; /* -5000 ~ +4999 ms */
        vTaskDelay(pdMS_TO_TICKS(30000 + jitter));
        const char *ip = get_local_ip();
        if (!ip || ip[0] == '\0' || strcmp(ip, "0.0.0.0") == 0) {
            ESP_LOGW(TAG, "SSDP alive: no IP yet");
            continue;
        }
        /* Update gateway dynamically (hotspot IP may change across reconnects) */
        const char *gw_dyn = get_local_gateway();
        if (gw_dyn && gw_dyn[0] && strcmp(gw_dyn, "0.0.0.0") != 0) {
            inet_pton(AF_INET, gw_dyn, &gw.sin_addr);
        }
        static const char *svc[] = {
            "upnp:rootdevice",
            "urn:schemas-upnp-org:device:MediaRenderer:1",
            "urn:schemas-upnp-org:service:AVTransport:1",
            "urn:schemas-upnp-org:service:RenderingControl:1",
            "urn:schemas-upnp-org:service:ConnectionManager:1",
        };
        for (int i = 0; i < 5; i++) {
            char msg[512];
            int len = snprintf(msg, sizeof(msg),
                "NOTIFY * HTTP/1.1\r\n"
                "HOST: 239.255.255.250:1900\r\n"
                "CACHE-CONTROL: max-age=1800\r\n"
                "LOCATION: http://%s:%d/device.xml\r\n"
                "SERVER: ESP32-DLNA/1.0 UPnP/1.0\r\n"
                "NTS: ssdp:alive\r\n"
                "NT: %s\r\n"
                "USN: uuid:%s::%s\r\n\r\n",
                ip, s_cfg->port, svc[i], s_cfg->uuid, svc[i]);
            sendto(sock, msg, len, 0, (struct sockaddr *)&dest, sizeof(dest));
            /* Unicast to gateway for hotspot compatibility */
            sendto(sock, msg, len, 0, (struct sockaddr *)&gw, sizeof(gw));
        }
        if (++alive_cnt % 4 == 0) /* log every ~60s */
            ESP_LOGI(TAG, "SSDP alive sent #%d (ip=%s)", alive_cnt, ip);
    }
    close(sock);
}

/* ── SSDP M-SEARCH responder ── */
static void ssdp_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { ESP_LOGE(TAG, "SSDP socket failed"); vTaskDelete(NULL); return; }
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(SSDP_PORT),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "SSDP bind failed"); close(sock); vTaskDelete(NULL); return;
    }
    struct ip_mreq mreq;
    inet_pton(AF_INET, SSDP_MULTICAST, &mreq.imr_multiaddr);
    mreq.imr_interface.s_addr = INADDR_ANY;
    setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    ESP_LOGI(TAG, "SSDP listening on port %d", SSDP_PORT);

    char *buf = malloc(2048);
    if (!buf) { close(sock); vTaskDelete(NULL); return; }

    while (1) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int n = recvfrom(sock, buf, 2047, 0, (struct sockaddr *)&from, &fromlen);
        if (n > 0) {
            buf[n] = '\0';
            if (!strstr(buf, "M-SEARCH")) continue;
            if (s_ssdp_suppressed) continue;  /* MiPlay 模式下不响应 SSDP */
            ESP_LOGI(TAG, "SSDP M-SEARCH from %s:%d (%d bytes)",
                     inet_ntoa(from.sin_addr), ntohs(from.sin_port), n);
            /* Replace \r\n with spaces for single-line logging */
            for (int j = 0; j < n; j++) {
                if (buf[j] == '\r' || buf[j] == '\n') buf[j] = '|';
            }
            ESP_LOGI(TAG, "  RAW: %.200s", buf);
            char *st = strstr(buf, "ST:");
            if (!st) continue;
            st += 3;
            while (isspace((unsigned char)*st)) st++;
            char stv[128] = {0};
            int i = 0;
            while (st[i] != '\r' && st[i] != '\n' && st[i] && i < 127) {
                stv[i] = st[i]; i++;
            }
            ESP_LOGI(TAG, "  ST='%s'", stv);
            bool all = !!strstr(stv, "ssdp:all");
            /* 随机延迟 0~1 秒响应（避免多设备同时响应造成网络风暴） */
            vTaskDelay(pdMS_TO_TICKS(esp_random() % 1000));
            if (all || strstr(stv, "rootdevice")) {
                char resp[512];
                int len = snprintf(resp, sizeof(resp),
                    "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=1800\r\nEXT:\r\n"
                    "LOCATION: http://%s:%d/device.xml\r\n"
                    "SERVER: ESP32-DLNA/1.0 UPnP/1.0\r\n"
                    "ST: upnp:rootdevice\r\n"
                    "USN: uuid:%s::upnp:rootdevice\r\n\r\n",
                    get_local_ip(), s_cfg->port, s_cfg->uuid);
                sendto(sock, resp, len, 0, (struct sockaddr *)&from, sizeof(from));
            }
            if (all || strstr(stv, "MediaRenderer")) {
                char resp[512];
                int len = snprintf(resp, sizeof(resp),
                    "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=1800\r\nEXT:\r\n"
                    "LOCATION: http://%s:%d/device.xml\r\n"
                    "SERVER: ESP32-DLNA/1.0 UPnP/1.0\r\n"
                    "ST: urn:schemas-upnp-org:device:MediaRenderer:1\r\n"
                    "USN: uuid:%s::urn:schemas-upnp-org:device:MediaRenderer:1\r\n\r\n",
                    get_local_ip(), s_cfg->port, s_cfg->uuid);
                sendto(sock, resp, len, 0, (struct sockaddr *)&from, sizeof(from));
            }
            if (all || strstr(stv, "AVTransport")) {
                char resp[512];
                int len = snprintf(resp, sizeof(resp),
                    "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=1800\r\nEXT:\r\n"
                    "LOCATION: http://%s:%d/device.xml\r\n"
                    "SERVER: ESP32-DLNA/1.0 UPnP/1.0\r\n"
                    "ST: urn:schemas-upnp-org:service:AVTransport:1\r\n"
                    "USN: uuid:%s::urn:schemas-upnp-org:service:AVTransport:1\r\n\r\n",
                    get_local_ip(), s_cfg->port, s_cfg->uuid);
                sendto(sock, resp, len, 0, (struct sockaddr *)&from, sizeof(from));
            }
        }
    }
    free(buf);
}

/* ── Public API ── */
esp_err_t custom_dlna_init(const custom_dlna_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    s_cfg = config;
    s_volume_cache = 50;
    s_mute_cache = 0;
    memset(s_subs, 0, sizeof(s_subs));
    s_sub_count = 0;
    s_next_sub_id = 1;

    /* 预置固定 SID 槽位（slot 0）：SID = 设备 UUID。
     * 网易云只在首次连接 SUBSCRIBE，之后一直 renew 同一 SID。
     * 预置该槽后，重启后 renew 能匹配，且首次 SUBSCRIBE new 会填充 URL。 */
    const char *dev_uuid = config->uuid ? config->uuid : "8db0797a-f01a-4949-8f59-51188b18180b";
    snprintf(s_subs[0].sid, sizeof(s_subs[0].sid), "uuid:%s", dev_uuid);
    s_subs[0].expiry_time = esp_timer_get_time() / 1000000 + SUB_TIMEOUT_SEC;
    s_subs[0].seq = 0;
    s_sub_count = 1;

    /* GENA 通知互斥锁 */
    if (!s_gena_mutex) s_gena_mutex = xSemaphoreCreateMutex();

    /* Start SSDP tasks */
    xTaskCreatePinnedToCore(ssdp_task, "ssdp", 8192, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(ssdp_alive_task, "ssdp_alive", 4096, NULL, 4, NULL, 0);

    /* Start HTTP server */
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = config->port;
    cfg.max_uri_handlers = 24;
    cfg.stack_size = 16384;
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = 7;
    cfg.send_wait_timeout = 10;
    cfg.recv_wait_timeout = 10;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    // C3 单核:显式绑 core0,避免 tskNO_AFFINITY 触发 xTaskCreatePinnedToCore 断言。
    cfg.core_id = 0;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed");
        return ESP_FAIL;
    }

    typedef struct { const uint8_t *start; const uint8_t *end; } xi;
    static xi s_dev_xml = { device_xml_start, device_xml_end };
    static xi s_av_xml  = { avtransport_xml_start, avtransport_xml_end };
    static xi s_rc_xml  = { renderingcontrol_xml_start, renderingcontrol_xml_end };
    static xi s_cm_xml  = { connectionmanager_xml_start, connectionmanager_xml_end };

    httpd_uri_t uris[] = {
        { .uri = "/device.xml",           .method = HTTP_GET, .handler = xml_handler,   .user_ctx = &s_dev_xml },
        { .uri = "/avtransport.xml",      .method = HTTP_GET, .handler = xml_handler,   .user_ctx = &s_av_xml },
        { .uri = "/renderingcontrol.xml", .method = HTTP_GET, .handler = xml_handler,   .user_ctx = &s_rc_xml },
        { .uri = "/connectionmanager.xml",.method = HTTP_GET, .handler = xml_handler,   .user_ctx = &s_cm_xml },
        { .uri = "/upnp/control/avtransport",       .method = HTTP_POST, .handler = avt_control_handler, .user_ctx = NULL },
        { .uri = "/upnp/control/renderingcontrol",  .method = HTTP_POST, .handler = rc_control_handler, .user_ctx = NULL },
        { .uri = "/upnp/control/connectionmanager", .method = HTTP_POST, .handler = cm_control_handler, .user_ctx = NULL },
        { .uri = "/upnp/event/avtransport",         .method = HTTP_ANY, .handler = event_handler, .user_ctx = NULL },
        { .uri = "/upnp/event/renderingcontrol",    .method = HTTP_ANY, .handler = event_handler, .user_ctx = NULL },
        { .uri = "/upnp/event/connectionmanager",   .method = HTTP_ANY, .handler = event_handler, .user_ctx = NULL },
        { .uri = "/*",                               .method = HTTP_ANY, .handler = wildcard_handler, .user_ctx = NULL },
    };
    for (int i = 0; i < sizeof(uris)/sizeof(uris[0]); i++)
        httpd_register_uri_handler(s_server, &uris[i]);
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, my_404_handler);

    /* Start GENA notification background task */
    notify_queue = xQueueCreate(NOTIFY_QUEUE_LEN, sizeof(notify_msg_t));
    if (notify_queue) {
        BaseType_t r = xTaskCreatePinnedToCore(gena_task, "gena_notify", 16384, NULL, 5, NULL, 0);
        if (r != pdPASS) {
            ESP_LOGE(TAG, "Failed to create GENA task");
            vQueueDelete(notify_queue);
            notify_queue = NULL;
        }
    } else {
        ESP_LOGE(TAG, "Failed to create notify queue — GENA disabled");
    }

    /* Start periodic position notify timer (every 1 second) */
    s_pos_notify_sem = xSemaphoreCreateBinary();
    if (s_pos_notify_sem) {
        xTaskCreatePinnedToCore(pos_notify_task, "pos_notify", 8192, NULL, 4, NULL, 0);
        const esp_timer_create_args_t timer_args = {
            .callback = periodic_pos_notify_cb,
            .name = "pos_notify",
        };
        esp_err_t timer_err = esp_timer_create(&timer_args, &s_pos_timer);
        if (timer_err == ESP_OK) {
            esp_timer_start_periodic(s_pos_timer, 3000000); /* 3 seconds */
            ESP_LOGI(TAG, "Position notify timer started (1s interval)");
        } else {
            ESP_LOGW(TAG, "Failed to create position timer: %s", esp_err_to_name(timer_err));
        }
    }

    ESP_LOGI(TAG, "Custom DLNA ready on port %d (UUID: %s)", config->port, config->uuid);
    return ESP_OK;
}
