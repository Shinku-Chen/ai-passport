// main/atri_app.h —— 应用状态机:页面切换、按键语义、自动存档、电量刷新、空闲休眠。
//
// 按键语义(三键):
//   标题/列表页: 上/下 移动光标,确定 进入,长按确定 返回上一层
//   正文页:     确定/上/下 推进(打字中=立即显示全文),长按确定 打开菜单
//   选项页:     上/下 选择,确定 确认
//   存档页:     上/下 选择,确定 存/读,长按确定 删除该槽(存档模式下)
#pragma once

#include "atri_save.h"
#include "atri_ui.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t btn;    // bsp_btn_t
    uint8_t ev;     // bsp_btn_ev_t
} atri_key_t;

typedef struct {
    atri_pack_t pack;
    atri_player_t player;
    atri_ui_t ui;
    atri_settings_t settings;

    atri_page_t page;
    atri_page_t settings_return;
    atri_page_t slots_return;

    bool slots_saving;
    int title_sel;
    int menu_sel;
    int settings_sel;
    int slots_sel;

    atri_layout_t layout_hint;   // 分页参数(16px 字、13 单位 x 5 行)

    uint16_t rendered_bg;   // 画面区当前显示的(背景/叠加/立绘/位置),避免重复解码
    uint16_t rendered_ovl;
    uint16_t rendered_chr;
    int16_t rendered_x;
    int16_t rendered_y;

    bool transition_pending;   // 换章过场:显示标题画,计时结束或按键后进正文
    uint32_t transition_ms;
    bool fast_forward;         // 长按上/下快进中(松手即停)
    uint32_t fast_forward_ms;
    int about_scroll;          // 关于页滚动位置(像素)

    int battery_percent;
    uint32_t battery_accum_ms;
    uint32_t idle_ms;

    char notice[32];
    uint32_t notice_ms;
    bool have_auto;
    bool started;             // 已经开始/继续过阅读
    bool sleep_requested;     // "关机"或标题页长按:请 main 进入 deep sleep
} atri_app_t;

// 初始化:打开资源包与 NVS、建界面、进入首个页面(警告页或标题页)。
// art_pixels 必须是 ATRI_ART_W * ATRI_ART_H 的静态缓冲;调用时需持有 LVGL 锁。
bool atri_app_init(atri_app_t *app, const uint8_t *pack_data, uint32_t pack_size,
                   uint16_t *art_pixels, const lv_font_t *font_cjk);

// 处理一次按键。调用时需持有 LVGL 锁。
void atri_app_key(atri_app_t *app, const atri_key_t *key);

// 周期性 tick(累计空闲时间、电量刷新间隔、过场计时)。调用时需持有 LVGL 锁。
void atri_app_tick(atri_app_t *app, uint32_t elapsed_ms);

// 空闲时长(毫秒),供 main 决定变暗/熄屏/深睡。
uint32_t atri_app_idle_ms(const atri_app_t *app);

// 进入深睡前保存进度。
void atri_app_before_sleep(atri_app_t *app);

// 深睡前在屏幕上显示提示(唤醒后设备会重启,状态无需恢复)。
void atri_app_show_sleeping(atri_app_t *app);

// 取走一次"立即休眠"请求(标题页长按确定 / 设置页"关机")。
bool atri_app_take_sleep_request(atri_app_t *app);

// 调试用:把指定章节/场景直接画进画面区(不改动玩家状态、不落盘),
// 供串口截图命令 ATRISHOT 使用(见 main.c)。调用时需持有 LVGL 锁。
bool atri_app_debug_render(atri_app_t *app, uint16_t chapter, uint16_t scene);
