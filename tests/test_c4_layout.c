// tests/test_c4_layout.c —— 棋盘排版计算的宿主测试(横屏 320x240 是主目标)。
#include "c4_layout.h"

#include "bsp_display_rounding.h"
#include "c4_model.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

// BSP 会把屏幕四角裁成这个半径的圆角;棋盘外框的四角像素必须落在圆角矩形内,
// 否则会被涂黑。这里直接复用 BSP 的纯几何函数,避免两套圆角定义漂移。
#define SCREEN_CORNER_RADIUS 30

static void assert_rect_inside_rounded_screen(c4_rect_t r, int screen_w, int screen_h)
{
    assert(!bsp_display_pixel_outside_rounded_rect(r.x, r.y, screen_w, screen_h,
                                                   SCREEN_CORNER_RADIUS));
    assert(!bsp_display_pixel_outside_rounded_rect(r.x + r.w - 1, r.y, screen_w, screen_h,
                                                   SCREEN_CORNER_RADIUS));
    assert(!bsp_display_pixel_outside_rounded_rect(r.x, r.y + r.h - 1, screen_w, screen_h,
                                                   SCREEN_CORNER_RADIUS));
    assert(!bsp_display_pixel_outside_rounded_rect(r.x + r.w - 1, r.y + r.h - 1,
                                                   screen_w, screen_h,
                                                   SCREEN_CORNER_RADIUS));
}

static void test_landscape_320x240(void)
{
    c4_layout_t l;
    c4_layout_compute(320, 240, &l);

    // 横屏 320x240:24px 状态条 + 6px 底边,10x7 棋盘 -> 29px 格子 -> 290x203。
    assert(l.cell == 29);
    assert(l.board.w == l.cell * C4_COLS);
    assert(l.board.h == l.cell * C4_ROWS);

    // 高度方向已经被抳满(棋盘尽量大),宽度方向还留有余量。
    const int avail_h = 240 - C4_LAYOUT_HEADER_H - C4_LAYOUT_FOOTER_H;
    assert(l.board.h <= avail_h);
    assert(l.board.h >= avail_h - C4_ROWS);

    // 水平居中(棋盘宽度是奇数时,左右最多差 1px)。
    assert(l.board.x == (320 - l.board.w) / 2);
    const int left_gap = l.board.x;
    const int right_gap = 320 - l.board.x - l.board.w;
    assert(right_gap - left_gap >= 0 && right_gap - left_gap <= 1);

    // 竖直位置在状态条下方、底边之上。
    assert(l.board.y >= C4_LAYOUT_HEADER_H);
    assert(l.board.y + l.board.h <= 240 - C4_LAYOUT_FOOTER_H);

    // 不压到四角圆角:棋盘本身和向外 2px 的自绘外框都不能被涂黑。
    assert_rect_inside_rounded_screen(l.board, 320, 240);
    c4_rect_t frame = { l.board.x - 2, l.board.y - 2, l.board.w + 4, l.board.h + 4 };
    assert_rect_inside_rounded_screen(frame, 320, 240);

    // 棋盘自绘对象要向外描边 2px,外框也得留在屏幕内。
    assert(l.board.x - 2 >= 0 && l.board.y - 2 >= 0);
    assert(l.board.x + l.board.w + 2 <= 320);
    assert(l.board.y + l.board.h + 2 <= 240);
}

static void test_cells_tile_without_gaps(void)
{
    c4_layout_t l;
    c4_layout_compute(320, 240, &l);

    for (int col = 0; col < C4_COLS; col++) {
        for (int row = 0; row < C4_ROWS; row++) {
            const c4_rect_t cell = c4_layout_cell(&l, col, row);

            assert(cell.w == l.cell && cell.h == l.cell);
            assert(cell.x >= l.board.x && cell.x + cell.w <= l.board.x + l.board.w);
            assert(cell.y >= l.board.y && cell.y + cell.h <= l.board.y + l.board.h);

            // 行 0 在最下面:行号越大越靠上。
            if (row + 1 < C4_ROWS) {
                const c4_rect_t above = c4_layout_cell(&l, col, row + 1);
                assert(above.y + above.h == cell.y);
            }
            if (col + 1 < C4_COLS) {
                const c4_rect_t right = c4_layout_cell(&l, col + 1, row);
                assert(right.x == cell.x + cell.w);
            }

            // 圆心就是格子中心。
            assert(c4_layout_col_center_x(&l, col) == cell.x + cell.w / 2);
            assert(c4_layout_row_center_y(&l, row) == cell.y + cell.h / 2);
        }
    }
}

static void test_disc_fits_inside_hole_and_cell(void)
{
    c4_layout_t l;
    c4_layout_compute(320, 240, &l);

    const int disc = c4_layout_disc_diameter(&l);

    assert(disc > 0);
    assert(disc < l.cell);

    // 圆圈密排:相邻两圆之间只留 C4_LAYOUT_DISC_GAP。
    assert(disc == l.cell - C4_LAYOUT_DISC_GAP);
    assert(disc >= l.cell * 4 / 5);

    for (int col = 0; col < C4_COLS; col++) {
        for (int row = 0; row < C4_ROWS; row++) {
            const c4_rect_t cell = c4_layout_cell(&l, col, row);
            const c4_rect_t d = c4_layout_disc(&l, col, row);

            assert(d.w == disc && d.h == disc);
            assert(d.x >= cell.x && d.x + d.w <= cell.x + cell.w);
            assert(d.y >= cell.y && d.y + d.h <= cell.y + cell.h);
            // 圆盘在格子里居中:奇数空隙时左右(上下)最多差 1px。
            const int left = d.x - cell.x;
            const int right = cell.x + cell.w - (d.x + d.w);
            const int top = d.y - cell.y;
            const int bottom = cell.y + cell.h - (d.y + d.h);
            assert(right - left >= 0 && right - left <= 1);
            assert(bottom - top >= 0 && bottom - top <= 1);
        }
    }
}

static void test_degenerate_sizes_stay_positive(void)
{
    const int sizes[][2] = { { 320, 240 }, { 240, 320 }, { 128, 128 }, { 40, 40 }, { 1, 1 } };

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        c4_layout_t l;
        c4_layout_compute(sizes[i][0], sizes[i][1], &l);

        assert(l.cell >= 1);
        assert(l.board.w == l.cell * C4_COLS);
        assert(l.board.h == l.cell * C4_ROWS);

        const int disc = c4_layout_disc_diameter(&l);
        assert(disc >= 0 && disc <= l.cell);
        if (l.cell >= 5) {
            assert(disc > 0 && disc < l.cell);      // 格子够大时:0 < 圆盘(= 孔) < 格
        }
    }

    // 竖屏也能算,只是格子更小(本应用固定横屏,这里只保证不越界)。
    c4_layout_t portrait;
    c4_layout_compute(240, 320, &portrait);
    assert(portrait.board.x >= 0);
    assert(portrait.board.x + portrait.board.w <= 240);

    // NULL 不崩。
    c4_layout_compute(320, 240, NULL);
    assert(c4_layout_col_center_x(NULL, 0) == 0);
    assert(c4_layout_disc_diameter(NULL) == 0);
}

int main(void)
{
    test_landscape_320x240();
    test_cells_tile_without_gaps();
    test_disc_fits_inside_hole_and_cell();
    test_degenerate_sizes_stay_positive();

    printf("test_c4_layout: PASS\n");
    return 0;
}
