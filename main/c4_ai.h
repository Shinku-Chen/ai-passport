// main/c4_ai.h —— 四子棋电脑对手:纯计算,无 ESP-IDF / LVGL 依赖。
#pragma once

#include "c4_model.h"

#include <stdint.h>

// 可选的分片回调:搜索每处理 C4_AI_BUDGET_INTERVAL 个节点调用一次。
// 返回 false = 预算用完,搜索收工,调用方拿已完成的最好一手。
// 片上可用它查墙钟并 vTaskDelay(1) 让出 CPU(否则会把 IDLE 任务饿到看门狗报警);
// 宿主测试传 NULL,搜索就完全由 max_depth / max_nodes 限定。
typedef bool (*c4_ai_budget_cb_t)(void *user);

// 分片间隔:越小则墙钟预算越准(超调 ≈ 访问这么多节点所需的时间),
// 但每次回调都要 vTaskDelay(1) 让出 CPU,间隔太小会让总耗时变长。
// C3 实测约 15k 节点/秒 → 1024 节点 ≈ 70ms 超调,相当合适。
#define C4_AI_BUDGET_INTERVAL 1024u

typedef struct {
    uint8_t max_depth;   // 迭代加深的深度上限;0 = 默认 8,会被夹到 2..8
    uint32_t max_nodes;  // 搜索节点上限,0 = 不限;用尽则采用已算出的最好一手
    uint32_t seed;       // 同分/失误走法的随机种子,0 = 按局面自身取值(同一局面结果稳定)
    // 失误率(0..100):这个百分比的对局会被随机走一步,连“一步取胜/必堵”都不看。
    // 低难度靠它放水,而不是靠降深度 —— 降深度只会让它不堵棋,看起来像坏了。
    uint8_t blunder_percent;
    c4_ai_budget_cb_t budget_cb;   // 可为 NULL
    void *budget_user;
    uint32_t nodes;      // 出参:本次搜索访问的节点数
} c4_ai_opts_t;

// 为 game 当前待落子方选一列。无合法列返回 -1。
// 保证:能一步取胜就取胜;对手下一手能赢就堵;其余交给迭代加深的 alpha-beta。
// opts 可为 NULL(默认深度 8、节点预算 400000);opts->nodes 会被回填。
int c4_ai_choose(const c4_game_t *game, c4_ai_opts_t *opts);
