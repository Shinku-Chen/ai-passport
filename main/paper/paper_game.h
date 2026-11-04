// main/paper/paper_game.h —— 剧情状态机 + 存档 + 解锁进度。
//
// 语义与源工程 src/common/engine.js 一致（同一份 ops 顺序）：
//   text  -> 停在这一句；sprite/background/set/clearSprites 立即生效继续走；
//   ifFalse(cond) 为假时跳到 c；jumpScene 切场景并把 cursor 设为目标 op；
//   choice 停下来等玩家选；end 结束。
// 存档保存：场景 id、cursor、背景、三个立绘槽、变量表、CG 解锁位、结局位。
#pragma once

#include "paper_pack.h"

#include <stdbool.h>
#include <stdint.h>

#define PAPER_SLOTS 8
#define PAPER_SLOT_AUTO 8      // 自动存档槽（不进手动槽列表）
#define PAPER_HISTORY_MAX 32
#define PAPER_HISTORY_TEXT 120

typedef enum {
    PAPER_STEP_TEXT = 0,
    PAPER_STEP_CHOICE,
    PAPER_STEP_END,
    PAPER_STEP_ERROR,
} paper_step_kind_t;

typedef struct {
    paper_step_kind_t kind;
    uint8_t speaker;          // 说话人下标
    const char *text;         // 指向正文缓存（下一次 step 前有效）
    uint16_t background;      // 当前背景图
    uint16_t sprites[3];      // 立绘槽（0xFFFF = 空）
    uint16_t choice;          // CHOICE：选项表下标
    int choice_count;
    uint16_t end_scene;       // END：所在场景
} paper_step_t;

typedef struct {
    uint16_t slot;
    char speaker[24];
    char text[PAPER_HISTORY_TEXT];
} paper_history_entry_t;

typedef struct {
    paper_pack_t pack;
    int32_t vars[192];
    uint8_t cg_unlocked[(80 + 7) / 8];
    uint8_t endings_seen;      // bit0..5
    bool meta_dirty;           // 解锁进度尚未成功落盘
    bool story_mode;           // true=主线（sf.* 回写周目记录）；false=章节浏览（只读携带）
    int32_t persist_vars[192]; // 源 progress.js 的 sf.* 周目变量
    uint8_t persist_set[24];   // 哪些 sf.* 变量"有记录"（192 bit）
    uint16_t scene_id;
    uint32_t cursor;          // 下一条要执行的 op
    int32_t text_op;          // 当前显示的 TEXT op（-1 无）
    int32_t choice_op;        // 当前停住的 CHOICE op（-1 无）
    uint16_t background;
    uint16_t sprites[3];
    uint8_t speaker;
    uint16_t last_choice;      // 用于回到选项前
    bool ended;
    paper_history_entry_t history[PAPER_HISTORY_MAX];
    int history_count;
    int history_head;
} paper_game_t;

bool paper_game_init(paper_game_t *g);
// 从某个场景开始（章节跳转 / 新游戏 = cg=1 场景）
bool paper_game_start(paper_game_t *g, uint16_t scene_id);
// 新游戏：入口取自包的 flowScene，并叠加源 progress.js 的 sf.* 周目变量
bool paper_game_new(paper_game_t *g);
bool paper_nvs_available(void);
// 推进一步：执行到下一句对白 / 选项 / 结局
const paper_step_t *paper_game_step(paper_game_t *g);
// 选择一个选项（index 从 0 起）
bool paper_game_choose(paper_game_t *g, int index);
// 历史
int paper_game_history_count(const paper_game_t *g);
const paper_history_entry_t *paper_game_history_at(const paper_game_t *g, int i);

// CG / 结局解锁
bool paper_game_cg_unlocked(const paper_game_t *g, int group);
void paper_game_unlock_cg(paper_game_t *g, int group);
// 按图片下标解锁其所属 CG 组；返回组号（-1 = 不属于任何组）。
int paper_game_unlock_image(paper_game_t *g, uint16_t image_index);
int paper_game_endings_seen(const paper_game_t *g);
void paper_game_capture_progress(paper_game_t *g);
// 该 sf.* 变量是否"有记录"（用于设置语义与测试）
bool paper_game_var_recorded(const paper_game_t *g, int index);

// 存档（NVS）。返回 false 表示无存档 / 版本不符 / 数据损坏。
bool paper_game_save(paper_game_t *g, int slot);
bool paper_game_load(paper_game_t *g, int slot);
bool paper_game_slot_exists(int slot);
bool paper_game_slot_brief(int slot, const paper_pack_t *pack, char *out, size_t cap);
bool paper_game_load_meta(paper_game_t *g);
bool paper_game_save_meta(paper_game_t *g);
void paper_game_flush_meta(paper_game_t *g);   // 重试未落盘的解锁进度   // "第三章 · 2026-..."
void paper_game_delete_slot(int slot);
