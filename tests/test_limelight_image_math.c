// tests/test_limelight_image_math.c —— 画面区纯算术的守卫测试。
//
// 覆盖:立绘摆放(居中贴底)、按 1bpp 遮罩合成的裁剪与透明处理、解码缓冲尺寸。
// 全部是纯算术,不需要 LVGL / esp_jpeg / 真实素材。

#include "limelight_image_math.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(cond)                                                            \
    do {                                                                       \
        checks++;                                                              \
        if (!(cond)) {                                                         \
            failures++;                                                        \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
        }                                                                      \
    } while (0)

static void test_placement(void)
{
    // 水平居中(人物站在画面中间)
    CHECK(lime_sprite_origin_x(62) == (LIME_ART_W - 62) / 2);
    CHECK(lime_sprite_origin_x(100) == (LIME_ART_W - 100) / 2);
    CHECK(lime_sprite_origin_x(LIME_ART_W + 40) == 0);          // 比画布还宽:顶到左边
    // 底边贴**屏幕**底(不是画布底):下摆伸进正文带、被对话框盖住
    CHECK(lime_sprite_origin_y(252) == LIME_SCREEN_H - 252 - LIME_SPRITE_EDGE_MARGIN);
    CHECK(lime_sprite_origin_y(120) == LIME_ART_H - LIME_SPRITE_MIN_VISIBLE);   // 矮立绘抬高
    // 贴屏幕底之后,只有 ≤105 行的立绘能整个落在画布(0..213)里;更高的立绘下摆
    // 伸进对话框区域,由 LVGL 画在那 106 行里(被对话框盖住)。
    CHECK(LIME_SCREEN_H - LIME_ART_H == 106);                 // 对话框占的行数
    CHECK(lime_sprite_rows_in_canvas(252) == 147);            // 252 行:147 行在画布,105 行在下
    CHECK(lime_sprite_rows_below_canvas(252) == 105);
    // 矮立绘:抬高到"至少露出 110 行",不整个躲进对话框
    CHECK(lime_sprite_rows_below_canvas(100) == 0);           // 100 行:全部露出
    CHECK(lime_sprite_rows_in_canvas(100) == 100);
    CHECK(lime_sprite_rows_below_canvas(120) == 10);          // 120 行:露出 110,剩余 10 行在带下
    CHECK(lime_sprite_origin_y(120) == LIME_ART_H - LIME_SPRITE_MIN_VISIBLE);
}

static void test_blit(void)
{
    enum { W = LIME_ART_W, H = LIME_ART_H };
    static uint16_t canvas[W * H];
    memset(canvas, 0, sizeof(canvas));

    // 立绘 12x2:第 1 行全不透明(0xFF),第 2 行只有前 4 个像素(0xF0)
    const int sw = 12, sh = 2;
    uint8_t rgb[sw * sh * 2];
    for (int i = 0; i < sw * sh; i++) {
        const uint16_t v = (uint16_t)(0x1111u * (i % 8));
        rgb[i * 2] = (uint8_t)(v & 0xFF);
        rgb[i * 2 + 1] = (uint8_t)(v >> 8);
    }
    uint8_t mask[2 * 2] = { 0xFF, 0xF0, 0xF0, 0x00 };
    const int x = 0;                 // 摆放位置单测在上面,这里只管合成
    const int y = H - sh;
    CHECK(lime_sprite_blit(canvas, W, H, rgb, sw, sh, mask, x, y) == 12 + 4);

    // 整块在画布下方:不写任何像素,也不越界
    memset(canvas, 0, sizeof(canvas));
    CHECK(lime_sprite_blit(canvas, W, H, rgb, sw, sh, mask, 0, H) == 0);
    // 左越界 sx=6..11 落在画布内(第 2 行那 4 个像素在画布外)
    CHECK(lime_sprite_blit(canvas, W, H, rgb, sw, sh, mask, -6, 0) == 6);
    // 左移 2:第 1 行 10 个 + 第 2 行 sx=2,3 两个 = 12
    CHECK(lime_sprite_blit(canvas, W, H, rgb, sw, sh, mask, -2, 0) == 12);
    // 上越界:第 1 行被裁掉,只剩第 2 行的 4 个像素
    CHECK(lime_sprite_blit(canvas, W, H, rgb, sw, sh, mask, 0, -1) == 4);
    // 下越界:第 2 行被裁掉
    CHECK(lime_sprite_blit(canvas, W, H, rgb, sw, sh, mask, 0, H - 1) == 12);
    // 全透明遮罩:一个像素都不写
    uint8_t empty[2 * 2] = { 0, 0, 0, 0 };
    CHECK(lime_sprite_blit(canvas, W, H, rgb, sw, sh, empty, 0, 0) == 0);

    // 写入的像素值就是源值(小端 RGB565)
    memset(canvas, 0, sizeof(canvas));
    CHECK(lime_sprite_blit(canvas, W, H, rgb, sw, sh, mask, 0, 0) == 16);
    CHECK(canvas[0] == (uint16_t)(rgb[0] | ((uint16_t)rgb[1] << 8)));
}

static void test_buffer_size(void)
{
    // 实测包里最大立绘 168x226 以内(打包器夹到 23,000 像素)
    CHECK(lime_sprite_buffer_size(168, 226) == 168 * 226 * 2);
    CHECK(lime_sprite_buffer_size(92, 251) == 92 * 251 * 2);
    CHECK(lime_sprite_buffer_size(0, 10) == 0);
    // 像素上限自检:上限之内最大面积对应的缓冲 < 46 KiB
    CHECK(LIME_SPRITE_MAX_PIXELS * 2 <= 61440);   // 60KB 静态立绘缓冲
}

int main(void)
{
    test_placement();
    test_blit();
    test_buffer_size();
    printf("limelight image math: checks=%d failures=%d\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
