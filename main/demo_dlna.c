// main/demo_dlna.c —— DLNA 投屏播放器演示页。
// 界面:进页后先连 WiFi,再启动 DLNA 服务(SSDP + HTTP/SOAP),然后展示播放页。
// 按键映射(用户确认,2026-08-27):
//   上短按/下短按 : 音量 + / −
//   上长按/下长按 : 切换播放(本地;对手机 App 的"上一首/下一首"无效,见架构评审)
//   OK           : 播放 / 暂停(OK 长按被 main.c 全局拦截返回菜单)
#include "demo.h"
#include "dlna_wifi.h"
#include "dlna_service.h"
#include "dlna_player.h"
#include "bsp_display.h"
#include "bsp_battery.h"   // bsp_battery_soc,右上角电量
#include "ui_pixel.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static lv_obj_t *s_scr, *s_status, *s_net, *s_password, *s_title, *s_vol, *s_batt, *s_mascot;
static volatile bool s_exiting;
static TaskHandle_t s_boot_task;
static lv_timer_t *s_timer;
// 开机时间戳(esp_timer 微秒)。OK 长按重置网络只在开机后 RESET_WINDOW_MS 内有效。
static int64_t s_boot_us;
#define RESET_WINDOW_MS 10000

// 供 boot 任务给 UI 打一行状态(锁 LVGL,跨任务调用)。
// boot 阶段把"正在连 WiFi / 启动服务"的提示写到联网行(第二行),不占用播放状态行。
static void set_status(const char *text)
{
    if (!bsp_lvgl_lock(500)) return;
    if (s_net) lv_label_set_text(s_net, text);
    bsp_lvgl_unlock();
}

// 定时器驱动 UI 刷新(播放状态/联网状态/标题/音量/电量)。
static void ui_tick(lv_timer_t *timer)
{
    (void)timer;
    if (s_exiting || !s_status) return;

    // 联网状态行:始终显示【设备自身的 IP】(局域网地址,DLNA/配网对外地址)。
    const char *ip = dlna_wifi_ip_str();
    char netbuf[96];
    char passbuf[96] = {0};
    if (dlna_wifi_is_ap_mode()) {
        // 配网热点模式:设备 IP = 192.168.4.1;额外一行给 SSID+密码供连接。
        char ssid[40], pwd[40];
        dlna_wifi_ap_credentials(ssid, sizeof(ssid), pwd, sizeof(pwd));
        snprintf(netbuf, sizeof(netbuf), "IP %s", ip);
        snprintf(passbuf, sizeof(passbuf), "%s  PWD:%s", ssid, pwd);
    } else if (dlna_wifi_is_connected()) {
        snprintf(netbuf, sizeof(netbuf), "IP %s", ip);
    } else {
        snprintf(netbuf, sizeof(netbuf), "WiFi...");
    }

    // 播放状态(一行)。
    const char *st_str = "";
    switch (dlna_player_get_state()) {
    case DLNA_PLAYER_IDLE:       st_str = "Ready"; break;
    case DLNA_PLAYER_CONNECTING: st_str = "Connecting..."; break;
    case DLNA_PLAYER_PLAYING:    st_str = "Playing"; break;
    case DLNA_PLAYER_PAUSED:     st_str = "Paused"; break;
    case DLNA_PLAYER_STOPPED:    st_str = "Stopped"; break;
    case DLNA_PLAYER_ERROR:      st_str = "Error"; break;
    }

    char title[128];
    dlna_player_get_title(title, sizeof(title));

    int soc = bsp_battery_soc();   // 右上角电量

    if (!bsp_lvgl_lock(500)) return;
    // 每行一个属性,不再把音量塞进状态行(音量单独 s_vol 行显示)。
    lv_label_set_text(s_status, st_str);
    if (s_net) lv_label_set_text(s_net, netbuf);
    if (s_password) lv_label_set_text(s_password, passbuf);
    lv_label_set_text(s_title, title);
    lv_label_set_text_fmt(s_vol, "Vol: %d%%", dlna_player_get_volume());
    if (soc < 0) {
        lv_label_set_text(s_batt, "--%");
    } else {
        lv_label_set_text_fmt(s_batt, "%d%%", soc);
        // 低电量(<20)变红,便于一眼判断。
        lv_obj_set_style_text_color(s_batt,
            (soc < 20) ? lv_color_hex(0xFF5A5A) : lv_color_hex(UI_INK), 0);
    }
    bsp_lvgl_unlock();
}

// 启动任务:连 WiFi → 起 DLNA 服务 → 通知 UI。
static void boot_task(void *arg)
{
    (void)arg;
    if (s_exiting) { vTaskDelete(NULL); return; }

    set_status("WiFi connecting...");
    dlna_wifi_result_t r = dlna_wifi_connect();
    if (s_exiting) { vTaskDelete(NULL); return; }

    if (r == DLNA_WIFI_OK) {
        set_status("");   // 已联网:清掉提示,待 ui_tick 显示设备 IP。
    } else if (r == DLNA_WIFI_AP) {
        set_status("AP mode");   // 配网模式,待 ui_tick 显示 SSID+IP。
    } else {
        set_status("WiFi failed");
        if (s_exiting) { vTaskDelete(NULL); return; }
    }

    esp_err_t err = dlna_service_start();
    if (s_exiting) { vTaskDelete(NULL); return; }
    if (err != ESP_OK) {
        set_status("DLNA failed");
    } else {
        set_status("DLNA ready");   // 提示语,稍后被 ui_tick 的 IP 行覆盖。
    }
    vTaskDelete(NULL);
}

void demo_dlna_enter(void)
{
    s_exiting = false;
    // 记录开机时刻,作为"OK 长按重置网络"的 10 秒窗口起点(开机即直启本页)。
    s_boot_us = esp_timer_get_time();
    // 无云变体:右上角留给电量,不放装饰云。
    s_scr = ui_pixel_screen_create_nocloud("DLNA PLAYER");
    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 12, 54, 216, 190, UI_PAPER);

    // 每行一个属性:播放状态 / 联网状态 / 曲名 / 音量,自上而下。
    s_status = lv_label_create(panel);
    lv_obj_set_width(s_status, 190);
    lv_obj_set_style_text_color(s_status, lv_color_hex(UI_SKY_DARK), 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 6, 8);
    lv_label_set_text(s_status, "Booting...");

    s_net = lv_label_create(panel);
    lv_obj_set_width(s_net, 190);
    lv_obj_set_style_text_color(s_net, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_text_font(s_net, &lv_font_montserrat_14, 0);
    lv_obj_align(s_net, LV_ALIGN_TOP_LEFT, 6, 34);
    lv_label_set_long_mode(s_net, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_net, "");

    // 配网模式下显示 SSID + PWD 的行(STA 联网时留空)。
    s_password = lv_label_create(panel);
    lv_obj_set_width(s_password, 190);
    lv_obj_set_style_text_color(s_password, lv_color_hex(UI_ORANGE), 0);
    lv_obj_set_style_text_font(s_password, &lv_font_montserrat_14, 0);
    lv_obj_align(s_password, LV_ALIGN_TOP_LEFT, 6, 60);
    lv_label_set_long_mode(s_password, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_password, "");

    s_title = lv_label_create(panel);
    lv_obj_set_width(s_title, 190);
    lv_obj_set_style_text_color(s_title, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_14, 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_LEFT, 6, 98);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_title, "No track");

    s_vol = lv_label_create(panel);
    lv_obj_set_style_text_color(s_vol, lv_color_hex(UI_YELLOW), 0);
    lv_obj_align(s_vol, LV_ALIGN_TOP_LEFT, 6, 128);
    lv_label_set_text_fmt(s_vol, "Vol: %d%%", 70);

    // 右上角电量(已去掉云,不会被遮挡)。
    s_batt = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_batt, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_batt, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_batt, LV_ALIGN_TOP_RIGHT, -6, 12);
    lv_label_set_text(s_batt, "--%");

    s_mascot = ui_pixel_mascot_create(s_scr, 101, 246);

    // 初始化播放管道(幂等),设默认音量。
    dlna_player_init();
    dlna_player_set_volume(70);

    // 启动引导任务与 UI 定时器。
    xTaskCreate(boot_task, "dlna_boot", 4096, NULL, 5, &s_boot_task);
    s_timer = lv_timer_create(ui_tick, 500, NULL);

    lv_screen_load(s_scr);
}

void demo_dlna_exit(void)
{
    s_exiting = true;
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    if (s_boot_task) { vTaskDelete(s_boot_task); s_boot_task = NULL; }
    dlna_service_stop();
    dlna_player_stop();
    if (s_scr) { lv_obj_delete(s_scr); s_scr = NULL; s_status = s_net = s_password = s_title = s_vol = s_batt = s_mascot = NULL; }
}

void demo_dlna_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    // OK 长按 = 重置网络,但只在开机后 RESET_WINDOW_MS(10s)内有效。
    // 启动窗口过了之后,OK 长按不再重置网络,避免正常播放时误触把网络清掉。
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        if (esp_timer_get_time() - s_boot_us < RESET_WINDOW_MS * 1000) {
            set_status("Reset WiFi (10s window)...");
            dlna_wifi_clear_credentials();
            vTaskDelay(pdMS_TO_TICKS(300));
            dlna_wifi_restart();   // 重启后无凭证 → 自动进 AP 配网
        } else {
            set_status("Reset window over");   // 已过 10 秒,提示不生效。
            ui_tick(NULL);
        }
        return;
    }
    // UP/DOWN 长按 = 切换播放(本地兜底)。
    if (ev == BSP_BTN_LONG && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
        if (dlna_player_get_state() == DLNA_PLAYER_PAUSED) {
            dlna_player_resume();
        } else {
            dlna_player_pause();
        }
        ui_pixel_mascot_jump(s_mascot);
        ui_tick(NULL);
        return;
    }
    if (ev != BSP_BTN_CLICK) return;

    int vol = dlna_player_get_volume();
    if (btn == BSP_BTN_UP) {
        vol += 10; if (vol > 100) vol = 100;
        dlna_player_set_volume(vol);
    } else if (btn == BSP_BTN_DOWN) {
        vol -= 10; if (vol < 0) vol = 0;
        dlna_player_set_volume(vol);
    } else if (btn == BSP_BTN_OK) {
        if (dlna_player_get_state() == DLNA_PLAYER_PLAYING) {
            dlna_player_pause();
        } else {
            dlna_player_resume();
        }
        ui_pixel_mascot_jump(s_mascot);
    }
    ui_tick(NULL);
}
