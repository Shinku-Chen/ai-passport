// main/parquet/parquet_game.h —— 《PARQUET》剧情状态机 + 存档 + 全局解锁。
//
// 语义与源 src/common/engine.js 一致：
//   text   停在这一句（背景/立绘/cut-in 是 text op 自带的视觉快照，读档重放即可恢复）
//   jumpScene  切场景并把 cursor 设为目标 op
//   choice 停下来等玩家确认；effect 写变量（sf.* 同时进全局周目记录）
//   end    结束；story 模式写当前场景的 clearFlag（对应 source engine 的 end 处理）
//
// 存档保存：场景、正文 op、当前页、模式（正文/待选）、待选光标、变量表、周目变量。
// 读档只合并新增全局位（不回退），任一校验失败都不改动当前状态。
#pragma once

#include "parquet_pack.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PQ_VAR_MAX 8
#define PQ_SLOT_COUNT 30
#define PQ_SLOT_AUTO 30
#define PQ_HISTORY_MAX 32
#define PQ_PREVIEW_BYTES 64
#define PQ_TEXT_MAX 512

typedef enum {
    PQ_STEP_NONE = 0,
    PQ_STEP_TEXT,
    PQ_STEP_CHOICE,
    PQ_STEP_END,
    PQ_STEP_ERROR,
} pq_step_kind_t;

typedef struct {
    pq_step_kind_t kind;
    uint16_t scene;
    uint32_t op;          // 场景内 op 下标
    uint16_t speaker;     // 0xFFFF = 旁白
    uint16_t visual;      // 视觉表下标
    uint16_t choice;      // 选项表下标
    uint8_t choice_count;
    uint8_t tag;          // 章节标签下标
    const char *text;     // 正文（下一次 step 前有效）
    const char *error;    // PQ_STEP_ERROR 的原因（静态字符串）
} pq_step_t;

typedef struct {
    uint16_t speaker;
    uint8_t tag;
    char text[96];
} pq_history_t;

typedef struct {
    uint16_t scene;
    uint8_t page;
    uint8_t mode;
    uint8_t tag;
    uint8_t story;
    char preview[PQ_PREVIEW_BYTES];
    uint32_t saved_at;
    bool valid;
} pq_save_info_t;

typedef struct {
    uint8_t speed;       // 0=慢 1=中 2=快 3=瞬间
    uint8_t auto_speed;  // 0=慢 1=标准 2=快
    uint8_t title_bg;    // 0..2
    uint8_t font;        // 0=16px 标准 1=20px 大
} pq_settings_t;

typedef struct {
    pq_pack_t pack;
    uint16_t scene;
    uint32_t cursor;      // 下一条要执行的 op（场景内）
    uint32_t text_op;     // 当前正文 op；0xFFFFFFFF = 无
    uint8_t mode;         // 0=正文 1=待选
    uint8_t choice_sel;
    uint16_t current_visual;  // 最近一次展示的视觉组合（待选存档需要）
    bool story;           // true=正篇/后日谈（写周目记录），false=章节回顾

    int32_t vars[PQ_VAR_MAX];
    uint8_t var_set;

    // 全局周目记录（独立 NVS 段）
    int32_t pvars[PQ_VAR_MAX];
    uint8_t pvar_set;
    uint64_t cg_seen;      // 37 个鉴赏组（位图，最多 64 组）
    uint16_t ending_seen;  // 3 个结局名
    bool meta_dirty;

    pq_history_t history[PQ_HISTORY_MAX];
    int hist_count;
    int hist_head;

    bool nvs_ok;
    char init_error[64];
} pq_game_t;

bool pq_game_init(pq_game_t *game);
bool pq_game_nvs_ok(const pq_game_t *game);

#ifdef PQ_PACK_USE_HOST_BLOBS
// 宿主测试：由测试加载真实资源包文件（固件里用 target_add_binary_data 的符号）。
bool pq_host_set_pack(const uint8_t *img, size_t img_size, const uint8_t *scn,
                      size_t scn_size);
#endif

// 从场景开头开始。story=true 时叠加全局 sf.* 周目变量。
void pq_game_start(pq_game_t *game, uint16_t scene, bool story);
// 推进一步。返回 false 表示状态无效（error 已填）。
bool pq_game_step(pq_game_t *game, pq_step_t *step);
// 确认当前待选（index 从 0 起）。
bool pq_game_choose(pq_game_t *game, int index);
int pq_game_choice_count(const pq_game_t *game);
const pq_choice_item_t *pq_game_choice_item(const pq_game_t *game, int index);
// 当前场景/op 的视觉快照（读档后 UI 重绘用）。
const pq_visual_t *pq_game_visual(const pq_game_t *game);

// 历史
int pq_game_history_count(const pq_game_t *game);
const pq_history_t *pq_game_history_at(const pq_game_t *game, int index);

// 变量（供测试/周目语义检查）
bool pq_game_var(const pq_game_t *game, uint16_t index, int32_t *out);
bool pq_game_global_var(const pq_game_t *game, uint16_t index, int32_t *out);
bool pq_game_global_var_recorded(const pq_game_t *game, uint16_t index);

// CG / 结局解锁
void pq_game_mark_cg(pq_game_t *game, uint16_t asset_id);
bool pq_game_cg_unlocked(const pq_game_t *game, uint16_t group);
bool pq_game_ending_seen(const pq_game_t *game, uint16_t name_index);
uint16_t pq_game_ending_count(const pq_game_t *game);

// 存档（NVS；slot 0..29，PQ_SLOT_AUTO 为自动档）。page 是正文分页下标。
bool pq_game_save(pq_game_t *game, int slot, uint8_t page);
bool pq_game_load(pq_game_t *game, int slot, uint8_t *page_out);
bool pq_game_slot_peek(int slot, const pq_pack_t *pack, pq_save_info_t *out);
bool pq_game_slot_exists(int slot);
bool pq_game_delete_slot(int slot);

// 全局周目记录落盘；失败保持 meta_dirty 供 UI 节流重试。
bool pq_game_save_meta(pq_game_t *game);
void pq_game_flush_meta(pq_game_t *game);

// 设置
bool pq_settings_load(pq_settings_t *out);
bool pq_settings_save(const pq_settings_t *settings);
