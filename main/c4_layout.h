// main/c4_layout.h —— 棋盘几何:纯计算,便于宿主测试覆盖排版边界。
#pragma once

// 顶部状态条高度。棋盘屏幕只有这一条文字带,下面全部留给棋盘。
#define C4_LAYOUT_HEADER_H 24
// 底部不排文字,只留一点空隙,免得棋盘外框贴到屏幕边缘。
#define C4_LAYOUT_FOOTER_H 6
// 相邻两个圆圈之间留出的可见空隙(棋盘上圆圈密排;每侧各半)。
#define C4_LAYOUT_DISC_GAP 3
// 棋盘左右各留出的最小边距。
#define C4_LAYOUT_SIDE_MARGIN 12

typedef struct {
    int x, y, w, h;   // 左上角 + 宽高,坐标是 LVGL 逻辑坐标(横屏 320x240)
} c4_rect_t;

typedef struct {
    int cell;         // 单格边长
    c4_rect_t board;  // 棋盘外框
} c4_layout_t;

// 按屏幕逻辑尺寸算出居中、不越界的 7x6 棋盘几何。
void c4_layout_compute(int screen_w, int screen_h, c4_layout_t *out);

// 第 col 列第 row 行的格子(行 0 在棋盘底部)。
c4_rect_t c4_layout_cell(const c4_layout_t *layout, int col, int row);

// 该格中间的圆盘外接矩形(比格子小一圈,直径 = w = h)。
// 棋盘上的孔按同一尺寸画 —— 棋子落下正好填满孔,两者不允许各改一个。
c4_rect_t c4_layout_disc(const c4_layout_t *layout, int col, int row);

int c4_layout_col_center_x(const c4_layout_t *layout, int col);
int c4_layout_row_center_y(const c4_layout_t *layout, int row);
// 圆盘直径,同时也是棋盘上孔的直径。
int c4_layout_disc_diameter(const c4_layout_t *layout);
