// main/anemoi/anemoi_script.h —— 《anemoi》剧情解释器（不依赖 ESP-IDF/LVGL）。
//
// 直接执行 tools/anemoi_pack.py 生成的指令流，语义对齐源快照的 engine-vm.js：
//   * 变量、dress/pose 记忆、立绘栈、背景/CG、changedBodies/changedCG
//   * ifFalse/ifTrue/goto/call/return/jumpScene/callScene/routeCheck
//   * dynamicChoice（文案/可用性/目标）、board（委托板）、end、omittedScene、skipGame
//   * helpers.js 里的脚本函数（按 <chapter>:<name> 再 <name> 查找）
//
// 存档只需要这份状态 + 页码；文本与图像都在包内，按 id 引用。
#pragma once

#include "anemoi_pack.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANEM_VAR_MAX 512
#define ANEM_ACTOR_MAX 32
#define ANEM_BODY_MAX 3
#define ANEM_CHOICE_MAX 16
#define ANEM_BOARD_MAX 26
#define ANEM_HISTORY_MAX 2
#define ANEM_HELPER_DEPTH 10
#define ANEM_VALUE_STACK 24
#define ANEM_ARRAY_POOL 96
#define ANEM_STACK_FRAMES 20
#define ANEM_LABEL_LEN 56
#define ANEM_SCRATCH_SLOTS 64
#define ANEM_SCRATCH_LEN 32
#define ANEM_SCRATCH_FLAG 0x8000u

typedef enum {
    ANEM_VAL_NUM = 0,
    ANEM_VAL_STR = 1,
    ANEM_VAL_ARR = 2,
} anem_val_kind_t;

// 8 字节：存档按原样序列化（下标 0..var_count-1）。
typedef struct {
    int32_t num;
    uint16_t text;     // ANEM_VAL_STR：TEXT 段 id；ANEM_VAL_ARR：arena 起始
    uint8_t kind;
    uint8_t len;       // 数组长度
} anem_value_t;

typedef struct {
    uint16_t name;     // 立绘名文本 id（解析后的 pose key，不含 @dress）
    uint16_t dress;
    bool valid;
} anem_body_slot_t;

typedef struct {
    uint16_t key;          // 委托 detail 文本 id（= _bbs_requestNNNN）
    uint16_t button;
    uint16_t preview;      // 任务场景号（源 preview 字段）
    uint16_t scene;        // 介绍场景号（源 scene 字段）
    uint16_t entry_task;   // 任务入口（源 previewEntry）
    uint16_t entry_letter; // 介绍入口（源 entry）
    int16_t position;
    int16_t character;
    int32_t time;
} anem_board_item_t;

typedef struct {
    uint16_t speaker;
    uint16_t text;
    uint8_t count;      // 选项/委托板条目数
    uint8_t kind;       // ANEM_FRAME_*
} anem_frame_t;

typedef struct {
    uint16_t label;     // 文本 id
    uint16_t detail;    // 委托正文文本 id（0 = 无）
    uint8_t enabled;
    uint8_t value;
    bool board;
} anem_choice_item_t;

// 可存档状态（体量固定；变量按 var_count 使用）。
typedef struct {
    uint16_t chapter;
    uint32_t cursor;
    uint16_t root_chapter;
    uint8_t progression;    // 0 = chapter，1 = story
    bool ended;
    uint16_t speaker;
    uint16_t text;
    uint16_t regular_bg;
    uint16_t cg;
    uint8_t body_count;
    uint16_t var_count;
    uint32_t changed;       // 演员位掩码：本场景内改过的立绘
    bool changed_cg;
    anem_body_slot_t bodies[ANEM_BODY_MAX];
    uint8_t dress[ANEM_ACTOR_MAX];
    uint16_t pose[ANEM_ACTOR_MAX];
    anem_value_t vars[ANEM_VAR_MAX];
} anem_state_t;

typedef struct {
    uint16_t image;
    int16_t x;
    int16_t y;
    bool valid;
} anem_sprite_t;

typedef struct {
    uint16_t back_image;      // 图像下标（ANEM_NONE16 = 缺图）
    uint16_t back_name;       // 背景名文本 id
    bool cg_layer;
    uint8_t sprite_count;
    anem_sprite_t sprites[ANEM_BODY_MAX];
} anem_picture_t;

typedef struct {
    uint16_t scene;
    uint32_t cursor;
    uint8_t board_stage;   // 0 = 普通调用，1 = 待读介绍，2 = 任务返回
    anem_board_item_t board;
} anem_call_frame_t;

enum {
    ANEM_FRAME_LINE = 0,
    ANEM_FRAME_CHOICE = 1,
    ANEM_FRAME_BOARD = 2,
    ANEM_FRAME_END = 3,
    ANEM_FRAME_BOUNDARY = 4,
    ANEM_FRAME_ERROR = 5,
};

typedef struct {
    anem_state_t st;
    const anem_pack_t *pack;
    uint8_t helper_depth;
    anem_value_t arena[ANEM_ARRAY_POOL];
    uint16_t arena_used;
    anem_call_frame_t calls[ANEM_STACK_FRAMES];
    uint8_t call_count;
    anem_board_item_t board[ANEM_BOARD_MAX];
    uint8_t board_count;
    anem_choice_item_t choice[ANEM_CHOICE_MAX];
    uint8_t choice_count;
    int32_t last_call_result;
    int32_t route_result;
    uint8_t route_prelude;     // 0 = 无，1 = sp，2 = hi
    int32_t rng;
    char notice[64];
    bool failed;
    char boundary_chapter[48];
    uint32_t choice_target_expr;
    bool arrive_pending;
    uint16_t arrive_var;
    struct {
        anem_state_t state;
        bool used;
    } history[ANEM_HISTORY_MAX];
    uint8_t history_count;
    uint8_t history_next;
    uint8_t history_text_count;
    uint16_t history_texts[24];
    uint16_t local_ids[24];
    uint8_t local_count;
    anem_value_t local_saved[ANEM_HELPER_DEPTH][24];
    uint8_t local_depth;
    // 运行期生成的短字符串（helper 的 left/right/mid/tostr 结果）。包里没有的
    // 字符串不能静默变成 id 0（那会变成别的台词），放进这个环形表，id 用
    // 0x8000|slot 表示；text_of()/anem_script_text() 负责解析。
    char scratch[ANEM_SCRATCH_SLOTS][ANEM_SCRATCH_LEN];
    uint8_t scratch_next;
} anem_script_t;

// 初始化：绑定包、缓存变量表与 cur_call.* 局部变量 id。
bool anem_script_init(anem_script_t *s, const anem_pack_t *pack);

// 新游戏：story=true 时从 story 流程入口开始（源 newState(flowScene, true)）。
void anem_script_new(anem_script_t *s, uint16_t scene, bool story);

// 从章节入口开始（标题的章节选择 / 继续游戏恢复后使用）。
bool anem_script_start_chapter(anem_script_t *s, uint16_t scene, uint16_t entry);

// 读一帧。返回 ANEM_FRAME_LINE/CHOICE/BOARD/END/BOUNDARY/ERROR。
anem_frame_t anem_script_read(anem_script_t *s);

// 当前选项/委托项。
const anem_choice_item_t *anem_script_choice_item(const anem_script_t *s, uint8_t index);

// 显式确认第 index 项。
bool anem_script_choose(anem_script_t *s, uint8_t index);

// 下一句（压历史并前进）。
void anem_script_next(anem_script_t *s);

// 上一句；没有历史返回 false。
bool anem_script_previous(anem_script_t *s);

// 历史台词（供历史页），0 = 最旧。
uint8_t anem_script_history_text_count(const anem_script_t *s);
uint16_t anem_script_history_text(const anem_script_t *s, uint8_t index);

// 当前画面。
void anem_script_picture(const anem_script_t *s, anem_picture_t *out);

// 章节名 / Day 标签（UTF-8，可能为空串，不返回 NULL）。
const char *anem_script_chapter_name(const anem_script_t *s);
const char *anem_script_day_label(const anem_script_t *s);

// 全局解锁等级（源 progress.js 的 storyUnlocks）。
uint8_t anem_script_unlock_level(const anem_script_t *s);

// 变量辅助（存档恢复）。
bool anem_var_id_by_name(const anem_pack_t *pack, const char *name, uint16_t *out);
const char *anem_var_name(const anem_pack_t *pack, uint16_t id);
anem_value_t anem_script_var_get(const anem_script_t *s, uint16_t id);
bool anem_script_var_set_num(anem_script_t *s, uint16_t id, int32_t num);
bool anem_script_var_set_text(anem_script_t *s, uint16_t id, uint16_t text);

// 源 routeCheck（测试可直接调用断言）。
bool anem_script_route_check(anem_script_t *s);

// native 调用（测试可断言覆盖；解释器内部也用它）。
bool anem_script_native(anem_script_t *s, const char *name, anem_value_t *args, uint8_t argc,
                        const anem_value_t *receiver, anem_value_t *out);

// 源 poseKey 解析（供 native 的 dress 刷新与测试复用）。
void anem_script__pose_key(anem_script_t *s, const char *raw, char *out, size_t cap);

// 解析脚本值里的文本 id：包内字符串或运行期 scratch 字符串。
const char *anem_script_text(const anem_script_t *s, uint16_t id);

const char *anem_script_error(const anem_script_t *s);
const char *anem_script_notice(const anem_script_t *s);

#ifdef __cplusplus
}
#endif
