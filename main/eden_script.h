// main/eden_script.h —— eden 的剧本执行器(纯逻辑,可宿主机测试)。
//
// 源工程的剧本是"场景 = 一段指令序列",指令只有 6 种(text/set/if/goto/jumpScene/end),
// 见 tools/eden_pack.py 与源仓库 src/common/engine.js。这里逐条对齐那套语义:
//   * 走到 text 就停住,把这一句交给界面;界面推进时再调 eden_script_advance。
//   * set 写变量、if 比较后跳转、goto 无条件跳转、jumpScene 换场景、end 收尾。
//   * 指令下标是"全包绝对下标",场景表给出每个场景的 [start, end) 区间,
//     走过区间末尾等同于剧本结束。
//
// 纯逻辑:不认识 LVGL / NVS / Flash,只吃 eden_pack_t 的只读视图。
#pragma once

#include "eden_pack.h"

#include <stdbool.h>
#include <stdint.h>

// 剧本变量上限。eden 只用到 4 个(解锁标记),留一点余量。
#define EDEN_VAR_MAX 8

// 单次推进最多执行多少条非 text 指令:防止坏包里的 goto 环把任务卡死。
#define EDEN_SCRIPT_STEP_LIMIT 256

typedef struct {
    uint16_t scene;              // 当前场景号(1 起)
    uint32_t op;                 // 当前指令的绝对下标
    int32_t vars[EDEN_VAR_MAX];  // 剧情变量(存档要一起持久化)
    bool ended;                  // 已走到剧本末尾
} eden_state_t;

typedef enum {
    EDEN_STEP_TEXT = 0,   // 拿到一句对白
    EDEN_STEP_END,        // 剧本结束
    EDEN_STEP_YIELD,      // 本次推进到上限仍未出结果(坏包/异常脚本)
} eden_step_t;

typedef struct {
    eden_step_t kind;
    uint16_t scene;      // 这句所属场景
    uint16_t speaker;    // SEC_TEXT 下标(空串 = 旁白)
    uint16_t text;       // SEC_TEXT 下标
    uint16_t frame;      // SEC_FRAME 下标(这一句要显示的画面)
} eden_line_t;

// 从某个场景的第一句重新开始。
void eden_state_reset(eden_state_t *state, uint16_t scene);

// 直接定位到某条指令(读档、章节跳转用)。scene/op 非法返回 false。
bool eden_state_seek(eden_state_t *state, const eden_pack_t *pack,
                     uint16_t scene, uint32_t op);

// 从 state->op 开始执行,直到拿到一句对白 / 剧本结束。
// 成功返回 EDEN_STEP_TEXT 或 EDEN_STEP_END;坏包返回 EDEN_STEP_YIELD。
eden_step_t eden_script_step(const eden_pack_t *pack, eden_state_t *state,
                             eden_line_t *line);

// 推进到下一句(仅当上一句是 TEXT 时有意义)。
void eden_script_advance(eden_state_t *state, const eden_pack_t *pack);

// 当前场景的章节说明下标(SEC_CHAP 中与 state->scene 匹配的那一条),找不到返回 -1。
int eden_script_chapter_of(const eden_pack_t *pack, uint16_t scene);
