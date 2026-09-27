// main/sanoba_app.h —— 应用状态机(《千恋＊万花》阅读器,从 ATRI 阅读器移植)。
//
// 页面切换、按键语义、自动存档、电量刷新、空闲休眠都在这里;剧情/分页/存档语义由
// main/sanoba_model.[ch]、main/sanoba_pack.[ch]、main/sanoba_save.[ch] 保持。
//
// 按键语义(三键):
//   标题/列表页: 上/下 移动光标,确定 进入,长按确定 返回上一层
//   正文页:     上/下短按 推进(打字中=立即显示全文,本句还有下一页=翻页),
//               长按上 快进(松手停),长按下 切换自动阅读,确定 打开菜单
//   章节卡:     任意键跳过(过场计时结束也跳过)
//   选项页:     上/下 选择,确定 确认
//   存档页:     上/下 选择,确定 存/读,长按确定 删除该档(存档模式下)
#pragma once

#include "sanoba_ui.h"
#include "sanoba_model.h"
#include "sanoba_save.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t btn;    // bsp_btn_t
    uint8_t ev;     // bsp_btn_ev_t
} sanoba_key_t;

typedef struct {
    sanoba_pack_t pack;
    sanoba_scn_t scn;
    sanoba_player_t player;   // 含 32 KB 解块缓冲:整个应用只此一份
    sanoba_ui_t ui;
    sanoba_settings_t settings;

    sanoba_page_t page;
    sanoba_page_t settings_return;
    sanoba_page_t slots_return;

    bool slots_saving;
    int title_sel;
    int menu_sel;
    int settings_sel;
    int slots_sel;

    sanoba_layout_t layout_hint;   // 分页参数(16px 字、26 单位 x 5 行)

    // 画面区当前显示的三个名字(空串 = 该层不画),避免同一张画面重复解码
    bool rendered_valid;
    char rendered_bg[SANOBA_NAME_MAX];
    char rendered_sprite[SANOBA_NAME_MAX];
    char rendered_ev[SANOBA_NAME_MAX];

    bool transition_pending;   // 章节卡:等按键或过场计时结束
    uint32_t transition_ms;
    // 当前章节的短标签(源数据里的 "chapter　4-8" → "4-8");画面左上角显示这个
    char chapter_short[SANOBA_TITLE_MAX];
    bool fast_forward;         // 长按上快进中(松手即停)
    uint32_t fast_forward_ms;
    bool auto_play;            // 自动阅读模式(长按下开关;任意键停止)
    uint32_t auto_ms;          // 自动模式:一句话读完后再等这么久就翻
    int about_scroll;          // 关于页滚动位置(像素)

    bool at_choice;            // 当前停在选项上(模型的"步骤类型"不落盘,由应用记)
    bool ended;                // 已经进入结局页

    int battery_percent;
    uint32_t battery_accum_ms;
    uint32_t idle_ms;

    char notice[32];
    uint32_t notice_ms;
    bool have_auto;
    bool started;             // 已经开始/继续过阅读
    bool sleep_requested;     // "关机"或标题页长按:请 main 进入 deep sleep
} sanoba_app_t;

// 初始化:打开图片包与剧本包、NVS、建界面、进入首个页面(提示页或标题页)。
// art_pixels 必须是 SANOBA_ART_W * SANOBA_ART_H 的静态缓冲;调用时需持有 LVGL 锁。
bool sanoba_app_init(sanoba_app_t *app, const uint8_t *pack_data, uint32_t pack_size,
                   const uint8_t *scn_data, uint32_t scn_size, uint16_t *art_pixels,
                   const lv_font_t *font_cjk);

// 处理一次按键。调用时需持有 LVGL 锁。
void sanoba_app_key(sanoba_app_t *app, const sanoba_key_t *key);

// 周期性 tick(累计空闲时间、电量刷新间隔、过场计时)。调用时需持有 LVGL 锁。
void sanoba_app_tick(sanoba_app_t *app, uint32_t elapsed_ms);

// 自动阅读是否正在自行翻页(停在选项或结局上时不算),供空闲熄灭豁免使用。
bool sanoba_app_auto_reading(const sanoba_app_t *app);

// 空闲时长(毫秒),供 main 决定变暗/熄屏/深睡。
uint32_t sanoba_app_idle_ms(const sanoba_app_t *app);

// 进入深睡前保存进度。
void sanoba_app_before_sleep(sanoba_app_t *app);

// 深睡前在屏幕上显示提示(唤醒后设备会重启,状态无需恢复)。
void sanoba_app_show_sleeping(sanoba_app_t *app);

// 取走一次"立即休眠"请求(标题页长按确定 / 设置页"关机")。
bool sanoba_app_take_sleep_request(sanoba_app_t *app);

// 调试用:把第 chapter 章(1 起,0 = 剧本开头)之后的第 step 句正文画进画面区。
// 新数据层没有"章号 -> 位置"的索引,定位靠从块 0 顺流推进,所以它会移动阅读位置
// (画面区之外不改动页面、不落盘),供串口截图命令使用(见 main.c)。需持有 LVGL 锁。
bool sanoba_app_debug_render(sanoba_app_t *app, uint16_t scenario, uint16_t step);

// 调试用:从第 chapter 章第 step 句开始阅读(改玩家状态、正常落盘),用于
// “开机直接进某个场景”(CMake 的 SANOBA_BOOT_SCENARIO,0 起)。调用时需持有 LVGL 锁。
bool sanoba_app_debug_start(sanoba_app_t *app, uint16_t scenario, uint16_t step);

// 调试:切到标题页(串口截图用)。
bool sanoba_app_debug_title(sanoba_app_t *app);
