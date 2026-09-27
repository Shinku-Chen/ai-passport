// main/tsxx_app.h —— 应用状态机:页面切换、按键语义、打字机节奏、自动存档、空闲休眠。
//
// 按键语义(三键,单线剧情 —— 任意键都能翻页,菜单放在长按上):
//   正文页:   上/下/确定 短按 推进一句(打字中 = 立即显示全文)
//             长按确定 打开菜单;长按上 快进(松手即停);长按下 自动阅读开关(任意键停)
//   选项页:   上/下 选择,确定 确认
//   标题/列表: 上/下 移光标,确定 进入,长按确定 返回上一层
//   存读档页: 上/下 选择,确定 存/读,长按确定 删除该槽(存档模式)
//
// 按键驱动会把落后于短按窗口的快速连按合并成一次"双击"事件 —— 双击必须当普通单击处理,
// 否则一路连按推进剧情时会丢输入(见 docs/reference/shinku-chen/three-key-reader-interaction)。
#pragma once

#include "tsxx_save.h"
#include "tsxx_ui.h"

#include <stdbool.h>
#include <stdint.h>

// 一屏最多几个选项(与 tsxx_ui.h 的 TSXX_CHOICE_MAX 一致)。
#define TSXX_CHOICE_MAX_LOCAL 5
// 章节标签("[CHAPTER 3-3]" 转成 "CHAPTER 3－3")的最大字节数。
#define TSXX_CHAPTER_LABEL 24

typedef struct {
    uint8_t btn;   // bsp_btn_t
    uint8_t ev;    // bsp_btn_ev_t
} tsxx_key_t;

typedef struct {
    tsxx_pack_t pack;
    tsxx_player_t player;
    tsxx_ui_t ui;
    tsxx_settings_t settings;

    tsxx_page_id_t page;
    tsxx_page_id_t settings_return;
    tsxx_page_id_t slots_return;

    bool slots_saving;
    int title_sel;
    int menu_sel;
    int settings_sel;
    int slots_sel;
    int chapters_sel;
    int chapters_top;      // 章节列表窗口的第一行

    tsxx_layout_t layout;  // 分页参数(16px 字、13 单位 x 5 行)

    // 画面区缓存:只在(背景, 事件图, 立绘)三元组真的变化时才重新解码。
    bool art_valid;
    uint8_t art_bg;
    uint8_t art_sprite;
    bool art_has_cg;
    tsxx_cg_t art_cg;

    // 章节点(启动时扫一遍,章节页与章节标签都用它)。
    tsxx_chapter_t chapters[TSXX_MAX_CHAPTERS];
    int chapter_count;

    bool auto_play;        // 自动阅读模式
    uint32_t auto_ms;      // 一句话读完后还要等多久才翻
    bool fast_forward;     // 长按上快进中(松手即停)
    uint32_t fast_forward_ms;
    uint32_t auto_save_ms; // 自动存档节流计时

    int battery_percent;
    uint32_t battery_accum_ms;
    uint32_t idle_ms;

    char notice[40];
    uint32_t notice_ms;

    bool have_auto;        // 自动存档存在(标题页的"继续阅读")
    // 已经选了"开始/继续"但还没过首次的同人移植提示:0 = 无,1 = 新游戏,2 = 继续阅读。
    // 开机直接进标题页,提示只在第一次真的要进正文前拦一次。
    uint8_t pending_start;
    bool started;          // 已经开始过阅读
    bool ended;            // 已抵达页表末尾
    bool sleep_requested;  // 标题页长按确定:请 main 进入 deep sleep
} tsxx_app_t;

// 初始化:打开资源包与 NVS、建界面、进入首个页面(警告页或标题页)。
// art_pixels 必须是 TSXX_ART_W * TSXX_ART_H 的静态缓冲;调用时需持有 LVGL 锁。
// pack_data/pack_size 指向已经整块在内存里的资源包(宿主机测试与平坦调试用)。
bool tsxx_app_init(tsxx_app_t *app, const uint8_t *pack_data, uint32_t pack_size,
                   uint16_t *art_pixels, const lv_font_t *font_cjk);

// 初始化(设备模式):资源包烧在 pack_partition 分区里,由 tsxx_pack_open_partition()
// 常驻映射脚本区间 + 滑动窗口读图片。其余语义与 tsxx_app_init() 相同;需持有 LVGL 锁。
bool tsxx_app_init_partition(tsxx_app_t *app, const char *pack_partition, uint16_t *art_pixels,
                             const lv_font_t *font_cjk);

// 处理一次按键。调用时需持有 LVGL 锁。
void tsxx_app_key(tsxx_app_t *app, const tsxx_key_t *key);

// 周期性 tick:打字机、自动阅读、快进、电量刷新、空闲计时。需持有 LVGL 锁。
void tsxx_app_tick(tsxx_app_t *app, uint32_t elapsed_ms);

// 自动阅读是否正在自行翻页(停在选项或结局上时不算),供空闲熄灭豁免使用。
bool tsxx_app_auto_reading(const tsxx_app_t *app);

// 空闲时长(毫秒),供 main 决定变暗/熄屏/深睡。
uint32_t tsxx_app_idle_ms(const tsxx_app_t *app);

// 进入深睡前保存进度与设置。
void tsxx_app_before_sleep(tsxx_app_t *app);

// 深睡前在屏幕上显示提示(唤醒后设备会重启,状态无需恢复)。
void tsxx_app_show_sleeping(tsxx_app_t *app);

// 取走一次"立即休眠"请求(标题页长按确定)。
bool tsxx_app_take_sleep_request(tsxx_app_t *app);

// 调试用:把指定页画出来(画面 + 正文 + 章节标签),不改动玩家状态、不落盘。
// 供串口截图命令 TSXXSHOT 使用(见 main.c)。调用时需持有 LVGL 锁。
bool tsxx_app_debug_render(tsxx_app_t *app, uint32_t page);

// 调试用:从指定页直接开始阅读(改玩家状态、正常落盘),用于串口命令 TSXXJUMP。
// 调用时需持有 LVGL 锁。
bool tsxx_app_debug_start(tsxx_app_t *app, uint32_t page);
