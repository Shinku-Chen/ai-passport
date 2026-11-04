// main/steins_image.h —— 320x180 RGB565 画布 + JPEG 合成。
//
// 画面层完全没有缩放缓冲:一块 320x180 的画布(115,200 B)就是全部像素内存,
// 每层 JPEG 由 TJpgDec 直接解到目标矩形里(可以部分出界,写像素时才裁剪),
// 因此"源的 object-fit:fill + 视口裁剪"语义被完整保留,不需要整帧 RGBA/PNG 缓存。
//
// 立绘的 alpha 在包里是半分辨率 1bpp 遮罩:解码时双线性放大成 5 级 alpha,
// 比硬阈值更接近源图抗锯齿的软边。
#pragma once

#include "steins_pack.h"

#include <stdbool.h>
#include <stdint.h>

#define STEINS_IMAGE_W STEINS_CANVAS_W
#define STEINS_IMAGE_H STEINS_CANVAS_H
#define STEINS_IMAGE_PIXELS (STEINS_IMAGE_W * STEINS_IMAGE_H)

typedef struct {
    uint16_t *pixels;
    struct _lv_obj_t *canvas;
    // 可选：按 pack 的 layer.opacity 压暗“非说话人”立绘（原作效果）。默认关。
    bool dim_speakers;
} steins_image_t;

// 最近一次合成失败的现场(供界面提示"哪一层图坏了"并保持缓存失效)。
typedef struct {
    uint16_t asset;
    uint16_t layer;
    uint8_t reason;     // STEINS_IMG_ERR_*
} steins_image_error_t;

enum {
    STEINS_IMG_OK = 0,
    STEINS_IMG_ERR_PACK,      // 资源下标/表项非法
    STEINS_IMG_ERR_PREPARE,   // TJpgDec 解析失败
    STEINS_IMG_ERR_SIZE,      // 声明尺寸与 JPEG 头不符
    STEINS_IMG_ERR_DECOMP,    // 解码失败
    STEINS_IMG_ERR_EMPTY,     // 一个像素都没写出去
};

bool steins_image_init(steins_image_t *image, struct _lv_obj_t *parent, uint16_t *pixels);
// 画布位置上移/下移(CG 全屏时居中,正文时贴顶)。
void steins_image_place(steins_image_t *image, int y);
void steins_image_clear(steins_image_t *image, uint16_t color);

// 把某个 view 的所有层按数组顺序(后者在上)合成到画布。
// 任一层失败时的结果是**确定的**:画布立刻被清成黑色(失败画面),刷一次屏,
// 然后返回 false 并填 err_out。这样绝对不会把“半张立绘”留在屏幕上。
// 下一次用同一个 view 重画成功时画布才回到正常内容。
bool steins_image_show_view(steins_image_t *image, const steins_pack_t *pack,
                            uint16_t view_index, steins_image_error_t *err_out);
// 单独一张图:fit=true 等比装进画布并居中(鉴赏/标题),false 则铺满画布。
// 解码失败时与 show_view 一致:清黑 + 刷屏 + 返回 false。
bool steins_image_show_asset(steins_image_t *image, const steins_pack_t *pack,
                             uint16_t asset_index, bool fit, steins_image_error_t *err_out);
// 解码一张图但不落地(自检用):返回 false 表示这张图在本包/本解码器下不可用。
bool steins_image_validate(const steins_pack_t *pack, uint16_t asset_index,
                           steins_image_error_t *err_out);

// 单层绘制(测试与 CG 叠加用),dst 矩形未裁剪。
bool steins_image_draw_asset(steins_image_t *image, const steins_pack_t *pack,
                             uint16_t asset_index, int x, int y, int w, int h,
                             uint16_t opacity, steins_image_error_t *err_out);
