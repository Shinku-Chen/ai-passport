// main/dlna_app.c —— DLNA 音频投屏接收器应用。
// 参考 MYHealer/dlna-esp32 的 DLNA MediaRenderer,适配 ESP32-C3 无 PSRAM 单核:
//   - DLNA 协议栈:custom_dlna(SSDP 发现 + SOAP 控制 + GENA 事件)
//   - 音频管线:dlna_audio(HTTP 拉流 → minimp3/esp_aac 解码 → bsp_audio I2S 输出)
//   - UI:极简状态页(标题栏 + 曲目名 + 进度条 + 状态 + 电量)
//   - 输入:三键(上/下/确定),替代参考项目的旋转编码器
//   - 多音源:按 SetAVTransportURI 的 User-Agent 切换音乐源配置
#include "dlna_app.h"
#include "dlna_audio.h"
#include "custom_dlna.h"
#include "bsp_wifi.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_battery.h"
#include "bsp_audio.h"
#include "bsp_pins.h"
#include "ui_pixel.h"
#include "miplay.h"
#include "net_prov.h"

#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "dlna_app";

#define DLNA_DEVICE_UUID "8db0797a-f01a-4949-8f59-51188b18180b"
#define DLNA_HTTP_PORT   8080

/* ─────────────── 播放状态机 ─────────────── */
typedef enum {
    PS_NO_MEDIA      = -1,
    PS_STOPPED       = 0,
    PS_PLAYING       = 1,
    PS_PAUSED        = 2,
    PS_TRANSITIONING = 3,
} play_state_t;

static play_state_t s_state = PS_NO_MEDIA;
static char *s_track_uri;
static int64_t s_boot_ms;   // 开机(本应用启动)时间戳,用于「10秒内长按OK清配置」

/* ─────────────── LVGL UI ─────────────── */
static lv_obj_t *s_scr;
static lv_obj_t *s_title_label;
static lv_obj_t *s_meta_label;     // 曲目名/艺人
static lv_obj_t *s_info_label;     // 网络信息(SSID/IP/网关/信号),或配网提示(SSID+密码)
static lv_obj_t *s_batt_label;     // 右上角电量百分比
static lv_obj_t *s_bar_bg;         // 进度条底
static lv_obj_t *s_bar_fill;       // 进度条填充
static lv_timer_t *s_ui_timer;
static bool s_ui_ready;

/* ─────────────── 状态字符串映射 ─────────────── */
static const char *state_str(play_state_t s)
{
    switch (s) {
        case PS_PLAYING:       return "PLAYING";
        case PS_PAUSED:        return "PAUSED_PLAYBACK";
        case PS_NO_MEDIA:      return "NO_MEDIA_PRESENT";
        case PS_TRANSITIONING: return "TRANSITIONING";
        default:               return "STOPPED";
    }
}

static void set_state(play_state_t s)
{
    if (s_state != s) {
        s_state = s;
        custom_dlna_notify_transport_state_async();
    }
}

/* ─────────────── 音源检测(多音源) ─────────────── */
static void detect_and_apply_music_source(const char *uri)
{
    (void)uri;
    const char *ua = custom_dlna_get_user_agent();
    if (!ua) return;
    if (strstr(ua, "QQMusic") || strstr(ua, "qqmusic") || strstr(ua, "QQ音乐")) {
        custom_dlna_set_music_source(MUSIC_SRC_QQ);
    } else if (strstr(ua, "Ximalaya") || strstr(ua, "小雅")) {
        custom_dlna_set_music_source(MUSIC_SRC_XIMALAYA);
    } else if (strstr(ua, "bilibili") || strstr(ua, "Bilibili")) {
        custom_dlna_set_music_source(MUSIC_SRC_BILIBILI);
    } else {
        custom_dlna_set_music_source(MUSIC_SRC_NETEASE);
    }
}

/* ─────────────── custom_dlna 回调实现 ─────────────── */
static const char *cb_get_transport_state(void) { return state_str(s_state); }
static const char *cb_get_uri(void)             { return s_track_uri ? s_track_uri : ""; }

static int cb_get_position_sec(void)
{
    return (int)(dlna_audio_get_position_ms() / 1000);
}

static int cb_get_position_ms(void)
{
    return (int)dlna_audio_get_position_ms();
}

static int cb_get_duration_sec(void)
{
    return (int)(dlna_audio_get_duration_ms() / 1000);
}

static int cb_get_volume(void) { return dlna_audio_get_volume(); }
static int cb_get_mute(void)   { return dlna_audio_is_muted() ? 1 : 0; }

static void cb_set_uri(const char *uri)
{
    if (!uri) return;
    detect_and_apply_music_source(uri);
    if (s_track_uri) free(s_track_uri);
    s_track_uri = strdup(uri);
    if (s_state == PS_NO_MEDIA) set_state(PS_STOPPED);
    ESP_LOGI(TAG, "SetURI: %s", s_track_uri);
}

static void cb_set_next_uri(const char *uri, const char *metadata)
{
    (void)metadata;
    if (uri) cb_set_uri(uri);
}

static void cb_set_metadata(const char *metadata)
{
    // 解析曲目名(尽力,仅取 <dc:title> 首值)。
    (void)metadata;
}

static void cb_play(void)
{
    if (!s_track_uri) { ESP_LOGW(TAG, "Play called with no URI"); return; }
    dlna_audio_play_uri(s_track_uri);
    set_state(PS_PLAYING);
}

static void cb_pause(void)
{
    dlna_audio_pause();
    set_state(PS_PAUSED);
}

static void cb_stop(void)
{
    dlna_audio_stop();
    set_state(PS_STOPPED);
}

static void cb_seek(int seconds)
{
    dlna_audio_seek(seconds);
}

static void cb_set_volume(int vol)
{
    dlna_audio_set_volume(vol);
}

static void cb_set_mute(int mute)
{
    dlna_audio_set_mute(mute != 0);
}

static void cb_next(void)
{
    ESP_LOGW(TAG, "next: 多音源下切歌由手机控制,此处不实现本地 playlist");
}

static void cb_previous(void)
{
    ESP_LOGW(TAG, "previous: 同上");
}

/* ─────────────── UI 构建 ─────────────── */
static void ui_build(void)
{
    s_scr = ui_pixel_screen_create("DLNA");

    // 右上角电量百分比(仅数字,无图标)
    s_batt_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_batt_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_batt_label, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_batt_label, LV_ALIGN_TOP_RIGHT, -10, 12);
    lv_label_set_text(s_batt_label, "---");

    // 标题(CJK 标题会遮盖标题栏,这里只放屏名,避免跟电量重叠)
    s_title_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_title_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_title_label, lv_color_hex(UI_PAPER), 0);
    lv_obj_align(s_title_label, LV_ALIGN_TOP_LEFT, 12, 8);

    // 网络信息区(已连接:SSID/IP/网关/信号;配网:SSID+密码)
    s_info_label = lv_label_create(s_scr);
    lv_obj_set_width(s_info_label, 216);
    lv_obj_set_style_text_font(s_info_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_info_label, lv_color_hex(UI_SKY_DARK), 0);
    lv_obj_align(s_info_label, LV_ALIGN_TOP_LEFT, 12, 38);
    lv_label_set_text(s_info_label, "Connecting...");

    // 曲目/艺人
    s_meta_label = lv_label_create(s_scr);
    lv_obj_set_width(s_meta_label, 216);
    lv_obj_set_style_text_font(s_meta_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_meta_label, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_meta_label, LV_ALIGN_TOP_LEFT, 12, 90);
    lv_label_set_text(s_meta_label, "No track");

    // 进度条
    s_bar_bg = ui_pixel_panel_create(s_scr, 12, 190, 216, 10, UI_MUTED);
    s_bar_fill = ui_pixel_panel_create(s_scr, 12, 190, 0, 10, UI_ORANGE);
    lv_obj_set_style_radius(s_bar_bg, 5, 0);
    lv_obj_set_style_radius(s_bar_fill, 5, 0);

    ui_pixel_mascot_create(s_scr, 101, 246);
    lv_screen_load(s_scr);
    s_ui_ready = true;
}

static void ui_refresh(void)
{
    if (!s_ui_ready) return;

    // 右上角电量(百分比)
    int soc = bsp_battery_soc();
    if (soc >= 0) {
        lv_label_set_text_fmt(s_batt_label, "%d%%", soc);
    }

    // 网络信息区:已连接 或 配网,两种状态。
    if (bsp_wifi_is_connected()) {
        lv_label_set_text_fmt(s_info_label,
                              "WiFi %s\nIP  %s\nGW  %s\n信号 %ddBm",
                              bsp_wifi_get_ssid(),
                              bsp_wifi_get_ip_str(),
                              bsp_wifi_get_gateway(),
                              bsp_wifi_get_current_rssi());
        lv_label_set_text(s_meta_label, "等待手机 DLNA 投屏");
    } else {
        // 配网/未连接:提示 SSID 与热点连接密码。
        lv_label_set_text_fmt(s_info_label,
                              "配网 SSID: AI-Passport-Prov\n连接密码: 00114514");
        if (s_track_uri) lv_label_set_text(s_meta_label, s_track_uri);
    }

    // 播放状态
    if (s_state == PS_PLAYING) {
        lv_label_set_text(s_title_label, "播放中");
    } else if (s_state == PS_PAUSED) {
        lv_label_set_text(s_title_label, "已暂停");
    } else if (s_state == PS_STOPPED) {
        lv_label_set_text(s_title_label, "已停止");
    } else {
        lv_label_set_text(s_title_label, "DLA");
    }

    // 进度条
    uint32_t pos = dlna_audio_get_position_ms();
    uint32_t dur = dlna_audio_get_duration_ms();
    if (dur) {
        lv_coord_t w = (lv_coord_t)(216 * pos / dur);
        if (w < 0) w = 0;
        lv_obj_set_width(s_bar_fill, w);
    }
}

static void ui_tick(lv_timer_t *timer)
{
    (void)timer;
    ui_refresh();
}

/* ─────────────── WiFi 事件回调 ─────────────── */
static void on_wifi_evt(bsp_wifi_state_t state, void *user)
{
    (void)user;
    // 网络信息由 ui_refresh 统一刷新(基于 bsp_wifi_is_connected()),这里只打日志。
    ESP_LOGI(TAG, "WiFi 状态: %d IP=%s", (int)state, bsp_wifi_get_ip_str());
}

/* ─────────────── 音频管线事件回调 ─────────────── */
static void on_audio_evt(dlna_audio_state_t state, const char *uri, void *user)
{
    (void)uri; (void)user;
    if (state == DLNA_AUDIO_ERROR) {
        ESP_LOGW(TAG, "播放出错");
        set_state(PS_STOPPED);
    } else if (state == DLNA_AUDIO_IDLE) {
        // 流播完回到待投
        set_state(PS_STOPPED);
    }
}

/* ─────────────── MiPlay 连接回调（I2S 互斥）───────────────
 * 小米妙播手机连上时,暂停 DLNA 播放并抑制 SSDP 发现,避免同一设备
 * 同时被两个投屏协议抢;断开后恢复 DLNA。 */
static void dlna_on_miplay_connected(bool connected)
{
    ESP_LOGI(TAG, "MiPlay %s", connected ? "connected → DLNA 暂停" : "disconnected → DLNA 恢复");
    custom_dlna_set_ssdp_suppressed(connected);
    if (connected) {
        cb_pause();
    } else {
        // 不自动续播,回到待投屏;由用户/手机重新发起。
        set_state(PS_STOPPED);
    }
}

/* ─────────────── DLNA/MiPlay 服务(仅已配网时启动,避免内存峰值) ─────────────── */
static void dlna_services_start(void)
{
    // 音频(先于播放;仅 DLNA 播放需要,配网阶段不起)
    dlna_audio_init(on_audio_evt, NULL);

    // DLNA 协议栈
    static const custom_dlna_config_t dlna_cfg = {
        .friendly_name     = "AI Passport",
        .uuid              = DLNA_DEVICE_UUID,
        .port              = DLNA_HTTP_PORT,
        .get_transport_state = cb_get_transport_state,
        .get_uri             = cb_get_uri,
        .get_position_sec    = cb_get_position_sec,
        .get_position_ms     = cb_get_position_ms,
        .get_duration_sec    = cb_get_duration_sec,
        .get_volume          = cb_get_volume,
        .get_mute            = cb_get_mute,
        .on_set_uri          = cb_set_uri,
        .on_set_next_uri     = cb_set_next_uri,
        .on_set_metadata     = cb_set_metadata,
        .on_play             = cb_play,
        .on_pause            = cb_pause,
        .on_stop             = cb_stop,
        .on_seek             = cb_seek,
        .on_set_volume       = cb_set_volume,
        .on_set_mute         = cb_set_mute,
        .on_next             = cb_next,
        .on_previous         = cb_previous,
    };
    custom_dlna_init(&dlna_cfg);
    ESP_LOGI(TAG, "DLNA 服务已启动(port=%d)", DLNA_HTTP_PORT);

    // MiPlay 小米妙播:注册 mDNS 广播 + TCP 8899,连接时暂停 DLNA(共享 I2S 互斥)。
    miplay_set_connected_cb(dlna_on_miplay_connected);
    esp_err_t merr = miplay_init();
    if (merr != ESP_OK) {
        ESP_LOGW(TAG, "MiPlay 初始化返回: %s", esp_err_to_name(merr));
    } else {
        ESP_LOGI(TAG, "MiPlay 服务已启动");
    }
}

/* ─────────────── 对外启动接口 ─────────────── */
void dlna_app_start(void)
{
    ESP_LOGI(TAG, "DLNA 接收器启动");
    s_boot_ms = esp_timer_get_time() / 1000;   // 记录启动时刻(ms),供长按 OK 判断窗口

    // 构建 UI
    ui_build();

    // WiFi 初始化(固定 SSID 直连;自动重连已禁用,改上层判定)
    bsp_wifi_set_evt_cb(on_wifi_evt, NULL);
    if (bsp_wifi_init(NULL) != ESP_OK) {
        ESP_LOGW(TAG, "WiFi 初始化失败,直接开配网");
    }

    // 开机即扫描附近 SSID(供配网页预填,无论是否已配网)
    bsp_wifi_scan();

    bool connected = false;
    if (bsp_wifi_has_credentials()) {
        // 已配网 → 尝试连接,10 秒超时
        ESP_LOGI(TAG, "尝试连接已保存 WiFi...");
        connected = (bsp_wifi_connect_sta(NULL, NULL, 10000) == ESP_OK);
        if (connected) {
            dlna_services_start();
        }
    }

    if (!connected) {
        // 未配网 或 连接失败 → 开软AP配网页(预填扫描列表)
        ESP_LOGI(TAG, "未配网/连接失败,开启 SoftAP 配网热点");
        bsp_wifi_start_ap("AI-Passport-Prov", "ai-passport");
        net_prov_start();
    }

    // UI 刷新定时器
    s_ui_timer = lv_timer_create(ui_tick, 250, NULL);
}

void dlna_app_on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if (!bsp_lvgl_lock(500)) return;

    // 三键语义映射(替代旋钮):
    //   上/下 = 无强焦点切换语义,留作预留(可调音量)。
    //   确定单击 = 播放/暂停切换;确定长按 = 回待投屏(停止)。
    //   【开机 10 秒内长按 OK = 清除 WiFi 配置并重启】(重新配网)
    if (btn == BSP_BTN_OK) {
        if (ev == BSP_BTN_LONG) {
            int64_t now_ms = esp_timer_get_time() / 1000;
            if (now_ms - s_boot_ms <= 10000) {
                ESP_LOGI(TAG, "开机10秒内长按OK:清除 WiFi 配置并重启");
                bsp_wifi_clear_credentials();
                bsp_lvgl_unlock();
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
                return;
            }
            cb_stop();
        } else if (ev == BSP_BTN_CLICK) {
            if (s_state == PS_PLAYING) cb_pause();
            else if (s_state == PS_PAUSED || s_state == PS_STOPPED) cb_play();
        }
    } else if (btn == BSP_BTN_UP) {
        // 音量+
        int v = dlna_audio_get_volume() + 5;
        if (v > 100) v = 100;
        dlna_audio_set_volume(v);
    } else if (btn == BSP_BTN_DOWN) {
        int v = dlna_audio_get_volume() - 5;
        if (v < 0) v = 0;
        dlna_audio_set_volume(v);
    }

    bsp_lvgl_unlock();
}

void dlna_app_stop(void)
{
    if (s_ui_timer) { lv_timer_delete(s_ui_timer); s_ui_timer = NULL; }
    dlna_audio_stop();
    dlna_audio_deinit();
    if (s_scr) { lv_obj_delete(s_scr); s_scr = NULL; s_ui_ready = false; }
}
