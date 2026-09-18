// main/c4_settings.h —— 标题屏菜单的纯逻辑:模式 / 难度 / 落子预览三档设置 + 选中行。
//
// 只依赖标准 C,便于宿主测试覆盖“双人模式下跳过难度行”“取值循环”这类边界。
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    C4_MODE_HUMAN_FIRST = 0,   // 人先手(玩家执琥珀色先下)
    C4_MODE_AI_FIRST,          // 电脑先手(电脑先下,玩家后手)
    C4_MODE_TWO_PLAYER,        // 双人同机轮流(琥珀色先手)
    C4_MODE_LINK,              // 两台设备蓝牙联机对战
    C4_MODE_COUNT,
} c4_mode_t;

typedef enum {
    C4_LEVEL_EASY = 0,
    C4_LEVEL_MEDIUM,
    C4_LEVEL_HARD,
    C4_LEVEL_COUNT,
} c4_level_t;

typedef enum {
    C4_PREVIEW_LANDING = 0,   // 预览画在真实落点
    C4_PREVIEW_TOP,           // 预览只画在该列最上一行
    C4_PREVIEW_COUNT,
} c4_preview_t;

// 菜单行,顺序即屏幕上的顺序。
typedef enum {
    C4_MENU_ROW_MODE = 0,
    C4_MENU_ROW_LEVEL,
    C4_MENU_ROW_PREVIEW,
    C4_MENU_ROW_START,
    C4_MENU_ROW_COUNT,
} c4_menu_row_t;

typedef struct {
    uint8_t mode;      // c4_mode_t
    uint8_t level;     // c4_level_t
    uint8_t preview;   // c4_preview_t
    uint8_t row;       // 当前选中的 c4_menu_row_t
} c4_settings_t;

// 默认:人先手 + 中等 + 顶部行预览,光标停在第一行。
void c4_settings_init(c4_settings_t *settings);

// 双人同机与联机模式下难度无意义,该行不参与选择也不可改。
bool c4_settings_row_enabled(const c4_settings_t *settings, int row);

// 上下移动选中行(环形,跳过被禁用的行);step 只取 ±1。
void c4_settings_move(c4_settings_t *settings, int step);

// 短按确定:切换当前行的取值;START 行不做任何事(由调用方开始对局)。
void c4_settings_cycle(c4_settings_t *settings);

bool c4_settings_is_start(const c4_settings_t *settings);

const char *c4_settings_row_label(int row);
const char *c4_settings_row_value(const c4_settings_t *settings, int row);
