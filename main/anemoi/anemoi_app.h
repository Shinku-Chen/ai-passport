// main/anemoi/anemoi_app.h —— 应用状态机（纯逻辑，渲染与 NVS 通过薄接口注入）。
//
// 屏幕：标题 / 正文 / 待选 / 菜单 / 存档 / 读档 / 章节 / 鉴赏列表 / CG /
//       历史 / 设置 / 关于 / 结局 / 分卷边界 / 错误。
// 三键语义见 docs/development/engineering/galgame-anemoi.md：
//   正文  UP 短按推进、UP 长按快进（松手停）、DOWN 短按历史、DOWN 长按自动、
//         OK 短按推进/补全打字、OK 长按菜单
//   列表  UP/DOWN 移动、OK 短按进入、OK 长按返回来源页
//   待选  UP/DOWN 移动、OK 短按显式确认、OK 长按菜单（返回后保留待选与光标）
//
// 所有 LVGL 写入都在渲染器里持 bsp_lvgl_lock()；本文件不接触 LVGL。
#pragma once

#include "anemoi_canvas.h"
#include "anemoi_image.h"
#include "anemoi_pack.h"
#include "anemoi_script.h"
#include "anemoi_store.h"
#include "anemoi_ui.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANEM_SLOT_META_MAX 31

typedef struct {
    bool valid;
    uint16_t chapter;
    uint16_t text;
    uint16_t page;
    uint8_t mode;
    uint32_t saved_at;
} anem_slot_meta_t;

enum {
    ANEM_KEY_UP = 0,
    ANEM_KEY_DOWN,
    ANEM_KEY_OK,
};

enum {
    ANEM_EV_PRESS = 0,
    ANEM_EV_CLICK,
    ANEM_EV_LONG,
    ANEM_EV_RELEASE,
};

typedef enum {
    ANEM_AUTO_OFF = 0,
    ANEM_AUTO_SLOW,
    ANEM_AUTO_NORMAL,
    ANEM_AUTO_FAST,
} anem_auto_speed_t;

typedef enum {
    ANEM_SPEED_INSTANT = 0,
    ANEM_SPEED_FAST,
    ANEM_SPEED_NORMAL,
    ANEM_SPEED_SLOW,
} anem_text_speed_t;

typedef struct {
    anem_pack_t pack;
    anem_script_t script;
    anem_canvas_t canvas;
    anem_image_ctx_t image;
    bool draw_art;               // 宿主逻辑测试可关掉真实解码

    uint8_t screen;
    uint8_t back_screen;         // 列表返回来源
    uint8_t menu_back;           // 菜单返回来源
    uint16_t sel[ANEM_SCR_COUNT];
    uint16_t scroll[ANEM_SCR_COUNT];
    uint8_t choice_sel;
    uint8_t overwrite_pending;   // 0 = 无；否则待覆盖的槽位 + 1

    // 打字机 / 分页
    const char *line_text;
    uint16_t line_chars;
    uint16_t typed_chars;
    uint16_t page;
    uint16_t pages;
    uint32_t next_type_ms;
    char page_lines[ANEM_LINE_MAX][ANEM_LINE_TEXT];
    uint8_t page_line_count;

    // 自动 / 快进
    uint8_t auto_speed;
    bool auto_enabled;
    // 返回栈：open_list() 压入来源，back_to() 弹出并恢复 back_screen，
    // 避免“A→B 再把 B 的 back_screen 改成 B 自己”导致退不出。
    uint8_t back_stack_screen[8];
    uint8_t back_stack_back[8];
    uint8_t back_depth;
    uint32_t auto_deadline;
    bool ff_held;
    uint32_t ff_next_ms;

    // 设置
    uint8_t text_speed;
    uint8_t panel_alpha;         // 0..100（渲染器用）

    // 存档
    anem_slot_meta_t slot_meta[ANEM_SLOT_META_MAX];  // [0] = 自动档
    anem_slot_t slot_tmp;
    uint8_t list_auto_index;

    // 全局解锁
    anem_progress_t progress;
    bool progress_dirty;
    bool progress_ready;
    uint32_t progress_retry_ms;

    // 当前画面（画布上要画的内容）
    uint16_t art_image;
    bool art_cg;
    uint8_t art_sprite_count;
    anem_sprite_t art_sprites[ANEM_BODY_MAX];
    uint16_t signature_bg;
    uint16_t signature_cg;
    uint8_t signature_bodies;
    uint16_t flow_scene;
    uint16_t title_image;

    // 鉴赏
    uint8_t cg_index;
    uint16_t cg_slot;
    bool art_dirty;
    bool art_failed;
    char notice[ANEM_NOTICE_TEXT];
    uint8_t notice_kind;
    uint32_t notice_until_ms;

    // 标题
    uint8_t title_level;
    bool has_auto;
    bool has_any_save;
    uint32_t now_ms;
} anem_app_t;

// 打开资源包并初始化（不在包内解码任何图）。
bool anem_app_init(anem_app_t *app, const void *pack_data, size_t pack_size);

// 重新读取存档元数据与全局进度（启动与读档页打开时调用）。
void anem_app_refresh_store(anem_app_t *app, uint32_t now_ms);

// 键事件。PRESS 只用于导航/开始快进；CLICK 是延迟确认；LONG 是长按动作。
void anem_app_key(anem_app_t *app, uint8_t key, uint8_t event, uint32_t now_ms);

// 时间推进：打字机、自动播放、快进、进度重试、提示过期。
void anem_app_tick(anem_app_t *app, uint32_t now_ms);

// 生成当前界面模型（渲染器与测试共用）。
void anem_app_view(anem_app_t *app, anem_view_t *view);

// 画面重绘（把 art_image 画进画布）。返回 false 表示解码失败。
bool anem_app_draw(anem_app_t *app);

// 进度落盘（标题/鉴赏/结局/睡前都会调用；失败保持 dirty 以便重试）。
bool anem_app_flush_progress(anem_app_t *app);

// 自动档落盘（换场 / 回标题 / 睡前）。
bool anem_app_flush_auto(anem_app_t *app);

// 当前是否需要把画布内容重绘到屏幕。
bool anem_app_art_dirty(const anem_app_t *app);

#ifdef __cplusplus
}
#endif
