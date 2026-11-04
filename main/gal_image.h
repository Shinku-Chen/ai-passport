// main/gal_image.h —— 画面合成:背景/CG(JPEG)+ 立绘(JPEG + 1bpp 遮罩)。
//
// 版面固定横屏 320x240,拆成两块 1:1 画布:
//   y 0..149   画面区(art canvas)
//   y 150..239 文本框背后的条带(strip canvas),半透明面板叠在上面
// 两块拼起来就是完整一帧,设备端不做缩放/裁剪,只做"解码 + 按遮罩拷贝"。
//
// 立绘最多三个槽位(左/中/右),每个槽位一张图,解码到同一块暂存再合成。
#pragma once

#include "gal_pack.h"

#include <stdbool.h>
#include <stdint.h>

struct _lv_obj_t;

#define GAL_ART_W 320
#define GAL_ART_H 150
// 条带(文本框背后的 90 行)以 1/2 分辨率存与画:被 55%~95% 面板盖住,看不出差别,
// 却省 43KB DRAM。屏上是 320x90,画布是 160x45,由 LVGL 放大 2 倍。
#define GAL_STRIP_W 160
#define GAL_STRIP_H 45
#define GAL_STRIP_SCALE 512     // 2x
#define GAL_STRIP_DISPLAY_W (GAL_STRIP_W * 2)
#define GAL_STRIP_DISPLAY_H (GAL_STRIP_H * 2)

// 同一角色不同表情的 JPEG 长度差上限（字节）。包内实测：同角色 <=40，不同角色 >=400。
#define GAL_SPRITE_SAME_CHAR_LEN 160u
#define GAL_SCREEN_H 240
#define GAL_SPRITE_MAX_W 96   // 本作立绘缩放后宽 90px(143x380 -> 240 高);留 6px 余量

typedef struct {
    struct _lv_obj_t *art_canvas;
    struct _lv_obj_t *strip_canvas;
    uint16_t *art_px;            // GAL_ART_W * GAL_ART_H,调用方提供(static)
    uint16_t *strip_px;          // GAL_STRIP_W * GAL_STRIP_H(半分辨率)
    uint8_t *scratch;            // 立绘解码暂存:GAL_SPRITE_MAX_W * GAL_ART_H * 2
    uint32_t scratch_size;
    uint32_t last_decode_ms;
    uint16_t shown;              // 当前画面图 id(GAL_NONE = 未画)
} gal_image_t;

// 建画布并绑定缓冲(需持有 LVGL 锁)。
bool gal_image_init(gal_image_t *img, struct _lv_obj_t *parent,
                    uint16_t *art_px, uint16_t *strip_px,
                    uint8_t *scratch, uint32_t scratch_size);

// 把一张包内图片(bg 或 CG)铺满两块画布。
bool gal_image_show(gal_image_t *img, const gal_pack_t *pack, uint16_t image_id);

// 在已画好的画面上叠加三个槽位的立绘;slot 为 GAL_NONE 表示空。
// 返回 1 = 有立绘且全部成功;0 = 没有立绘(正常);-1 = 某张要求的立绘失败。
int gal_image_draw_sprites_checked(gal_image_t *img, const gal_pack_t *pack,
                                   const uint16_t spr[3]);
bool gal_image_draw_sprites(gal_image_t *img, const gal_pack_t *pack, const uint16_t spr[3]);

// 清成黑色(换页/标题页时会用到)。
void gal_image_clear(gal_image_t *img);

// 测试钩子：累计 blit 次数（验证同一帧内同一 sprite 只画一次）。
int gal_image_test_blits(void);
void gal_image_test_reset_blits(void);
