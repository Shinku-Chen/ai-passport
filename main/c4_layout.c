// main/c4_layout.c —— 棋盘几何实现(无 ESP-IDF / LVGL 依赖)。
#include "c4_layout.h"

#include "c4_model.h"

#include <string.h>

static c4_rect_t rect_make(int x, int y, int w, int h)
{
    c4_rect_t r = { x, y, w, h };
    return r;
}

// 圆盘直径,同时也是棋盘上孔的直径:相邻两圆之间只留 C4_LAYOUT_DISC_GAP,
// 所以圆圈在棋盘上密排。孔和棋子共用这一个尺寸 —— 棋子落下正好填满孔。
int c4_layout_disc_diameter(const c4_layout_t *layout)
{
    if (!layout || layout->cell <= 0) return 0;

    int d = layout->cell - C4_LAYOUT_DISC_GAP;
    if (d < 1) d = 1;                    // 极小格子至少画 1px
    if (d > layout->cell) d = layout->cell;
    return d;
}

void c4_layout_compute(int screen_w, int screen_h, c4_layout_t *out)
{
    if (!out) return;

    memset(out, 0, sizeof(*out));

    const int avail_w = screen_w - 2 * C4_LAYOUT_SIDE_MARGIN;
    const int avail_h = screen_h - C4_LAYOUT_HEADER_H - C4_LAYOUT_FOOTER_H;

    int cell = (avail_w > 0) ? avail_w / C4_COLS : 0;
    const int cell_h = (avail_h > 0) ? avail_h / C4_ROWS : 0;
    if (cell <= 0 || (cell_h > 0 && cell_h < cell)) cell = cell_h;
    if (cell <= 0) cell = 1;
    out->cell = cell;

    out->board.w = cell * C4_COLS;
    out->board.h = cell * C4_ROWS;
    out->board.x = (screen_w - out->board.w) / 2;
    out->board.y = C4_LAYOUT_HEADER_H +
                   ((avail_h > 0) ? (avail_h - out->board.h) / 2 : 0);
}

c4_rect_t c4_layout_cell(const c4_layout_t *layout, int col, int row)
{
    if (!layout) return rect_make(0, 0, 0, 0);
    return rect_make(layout->board.x + col * layout->cell,
                     layout->board.y + (C4_ROWS - 1 - row) * layout->cell,
                     layout->cell, layout->cell);
}

c4_rect_t c4_layout_disc(const c4_layout_t *layout, int col, int row)
{
    const c4_rect_t cell = c4_layout_cell(layout, col, row);
    const int d = c4_layout_disc_diameter(layout);
    return rect_make(cell.x + (cell.w - d) / 2, cell.y + (cell.h - d) / 2, d, d);
}

int c4_layout_col_center_x(const c4_layout_t *layout, int col)
{
    if (!layout) return 0;
    return layout->board.x + col * layout->cell + layout->cell / 2;
}

int c4_layout_row_center_y(const c4_layout_t *layout, int row)
{
    if (!layout) return 0;
    return layout->board.y + (C4_ROWS - 1 - row) * layout->cell + layout->cell / 2;
}
