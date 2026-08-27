// main/dlna_wifi.c —— DLNA 应用的 WiFi 连接 + 配网。
// 复用 demo_radio.c 的 netif/event-loop/NVS 准备模式(只做这一回初始化),
// 并扩展为:读 NVS 里的 SSID/密码 → 尝试连接 → 连不上则转 AP 配网。
// 配网动作(用户连上 AP 后访问 http://192.168.4.1 提交)依赖 demo 层 UI,这里只管
// 网络的连/断状态与凭证读写。
#include "dlna_wifi.h"

#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "dlna_wifi";

#define NVS_NAMESPACE   "dlna_wifi"
#define NVS_KEY_SSID    "ssid"
#define NVS_KEY_PASS    "pass"

#define CONNECT_TIMEOUT_MS  (15000)
#define CONNECT_RETRY_MAX   2

static bool s_nvs_ready;
static bool s_netif_ready;
static bool s_event_loop_ready;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;   // AP 配网模式的 netif(含 DHCP server)
static bool s_wifi_started;

// wait_got_ip 用的 GOT_IP 事件状态。
static volatile bool s_got_ip;
static esp_event_handler_instance_t s_got_ip_inst;
static SemaphoreHandle_t s_got_ip_sem;

// AP 配网热点的 SSID(密码固定,见下)。
#define AP_SSID   "FoloToy-DLNA-AP"
// 热点密码固定为 66495386(用户要求,避免每次开机随机变、手机连不上)。
#define AP_PASS   "66495386"
// 热点密码缓冲(固定密码拷贝到这,供 ap_credentials/pass_get 返回)。
static char s_ap_pass[16];
// AP 配网模式标志。
static bool s_ap_mode;

// 开机预扫的 WiFi 列表缓存(配网页直接用它,不用在线扫)。
#define CACHED_NET_MAX 20
static char s_cached_nets[CACHED_NET_MAX][33];
static int  s_cached_count;

// IP_EVENT_STA_GOT_IP 回调:置位标志并唤醒等待的信号量。
static void got_ip_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id; (void)data;
    s_got_ip = true;
    if (s_got_ip_sem) xSemaphoreGive(s_got_ip_sem);
}

esp_err_t dlna_wifi_init(void)
{
    // 幂等:已初始化则直接返回。nvs/netif/event loop 重复创建会出错(如
    // esp_event_loop_create_default 第二次调用返回 INVALID_STATE)。
    if (s_nvs_ready && s_netif_ready && s_event_loop_ready) return ESP_OK;

    if (!s_nvs_ready) {
        esp_err_t err = nvs_flash_init();
        if (err != ESP_OK) {
            // 不自动擦除可能存在的其它数据(遵循 demo_radio.c 的示例原则)。
            ESP_LOGE(TAG, "NVS 初始化失败: %s;未自动擦除", esp_err_to_name(err));
            return err;
        }
        s_nvs_ready = true;
    }

    if (!s_netif_ready) {
        esp_err_t err = esp_netif_init();
        if (err != ESP_OK) return err;
        s_netif_ready = true;
    }

    if (!s_event_loop_ready) {
        esp_err_t err = esp_event_loop_create_default();
        if (err != ESP_OK) return err;
        s_event_loop_ready = true;
    }

    return ESP_OK;
}

// 读 NVS 里保存的凭证;没有或无效(空 SSID)则返回 false(需要开 AP 配网)。
static bool load_credentials(char *ssid, size_t ssid_max,
                             char *pass, size_t pass_max)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    size_t slen = ssid_max, plen = pass_max;
    esp_err_t e1 = nvs_get_str(h, NVS_KEY_SSID, ssid, &slen);
    esp_err_t e2 = nvs_get_str(h, NVS_KEY_PASS, pass, &plen);
    nvs_close(h);
    // slen == 1 表示只存了 '\0' 空串;ssid[0]=='\0' 也是空。这类凭证无效,必须当没凭证,
    // 否则会拿空 SSID 去连 WiFi → 失败 → 反复转 AP 配网。
    bool ok_ssid = (e1 == ESP_OK && slen > 1 && ssid[0] != '\0');
    bool ok_pass = (e2 == ESP_OK && plen > 1 && pass[0] != '\0');
    return ok_ssid && ok_pass;
}

// 保存凭证(配网成功后由 UI 层调用)。放在这里避免多余头。
esp_err_t dlna_wifi_save_credentials(const char *ssid, const char *pass)
{
    // 拒存空 SSID——否则会存进一个空凭证,下次开机拿去连空 WiFi → 反复失败转 AP。
    if (!ssid || ssid[0] == '\0') { ESP_LOGW(TAG, "拒存空的 SSID"); return ESP_FAIL; }

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return ESP_ERR_NVS_NOT_FOUND;
    esp_err_t e1 = nvs_set_str(h, NVS_KEY_SSID, ssid);
    esp_err_t e2 = nvs_set_str(h, NVS_KEY_PASS, pass);
    esp_err_t e3 = nvs_commit(h);
    nvs_close(h);
    if (e1 != ESP_OK || e2 != ESP_OK || e3 != ESP_OK) return ESP_FAIL;
    ESP_LOGI(TAG, "已保存网络凭证");
    return ESP_OK;
}

// 清除已保存的网络凭证(用于"长按 OK 重置网络")。清掉后重启会回到 AP 配网。
esp_err_t dlna_wifi_clear_credentials(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return ESP_ERR_NVS_NOT_FOUND;
    // 两个 key 都可能不存在(NOT_FOUND 是正常的前一次未保存),只把真正的错误当失败。
    esp_err_t e1 = nvs_erase_key(h, NVS_KEY_SSID);
    esp_err_t e2 = nvs_erase_key(h, NVS_KEY_PASS);
    esp_err_t e3 = nvs_commit(h);
    nvs_close(h);
    if ((e1 != ESP_OK && e1 != ESP_ERR_NVS_NOT_FOUND) ||
        (e2 != ESP_OK && e2 != ESP_ERR_NVS_NOT_FOUND)) return ESP_FAIL;
    if (e3 != ESP_OK) return e3;
    ESP_LOGI(TAG, "已清除网络凭证(重置网络)");
    return e3;
}

// 等待 STA 拿到 IP:注册 GOT_IP 事件,用信号量阻塞,超时返回失败。
// 这在单次调用里完成,避免 connect_to_sta 内乱循环。
static esp_err_t wait_got_ip(const char *ssid)
{
    // 注册 GOT_IP 事件回调(got_ip_cb 置位标志 + give 信号量),然后阻塞等待。
    if (!s_got_ip_sem) s_got_ip_sem = xSemaphoreCreateBinary();
    if (!s_got_ip_sem) return ESP_ERR_NO_MEM;

    s_got_ip = false;
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, got_ip_cb,
                                        NULL, &s_got_ip_inst);

    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_STATE) {  // 已连接时返回 STATE
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_got_ip_inst);
        return err;
    }

    xSemaphoreTake(s_got_ip_sem, pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_got_ip_inst);

    if (s_got_ip) { ESP_LOGI(TAG, "已连上 %s", ssid); return ESP_OK; }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t connect_to_sta(const char *ssid, const char *pass)
{
    if (!s_event_loop_ready || !s_netif_ready) return ESP_ERR_INVALID_STATE;

    if (!s_sta_netif) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        if (!s_sta_netif) return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_WIFI_INIT_STATE) return err;

    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);

    wifi_config_t conf = {0};
    strncpy((char *)conf.sta.ssid, ssid, sizeof(conf.sta.ssid));
    strncpy((char *)conf.sta.password, pass, sizeof(conf.sta.password) - 1);
    conf.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    err = esp_wifi_set_config(WIFI_IF_STA, &conf);
    if (err != ESP_OK) return err;

    err = esp_wifi_start();
    if (err != ESP_OK) return err;
    s_wifi_started = true;

    // DLNA/SSDP 依赖流畅地接收组播(239.255.255.250:1900 的 M-SEARCH)。
    // STA 默认是 WIFI_PS_MIN_MODEM,会在 DTIM 间隙休眠,导致组播帧经常收不到。
    // 关掉 Modem-sleep(改为持续唤醒),保证组播能及时到达(牺牲一点功耗)。
    // 必须在 esp_wifi_start() 之后调用,且需在收到 IP 前生效。
    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_ps(WIFI_PS_NONE) 失败: %s", esp_err_to_name(err));
    }

    return wait_got_ip(ssid);
}

// 固定热点密码:把 AP_PASS 拷入 s_ap_pass(密码每次开机不变,方便用户连热点)。
static void gen_ap_pass(void)
{
    strncpy(s_ap_pass, AP_PASS, sizeof(s_ap_pass) - 1);
    s_ap_pass[sizeof(s_ap_pass) - 1] = '\0';
}

// 启动 softAP 配网热点。用户连上后在手机/电脑访问 http://192.168.4.1 提交 WiFi。
esp_err_t dlna_wifi_start_ap(void)
{
    esp_err_t e = dlna_wifi_init();
    if (e != ESP_OK) return e;

    // 必须为 AP 创建默认 netif —— 只有这样 esp_wifi_start() 才会给热点配
    // 192.168.4.1 并启动内置 DHCP 服务器,手机连上才能拿到 IP、访问配网页。
    // (对比:STA 模式在 connect_to_sta 里建了 esp_netif_create_default_wifi_sta)
    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (!s_ap_netif) { ESP_LOGE(TAG, "创建 AP netif 失败"); return ESP_FAIL; }
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    e = esp_wifi_init(&cfg);
    if (e != ESP_OK && e != ESP_ERR_WIFI_INIT_STATE) return e;

    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_AP);

    // 每次开热点都重新生成 8 位随机数字密码,写进配置与日志(屏幕上也会显示)。
    gen_ap_pass();

    wifi_config_t ap = {0};
    strncpy((char *)ap.ap.ssid, AP_SSID, sizeof(ap.ap.ssid) - 1);
    strncpy((char *)ap.ap.password, s_ap_pass, sizeof(ap.ap.password) - 1);
    ap.ap.ssid_len = strlen(AP_SSID);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;

    e = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (e != ESP_OK) return e;

    e = esp_wifi_start();
    if (e != ESP_OK) return e;
    s_wifi_started = true;
    s_ap_mode = true;

    ESP_LOGI(TAG, "AP 配网已开启: SSID=%s PASS=%s (IP 192.168.4.1)", AP_SSID, s_ap_pass);
    return ESP_OK;
}

bool dlna_wifi_is_ap_mode(void)
{
    return s_ap_mode;
}

int dlna_wifi_scan_networks(char names[][33], int max_networks)
{
    if (max_networks <= 0) return -1;

    // AP 模式下要扫描,必须先有一个 STA netif;没有它,esp_wifi_set_mode(STA)
    // 之后扫描会找不到可用的 STA 接口而崩溃。确保 STA netif 存在。
    if (!s_sta_netif) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        if (!s_sta_netif) {
            ESP_LOGE(TAG, "scan: 创建 STA netif 失败");
            return -1;
        }
    }

    // wifi 必须已 init + start 才能扫描。开机早期(prefetch)可能还没 init,补上。
    if (!s_wifi_started) {
        wifi_init_config_t icfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_err_t ie = esp_wifi_init(&icfg);
        if (ie != ESP_OK && ie != ESP_ERR_WIFI_INIT_STATE) {
            ESP_LOGE(TAG, "scan: esp_wifi_init 失败 %s", esp_err_to_name(ie));
            return -1;
        }
        esp_err_t st = esp_wifi_start();
        if (st != ESP_OK && st != ESP_ERR_WIFI_STATE) {
            ESP_LOGE(TAG, "scan: esp_wifi_start 失败 %s", esp_err_to_name(st));
            return -1;
        }
        s_wifi_started = true;
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // 切到 STA 模式(会短暂断连,手机连的 AP 暂不可用),扫描完恢复 AP。
    esp_err_t e = esp_wifi_set_mode(WIFI_MODE_STA);
    if (e != ESP_OK) { ESP_LOGE(TAG, "scan: 切 STA 失败 %s", esp_err_to_name(e)); return -1; }
    vTaskDelay(pdMS_TO_TICKS(200));

    // 触发扫描(阻塞等完成)。
    wifi_scan_config_t scan_cfg = { .ssid = NULL, .bssid = NULL,
                                     .channel = 0, .scan_type = WIFI_SCAN_TYPE_ACTIVE,
                                     .show_hidden = true };
    e = esp_wifi_scan_start(&scan_cfg, true);
    if (e != ESP_OK) { ESP_LOGE(TAG, "scan start 失败 %s", esp_err_to_name(e)); }

    // 取结果(最多 max_networks 个)。
    int count = 0;
    uint16_t num = 0, got = (uint16_t)max_networks;
    if (e == ESP_OK && esp_wifi_scan_get_ap_num(&num) == ESP_OK) {
        if (num < got) got = num;
        if (got > 0) {
            wifi_ap_record_t *recs = calloc(got, sizeof(wifi_ap_record_t));
            if (recs) {
                if (esp_wifi_scan_get_ap_records(&got, recs) == ESP_OK) {
                    for (uint16_t i = 0; i < got; i++) {
                        // 跳过空 SSID;截断到 32 字节 + 末尾 0。
                        const char *ss = (const char *)recs[i].ssid;
                        size_t len = strlen(ss);
                        if (len > 32) len = 32;
                        memcpy(names[count], ss, len);
                        names[count][len] = '\0';
                        count++;
                    }
                }
                free(recs);
            }
        }
    }
    esp_wifi_scan_stop();

    // 恢复到 AP 模式,让热点继续。
    e = esp_wifi_set_mode(WIFI_MODE_AP);
    vTaskDelay(pdMS_TO_TICKS(300));

    ESP_LOGI(TAG, "扫描到 %d 个 WiFi", count);
    return count;
}

// 开机时预扫一次附近 WiFi 并缓存。放在 connect/开热点之前调用——此时射频尚未被
// AP 占用,能扫到周围网络;之后配网页直接读这份缓存,不必等切 AP 后再在线扫。
// 注意:这里会短暂切 STA 扫描(若当前在 AP 模式),扫描完恢复到当前模式。
void dlna_wifi_prefetch_networks(void)
{
    char tmp[CACHED_NET_MAX][33];
    int n = dlna_wifi_scan_networks(tmp, CACHED_NET_MAX);
    s_cached_count = 0;
    if (n <= 0) { ESP_LOGW(TAG, "预扫未取到 WiFi 列表 (%d)", n); return; }

    // 拷贝到缓存,去重。
    for (int i = 0; i < n && s_cached_count < CACHED_NET_MAX; i++) {
        if (tmp[i][0] == '\0') continue;
        bool dup = false;
        for (int j = 0; j < s_cached_count; j++) {
            if (strcmp(s_cached_nets[j], tmp[i]) == 0) { dup = true; break; }
        }
        if (!dup) {
            strncpy(s_cached_nets[s_cached_count], tmp[i], 32);
            s_cached_nets[s_cached_count][32] = '\0';
            s_cached_count++;
        }
    }
    ESP_LOGI(TAG, "预扫缓存 %d 个 WiFi", s_cached_count);
}

int dlna_wifi_get_cached_networks(char names[][33], int max_networks)
{
    if (!names || max_networks <= 0) return 0;
    int n = s_cached_count < max_networks ? s_cached_count : max_networks;
    for (int i = 0; i < n; i++) {
        strncpy(names[i], s_cached_nets[i], 32);
        names[i][32] = '\0';
    }
    return n;
}

void dlna_wifi_ap_credentials(char *ssid, size_t ssid_max,
                              char *pass, size_t pass_max)
{
    if (s_ap_mode) {
        if (ssid && ssid_max > 0) { strncpy(ssid, AP_SSID, ssid_max - 1); ssid[ssid_max - 1] = '\0'; }
        if (pass && pass_max > 0) { strncpy(pass, s_ap_pass, pass_max - 1); pass[pass_max - 1] = '\0'; }
    } else {
        if (ssid && ssid_max > 0) ssid[0] = '\0';
        if (pass && pass_max > 0) pass[0] = '\0';
    }
}

// 取当前热点密码(供配网页显示),非 AP 模式返回空串。
const char *dlna_wifi_ap_pass_get(void)
{
    return s_ap_mode ? s_ap_pass : "";
}

dlna_wifi_result_t dlna_wifi_connect(void)
{
    // 确保网络栈已初始化(nvs/netif/event loop)。调用方可能没提前 init,
    // 这里幂等触发,避免 esp_wifi_start 时 tcpip 线程未起导致 assert。
    esp_err_t e = dlna_wifi_init();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "网络栈初始化失败: %s", esp_err_to_name(e));
        return DLNA_WIFI_FAIL;
    }

    // 开机先预扫一次附近 WiFi 并缓存:此时射频尚未被 AP 占用,能扫到周围网络。
    // 之后无论连 STA 成功还是转 AP 配网,配网页都能直接展示这份列表,不必等在线扫。
    dlna_wifi_prefetch_networks();

    char ssid[64], pass[64];
    bool have = load_credentials(ssid, sizeof(ssid), pass, sizeof(pass));

    if (have) {
        for (int attempt = 0; attempt < CONNECT_RETRY_MAX; attempt++) {
            if (connect_to_sta(ssid, pass) == ESP_OK) return DLNA_WIFI_OK;
            // 连不上时停掉 STA,准备切 AP(避免模式切换残留)。
            if (s_wifi_started) esp_wifi_stop();
        }
        ESP_LOGW(TAG, "已知 SSID 连接失败(%s),转 AP 配网", ssid);
        return (dlna_wifi_start_ap() == ESP_OK) ? DLNA_WIFI_AP : DLNA_WIFI_FAIL;
    }
    // 没有凭证 → 直接进 AP 配网。
    ESP_LOGW(TAG, "无网络凭证,启动 AP 配网");
    return (dlna_wifi_start_ap() == ESP_OK) ? DLNA_WIFI_AP : DLNA_WIFI_FAIL;
}

bool dlna_wifi_is_connected(void)
{
    if (s_ap_mode || !s_sta_netif) return false;
    esp_netif_ip_info_t ip;
    return esp_netif_get_ip_info(s_sta_netif, &ip) == ESP_OK && ip.ip.addr != 0;
}

const char *dlna_wifi_ip_str(void)
{
    static char ipbuf[32] = {0};
    // AP 模式固定 192.168.4.1;STA 模式取实际 IP。
    if (s_ap_mode) {
        snprintf(ipbuf, sizeof(ipbuf), "192.168.4.1");
    } else if (dlna_wifi_is_connected()) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(s_sta_netif, &ip) == ESP_OK) {
            snprintf(ipbuf, sizeof(ipbuf), IPSTR, IP2STR(&ip.ip));
        }
    } else {
        ipbuf[0] = '\0';
    }
    return ipbuf;
}

void dlna_wifi_restart(void)
{
    ESP_LOGI(TAG, "1 秒后重启以应用新凭证...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}
