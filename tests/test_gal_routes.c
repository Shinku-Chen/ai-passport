// tests/test_gal_routes.c —— 用真实资源包遍历全部剧情路线(宿主运行)。
//
// 目的:锁死"界面可见选项 -> 模型组内序号"的换算。UI 只能走 gal_choice_pick;
// 本测试用与 UI 相同的入口遍历、并在每个选项上校验:
//   * 选择成功(不因下标越界被拒绝);
//   * 落点就是该选项自己的 target;
//   * 变量按该选项的 actions 变化(逐项比对);
// 最后统计:15 组选项 / 53 个选项全部被实际执行过,ending_mask=0xF,零错误。
#include "gal_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        failures++; \
    } \
} while (0)

#define MAX_STATES 20000

typedef struct {
    gal_play_t play;
    char path[256];
} state_t;

static state_t g_states[MAX_STATES];
static int g_count;
static uint8_t g_group_seen[64];
static uint8_t g_option_seen[256];
static int g_choice_stops, g_terminals, g_errors;
static uint32_t g_end_mask;

static int dedupe(const gal_play_t *p)
{
    for (int i = 0; i < g_count; ++i) {
        if (g_states[i].play.pc == p->pc &&
            memcmp(g_states[i].play.vars, p->vars, sizeof(p->vars)) == 0) {
            return 1;
        }
    }
    return 0;
}

static void push(const gal_play_t *p, const char *path)
{
    if (g_count >= MAX_STATES) { g_errors++; return; }
    g_states[g_count].play = *p;
    snprintf(g_states[g_count].path, sizeof(g_states[g_count].path), "%s", path);
    g_count++;
}

static void apply_actions(int16_t *vars, const gal_pack_t *pack, const gal_option_t *opt)
{
    for (int i = 0; i < opt->act_count; ++i) {
        const uint32_t ai = (uint32_t)opt->act_first + (uint32_t)i;
        if (ai >= pack->action_count) continue;
        const gal_action_t *a = &pack->actions[ai];
        if (a->var >= GAL_MAX_VARS) continue;
        if (a->op == GAL_OP_SET) vars[a->var] = a->value;
        else if (a->op == GAL_OP_ADD) vars[a->var] = (int16_t)(vars[a->var] + a->value);
        else if (a->op == GAL_OP_SUB) vars[a->var] = (int16_t)(vars[a->var] - a->value);
    }
}

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "main/gal_data/gal_pack.bin";
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
    fseek(f, 0, SEEK_END);
    const long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *blob = malloc((size_t)len);
    if (!blob || fread(blob, 1, (size_t)len, f) != (size_t)len) { fclose(f); return 2; }
    fclose(f);

    gal_pack_t pack;
    const char *err = NULL;
    if (!gal_pack_open(&pack, blob, (uint32_t)len, &err)) {
        fprintf(stderr, "bad pack: %s\n", err ? err : "?");
        return 2;
    }

    gal_play_t start;
    gal_play_reset(&start);
    push(&start, "start");

    for (int head = 0; head < g_count; ++head) {
        gal_play_t p = g_states[head].play;
        int stopped = 0;
        for (int steps = 0; steps < 50000 && !stopped; ++steps) {
            const gal_step_t step = gal_play_next(&p, &pack);
            switch (step.kind) {
            case GAL_STEP_LINE:
            case GAL_STEP_CHAPTER:
                continue;
            case GAL_STEP_UNLOCK_END:
                g_end_mask |= 1u << step.index;
                continue;
            case GAL_STEP_ENDING:
                g_end_mask |= 1u << step.index;
                g_terminals++;
                stopped = 1;
                break;
            case GAL_STEP_END:
                g_terminals++;
                stopped = 1;
                break;
            case GAL_STEP_ERROR:
                g_errors++;
                stopped = 1;
                break;
            case GAL_STEP_CHOICE: {
                g_choice_stops++;
                const gal_choice_t *ch = &pack.choices[step.index];
                if (step.index < sizeof(g_group_seen)) g_group_seen[step.index] = 1;
                uint16_t visible[8];
                const int n = gal_choice_visible(&pack, &p, step.index, visible, 8);
                CHECK(n >= 2 && n <= (int)ch->opt_count, "group %u visible %d",
                      (unsigned)step.index, n);
                for (int i = 0; i < n; ++i) {
                    const uint16_t global = visible[i];
                    const gal_option_t *opt = &pack.options[global];
                    if (global < sizeof(g_option_seen)) g_option_seen[global] = 1;

                    gal_play_t next = p;
                    // 界面路径:可见序号 -> gal_choice_pick(内部换算成组内序号)。
                    const bool ok = gal_choice_pick(&next, &pack, step.index, i);
                    CHECK(ok, "reject group=%u visible=%d global=%u pc=%u",
                          (unsigned)step.index, i, (unsigned)global, (unsigned)p.pc);
                    if (!ok) { g_errors++; continue; }

                    CHECK(next.pc == opt->target,
                          "group %u visible %d landed pc=%u expected %u",
                          (unsigned)step.index, i, (unsigned)next.pc, (unsigned)opt->target);

                    int16_t expect[GAL_MAX_VARS];
                    memcpy(expect, p.vars, sizeof(expect));
                    apply_actions(expect, &pack, opt);
                    CHECK(memcmp(expect, next.vars, sizeof(expect)) == 0,
                          "group %u visible %d vars mismatch", (unsigned)step.index, i);

                    char child[256];
                    snprintf(child, sizeof(child), "%s>%u.%d", g_states[head].path,
                             (unsigned)step.index, i);
                    if (!dedupe(&next)) push(&next, child);
                }
                stopped = 1;
                break;
            }
            default:
                g_errors++;
                stopped = 1;
                break;
            }
        }
        if (!stopped) { g_errors++; }
    }

    int groups = 0, options = 0;
    for (size_t i = 0; i < sizeof(g_group_seen); ++i) groups += g_group_seen[i];
    for (size_t i = 0; i < sizeof(g_option_seen); ++i) options += g_option_seen[i];

    printf("routes: states=%d choice_stops=%d terminals=%d ending_mask=0x%x "
           "groups=%d options=%d errors=%d\n",
           g_count, g_choice_stops, g_terminals, g_end_mask, groups, options, g_errors);
    CHECK(groups == 15, "groups exercised %d", groups);
    CHECK(options == 53, "options exercised %d", options);
    CHECK(g_end_mask == 0xFu, "ending mask 0x%x", g_end_mask);
    CHECK(g_errors == 0, "route errors %d", g_errors);
    CHECK(failures == 0, "%d assertion failures", failures);

    free(blob);
    if (failures) return 1;
    printf("routes ok\n");
    return 0;
}
