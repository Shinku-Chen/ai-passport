// main/summer_model.h —— 剧本虚拟机、变量表、存档编解码、周目/鉴赏进度。
//
// 纯逻辑层：只依赖 summer_pack，不依赖 ESP-IDF / LVGL，宿主机测试直接编译。
// 语义与 RW5 源 src/common/engine.js 的 readFrame() 一致：
//   text 停在当前 op；image/set/add/sub/if/goto/call/callScene/jumpScene/
//   return/end/clear/nop 都是立即执行；choice/map 停下等玩家确认。
// 存档写"下一个要读的 op"（text/choice/map 都指向该 op），恢复后重新执行该 op
// 就能得到同一句/同一待选，不需要把正文再抄一份。
#pragma once

#include "summer_pack.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SUMMER_VAR_CAP 768
#define SUMMER_STACK_CAP 32
#define SUMMER_MAP_GROUPS 32
#define SUMMER_MAP_VALUES 64
#define SUMMER_PROGRESS_ENDING_MAX 14
#define SUMMER_GALLERY_GROUPS_MAX 256
#define SUMMER_SAVE_VERSION 2u
#define SUMMER_SAVE_CAP 4096u

#define SUMMER_MODE_STORY 0u
#define SUMMER_MODE_CHAPTER 1u

// 标题可选项（按源 title-progress.js 的 titleActions()）。
enum {
    SUMMER_TITLE_START = 1u << 0,
    SUMMER_TITLE_RESTART = 1u << 1,
    SUMMER_TITLE_ALKA = 1u << 2,
    SUMMER_TITLE_POCKET = 1u << 3,
};

typedef struct {
    uint32_t id;
    int32_t value;
} summer_var_t;

typedef struct {
    summer_var_t entries[SUMMER_VAR_CAP];
    uint16_t count;
} summer_vars_t;

typedef struct {
    uint16_t cleared;          // 结局 1..14 的位
    uint8_t restart_stage;     // 0..4
    uint32_t globals[6];       // 12001/12002/12010/12011/12012/12013
    uint8_t seen_cg[(SUMMER_GALLERY_GROUPS_MAX + 7) / 8];
    bool dirty;                // 尚未成功落盘
    uint32_t retry_ms;         // 节流/重试计数
} summer_progress_t;

typedef struct {
    uint8_t mode;              // STORY / CHAPTER
    bool ended;
    uint16_t scene_id;
    uint16_t scene_index;
    uint16_t root_scene;
    uint32_t op_index;         // 下一个要执行的 op
    uint32_t op_offset;        // OPS 段内字节偏移
    uint16_t bg;               // 源图 id；0 = 黑屏
    uint16_t body;
    uint8_t stack_depth;
    uint16_t stack_scene[SUMMER_STACK_CAP];
    uint32_t stack_op[SUMMER_STACK_CAP];
    uint16_t choice_sel;
    uint8_t map_bits[(SUMMER_MAP_GROUPS * SUMMER_MAP_VALUES + 7) / 8];
    uint16_t preview_scene;   // previewMap: scene at the start of the scan
    bool preview_mode;        // previewMap: stop immediately after a scene change
    char date[24];            // strings[106] raw (full-width digits)
    summer_vars_t vars;
    uint8_t error;             // 0 ok / 1 bad op / 2 stack / 3 expr / 4 var cap
} summer_vm_t;

typedef enum {
    SUMMER_STEP_TEXT = 0,
    SUMMER_STEP_CHOICE,
    SUMMER_STEP_MAP,
    SUMMER_STEP_END,
    SUMMER_STEP_YIELD,
    SUMMER_STEP_ERROR,
    SUMMER_STEP_SCENE,        // previewMap only: scene just changed
} summer_step_kind_t;

typedef struct {
    summer_step_kind_t kind;
    uint32_t speaker;          // TEXT：说话人下标
    uint32_t text;             // TEXT：TEXT 段下标
    uint32_t group;            // CHOICE/MAP：组下标
    uint8_t error;
} summer_step_t;

typedef struct {
    int item_index;            // map 组内项目下标
    int32_t value;
    uint16_t dest_scene;
    uint32_t dest_op;
    char label[96];            // 场景名 + 可选提示（UTF-8）
} summer_map_choice_t;

typedef struct {
    uint8_t mode;
    bool ended;
    uint16_t scene_id;
    uint16_t root_scene;
    uint32_t op_index;
    uint8_t page;
    uint8_t choice_sel;
    uint16_t bg;
    uint16_t body;
    char date[24];
    uint8_t stack_depth;
    uint16_t stack_scene[SUMMER_STACK_CAP];
    uint32_t stack_op[SUMMER_STACK_CAP];
    uint16_t var_count;
    uint16_t seen_cg_count;
    uint16_t cleared;
    summer_vars_t vars;
    uint8_t map_bits[(SUMMER_MAP_GROUPS * SUMMER_MAP_VALUES + 7) / 8];
    uint8_t seen_cg[(SUMMER_GALLERY_GROUPS_MAX + 7) / 8];
} summer_save_t;

// ---- 变量
void summer_vars_init(summer_vars_t *vars);
int32_t summer_vars_get(const summer_vars_t *vars, uint32_t id);
bool summer_vars_set(summer_vars_t *vars, uint32_t id, int32_t value);
int summer_vars_clear_count(const summer_vars_t *vars);
// 遍历非零项（存档用）。
int summer_vars_count_nonzero(const summer_vars_t *vars);

// ---- 表达式（内部使用，也开放给宿主机测试）
bool summer_expr_eval(const summer_pack_t *pack, uint32_t expr_id, summer_vm_t *vm,
                      int32_t *out);

// ---- 虚拟机
void summer_vm_init(summer_vm_t *vm);
bool summer_vm_start_story(summer_vm_t *vm, const summer_pack_t *pack);
bool summer_vm_start_chapter(summer_vm_t *vm, const summer_pack_t *pack, uint16_t scene_id);
bool summer_vm_start_flow(summer_vm_t *vm, const summer_pack_t *pack, int action);
// 执行到下一个停点。最多 guard 条指令；返回 YIELD 时上层再调一次。
summer_step_t summer_vm_step(summer_vm_t *vm, const summer_pack_t *pack, int guard);
// 停在 text/choice/map 后前进一条（text 推进到下一句，choice/map 由 choose 调用）。
bool summer_vm_advance(summer_vm_t *vm, const summer_pack_t *pack);
// 选择：item_index 是组内下标（不是"可见下标"）。成功写入变量并前进。
bool summer_vm_choose(summer_vm_t *vm, const summer_pack_t *pack, uint32_t group, int item_index);
bool summer_vm_choose_map(summer_vm_t *vm, const summer_pack_t *pack, uint32_t group,
                          int item_index);
// 过滤后的可见选项组内下标；返回数量。
int summer_vm_visible_choices(summer_vm_t *vm, const summer_pack_t *pack, uint32_t group,
                              int *out, int cap);
// map 预览（源 previewMap）：最多 34 条，返回数量。
int summer_vm_preview_map(summer_vm_t *vm, const summer_pack_t *pack, uint32_t group,
                          summer_map_choice_t *out, int cap);
// strings[106] 的规范日期显示转换（全角数字→ASCII，只接受完整 月/日 尾）。
// 供 sceneLabel 与宿主回归共用；out 必须 NUL 结尾，cap 不足时返回 false。
bool summer_date_to_ascii(const char *raw, char *out, size_t cap);
// 章节/日期标签（源 sceneLabel）。返回静态缓冲，调用方应立即复制。
const char *summer_vm_scene_label(const summer_vm_t *vm, const summer_pack_t *pack);

// 周目进度应用/回写。
void summer_progress_init(summer_progress_t *progress);
void summer_progress_apply(summer_vm_t *vm, const summer_pack_t *pack,
                           const summer_progress_t *progress);
bool summer_progress_sync(summer_vm_t *vm, const summer_pack_t *pack,
                          summer_progress_t *progress);
bool summer_progress_merge(summer_progress_t *progress, uint16_t cleared,
                           const uint8_t *seen_cg, uint16_t seen_len);
int summer_progress_heroine_count(const summer_progress_t *progress);
void summer_progress_reconcile(summer_progress_t *progress);
unsigned summer_progress_title_actions(const summer_progress_t *progress);
bool summer_progress_has_pocket(const summer_progress_t *progress);
bool summer_progress_has_alka(const summer_progress_t *progress);
bool summer_progress_cg_seen(const summer_progress_t *progress, int group);
bool summer_progress_set_cg(summer_progress_t *progress, int group);
bool summer_progress_encode(const summer_progress_t *progress, uint8_t *out, size_t cap,
                            size_t *len);
bool summer_progress_decode(summer_progress_t *progress, const uint8_t *data, size_t len);
// 旧档合并：只加不删，返回是否有变化。
bool summer_progress_merge_old(summer_progress_t *progress, uint16_t cleared,
                               const uint8_t *seen_cg, uint16_t seen_len);

// 结束：flow 场景的 end op 下标 -> 结局 id；0 = 不是 flow 结局。
int summer_vm_ending(const summer_vm_t *vm, const summer_pack_t *pack);

// ---- 存档
size_t summer_save_encode(const summer_vm_t *vm, uint8_t page, const summer_progress_t *progress,
                          uint8_t *out, size_t cap);
bool summer_save_decode(const summer_pack_t *pack, const uint8_t *data, size_t len,
                        summer_save_t *out, char *err, size_t err_cap);
bool summer_save_to_vm(const summer_save_t *save, summer_vm_t *vm,
                       const summer_pack_t *pack);
// 从存档次下标/正文 op 生成预览（不解码整包）：用于读档列表。
bool summer_save_preview(const summer_pack_t *pack, const summer_save_t *save, char *out,
                         size_t cap);

// CRC32（IEEE，和 zlib 一致）。
uint32_t summer_crc32(const uint8_t *data, size_t len);
