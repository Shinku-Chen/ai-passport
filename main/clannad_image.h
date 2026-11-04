// main/clannad_image.h —— 320x240 横屏画面合成：JPEG 背景/CG + JPEG+1bpp 遮罩立绘。
//
// TJpgDec 的回调逐 MCU 直接写入 320x240 RGB565 画布，任意缩放由回调完成：
// 不需要整图解码输出缓冲，也不需要第二块画布。
#pragma once

#include "clannad_pack.h"

#include <stdbool.h>
#include <stdint.h>

struct _lv_obj_t;

#define CLAN_SCREEN_W 320
#define CLAN_SCREEN_H 240
#define CLAN_IMG_W CLAN_SCREEN_W
#define CLAN_IMG_H 168   /* canvas is the art window; the panel is LVGL */
#define CLAN_ART_H 168
#define CLAN_PANEL_Y 162
#define CLAN_PANEL_H 78
// 阅读页硬指标：名字框外（压在画面上）、正文末行离屏幕底 ≥2px。
#define CLAN_NAME_Y 142
#define CLAN_LINES_TOP (CLAN_PANEL_Y + 20)
#define CLAN_BG_W 224
#define CLAN_BG_H 168
#define CLAN_BG_X 48
// 标题画是 320x240（4:3 竖高），画布只有 168 行：按 1:1 宽度铺满 320，
// 纵向居中裁切（顶部/底部各去掉 36 行），不做任何横向/纵向缩放。
#define CLAN_TITLE_H 240
#define CLAN_TITLE_Y (-(CLAN_TITLE_H - CLAN_IMG_H) / 2)
#define CLAN_ART_TOP 0
#define CLAN_SPRITE_H 160

typedef struct {
    struct _lv_obj_t *canvas;
    uint16_t *pixels;
} clannad_image_t;

bool clannad_image_init(clannad_image_t *img, struct _lv_obj_t *parent,
                        uint16_t *pixels);

// 清成 CLANNAD 主题深绿渐变（立绘框外/无背景时）。
void clannad_image_clear(clannad_image_t *img);

// 场景：背景完整 fit 到 224x168（左右 48px 主题边），立绘按源锚点等比缩放、
// 底边贴 CLAN_ART_H，最多 3 个。
bool clannad_image_show_scene(clannad_image_t *img, const clannad_pack_t *pack,
                              uint16_t background, const uint16_t bodies[3],
                              uint8_t body_count);

// CG 鉴赏：完整 fit 到 224x168 画面区（4:3，不裁切；字幕在下方 72px 面板）。
bool clannad_image_show_cg(clannad_image_t *img, const clannad_pack_t *pack,
                           uint16_t asset);
// 标题/UI 图（JPEG+遮罩）画到画布上的任意矩形。
bool clannad_image_show_asset(clannad_image_t *img, const clannad_pack_t *pack,
                              uint16_t asset, int x, int y, int w, int h,
                              bool use_mask);

// 只做 JPEG/声明尺寸/遮罩范围预检，不写画布；用于读档前拒绝坏图。
bool clannad_image_validate(const clannad_pack_t *pack, uint16_t asset);

// 立绘几何（供宿主回归）：返回 dst 矩形，失败返回 false。
bool clannad_image_portrait_box(uint8_t count, uint8_t index,
                                const clannad_asset_t *asset, int *x, int *y,
                                int *w, int *h);

void clannad_image_invalidate(clannad_image_t *img);
