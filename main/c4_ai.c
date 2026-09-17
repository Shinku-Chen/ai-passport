// main/c4_ai.c —— 迭代加深 + alpha-beta 的四子棋电脑对手。
//
// 设计取舍:
//  * 用【节点预算】而不是墙钟时间限制搜索 —— 同一局面结果稳定,可在宿主测试里断言,
//    也省掉一层时钟依赖。默认预算下的耗时在 ESP32-C3 上约 0.5 秒量级。
//  * 窗口权重只做“限深处的启发”,胜负由搜索本身判定,所以权重不必调到极致。
#include "c4_ai.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define C4_AI_SCORE_INF 100000000
#define C4_AI_SCORE_WIN 1000000
#define C4_AI_DEFAULT_NODES 400000u
#define C4_AI_MIN_DEPTH 2
#define C4_AI_MAX_DEPTH 8

// 中心优先:既提高剪枝效率,又让同分时更倾向占中间列。
static const int C4_AI_ORDER[C4_COLS] = { 4, 5, 3, 6, 2, 7, 1, 8, 0, 9 };

// 窗口内只有同一方的 k 枚棋子时的分值(4 枚那个由搜索的胜负判决接管)。
static const int C4_AI_WEIGHT[C4_WIN_LEN + 1] = {0, 1, 8, 64, 0};

static const int C4_DIRS[4][2] = {
    {1, 0}, {0, 1}, {1, 1}, {1, -1},
};

typedef struct {
    c4_game_t game;
    uint32_t max_nodes;
    uint32_t nodes;
    c4_ai_budget_cb_t budget_cb;
    void *budget_user;
    bool exhausted;   // 预算/时间用尽:当前这一层的结果不完整
} c4_search_t;

static uint32_t xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x ? x : 0x9E3779B9u;
    return *state;
}

// 局面散列,用作默认随机种子:同一局面结果稳定,不同局面走法有变化。
static uint32_t hash_game(const c4_game_t *game)
{
    uint32_t h = 2166136261u;
    for (int col = 0; col < C4_COLS; col++) {
        h = (h ^ game->heights[col]) * 16777619u;
        for (int row = 0; row < C4_ROWS; row++) {
            h = (h ^ game->cells[col][row]) * 16777619u;
        }
    }
    h = (h ^ game->turn) * 16777619u;
    h = (h ^ game->moves) * 16777619u;
    return h ? h : 0x9E3779B9u;
}

// 窗口表:全部「四个格子」的组合。开局前算一次,叶评估就不必每轮重算方向与边界。
// 搜索只在 AI worker 里单线程运行,所以用文件级静态表是安全的。
typedef struct { uint8_t col[4]; uint8_t row[4]; } c4_window_t;

// 10x7 棋盘一共 145 个「四格窗口」(横 49 + 竖 40 + 两个对角各 28),留些余量。
#define C4_WINDOW_MAX 160
static c4_window_t s_windows[C4_WINDOW_MAX];
static int s_window_count;
static bool s_windows_ready;

static void windows_init(void)
{
    if (s_windows_ready) return;

    s_window_count = 0;
    for (int col = 0; col < C4_COLS; col++) {
        for (int row = 0; row < C4_ROWS; row++) {
            for (size_t d = 0; d < sizeof(C4_DIRS) / sizeof(C4_DIRS[0]); d++) {
                const int dc = C4_DIRS[d][0];
                const int dr = C4_DIRS[d][1];
                const int end_col = col + dc * (C4_WIN_LEN - 1);
                const int end_row = row + dr * (C4_WIN_LEN - 1);
                if (end_col < 0 || end_col >= C4_COLS) continue;
                if (end_row < 0 || end_row >= C4_ROWS) continue;
                if (s_window_count >= C4_WINDOW_MAX) break;

                c4_window_t *w = &s_windows[s_window_count++];
                for (int i = 0; i < C4_WIN_LEN; i++) {
                    w->col[i] = (uint8_t)(col + dc * i);
                    w->row[i] = (uint8_t)(row + dr * i);
                }
            }
        }
    }
    s_windows_ready = true;
}

// 从“当前待落子方”的视角给静态分值。
static int evaluate(const c4_game_t *game)
{
    const uint8_t me = game->turn;
    const uint8_t opp = (me == C4_P1) ? C4_P2 : C4_P1;
    int score = 0;

    for (int w = 0; w < s_window_count; w++) {
        const c4_window_t *win = &s_windows[w];
        int mine = 0, theirs = 0;

        for (int i = 0; i < C4_WIN_LEN; i++) {
            const uint8_t v = game->cells[win->col[i]][win->row[i]];
            if (v == me) mine++;
            else if (v == opp) theirs++;
        }
        if (mine != 0 && theirs != 0) continue;
        // 对方威胁给双倍权重:限深处宁可少给自己铺路,也别放着对手的三连不管。
        score += C4_AI_WEIGHT[mine] - 2 * C4_AI_WEIGHT[theirs];
    }
    return score;
}

static void undo_move(c4_game_t *game, int col, uint8_t player, int row)
{
    game->cells[col][row] = C4_EMPTY;
    game->heights[col]--;
    game->moves--;
    game->turn = player;
    game->status = C4_ONGOING;
    game->winner = C4_EMPTY;
    game->win_len = 0;
}

// 返回“当前待落子方”视角的分数。depth 为剩余深度,<=0 时返回静态评估。
static int negamax(c4_search_t *s, int depth, int alpha, int beta)
{
    c4_game_t *g = &s->game;

    s->nodes++;

    if (s->budget_cb != NULL && (s->nodes % C4_AI_BUDGET_INTERVAL) == 0 &&
        !s->budget_cb(s->budget_user)) {
        // 预算用完:这一层的结果会被根节点丢掉,返回什么都行。
        s->exhausted = true;
        return evaluate(g);
    }

    bool any_move = false;
    for (int col = 0; col < C4_COLS; col++) {
        if (g->heights[col] < C4_ROWS) { any_move = true; break; }
    }
    if (!any_move) return 0;                     // 棋盘已满:平局
    if (depth <= 0) return evaluate(g);

    int best = -C4_AI_SCORE_INF;
    for (size_t i = 0; i < sizeof(C4_AI_ORDER) / sizeof(C4_AI_ORDER[0]); i++) {
        const int col = C4_AI_ORDER[i];
        if (g->heights[col] >= C4_ROWS) continue;

        c4_move_t m = c4_play(g, col);
        int score;
        if (m.status == C4_WIN) {
            score = C4_AI_SCORE_WIN + depth;     // 越早取胜分越高
        } else if (m.status == C4_DRAW) {
            score = 0;
        } else {
            score = -negamax(s, depth - 1, -beta, -alpha);
        }
        undo_move(g, col, m.player, m.row);

        if (score > best) best = score;
        if (best > alpha) alpha = best;
        if (alpha >= beta) break;

        if (s->max_nodes != 0 && s->nodes >= s->max_nodes) {
            s->exhausted = true;
            break;
        }
    }
    return best;
}

// 搜索前的廉价判断:① 自己能一步赢 ② 对手下一步能赢必须先堵。按中心优先返回第一列。
static int immediate_move(const c4_game_t *game)
{
    for (size_t i = 0; i < sizeof(C4_AI_ORDER) / sizeof(C4_AI_ORDER[0]); i++) {
        const int col = C4_AI_ORDER[i];
        if (!c4_can_play(game, col)) continue;

        c4_game_t probe = *game;
        if (c4_play(&probe, col).status == C4_WIN) return col;
    }

    for (size_t i = 0; i < sizeof(C4_AI_ORDER) / sizeof(C4_AI_ORDER[0]); i++) {
        const int col = C4_AI_ORDER[i];
        if (!c4_can_play(game, col)) continue;

        c4_game_t probe = *game;
        const uint8_t me = probe.turn;
        probe.turn = (me == C4_P1) ? C4_P2 : C4_P1;   // 假装轮到对手
        c4_move_t reply = c4_play(&probe, col);
        if (reply.status == C4_WIN) return col;
    }
    return -1;
}

int c4_ai_choose(const c4_game_t *game, c4_ai_opts_t *opts)
{
    if (!game || game->status != C4_ONGOING) return -1;

    windows_init();

    int legal[C4_COLS];
    const int legal_count = c4_legal_cols(game, legal);
    if (legal_count == 0) return -1;
    if (legal_count == 1) return legal[0];

    int max_depth = C4_AI_MAX_DEPTH;
    uint32_t max_nodes = C4_AI_DEFAULT_NODES;
    uint32_t seed = 0;
    uint8_t blunder_percent = 0;
    if (opts) {
        if (opts->max_depth > 0) max_depth = opts->max_depth;
        max_nodes = opts->max_nodes;
        seed = opts->seed;
        blunder_percent = opts->blunder_percent;
        opts->nodes = 0;
    }
    if (max_depth < C4_AI_MIN_DEPTH) max_depth = C4_AI_MIN_DEPTH;
    if (max_depth > C4_AI_MAX_DEPTH) max_depth = C4_AI_MAX_DEPTH;
    if (seed == 0) seed = hash_game(game);

    // 低难度放水:直接随机走一步(先于必胜/必堵判断,所以真的会漏棋)。
    if (blunder_percent > 0) {
        const uint32_t roll = xorshift32(&seed) % 100u;
        if (roll < blunder_percent) {
            const uint32_t pick = xorshift32(&seed) % (uint32_t)legal_count;
            return legal[pick];
        }
    }

    const int forced = immediate_move(game);
    if (forced >= 0) return forced;

    c4_search_t s;
    memset(&s, 0, sizeof(s));
    s.game = *game;
    s.max_nodes = max_nodes;
    s.budget_cb = opts ? opts->budget_cb : NULL;
    s.budget_user = opts ? opts->budget_user : NULL;

    int order[C4_COLS];
    memcpy(order, C4_AI_ORDER, sizeof(order));

    int best_cols[C4_COLS];
    int best_count = 0;

    for (int depth = C4_AI_MIN_DEPTH; depth <= max_depth && !s.exhausted; depth++) {
        int iter_cols[C4_COLS];
        int iter_count = 0;
        int iter_score = -C4_AI_SCORE_INF;
        bool complete = true;

        for (int i = 0; i < C4_COLS; i++) {
            const int col = order[i];
            if (!c4_can_play(&s.game, col)) continue;

            c4_move_t m = c4_play(&s.game, col);
            int score;
            if (m.status == C4_WIN) {
                score = C4_AI_SCORE_WIN + depth;
            } else if (m.status == C4_DRAW) {
                score = 0;
            } else if (iter_count == 0) {
                score = -negamax(&s, depth - 1, -C4_AI_SCORE_INF, C4_AI_SCORE_INF);
            } else {
                // 窗口收紧到 (iter_score-1, +inf):同分走法仍返回精确值,便于随机挑一个。
                score = -negamax(&s, depth - 1, -C4_AI_SCORE_INF, -(iter_score - 1));
            }
            undo_move(&s.game, col, m.player, m.row);

            if (s.exhausted) {
                complete = false;
                break;
            }

            if (score > iter_score) {
                iter_score = score;
                iter_count = 0;
            }
            if (score >= iter_score) iter_cols[iter_count++] = col;
        }

        if (iter_count > 0 && (complete || best_count == 0)) {
            best_count = iter_count;
            memcpy(best_cols, iter_cols, sizeof(int) * (size_t)iter_count);
        } else {
            break;
        }

        // 下一层先把上一层的最好一列放最前,提高剪枝效率。
        const int first = best_cols[0];
        int at = 0;
        for (int i = 0; i < C4_COLS; i++) {
            if (order[i] == first) { at = i; break; }
        }
        for (int i = at; i > 0; i--) order[i] = order[i - 1];
        order[0] = first;
    }

    if (opts) opts->nodes = s.nodes;
    if (best_count == 0) return legal[0];

    const uint32_t pick = xorshift32(&seed) % (uint32_t)best_count;
    return best_cols[pick];
}
