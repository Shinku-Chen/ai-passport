// main/steins0_image.h —— 320x240 画面合成:JPEG + 1bpp 遮罩,逐块解码写入 RGB565 画布。
#pragma once

#include "steins0_pack.h"

#include <stdbool.h>
#include <stdint.h>

struct _lv_obj_t;

#define SG0_SCREEN_W 320
#define SG0_SCREEN_H 240
#define SG0_IMG_W 320
#define SG0_IMG_H 180
#define SG0_VIEW_W 432   // 源场景坐标系
#define SG0_VIEW_H 243

typedef struct {
    struct _lv_obj_t *canvas;
    uint16_t *pixels;
    uint16_t width;
    uint16_t height;
} steins0_image_t;

bool steins0_image_init(steins0_image_t *image, struct _lv_obj_t *parent, uint16_t *pixels);
bool steins0_image_init_ex(steins0_image_t *image, struct _lv_obj_t *parent, uint16_t *pixels,
                           uint16_t width, uint16_t height);
void steins0_image_clear(steins0_image_t *image);

// 场景:按源 view 图层顺序绘制(背景 + 立绘 + 表情补丁),等比缩放到 320x180。
// 任一图层失败返回 false(调用方保持缓存失效并提示)。
bool steins0_image_show_scene(steins0_image_t *image, const sg0_pack_t *pack,
                              uint16_t view, bool draw);
bool steins0_image_show_cg(steins0_image_t *image, const sg0_pack_t *pack, uint16_t asset,
                           bool draw);
bool steins0_image_show_title(steins0_image_t *image, const sg0_pack_t *pack, uint16_t asset,
                              bool draw);
bool steins0_image_show_card(steins0_image_t *image, const sg0_pack_t *pack, uint16_t asset,
                             bool draw);
bool steins0_image_show_stamp(steins0_image_t *image, const sg0_pack_t *pack, uint16_t asset,
                              bool draw);

// 只解码、不写画布:用于换场/读档前的完整性预检。
bool steins0_image_validate_asset(const sg0_pack_t *pack, uint16_t asset);
bool steins0_image_validate_view(const sg0_pack_t *pack, uint16_t view);

// 公开给宿主几何回归。
void steins0_image_layer_box(const sg0_layer_t *layer, int *x, int *y, int *w, int *h);
