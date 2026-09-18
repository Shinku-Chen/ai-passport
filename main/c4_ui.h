// main/c4_ui.h —— 四子棋界面(横屏 320x240)。只负责“画”,规则与菜单逻辑在 c4_model / c4_settings。
//
// 线程约定:本模块所有函数都必须在【持 bsp_lvgl_lock()】时调用(它们直接操作
// LVGL 对象)。对象在 c4_ui_build() 里一次建好、跨局复用,不做删除,因此不需要
// 处理“删屏前停任务”的收尾。
#pragma once

#include "c4_model.h"
#include "c4_settings.h"

#include <stdint.h>

// 建屏 + 建电池定时器。必须在 bsp_lvgl_init() 与 bsp_lvgl_set_landscape(true) 之后调用一次。
void c4_ui_build(void);

void c4_ui_battery_start(void);

// 载入设置屏(启动/长按确定返回)或棋盘屏。
void c4_ui_show_menu(void);
void c4_ui_show_board(void);

// 进入 deep sleep 前的提示:在两张屏上都写下「按任意键唤醒」。
void c4_ui_show_sleeping(void);

// 联机屏:搜索/连接/等对端时的专用界面(与设置屏、棋盘屏并列的第三种屏)。
void c4_ui_show_link(void);

// 重画联机屏的两行文字:状态大字 + 提示小字。
void c4_ui_render_link(const char *status, uint32_t status_color, const char *hint);

// 改写设置屏底部的提示行(联机验证用来显示 BLE 状态)。
void c4_ui_set_menu_hint(const char *text);

// 重画设置屏:选中行高亮、被禁用的行变暗。
void c4_ui_render_menu(const c4_settings_t *settings);

// 重画棋盘屏。cursor_col < 0 表示不画落子光标(例如电脑思考中);
// preview 决定预览盘画在真实落点还是该列最上一行。
void c4_ui_render(const c4_game_t *game, int cursor_col, c4_preview_t preview,
                  const char *status, uint32_t status_color);

// 落子动画:该列最上方的位置滑到落点。模型需已落子,动画只是视觉过渡。
void c4_ui_drop_anim(int col, int row);
