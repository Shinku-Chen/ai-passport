// main/anemoi/anemoi_render.h —— 把界面模型画成 LVGL 对象（唯一接触 LVGL 的界面层）。
#pragma once

#include "anemoi_app.h"
#include "anemoi_ui.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 创建对象树。必须在 bsp_lvgl_lock() 之后调用。
bool anem_render_init(void);

// 应用一帧模型。调用者必须持 bsp_lvgl_lock()。
void anem_render_apply(const anem_view_t *view, uint8_t panel_alpha);

// 画布像素变了之后让 LVGL 重画图片（持锁调用）。
// 把画布的 lv_image_dsc_t 绑到画面控件（canvas RGB565，320x180）。
void anem_render_set_art(const void *dsc, uint16_t w, uint16_t h);
void anem_render_invalidate_art(void);

#ifdef __cplusplus
}
#endif
