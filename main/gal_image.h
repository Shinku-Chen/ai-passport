// main/gal_image.h —— 画面合成:320x150 画布 + 160x45 条带(518/256 显示)+ 立绘遮罩。
//
// 背景/CG/标题图在打包时已按 320x240 等比 fit(不裁切),拆成上下两段:
//   上段 320x150 解码到画布;下段 160x45 解码到条带,LVGL 放大 518/256 显示。
// 阅读时条带被不透明对白带盖住;鉴赏/标题页隐藏对白带,两段拼成完整 320x240。
// 立绘是 JPEG + 1bpp alpha 遮罩,解码到 150x150 暂存后按遮罩拷贝。
#pragma once

#include "gal_pack.h"
#include "gal_vm.h"

#include <stdbool.h>
#include <stdint.h>

struct _lv_obj_t;

#define GAL_ART_W 320
#define GAL_ART_H 150
#define GAL_IMAGE_BODY_BOTTOM 176   // bottom boundary; last body pixel = 175 (touches panel top)
#define GAL_IMAGE_BG_OFFSET 3   // preserve previous content offset; pack also top-covers
#define GAL_STRIP_W 160
#define GAL_STRIP_H 45
#define GAL_STRIP_SCALE 518
#define GAL_SCREEN_W 320
#define GAL_SCREEN_H 240
#define GAL_PORTRAIT_MAX 150

struct gal_overlay;
typedef struct {
    struct _lv_obj_t *art_canvas;
    struct _lv_obj_t *strip_canvas;
    uint16_t *art_px;
    uint16_t *strip_px;
    uint8_t *scratch;
    uint32_t scratch_size;
    struct gal_overlay *overlay; // 320x26 lower-body layer, above strip/art
    uint16_t shown;          // 当前背景图 id(GAL_NONE = 空)
    bool art_valid;          // 上段解码成功(仅全部组件成功后置位)
    bool strip_valid;        // 下段解码成功(仅全部组件成功后置位)
    bool body_valid;         // 当前帧所有立绘解码成功;失败立即回滚背景
    uint32_t last_ms;
} gal_image_t;

bool gal_image_init(gal_image_t *img, struct _lv_obj_t *parent,
                    uint16_t *art_px, uint16_t *strip_px,
                    uint8_t *scratch, uint32_t scratch_size);

// 清空两段画布(黑)。
void gal_image_clear(gal_image_t *img);

// 显示整屏图(kind FULL)或手册图(kind ART)。失败时两段画布统一清黑、
// art/strip/body 全部无效,返回 false;不会保留半帧或解锁。
bool gal_image_show(gal_image_t *img, const gal_pack_t *pack, uint16_t image_id,
                    const char **err);

// 在当前背景上叠加 VM 解出的立绘。返回 1=全部成功,0=没有立绘,-1=有失败。
// 任一立绘失败会立即重绘完整背景(不是保留部分立绘),并令所有缓存无效。
int gal_image_draw_bodies(gal_image_t *img, const gal_pack_t *pack,
                          const gal_sprite_t *bodies, uint8_t count);

// 仅重画立绘(背景已显示时用)。
bool gal_image_redraw_bodies(gal_image_t *img, const gal_pack_t *pack,
                             const gal_sprite_t *bodies, uint8_t count);

// 该图是否有完整上/下两段(鉴赏/标题全屏用)。
bool gal_image_is_full(const gal_pack_t *pack, uint16_t image_id, const uint8_t **top,
                       uint32_t *top_len, const uint8_t **bottom, uint32_t *bottom_len);

const gal_image_t *gal_image_state(const gal_image_t *img);
// Test/diagnostic accessor: overlay pixels are global y=150..175, x=0..319.
bool gal_image_overlay_pixel(const gal_image_t *img, int x, int y, uint16_t *out);
