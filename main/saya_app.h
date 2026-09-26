// main/saya_app.h —— 应用状态机:页面切换、按键语义、自动存档、电量刷新。
//
// 按键语义(三键):
//   标题/列表页: 上/下 移动光标(到顶/到底停住,不绕圈),确定 进入/确认,长按确定 返回
//   正文页:     确定 打开菜单;上 短按=下一段(打字中先补全),上 长按=快进,松开=停;
//               下 短按=回看上一页
//   选项页:     上/下 选择(不绕圈),确定 确认
//   关于页:     上/下 滚动正文(短按一行/长按四行),确定 返回
#pragma once

#include "bsp_button.h"
#include "saya_save.h"
#include "saya_ui.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t btn;    // bsp_btn_t
    uint8_t ev;     // bsp_btn_ev_t
} saya_key_t;

typedef struct {
    saya_pack_t pack;
    saya_player_t player;
    saya_ui_t ui;
    saya_settings_t settings;

    saya_page_t page;
    saya_page_t settings_return;
    saya_page_t slots_return;
    saya_page_t about_return;

    bool slots_saving;
    int title_sel;
    int menu_sel;
    int settings_sel;
    int slots_sel;

    uint16_t rendered_bg;   // 画面区当前显示的(背景, 立绘),用于避免重复解码
    uint16_t rendered_fg;
    int battery_percent;
    uint32_t battery_accum_ms;
    uint32_t idle_ms;
    bool fast_forward;        // "上"长按期间的自动推进
    uint32_t ff_accum_ms;
    char notice[24];
    uint32_t notice_ms;
    bool have_auto;
    bool started;      // 已经开始阅读(用于"继续")
} saya_app_t;

typedef struct {
    uint16_t *art_pixels;         // 320x136 RGB565 画布缓冲
    uint8_t *sprite_scratch;      // 立绘解码缓冲
    uint32_t sprite_scratch_size; // 至少 320*136*2
} saya_app_buffers_t;

// 初始化:打开资源包与 NVS、建界面、进入首个页面(警告页或标题页)。
bool saya_app_init(saya_app_t *app, const uint8_t *pack_data, uint32_t pack_size,
                   const saya_app_buffers_t *buffers);

// 处理一次按键(在输入任务里调用,内部自行加 LVGL 锁)。
void saya_app_key(saya_app_t *app, const saya_key_t *key);

// 周期性 tick:累计空闲时间与电量刷新间隔。
void saya_app_tick(saya_app_t *app, uint32_t elapsed_ms);

// 空闲时长(毫秒),供 main 决定变暗/深睡。
uint32_t saya_app_idle_ms(const saya_app_t *app);

// 进入深睡前保存进度。
void saya_app_before_sleep(saya_app_t *app);

// 深睡前在屏幕上显示提示(唤醒后设备会重启,状态无需恢复)。
void saya_app_show_sleeping(saya_app_t *app);
