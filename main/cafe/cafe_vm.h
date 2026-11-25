// main/cafe/cafe_vm.h —— 与源 engine.js 同语义的剧情游标/条件/永久进度 VM。
//
// 源语义（xuanyide01/cafestella-rw5 src/common/engine.js）：
//   * locate: 帧未播完返回 text/op；到节点尾先结算 complete(章节试读除外)，
//     再看 choices，再按顺序选第一个 when 匹配的 next。
//   * flags(): 本周目 state.flag + flagRules 派生值 + checkAnyClear(任一女主线通关)。
//   * choose: 写 flag[effect[0]]=effect[1]，跳到目标节点。
//   * 历史最多 24 步，previous 恢复快照。
// 本模块只依赖 cafe_pack 的只读元数据，可在宿主机上原样跑。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define CAFE_FLAG_MAX 128
#define CAFE_VM_HISTORY 24
#define CAFE_PROGRESS_BITS 13

enum {
    CAFE_LOC_ERROR = 0,
    CAFE_LOC_TEXT,
    CAFE_LOC_OP,
    CAFE_LOC_CHOICE,
    CAFE_LOC_END,
};

enum {
    CAFE_PROGRESSION_STORY = 0,
    CAFE_PROGRESSION_CHAPTER = 1,
};

typedef struct {
    uint16_t node;
    uint16_t index;
    uint32_t progress;
    uint8_t progression;
    uint8_t last_complete;   // 0xFF = 无
    uint8_t flags[CAFE_FLAG_MAX];
} cafe_vm_snapshot_t;

typedef struct {
    uint16_t node;
    uint16_t index;
    uint8_t flags[CAFE_FLAG_MAX];
    uint32_t progress;
    uint8_t progression;
    uint8_t last_complete;
    uint8_t history_count;
    uint8_t history_head;
    cafe_vm_snapshot_t history[CAFE_VM_HISTORY];
} cafe_vm_t;

// 永久进度位（与 tools/cafe_pack.py PROGRESS_KEYS 顺序一致）。
enum {
    CAFE_PROG_BAD = 0,
    CAFE_PROG_NORMAL,
    CAFE_PROG_KANNA,
    CAFE_PROG_NATSUME,
    CAFE_PROG_NOZOMI,
    CAFE_PROG_MEI,
    CAFE_PROG_SUZUNE,
    CAFE_PROG_KANNA_AFTER,
    CAFE_PROG_NATSUME_AFTER,
    CAFE_PROG_NOZOMI_AFTER,
    CAFE_PROG_MEI_AFTER,
    CAFE_PROG_SUZUNE_AFTER,
    CAFE_PROG_LAST_CLEARED,
};

void cafe_vm_init(cafe_vm_t *vm, uint32_t permanent_progress);
void cafe_vm_new_game(cafe_vm_t *vm, uint32_t permanent_progress);
void cafe_vm_checkpoint(const cafe_vm_t *vm, cafe_vm_snapshot_t *out);
bool cafe_vm_restore(cafe_vm_t *vm, const cafe_vm_snapshot_t *snap);

// 位置：返回 CAFE_LOC_*。choice 时 *choice_first / *choice_count 有效。
int cafe_vm_locate(cafe_vm_t *vm, uint16_t *node_out, uint16_t *index_out,
                   uint16_t *choice_first, uint8_t *choice_count);
int cafe_vm_advance(cafe_vm_t *vm);
int cafe_vm_choose(cafe_vm_t *vm, uint8_t option_index);
int cafe_vm_skip_chapter(cafe_vm_t *vm);
int cafe_vm_previous(cafe_vm_t *vm);

bool cafe_vm_has_history(const cafe_vm_t *vm);
uint8_t cafe_vm_flag(const cafe_vm_t *vm, uint8_t flag_id);
bool cafe_vm_matches(const cafe_vm_t *vm, uint16_t when_index);
bool cafe_vm_check_any_clear(const cafe_vm_t *vm);

// 源状态/存档用。
static inline bool cafe_vm_progress_has(const cafe_vm_t *vm, uint8_t bit)
{
    return bit < CAFE_PROGRESS_BITS && (vm->progress & (1u << bit)) != 0;
}
static inline bool cafe_vm_progress_is_main(uint8_t bit)
{
    return bit >= CAFE_PROG_KANNA && bit <= CAFE_PROG_SUZUNE;
}
