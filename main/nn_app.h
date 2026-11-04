// main/nn_app.h —— 《男娘恋爱物语》阅读器的应用状态机（页面、按键、存档、鉴赏）。
//
// 与界面分离：nn_app 只处理“现在该显示什么、按键意味着什么、进度存到哪”，
// 具体的 LVGL 对象在 nn_ui 里。因此 nn_app 可以只依赖 nn_script / nn_save
// 的类型，便于宿主侧审视状态迁移。
#pragma once

#include "gal_text.h"
#include "nn_pack.h"
#include "nn_save.h"
#include "nn_script.h"
#include "nn_ui.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 按键与事件：数值与 components/bsp/include/bsp_button.h 的 bsp_btn_t /
// bsp_btn_ev_t 一致（nn_app.c 里有 _Static_assert 守着）。这里重新声明一份，
// 是为了让状态机在宿主测试里不依赖 BSP 头文件。
typedef enum { NN_BTN_UP = 0, NN_BTN_DOWN = 1, NN_BTN_OK = 2 } nn_btn_t;
typedef enum {
    NN_EV_PRESS = 0,
    NN_EV_CLICK = 1,
    NN_EV_DOUBLE = 2,
    NN_EV_LONG = 3,
    NN_EV_RELEASE = 4,
} nn_btn_ev_t;

typedef struct {
    uint8_t btn;   // nn_btn_t（= bsp_btn_t）
    uint8_t ev;    // nn_btn_ev_t（= bsp_btn_ev_t）
} nn_key_t;

#define NN_HISTORY_MAX 32
// 返回栈深度：正文/菜单/设置/关于 四层足够；栈满时丢最旧的一层（保留最近的来源）。
#define NN_RETURN_STACK_MAX 4

typedef struct {
    const nn_pack_t *pack;
    nn_ui_t ui;
    nn_rt_t rt;
    nn_settings_t settings;

    nn_page_t page;
    // 返回栈：进入子页时压栈，长按 OK 统一弹栈，栈空回标题。
    // 之前是单个 return_page：设置→关于会把它改写成 SETTINGS，第二次返回自指。
    nn_page_t ret_stack[NN_RETURN_STACK_MAX];
    uint8_t ret_depth;
    bool choice_pending;       // 选项已出现但还没选：离开选项页不得丢掉分支

    // 当前这句
    const char *line_text;
    uint16_t line_len;
    uint16_t line_speaker;
    uint16_t line_scene;
    uint16_t line_op;          // 该句 SAY 操作的下标（存档点）
    uint32_t page_off[GAL_TEXT_MAX_PAGES + 1];
    int page_count;
    int page_index;

    // 打字机 / 自动 / 快进
    int shown_chars;   // 打字机已显示的字符数（不是字节数）
    uint32_t type_accum_ms;
    bool typing;
    bool auto_mode;
    bool ff_hold;
    bool suppress_click;   // 长按 OK 后驱动补发的 CLICK 必须吞掉，否则会立刻"下一句"
    uint32_t suppress_tick; // 吞 CLICK 的失效时间戳（ms），避免永久吞掉后续确认

    uint32_t ff_accum_ms;
    uint32_t auto_accum_ms;
    bool auto_wait;

    bool slots_saving;         // 存档页当前是“保存”还是“读取”（不靠返回栈猜）

    // 选项
    uint8_t choice_count;
    int choice_sel;

    // 列表光标
    int title_sel;
    int menu_sel;
    int slot_sel;
    int settings_sel;
    int history_sel;
    int history_top;
    int gallery_sel;
    int gallery_page;

    // 结局
    uint8_t ending_id;

    // 历史（只存指针：文本在 flash 包里，不占 RAM）
    const char *hist_text[NN_HISTORY_MAX];
    uint16_t hist_speaker[NN_HISTORY_MAX];
    int hist_count;
    int hist_head;

    // 状态
    uint32_t uptime_s;
    uint32_t idle_ms;
    int battery;
    uint32_t battery_accum_ms;
    char header_buf[24];
    bool error_shown;
} nn_app_t;

// 初始化：pack 必须已经解析成功。返回 false 表示界面创建失败。
bool nn_app_init(nn_app_t *app, const nn_pack_t *pack);
void nn_app_key(nn_app_t *app, const nn_key_t *key);
// 离开再回到游戏页时，分带内容可能已被别的页面覆盖 → 强制下帧重画。
void nn_app_invalidate_view(void);
void nn_app_tick(nn_app_t *app, uint32_t elapsed_ms);

// 空闲时长（毫秒）：按键、自动推进都会清零；用于调暗/熄屏/休眠判定。
uint32_t nn_app_idle_ms(const nn_app_t *app);

// 睡前把自动进度落盘。
void nn_app_flush(nn_app_t *app);

#ifdef __cplusplus
}
#endif
