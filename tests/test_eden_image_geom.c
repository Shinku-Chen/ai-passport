/*
 * tests/test_eden_image_geom.c —— 补丁贴图的几何边界(P2-5)。
 *
 * 原实现的 blit_patch 用 (CANVAS_W - x) 直接当列数,x >= CANVAS_W 会无符号回绕成
 * 一个巨大的长度交给 memcpy。这里的用例就是那组越界与"恰好压边"的坐标。
 *
 * 构建:cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_eden_image_geom.c
 */
#include "eden_image_geom.h"

#include <stdio.h>

static int failures;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            failures++;                                                     \
            fprintf(stderr, "FAIL %s:%d ", __FILE__, __LINE__);             \
            fprintf(stderr, __VA_ARGS__);                                   \
            fputc('\n', stderr);                                            \
        }                                                                   \
    } while (0)

#define CANVAS_W 264u
#define CANVAS_H 168u

static void expect(bool ok, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                   uint16_t cols, uint16_t rows)
{
    eden_blit_span_t span = { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF };
    const bool result = eden_blit_span(x, y, w, h, CANVAS_W, CANVAS_H, &span);
    CHECK(result == ok, "(%u,%u) %ux%u 返回 %d,期望 %d", (unsigned)x, (unsigned)y,
          (unsigned)w, (unsigned)h, (int)result, (int)ok);
    if (ok) {
        CHECK(span.x == x && span.y == y, "原点被改动");
        CHECK(span.cols == cols && span.rows == rows,
              "(%u,%u) %ux%u 裁剪成 %ux%u,期望 %ux%u", (unsigned)x, (unsigned)y,
              (unsigned)w, (unsigned)h, (unsigned)span.cols, (unsigned)span.rows,
              (unsigned)cols, (unsigned)rows);
    }
}

int main(void)
{
    // 完全在内
    expect(true, 0, 0, 10, 10, 10, 10);
    expect(true, 100, 100, 104, 68, 104, 68);

    // 右/下边界裁剪
    expect(true, 263, 0, 10, 10, 1, 10);
    expect(true, 0, 167, 10, 10, 10, 1);
    expect(true, 200, 160, 100, 20, 64, 8);

    // 完全在外 —— 这几条正是原来的下溢点
    expect(false, CANVAS_W, 0, 10, 10, 0, 0);
    expect(false, CANVAS_W + 1, 0, 10, 10, 0, 0);
    expect(false, 0, CANVAS_H, 10, 10, 0, 0);
    expect(false, 65535, 0, 10, 10, 0, 0);
    expect(false, 0, 65535, 10, 10, 0, 0);

    // 尺寸为 0
    expect(false, 0, 0, 0, 10, 0, 0);
    expect(false, 0, 0, 10, 0, 0, 0);

    // 加法回绕:x+w 超过 65535 时不能算出"合法"的小值
    expect(false, 65535, 0, 65535, 10, 0, 0);
    expect(true, 65530 % CANVAS_W, 0, 65535, 10, CANVAS_W - (65530 % CANVAS_W), 10);

    // 空缓冲
    eden_blit_span_t span;
    CHECK(!eden_blit_span(0, 0, 10, 10, 0, CANVAS_H, &span), "canvas_w=0 应拒绝");
    CHECK(!eden_blit_span(0, 0, 10, 10, CANVAS_W, 0, &span), "canvas_h=0 应拒绝");
    CHECK(!eden_blit_span(0, 0, 10, 10, CANVAS_W, CANVAS_H, NULL), "span=NULL 应拒绝");

    if (failures != 0) {
        fprintf(stderr, "%d 项检查失败\n", failures);
        return 1;
    }
    printf("eden image geometry: OK\n");
    return 0;
}
