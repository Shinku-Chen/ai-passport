// main/anemoi/anemoi_store.h —— 存档 / 自动档 / 全局解锁的编解码与校验。
//
// 三份记录各自一个 NVS 键，互不覆盖：
//   anemoi_save_00..29  30 个手动存档
//   anemoi_auto         自动档（换场 / 回标题 / 睡前写入；独立于手动槽）
//   anemoi_progress     全局解锁（g:/z: 变量 + 见过的 CG/结局位），版本化
//
// 本层只做“记录 <-> 字节”的编解码与校验；NVS 读写走 anem_store_io_t，
// 宿主机测试注入失败/坏数据，不需要复制业务逻辑。
// 任何校验失败（版本、长度、CRC、下标越界、NUL、mode/page 非法）都返回 false
// 且不改动调用方结构体 —— 读旧档失败不会污染当前状态。
#pragma once

#include "anemoi_script.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANEM_SLOT_COUNT 30
#define ANEM_SAVE_VERSION 3u
#define ANEM_PROGRESS_VERSION 2u
#define ANEM_PAGE_MAX 64
#define ANEM_CG_BITS 128
#define ANEM_ENDING_BITS 32

typedef struct {
    bool valid;
    uint8_t mode;          // 0 = 正文，1 = 待选
    uint8_t progression;   // 0 = chapter，1 = story
    uint16_t chapter;
    uint16_t root_chapter;
    uint32_t cursor;
    uint16_t page;
    uint16_t speaker;
    uint16_t text;
    uint16_t regular_bg;
    uint16_t cg;
    uint32_t changed;
    uint8_t changed_cg;
    uint8_t body_count;
    uint16_t var_count;
    uint32_t saved_at;
    anem_body_slot_t bodies[ANEM_BODY_MAX];
    uint8_t dress[ANEM_ACTOR_MAX];
    uint16_t pose[ANEM_ACTOR_MAX];
    anem_value_t vars[ANEM_VAR_MAX];
} anem_slot_t;

typedef struct {
    uint8_t version;
    uint8_t level;
    uint8_t cg[ANEM_CG_BITS / 8];
    uint8_t ending[ANEM_ENDING_BITS / 8];
    uint16_t var_count;
    anem_value_t vars[ANEM_VAR_MAX];
} anem_progress_t;

typedef struct {
    // 读：成功写入 out 并返回 true；记录不存在也要返回 false。
    bool (*read)(const char *key, void *out, size_t size, size_t *out_size);
    // 写：nvs_set_blob 与 nvs_commit 都成功才返回 true。
    bool (*write)(const char *key, const void *data, size_t size);
} anem_store_io_t;

// 绑定存储适配器（宿主测试注入假适配器）。传 NULL 用默认 NVS 适配器。
void anem_store_bind(const anem_store_io_t *io);
const anem_store_io_t *anem_store_current_io(void);

// NVS 初始化（只读打开命名空间，绝不 nvs_flash_erase）。失败返回 false，
// 调用方降级为“本次运行内有效”，不得提示保存成功。
bool anem_store_init(void);

void anem_slot_clear(anem_slot_t *slot);
void anem_progress_clear(anem_progress_t *progress);

// 存档 <-> 状态。
void anem_slot_from_state(anem_slot_t *slot, const anem_script_t *script, uint8_t mode,
                          uint16_t page, uint32_t saved_at);
bool anem_slot_apply(const anem_slot_t *slot, anem_script_t *script);
// 槽位预览（“Day 3 · …”），返回写入长度；空槽返回 0。
size_t anem_slot_preview(const anem_slot_t *slot, const anem_pack_t *pack, char *out,
                         size_t cap);

// 全局解锁：从脚本状态提取 g:/z: 变量与等级。
void anem_progress_from_script(anem_progress_t *progress, const anem_script_t *script);
bool anem_progress_apply(const anem_progress_t *progress, anem_script_t *script);
bool anem_progress_has_cg(const anem_progress_t *progress, uint8_t index);
void anem_progress_mark_cg(anem_progress_t *progress, uint8_t index);
bool anem_progress_has_ending(const anem_progress_t *progress, uint8_t index);
void anem_progress_mark_ending(anem_progress_t *progress, uint8_t index);

// 单条记录的字节长度（测试与适配器断言布局）。
// 共享工作区（非重入）：成功返回零初始化 slot，失败（占用中）返回 NULL。
// 固定内部堆工作区（单次分配，运行期只此一份；busy 时 acquire 失败）。
bool anem_store_workspace_init(void);
bool anem_store_workspace_acquire(void);
void anem_store_workspace_release(void);
anem_slot_t *anem_store_workspace_slot(void);
anem_progress_t *anem_store_workspace_progress(void);

size_t anem_slot_blob_size(void);
size_t anem_progress_blob_size(void);

// 编解码：失败返回 false 且不写出/不改动 out。
bool anem_slot_encode(const anem_slot_t *slot, uint8_t *buf, size_t size);
bool anem_slot_decode(const uint8_t *buf, size_t size, anem_slot_t *slot);
bool anem_progress_encode(const anem_progress_t *progress, uint8_t *buf, size_t size);
bool anem_progress_decode(const uint8_t *buf, size_t size, anem_progress_t *progress);

// 槽位读写；返回 false 表示没读到/没写进去。
bool anem_store_load_slot(uint8_t index, anem_slot_t *slot);
bool anem_store_save_slot(uint8_t index, const anem_slot_t *slot);
bool anem_store_load_auto(anem_slot_t *slot);
bool anem_store_save_auto(const anem_slot_t *slot);
bool anem_store_load_progress(anem_progress_t *progress);
bool anem_store_save_progress(const anem_progress_t *progress);

#ifdef __cplusplus
}
#endif
