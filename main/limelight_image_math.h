// main/limelight_image_math.h —— 画面区里"纯算术"的部分(不依赖 LVGL / esp_jpeg)。
//
// 拆出来是为了在宿主机上直接跑测试(tests/test_limelight_image_math.c):立绘摆放与
// 按遮罩合成、解码缓冲尺寸。这些都和芯片无关。
//
// 版面(竖屏 240x320):
//   y   0..213   画面区 = 一张 240x214 的 RGB565 画布(背景/CG + 立绘合成结果)
//   y 210..319   正文带:由 LVGL 画的半透明渐变蓝底 + 上面的文字(不进画布)
// 画布只做 214 行而不是 320 行,是为了让整块画布 + 立绘解码缓冲塞得进
// ESP32-C3 的 313.8 KiB DRAM(见 docs 里那份 RAM 预算表)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 面板尺寸与画布尺寸。
#define LIME_SCREEN_W 240
#define LIME_SCREEN_H 320
#define LIME_ART_W 240
#define LIME_ART_H 214

// 立绘上限,与 tools/limelight_material_pack.py 的 SPRITE_MAX_W/H/PIXELS 一致。
#define LIME_SPRITE_MAX_W 168
#define LIME_SPRITE_MAX_H 252
#define LIME_SPRITE_MAX_PIXELS 23000
#define LIME_SPRITE_EDGE_MARGIN 1      // 贴底/右边留一点边
// 立绘至少要露出这么多行(在正文带上方)。矮立绘(实测有一批 168x100 的)如果也贴
// 屏幕底,会整个落在对话框后面看不见,所以给它们一个上限。
#define LIME_SPRITE_MIN_VISIBLE 110

// 立绘摆放:贴屏幕右下角 —— x 贴右,底边贴**屏幕**底(y = 320 - h - 边距),
// 这样下摆会伸进正文带那 106 行里、被对话框盖住(而不是被画面区底边切断)。
int lime_sprite_origin_x(int sprite_w);
int lime_sprite_origin_y(int sprite_h);

// 立绘落在画布(240x214)以内的行数;剩下的行由 LVGL 画在对话框下面。
int lime_sprite_rows_in_canvas(int sprite_h);
int lime_sprite_rows_below_canvas(int sprite_h);

// 立绘合成:mask 是解好的 1bpp 逐行位图(每行 ceil(w/8) 字节,行首在最高位,
// 1 = 不透明);rgb565 是小端 RGB565 紧密排列(设备上是 JPEG 解码结果)。
// 越界部分按画布裁剪。返回写入的像素数。
uint32_t lime_sprite_blit(uint16_t *canvas, int canvas_w, int canvas_h,
                          const uint8_t *rgb565, int sprite_w, int sprite_h,
                          const uint8_t *mask, int x, int y);

// 立绘解码缓冲需要多少字节(按素材包里最大的立绘算)。
uint32_t lime_sprite_buffer_size(int max_w, int max_h);
