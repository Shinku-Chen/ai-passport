// main/c4_model.c —— 四子棋纯规则实现(无 ESP-IDF / LVGL 依赖)。
#include "c4_model.h"

#include <string.h>

static bool in_bounds(int col, int row)
{
    return col >= 0 && col < C4_COLS && row >= 0 && row < C4_ROWS;
}

// 本局的四条检测方向。只取“右上/上/右下/右”的轴,反方向靠 neg 侧一起数。
static const int C4_DIRS[4][2] = {
    {1, 0}, {0, 1}, {1, 1}, {1, -1},
};

void c4_reset(c4_game_t *game, uint8_t first_player)
{
    if (!game) return;

    memset(game, 0, sizeof(*game));
    game->turn = (first_player == C4_P2) ? C4_P2 : C4_P1;
}

int c4_landing_row(const c4_game_t *game, int col)
{
    if (!game || game->status != C4_ONGOING) return -1;
    if (col < 0 || col >= C4_COLS) return -1;
    if (game->heights[col] >= C4_ROWS) return -1;
    return (int)game->heights[col];
}

bool c4_can_play(const c4_game_t *game, int col)
{
    return c4_landing_row(game, col) >= 0;
}

int c4_legal_cols(const c4_game_t *game, int *cols)
{
    int n = 0;
    if (!game) return 0;
    for (int col = 0; col < C4_COLS; col++) {
        if (c4_can_play(game, col)) {
            if (cols) cols[n] = col;
            n++;
        }
    }
    return n;
}

int c4_seek_column(const c4_game_t *game, int from, int step)
{
    if (!game || (step != 1 && step != -1)) return -1;
    if (c4_legal_cols(game, NULL) == 0) return -1;

    int col = from;
    for (int i = 0; i < C4_COLS; i++) {
        col += step;
        if (col < 0) col = C4_COLS - 1;
        if (col >= C4_COLS) col = 0;
        if (c4_can_play(game, col)) return col;
    }
    return -1;
}

// 从 (col,row) 沿 (dc,dr) 数同色连续棋子,分别给出两个方向的延伸长度。
static int line_run(const c4_game_t *game, int col, int row, int dc, int dr,
                    uint8_t player, int *neg_len, int *pos_len)
{
    int neg = 0;
    while (neg < C4_WIN_LEN) {
        int c = col - dc * (neg + 1);
        int r = row - dr * (neg + 1);
        if (!in_bounds(c, r) || game->cells[c][r] != player) break;
        neg++;
    }

    int pos = 0;
    while (pos < C4_WIN_LEN) {
        int c = col + dc * (pos + 1);
        int r = row + dr * (pos + 1);
        if (!in_bounds(c, r) || game->cells[c][r] != player) break;
        pos++;
    }

    *neg_len = neg;
    *pos_len = pos;
    return neg + 1 + pos;
}

// 把包含落点的 C4_WIN_LEN 个连续棋子写进 win_cols / win_rows。
static void record_win(c4_game_t *game, int col, int row, int dc, int dr,
                       int neg, int pos)
{
    int take_neg = (neg < C4_WIN_LEN - 1) ? neg : (C4_WIN_LEN - 1);
    int take_pos = C4_WIN_LEN - 1 - take_neg;
    if (take_pos > pos) {
        take_neg += take_pos - pos;
        take_pos = pos;
        if (take_neg > neg) take_neg = neg;
    }

    int n = 0;
    for (int i = take_neg; i >= 1; i--) {
        game->win_cols[n] = (uint8_t)(col - dc * i);
        game->win_rows[n] = (uint8_t)(row - dr * i);
        n++;
    }
    game->win_cols[n] = (uint8_t)col;
    game->win_rows[n] = (uint8_t)row;
    n++;
    for (int i = 1; i <= take_pos; i++) {
        game->win_cols[n] = (uint8_t)(col + dc * i);
        game->win_rows[n] = (uint8_t)(row + dr * i);
        n++;
    }
    game->win_len = (uint8_t)n;
}

c4_move_t c4_play(c4_game_t *game, int col)
{
    c4_move_t move;
    memset(&move, 0, sizeof(move));
    move.row = -1;

    if (!game) return move;

    move.col = (col < 0 || col > 255) ? 0 : (uint8_t)col;
    move.player = game->turn;
    move.status = game->status;
    move.winner = game->winner;

    int row = c4_landing_row(game, col);
    if (row < 0) return move;

    const uint8_t player = game->turn;
    game->cells[col][row] = player;
    game->heights[col]++;
    game->moves++;

    move.ok = true;
    move.row = (int8_t)row;

    for (size_t d = 0; d < sizeof(C4_DIRS) / sizeof(C4_DIRS[0]); d++) {
        int neg = 0, pos = 0;
        int run = line_run(game, col, row, C4_DIRS[d][0], C4_DIRS[d][1], player,
                           &neg, &pos);
        if (run >= C4_WIN_LEN) {
            record_win(game, col, row, C4_DIRS[d][0], C4_DIRS[d][1], neg, pos);
            game->status = C4_WIN;
            game->winner = player;
            break;
        }
    }

    if (game->status == C4_ONGOING && game->moves >= C4_COLS * C4_ROWS) {
        game->status = C4_DRAW;
    }
    if (game->status == C4_ONGOING) {
        game->turn = (player == C4_P1) ? C4_P2 : C4_P1;
    }

    move.status = game->status;
    move.winner = game->winner;
    return move;
}

uint8_t c4_cell_get(const c4_game_t *game, int col, int row)
{
    if (!game || !in_bounds(col, row)) return C4_EMPTY;
    return game->cells[col][row];
}

bool c4_is_winning_cell(const c4_game_t *game, int col, int row)
{
    if (!game || game->status != C4_WIN) return false;
    for (int i = 0; i < game->win_len; i++) {
        if (game->win_cols[i] == col && game->win_rows[i] == row) return true;
    }
    return false;
}
