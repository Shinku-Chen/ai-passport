// main/dracu_app.h —— 应用状态机(《DRACU-RIOT!》阅读器,从 ATRI / 千恋万花阅读器移植)。
//
// 页面切换、按键语义、自动存档、电量刷新、空闲休眠都在这里;剧情推进/回退/分页/存档
// 语义由 main/dracu_model.[ch]、main/dracu_scn.[ch]、main/dracu_pack.[ch] 保持。
//
// 按键语义(三键):
//   标题/章节/列表页: 上/下 移动光标,确定 进入,长按确定 返回上一层
//   正文页:     上/下短按 推进(打字中=立即显示全文,本句还有下一页=翻页),
//               长按上 快进(松手停),长按下 切换自动阅读,确定 打开菜单
//   章节卡:     任意键跳过(过场计时结束也跳过)
//   选项页:     上/下 选择,确定 确认
//   存档页:     上/下 选择,确定 存/读,长按确定 删除该档(存档模式下)
#pragma once

#include "dracu_save.h"
#include "dracu_scn.h"
#include "dracu_ui.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t btn;    // bsp_btn_t
    uint8_t ev;     // bsp_btn_ev_t
} dracu_key_t;

typedef struct {
    dracu_pack_t pack;
    dracu_scn_t scn;
    dracu_player_t player;
    dracu_ui_t ui;
    dracu_settings_t settings;
    // 剧本包的解块缓冲:读一页、读一句都靠它(本板没有 PSRAM,只能给一块)
    uint8_t scn_scratch[DRACU_BLOCK_BUFFER];

    dracu_screen_t page;
    dracu_screen_t settings_return;
    dracu_screen_t slots_return;
    dracu_screen_t chapters_return;

    bool slots_saving;
    int title_sel;
    int menu_sel;
    int settings_sel;
    int slots_sel;
    int chapters_sel;

    dracu_layout_t layout_hint;   // 分页参数(16px 字、26 单位 x 5 行)
    uint16_t title_bg;            // 标题图在背景池里的 id(启动时按名字查一次)

    bool transition_pending;   // 章节卡:等按键或过场计时结束
    uint32_t transition_ms;
    bool fast_forward;         // 长按上快进中(松手即停)
    uint32_t fast_forward_ms;
    bool auto_play;            // 自动阅读模式(长按下开关;任意键停止)
    uint32_t auto_ms;          // 自动模式:一屏读完后再等这么久就翻
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
} dracu_app_t;

// 初始化:打开图片包与剧本包、NVS、建界面、进入首个页面(提示页或标题页)。
// art_pixels 必须是 DRACU_ART_W * DRACU_ART_H 的静态缓冲;调用时需持有 LVGL 锁。
bool dracu_app_init(dracu_app_t *app, const uint8_t *pack_data, uint32_t pack_size,
                    const uint8_t *scn_data, uint32_t scn_size, uint16_t *art_pixels,
                    const lv_font_t *font_cjk);

// 处理一次按键。调用时需持有 LVGL 锁。
void dracu_app_key(dracu_app_t *app, const dracu_key_t *key);

// 周期性 tick(累计空闲时间、电量刷新间隔、过场计时)。调用时需持有 LVGL 锁。
void dracu_app_tick(dracu_app_t *app, uint32_t elapsed_ms);

// 自动阅读是否正在自行翻页(停在选项或结局上时不算),供空闲熄灭豁免使用。
bool dracu_app_auto_reading(const dracu_app_t *app);

// 屏幕是否必须保持常亮:自动阅读开着(哪怕正停在选项/结局上)、快进中、章节卡过场中
// 都算。玩家的硬要求是「自动模式不息屏、不暗屏」,所以这里的判断要比 auto_reading 宽。
bool dracu_app_wants_screen_on(const dracu_app_t *app);

// 空闲时长(毫秒),供 main 决定变暗/熄屏/深睡。
uint32_t dracu_app_idle_ms(const dracu_app_t *app);

// 进入深睡前保存进度。
void dracu_app_before_sleep(dracu_app_t *app);

// 深睡前在屏幕上显示提示(唤醒后设备会重启,状态无需恢复)。
void dracu_app_show_sleeping(dracu_app_t *app);

// 取走一次"立即休眠"请求(标题页长按确定 / 设置页"关机")。
bool dracu_app_take_sleep_request(dracu_app_t *app);

// 调试用:把第 page 页画进画面区(会移动阅读位置,但不改页面、不落盘),串口截图用。
bool dracu_app_debug_render(dracu_app_t *app, uint32_t page);

// 调试用:从第 page 页开始阅读(改玩家状态、正常落盘),用于"开机直接进某一页"。
bool dracu_app_debug_start(dracu_app_t *app, uint32_t page);

// 调试:截图前把打字机收尾 / 关掉自动推进,让画面稳定下来。
// (不做这一步的话,LVGL 的局部重绘会在整帧捕获期间混进好几帧文字,截出来的字是花的。)
void dracu_app_debug_settle(dracu_app_t *app);

// 调试:切到标题页(串口截图用)。
bool dracu_app_debug_title(dracu_app_t *app);

// 调试:往回退一屏(页内上一屏 / 上一页,受模型的堵死页限制)。
bool dracu_app_debug_back(dracu_app_t *app);

// 调试:走一次「跳过章节」的完整路径(与菜单里那一条相同)。
bool dracu_app_debug_skip_chapter(dracu_app_t *app);

// 调试:存/读档。slot >= 0 为手动档位,slot < 0 为自动档。
bool dracu_app_debug_save(dracu_app_t *app, int slot);
bool dracu_app_debug_load(dracu_app_t *app, int slot);

// 调试:把当前阅读位置写成一行文本。
int dracu_app_debug_info(dracu_app_t *app, char *out, size_t capacity);
