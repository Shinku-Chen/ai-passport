// main/c4_model.h —— 四子棋(AI Passport 版)的纯规则模型。
//
// 只依赖标准 C:不碰 ESP-IDF / LVGL,所以 tests/ 下的宿主测试可以直接编译它。
// 棋盘存储为 cells[列][行],行 0 是棋盘底部;heights[列] 同时是“下一个空行”的下标。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define C4_COLS 10
#define C4_ROWS 7
#define C4_WIN_LEN 4

typedef enum {
    C4_EMPTY = 0,
    C4_P1 = 1,   // 先手,界面上是玩家(琥珀色)
    C4_P2 = 2,   // 后手,界面上是电脑(珊瑚红)
} c4_cell_t;

typedef enum {
    C4_ONGOING = 0,
    C4_WIN,
    C4_DRAW,
} c4_status_t;

typedef struct {
    uint8_t cells[C4_COLS][C4_ROWS];
    uint8_t heights[C4_COLS];          // 每列已落子数
    uint8_t turn;                      // 轮到谁:c4_cell_t
    uint8_t status;                    // c4_status_t
    uint8_t winner;                    // 胜者;未分胜负为 C4_EMPTY
    uint8_t moves;                     // 已落子总数
    uint8_t win_cols[C4_WIN_LEN];      // 构成四连的坐标(仅 status == C4_WIN 有效)
    uint8_t win_rows[C4_WIN_LEN];
    uint8_t win_len;
} c4_game_t;

typedef struct {
    bool ok;          // false = 该列不可落子
    uint8_t col;
    int8_t row;       // 落点;ok 为 false 时是 -1
    uint8_t player;   // 本手落子方
    uint8_t status;   // 落子后的 c4_status_t
    uint8_t winner;
} c4_move_t;

// 重置为开局;first_player 为 C4_P1 或 C4_P2(非法值按 C4_P1 处理)。
void c4_reset(c4_game_t *game, uint8_t first_player);

// 该列还能落子吗(列下标非法、列满、或本局已结束都返回 false)。
bool c4_can_play(const c4_game_t *game, int col);

// 该列下一枚棋子的落点行;列不可落子返回 -1。
int c4_landing_row(const c4_game_t *game, int col);

// 收集全部可落子的列(按列下标升序),返回个数。cols 至少能放 C4_COLS 个。
int c4_legal_cols(const c4_game_t *game, int *cols);

// 从 from 出发按 step(+1/-1)环形找下一个可落子的列,用于光标移动;没有返回 -1。
int c4_seek_column(const c4_game_t *game, int from, int step);

// 在 col 落子并结算胜负。game 已结束或列非法时不改变任何状态。
c4_move_t c4_play(c4_game_t *game, int col);

// 读格子;越界返回 C4_EMPTY。
uint8_t c4_cell_get(const c4_game_t *game, int col, int row);

// (col,row) 是否属于本局取胜的四连(界面高亮用)。
bool c4_is_winning_cell(const c4_game_t *game, int col, int row);
