// tests/test_c4_ai.c —— 四子棋电脑对手的宿主测试(只验可判定行为,不验棋力)。
#include "c4_ai.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void play_moves(c4_game_t *game, const int *cols, int count)
{
    for (int i = 0; i < count; i++) {
        const c4_move_t move = c4_play(game, cols[i]);
        assert(move.ok);
        assert(move.status == C4_ONGOING);
    }
}

static c4_ai_opts_t fast_opts(uint8_t depth, uint32_t nodes)
{
    c4_ai_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.max_depth = depth;
    opts.max_nodes = nodes;
    return opts;
}

static void assert_choose_is_legal(const c4_game_t *game, int col)
{
    assert(col >= 0 && col < C4_COLS);
    assert(c4_can_play(game, col));
}

static void test_takes_immediate_win(void)
{
    // 轮到的这一方在行 0 已连三子:必须立刻补第四子,而不是去堵对手。
    c4_game_t g;
    c4_reset(&g, C4_P1);
    const int setup[] = { 0, 5, 1, 5, 2, 6 };
    play_moves(&g, setup, 6);

    assert(g.turn == C4_P1);
    c4_ai_opts_t opts = fast_opts(4, 0);
    const int col = c4_ai_choose(&g, &opts);
    assert(col == 3);
    assert(c4_play(&g, col).status == C4_WIN);
}

static void test_blocks_immediate_loss(void)
{
    // 轮到电脑,对手(先手方)在行 0 已连三子:唯一的活路是堵在 3 列。
    c4_game_t g;
    c4_reset(&g, C4_P1);
    const int setup[] = { 0, 4, 1, 5, 2 };
    play_moves(&g, setup, 5);

    assert(g.turn == C4_P2);
    c4_ai_opts_t opts = fast_opts(4, 0);
    const int col = c4_ai_choose(&g, &opts);
    assert(col == 3);

    // 堵住之后对手无法立刻取胜。
    const c4_move_t block = c4_play(&g, col);
    assert(block.ok && block.status == C4_ONGOING);
    for (int probe = 0; probe < C4_COLS; probe++) {
        if (!c4_can_play(&g, probe)) continue;
        c4_game_t after = g;
        assert(c4_play(&after, probe).status != C4_WIN);
    }
}

static void test_prefers_own_win_over_blocking(void)
{
    // 自己有一步杀(0 列竖四)而对手也有一步杀(3 列横四):先赢,别去堵。
    c4_game_t g;
    c4_reset(&g, C4_P1);
    const int setup[] = { 0, 4, 0, 5, 0, 6 };
    play_moves(&g, setup, 6);

    assert(g.turn == C4_P1);
    assert(c4_cell_get(&g, 4, 0) == C4_P2);
    assert(c4_cell_get(&g, 5, 0) == C4_P2);
    assert(c4_cell_get(&g, 6, 0) == C4_P2);

    c4_ai_opts_t opts = fast_opts(4, 0);
    const int col = c4_ai_choose(&g, &opts);
    assert(col == 0);
    assert(c4_play(&g, col).status == C4_WIN);
}

static void test_no_legal_move_returns_minus_one(void)
{
    c4_game_t g;

    // 已分出胜负的对局不再落子。
    c4_reset(&g, C4_P1);
    play_moves(&g, (const int[]){ 0, 6, 1, 6, 2, 6 }, 6);
    assert(c4_play(&g, 3).status == C4_WIN);
    assert(c4_ai_choose(&g, NULL) == -1);

    // 满盘(直接摆状态,不必造一局无胜负的完整对局)。
    memset(&g, 0, sizeof(g));
    for (int col = 0; col < C4_COLS; col++) {
        g.heights[col] = C4_ROWS;
        for (int row = 0; row < C4_ROWS; row++) {
            g.cells[col][row] = ((col + row) % 2 == 0) ? C4_P1 : C4_P2;
        }
    }
    g.moves = C4_COLS * C4_ROWS;
    g.turn = C4_P1;
    g.status = C4_DRAW;
    assert(c4_legal_cols(&g, NULL) == 0);
    assert(c4_ai_choose(&g, NULL) == -1);

    // 只剩一列可走:直接给出那一列,不必搜。
    memset(&g, 0, sizeof(g));
    for (int col = 0; col < C4_COLS; col++) {
        if (col == 3) continue;
        g.heights[col] = C4_ROWS;
        for (int row = 0; row < C4_ROWS; row++) {
            g.cells[col][row] = ((col + row) % 2 == 0) ? C4_P1 : C4_P2;
        }
    }
    g.moves = (C4_COLS - 1) * C4_ROWS;
    g.turn = C4_P1;
    g.status = C4_ONGOING;
    assert(c4_legal_cols(&g, NULL) == 1);
    assert(c4_ai_choose(&g, NULL) == 3);

    // 空指针不崩。
    assert(c4_ai_choose(NULL, NULL) == -1);
}

static void test_node_budget_and_depth_are_safe(void)
{
    c4_game_t g;
    c4_reset(&g, C4_P1);
    play_moves(&g, (const int[]){ 3, 3, 2, 4 }, 4);

    // 预算小到“第一层都跑不完”也必须给出合法列,并如实回报节点数。
    c4_ai_opts_t tiny = fast_opts(8, 1);
    const int col_a = c4_ai_choose(&g, &tiny);
    assert_choose_is_legal(&g, col_a);
    assert(tiny.nodes >= 1);

    // 深度参数会被夹到 2..8。
    c4_ai_opts_t depth_one = fast_opts(1, 20000);
    assert_choose_is_legal(&g, c4_ai_choose(&g, &depth_one));
    c4_ai_opts_t depth_huge = fast_opts(200, 20000);
    assert_choose_is_legal(&g, c4_ai_choose(&g, &depth_huge));
}

static void test_deterministic_for_same_seed(void)
{
    c4_game_t g;
    c4_reset(&g, C4_P1);
    play_moves(&g, (const int[]){ 3, 2, 4 }, 3);

    c4_ai_opts_t opts = fast_opts(6, 60000);
    opts.seed = 12345;
    const int first = c4_ai_choose(&g, &opts);

    opts.seed = 12345;
    const int second = c4_ai_choose(&g, &opts);
    assert(first == second);
    assert(opts.nodes > 0);

    // seed = 0 时按局面取种子,同一局面同样稳定。
    c4_ai_opts_t auto_opts = fast_opts(6, 60000);
    assert(c4_ai_choose(&g, &auto_opts) == c4_ai_choose(&g, &auto_opts));
}

static void test_blunder_percent_lets_it_slip(void)
{
    // 失误率 100%:每一步都随机,连一步取胜都不看(低难度靠这个放水)。
    c4_game_t g;
    c4_reset(&g, C4_P1);
    play_moves(&g, (const int[]){ 0, 5, 1, 5, 2, 6 }, 6);

    c4_ai_opts_t always = fast_opts(4, 0);
    always.blunder_percent = 100;
    bool saw_non_winning = false;
    for (uint32_t seed = 1; seed <= 40; seed++) {
        always.seed = seed;
        const int col = c4_ai_choose(&g, &always);
        assert_choose_is_legal(&g, col);
        if (col != 3) saw_non_winning = true;   // 3 列是唯一的一步取胜
    }
    assert(saw_non_winning);

    // 失误率 0:同一局面每次都取那一步杀。
    c4_ai_opts_t never = fast_opts(4, 0);
    never.blunder_percent = 0;
    for (uint32_t seed = 1; seed <= 40; seed++) {
        never.seed = seed;
        assert(c4_ai_choose(&g, &never) == 3);
    }
}

static void test_self_play_terminates(void)
{
    c4_game_t g;
    c4_reset(&g, C4_P1);

    c4_ai_opts_t opts = fast_opts(4, 20000);
    int played = 0;
    while (g.status == C4_ONGOING) {
        const int col = c4_ai_choose(&g, &opts);
        assert_choose_is_legal(&g, col);
        const c4_move_t move = c4_play(&g, col);
        assert(move.ok);
        played++;
        assert(played <= C4_COLS * C4_ROWS);
    }

    assert(g.status == C4_WIN || g.status == C4_DRAW);
    if (g.status == C4_WIN) assert(g.winner == C4_P1 || g.winner == C4_P2);
}

int main(void)
{
    test_takes_immediate_win();
    test_blocks_immediate_loss();
    test_prefers_own_win_over_blocking();
    test_no_legal_move_returns_minus_one();
    test_node_budget_and_depth_are_safe();
    test_blunder_percent_lets_it_slip();
    test_deterministic_for_same_seed();
    test_self_play_terminates();

    printf("test_c4_ai: PASS\n");
    return 0;
}
