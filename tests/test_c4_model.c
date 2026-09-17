// tests/test_c4_model.c —— 四子棋规则模型的宿主测试。
#include "c4_model.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

// 生成自宿主搜索的一局“无胜负填满棋盘”落子序列(逐列填满,全程无人四连)。
static const int DRAW_SEQUENCE[C4_COLS * C4_ROWS] = {
    5, 5, 5, 5, 5, 5, 5, 6, 6, 6,
    6, 6, 6, 6, 4, 4, 4, 4, 4, 4,
    4, 7, 7, 7, 7, 7, 7, 7, 3, 3,
    3, 3, 3, 3, 3, 8, 8, 8, 8, 8,
    8, 8, 9, 2, 2, 2, 2, 2, 2, 2,
    1, 9, 9, 9, 9, 9, 9, 1, 1, 1,
    1, 1, 0, 0, 0, 0, 0, 0, 0, 1,
};static void play_all(c4_game_t *game, const int *cols, int count, c4_status_t expect_final)
{
    assert(count > 0);

    for (int i = 0; i < count - 1; i++) {
        const c4_move_t move = c4_play(game, cols[i]);
        assert(move.ok);
        assert(move.col == (uint8_t)cols[i]);
        assert(move.row >= 0 && move.row < C4_ROWS);
        assert(move.status == C4_ONGOING);   // 中途不该提前结束
    }

    const c4_move_t last = c4_play(game, cols[count - 1]);
    assert(last.ok);
    assert(last.col == (uint8_t)cols[count - 1]);
    assert(last.status == expect_final);
    assert(game->status == expect_final);
}

static void assert_no_winner(const c4_game_t *game)
{
    assert(game->status == C4_ONGOING);
    assert(game->winner == C4_EMPTY);
    assert(game->win_len == 0);
}

static void test_reset_and_gravity(void)
{
    c4_game_t g;
    c4_reset(&g, C4_P1);

    assert(g.turn == C4_P1);
    assert(g.status == C4_ONGOING);
    assert(g.moves == 0);
    for (int col = 0; col < C4_COLS; col++) {
        assert(g.heights[col] == 0);
        assert(c4_landing_row(&g, col) == 0);
        assert(c4_can_play(&g, col));
        for (int row = 0; row < C4_ROWS; row++) {
            assert(c4_cell_get(&g, col, row) == C4_EMPTY);
        }
    }

    // 落子按重力从行 0 往上堆。
    for (int i = 0; i < C4_ROWS; i++) {
        const c4_move_t move = c4_play(&g, 2);
        assert(move.ok);
        assert(move.row == i);
        assert(g.heights[2] == (uint8_t)(i + 1));
    }
    // 一列填满后轮到谁由列高奇偶决定(现在是 C4_ROWS 手之后)。
    assert(g.turn == ((C4_ROWS % 2 == 0) ? C4_P1 : C4_P2));

    // 列满 / 越界 / 非本列:全部拒绝,且不改变状态。
    const uint8_t moves_before = g.moves;
    assert(!c4_can_play(&g, 2));
    assert(!c4_play(&g, 2).ok);
    assert(!c4_play(&g, -1).ok);
    assert(!c4_play(&g, C4_COLS).ok);
    assert(c4_play(&g, 2).row == -1);
    assert(g.moves == moves_before);
}

static void test_horizontal_win(void)
{
    c4_game_t g;
    c4_reset(&g, C4_P1);

    const int moves[] = { 0, 6, 1, 6, 2, 6, 3 };   // P1 在行 0 连成四子
    play_all(&g, moves, 7, C4_WIN);

    assert(g.winner == C4_P1);
    assert(g.win_len == C4_WIN_LEN);
    for (int col = 0; col < C4_WIN_LEN; col++) {
        assert(c4_is_winning_cell(&g, col, 0));
    }
    assert(!c4_is_winning_cell(&g, 4, 0));
    assert(!c4_is_winning_cell(&g, 0, 1));

    // 终局后不再接受落子,轮次也不推进。
    const uint8_t turn = g.turn;
    assert(!c4_can_play(&g, 4));
    assert(!c4_play(&g, 4).ok);
    assert(g.turn == turn);
    assert(c4_seek_column(&g, 0, 1) == -1);
    assert(c4_legal_cols(&g, NULL) == 0);
}

static void test_vertical_win(void)
{
    c4_game_t g;
    c4_reset(&g, C4_P1);

    const int moves[] = { 0, 1, 0, 1, 0, 1, 0 };
    play_all(&g, moves, 7, C4_WIN);

    assert(g.winner == C4_P1);
    assert(g.win_len == C4_WIN_LEN);
    for (int row = 0; row < C4_WIN_LEN; row++) {
        assert(c4_is_winning_cell(&g, 0, row));
    }
}

static void test_diagonal_wins(void)
{
    // "\" 方向:P1 拿到 (0,0) (1,1) (2,2) (3,3)
    c4_game_t g;
    c4_reset(&g, C4_P1);
    const int down[] = { 0, 1, 1, 2, 6, 2, 2, 3, 6, 3, 6, 3, 3 };
    play_all(&g, down, 13, C4_WIN);
    assert(g.winner == C4_P1);
    assert(g.win_len == C4_WIN_LEN);
    for (int i = 0; i < C4_WIN_LEN; i++) {
        assert(c4_is_winning_cell(&g, i, i));
    }

    // "/" 方向:P1 拿到 (6,0) (5,1) (4,2) (3,3)
    c4_reset(&g, C4_P1);
    const int up[] = { 6, 5, 5, 4, 0, 4, 4, 3, 0, 3, 0, 3, 3 };
    play_all(&g, up, 13, C4_WIN);
    assert(g.winner == C4_P1);
    assert(g.win_len == C4_WIN_LEN);
    for (int i = 0; i < C4_WIN_LEN; i++) {
        assert(c4_is_winning_cell(&g, 6 - i, i));
    }
}

static void test_gap_is_not_a_win(void)
{
    c4_game_t g;
    c4_reset(&g, C4_P1);

    // P1: (0,0) (1,0) (2,0) 与 (4,0),中间 (3,0) 是对手的 —— 不算四连。
    const int moves[] = { 0, 3, 1, 5, 2, 5, 4 };
    play_all(&g, moves, 7, C4_ONGOING);
    assert_no_winner(&g);
}

static void test_five_in_a_row_records_four(void)
{
    c4_game_t g;
    c4_reset(&g, C4_P1);

    // 落中间那枚时同时补出两条三连 -> 五连。高亮只记四子,且必须连续。
    const int moves[] = { 0, 6, 1, 6, 3, 6, 4, 5, 2 };
    play_all(&g, moves, 9, C4_WIN);

    assert(g.winner == C4_P1);
    assert(g.win_len == C4_WIN_LEN);

    int cols[C4_WIN_LEN];
    int rows[C4_WIN_LEN];
    for (int i = 0; i < C4_WIN_LEN; i++) {
        cols[i] = g.win_cols[i];
        rows[i] = g.win_rows[i];
        assert(c4_cell_get(&g, cols[i], rows[i]) == C4_P1);
    }

    // 同一行、列号互不相同、并且逐个 +1(连续)。
    for (int i = 0; i < C4_WIN_LEN; i++) {
        assert(rows[i] == 0);
        for (int j = i + 1; j < C4_WIN_LEN; j++) {
            assert(cols[i] != cols[j]);
        }
    }
    for (int i = 1; i < C4_WIN_LEN; i++) {
        assert(cols[i] == cols[i - 1] + 1);
    }
}

static void test_seek_column_wraps_and_skips_full(void)
{
    c4_game_t g;
    c4_reset(&g, C4_P1);

    assert(c4_seek_column(&g, 0, 1) == 1);
    assert(c4_seek_column(&g, 0, -1) == C4_COLS - 1);
    assert(c4_seek_column(&g, C4_COLS - 1, 1) == 0);

    // 填满最后两列后,从它们左边往右应该绕回第 0 列。
    // 交替落列,顺便避免某一方先连成四子把局面提前结束掉。
    const int last = C4_COLS - 1;
    // a,b,b,a 的锯齿顺序保证每列内部颜色交替 —— 否则先手自己会竖着连成四子。
    for (int i = 0; i < C4_ROWS * 2; i++) {
        const int phase = i % 4;
        const int col = (phase == 0 || phase == 3) ? last - 1 : last;
        const c4_move_t move = c4_play(&g, col);
        assert(move.ok);
        assert(move.status == C4_ONGOING);
    }
    assert(g.heights[last - 1] == C4_ROWS && g.heights[last] == C4_ROWS);

    assert(c4_seek_column(&g, last - 2, 1) == 0);
    assert(c4_seek_column(&g, 0, -1) == last - 2);
    assert(c4_seek_column(&g, last - 2, -1) == last - 3);

    // 非法入参不崩。
    assert(c4_seek_column(&g, 4, 0) == -1);
    assert(c4_seek_column(NULL, 0, 1) == -1);
}

static void test_legal_cols(void)
{
    c4_game_t g;
    c4_reset(&g, C4_P1);

    int cols[C4_COLS];
    assert(c4_legal_cols(&g, cols) == C4_COLS);
    for (int i = 0; i < C4_COLS; i++) assert(cols[i] == i);

    for (int i = 0; i < C4_ROWS; i++) assert(c4_play(&g, 3).ok);
    assert(c4_legal_cols(&g, cols) == C4_COLS - 1);
    assert(cols[0] == 0 && cols[1] == 1 && cols[2] == 2 && cols[3] == 4);
}

static void test_draw_and_frozen_state(void)
{
    c4_game_t g;
    c4_reset(&g, C4_P1);

    for (int i = 0; i < C4_COLS * C4_ROWS - 1; i++) {
        const c4_move_t move = c4_play(&g, DRAW_SEQUENCE[i]);
        assert(move.ok);
        assert(move.status == C4_ONGOING);
    }
    assert(g.moves == C4_COLS * C4_ROWS - 1);
    assert_no_winner(&g);

    const c4_move_t last = c4_play(&g, DRAW_SEQUENCE[C4_COLS * C4_ROWS - 1]);
    assert(last.ok);
    assert(last.status == C4_DRAW);
    assert(g.status == C4_DRAW);
    assert(g.winner == C4_EMPTY);
    assert(g.win_len == 0);

    assert(c4_legal_cols(&g, NULL) == 0);
    assert(!c4_play(&g, 0).ok);
}

static void test_null_and_bounds(void)
{
    assert(c4_cell_get(NULL, 0, 0) == C4_EMPTY);
    assert(c4_landing_row(NULL, 0) == -1);
    assert(!c4_can_play(NULL, 0));
    assert(!c4_is_winning_cell(NULL, 0, 0));
    assert(!c4_play(NULL, 0).ok);
    c4_reset(NULL, C4_P1);                 // 不应崩溃

    c4_game_t g;
    c4_reset(&g, C4_P2);
    assert(g.turn == C4_P2);
    c4_reset(&g, 0);                       // 非法先手按 P1 处理
    assert(g.turn == C4_P1);

    assert(c4_cell_get(&g, -1, 0) == C4_EMPTY);
    assert(c4_cell_get(&g, C4_COLS, C4_ROWS) == C4_EMPTY);
}

int main(void)
{
    test_reset_and_gravity();
    test_horizontal_win();
    test_vertical_win();
    test_diagonal_wins();
    test_gap_is_not_a_win();
    test_five_in_a_row_records_four();
    test_seek_column_wraps_and_skips_full();
    test_legal_cols();
    test_draw_and_frozen_state();
    test_null_and_bounds();

    printf("test_c4_model: PASS\n");
    return 0;
}
