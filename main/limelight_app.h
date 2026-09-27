// main/limelight_app.h —— 应用状态机(从 ATRI 阅读器 main/atri_app.[ch] 移植)。
//
// 页面切换、按键语义、自动存档、电量刷新、空闲计时都在这里;剧情推进/分页/存档语义
// 由 main/limelight_model.[ch] 保持不变,资源与剧本读取在 limelight_assets/script。
//
// 按键语义(三键):
//   标题/列表页: 上/下 移动光标,确定 进入,长按确定 返回上一层
//   正文页:     上/下短按 推进(打字中 = 立即显示全文),长按上 快进(松手停,靠 BSP 的
//               抬起事件),
//               长按下 切换自动阅读(任意键停),确定 打开菜单
//   选项页:     上/下 选择,确定 确认
//   存档页:     上/下 选择,确定 存/读,长按确定 删除该槽(存档模式下)
//   鉴赏页:     上/下 翻上一张/下一张,确定 或 长按确定 返回
#pragma once

#include "limelight_assets.h"
#include "limelight_model.h"
#include "limelight_save.h"
#include "limelight_script.h"
#include "limelight_ui.h"

#include <stdbool.h>
#include <stdint.h>

#define LIME_CHAPTER_MAX 256                 // 源数据 230 个章节标记
#define LIME_CHAPTER_LABEL_MAX 24
#define LIME_GALLERY_MAX 512                 // CG 条目上限(实测 356)

typedef struct {
    uint8_t btn;    // bsp_btn_t
    uint8_t ev;     // bsp_btn_ev_t
} lime_key_t;

typedef struct {
    lime_script_t script;
    lime_assets_t assets;
    lime_player_t player;
    lime_ui_t ui;
    lime_settings_t settings;

    lime_page_t page;
    lime_page_t settings_return;
    lime_page_t slots_return;
    lime_page_t gallery_return;

    bool slots_saving;
    int title_sel;
    int menu_sel;
    int settings_sel;
    int slots_sel;
    int chapter_sel;
    int gallery_index;
    int gallery_count;
    int16_t gallery_entries[LIME_GALLERY_MAX];

    lime_layout_t layout;

    int rendered_bg;        // 画面区当前显示的背景/CG 条目,避免重复解码
    int rendered_sprite;

    bool have_auto;         // 有可续读的自动存档
    bool started;           // 已经开始/继续过阅读

    bool transition_pending;
    uint32_t transition_ms;
    bool fast_forward;
    uint32_t fast_forward_ms;
    bool auto_play;
    uint32_t auto_ms;

    int battery_percent;
    uint32_t battery_accum_ms;
    uint32_t idle_ms;

    char notice[32];
    uint32_t notice_ms;
    bool sleep_requested;

    // 章节表:从剧本包复制出来(块缓存会换,指针会失效)。
    int chapter_count;
    uint32_t chapter_first_ids[LIME_CHAPTER_MAX];
    uint8_t chapter_major[LIME_CHAPTER_MAX];   // 章(如 10)
    uint8_t chapter_minor[LIME_CHAPTER_MAX];   // 节(如 3)
    char chapter_labels[LIME_CHAPTER_MAX][LIME_CHAPTER_LABEL_MAX];
    const char *chapter_label_ptrs[LIME_CHAPTER_MAX];
} lime_app_t;

// 初始化:打开两个包、读设置、建界面、进入首个页面(提示页或标题页)。
// art_pixels / sprite_buffer / mask_buffer 由调用方静态提供(尺寸见 limelight_image.h)。
bool lime_app_init(lime_app_t *app, const uint8_t *script_data, uint32_t script_size,
                   const uint8_t *asset_data, uint32_t asset_size, uint16_t *art_pixels,
                   uint8_t *sprite_buffer, uint32_t sprite_capacity, uint8_t *mask_buffer,
                   uint32_t mask_capacity, uint8_t *script_cache, uint32_t script_cache_size,
                   const lv_font_t *font_cjk);

// 处理一次按键。调用时需持有 LVGL 锁。
void lime_app_key(lime_app_t *app, const lime_key_t *key);


// 周期性 tick(累计空闲时间、电量刷新间隔、过场/快进/自动阅读计时)。需持有 LVGL 锁。
void lime_app_tick(lime_app_t *app, uint32_t elapsed_ms);

// 自动阅读是否正在自行翻页(停在选项/结局上时不算),供空闲熄屏豁免使用。
bool lime_app_auto_reading(const lime_app_t *app);

// 空闲时长(毫秒),供 main 决定变暗/熄屏/深睡。
uint32_t lime_app_idle_ms(const lime_app_t *app);

// 进入深睡前保存进度。
void lime_app_before_sleep(lime_app_t *app);

// 取走一次"立即休眠"请求(标题页长按确定 / 设置页"关机")。
bool lime_app_take_sleep_request(lime_app_t *app);

// 调试用:从指定 id 直接开始阅读(改玩家状态、正常落盘),供开机直接进某一章。
bool lime_app_debug_start(lime_app_t *app, uint32_t id);

// 调试用:切到标题页。
bool lime_app_debug_title(lime_app_t *app);

// 调试用:按名字切到列表页(chapters/gallery/menu/settings/about/title)。
bool lime_app_debug_page(lime_app_t *app, const char *name);

// 调试用:重画画面区(串口抓帧前调用,否则 memset 后的画面区是黑的)。
void lime_app_debug_redraw_art(lime_app_t *app);

// 调试用:直接开关自动阅读(串口验收用)。
void lime_app_debug_set_auto(lime_app_t *app, bool on);
