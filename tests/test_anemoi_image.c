// tests/test_anemoi_image.c —— 真实 JPEG / BLOCK 解码 + 坏图边界。
//
// 夹具由 tools/anemoi_fixtures.py 生成：一张左红右绿的 JPEG、一个 4x4 调色板
// BLOCK（含半透明与全透明像素）、截断 JPEG、坏块表、越界调色板下标。
#include "anemoi_canvas.h"
#include "anemoi_image.h"
#include "anemoi_pack.h"

#include "anemoi_test_util.h"

#include <stdio.h>
#include <string.h>

static uint16_t rgb565(int r, int g, int b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static int decode_rgb(uint16_t pixel, int *r, int *g, int *b)
{
    *r = (pixel >> 11) & 0x1F;
    *g = (pixel >> 5) & 0x3F;
    *b = pixel & 0x1F;
    return 0;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "tests/anemoi/fixtures/images_pack.bin";
    size_t size = 0;
    unsigned char *blob = anem_test_read(path, &size);
    if (blob == NULL) {
        fprintf(stderr, "SKIP: 缺夹具 %s（先运行 tools/anemoi_fixtures.py）\n", path);
        return 77;
    }
    anem_pack_t pack;
    ANEM_CHECK(anem_pack_open(&pack, blob, size));
    anem_canvas_t canvas;
    ANEM_CHECK(anem_canvas_init(&canvas));
    anem_image_ctx_t ctx;
    anem_image_init(&ctx);

    // 1) 真实 JPEG：左半红、右半绿（压缩噪声允许一定误差）
    anem_canvas_fill(&canvas, 0x0000);
    ANEM_CHECK(anem_image_draw_scene(&ctx, &canvas, &pack, 0));
    int r, g, b;
    // 32x16 的图在画布上居中：dx = (320-32)/2 = 144, dy = (180-16)/2 = 82
    const int dx = (ANEM_CANVAS_W - 32) / 2;
    const int dy = (ANEM_CANVAS_H - 16) / 2;
    decode_rgb(canvas.pixels[(dy + 4) * ANEM_CANVAS_W + dx + 4], &r, &g, &b);
    ANEM_CHECK_MSG(r > 24 && g < 8, "JPEG 左上应为红色，实际 r=%d g=%d b=%d", r, g, b);
    decode_rgb(canvas.pixels[(dy + 4) * ANEM_CANVAS_W + dx + 27], &r, &g, &b);
    ANEM_CHECK_MSG(g > 40 && r < 8, "JPEG 右上应为绿色，实际 r=%d g=%d b=%d", r, g, b);

    // 2) BLOCK：逐像素对照（含 alpha 混合与透明像素）
    anem_canvas_fill(&canvas, rgb565(0, 0, 0));
    ANEM_CHECK(anem_image_draw_sprite(&ctx, &canvas, &pack, 1, 10, 20));
    // (0,0) 红、 (1,0) 绿、 (2,0) 半透明蓝、 (3,0) 透明（保留黑底）
    decode_rgb(canvas.pixels[20 * ANEM_CANVAS_W + 10], &r, &g, &b);
    ANEM_CHECK(r > 24 && g < 8);
    decode_rgb(canvas.pixels[20 * ANEM_CANVAS_W + 11], &r, &g, &b);
    ANEM_CHECK(g > 40 && r < 8);
    decode_rgb(canvas.pixels[20 * ANEM_CANVAS_W + 12], &r, &g, &b);
    ANEM_CHECK_MSG(b > 8 && r == 0, "半透明蓝应留下蓝色分量，实际 r=%d g=%d b=%d", r, g, b);
    ANEM_CHECK(canvas.pixels[20 * ANEM_CANVAS_W + 13] == 0);

    // 3) 纯色
    ANEM_CHECK(anem_image_draw_scene(&ctx, &canvas, &pack, 4));
    ANEM_CHECK(canvas.pixels[5] == rgb565(0x12, 0x34, 0x00) ||
               (canvas.pixels[5] & 0xF800) == 0x1000);

    // 4) 坏图：截断 JPEG 必须失败且不报成功
    ANEM_CHECK(!anem_image_draw_scene(&ctx, &canvas, &pack, 2));
    // 5) 坏 BLOCK：块长越界
    ANEM_CHECK(!anem_image_draw_sprite(&ctx, &canvas, &pack, 3, 0, 0));
    // 6) 越界/无效条目
    ANEM_CHECK(!anem_image_draw_scene(&ctx, &canvas, &pack, 0xFFFF));
    ANEM_CHECK(!anem_image_check(&pack, 0xFFFF));

    free(blob);
    printf("test_anemoi_image: PASS\n");
    return 0;
}
