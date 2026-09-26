// main/starry_ui.h —— 页面绘制层:把当前页面的内容交给渲染器画出来。
//
// 本模块只负责"画",不决定剧情走向与按键语义 —— 状态机在 starry_app.c。
#pragma once

#include "starry_app.h"

// 按 app->page 画一屏;force = 丢弃"画面/文本框已画"的去重缓存,整页重画。
void starry_ui_draw(starry_app_t *app, bool force);

// 当前页面里被选中行等状态变化时,只需要重画这一页(菜单/列表页用)。
void starry_ui_redraw_page(starry_app_t *app);

// 只重画阅读页的说话人 + 正文(打字机每推进一个字就调它)。
void starry_ui_draw_text(starry_app_t *app);
