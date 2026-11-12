// main/clannad_vm.h —— 纯逻辑：RW5 虚拟机、变量/字符串、存档与通关记录编解码。
//
// 这一层不依赖 ESP-IDF / LVGL：宿主机测试直接编译，生产固件也用它。
// 语义严格对照源快照的 src/common/engine.js（详见 tools/clannad_audit.py 的
// 独立解释器与 docs/reference/shinku-chen/clannad/content.md）。
#pragma once

#include "clannad_pack.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Worst observed source state is 195 live variables. 512 power-of-two slots
// keep the 3/4 probe ceiling at 384 while preserving mask-based hashing.
#define CLAN_VM_VAR_SLOTS 512
#define CLAN_VM_STRING_POOL 4096
#define CLAN_VM_STACK_MAX 24
#define CLAN_VM_COUNTERS 8
#define CLAN_VM_CHOICES 8
#define CLAN_VM_CHOICE_TEXT 224
#define CLAN_VM_PREVIEW 96
#define CLAN_VM_NAME 64
// The 195-variable capacity witness needs 1,415 raw bytes. 3 KiB leaves a
// bounded margin for longer strings; snapshots use the same compiled bound.
#define CLAN_VM_SAVE_RAW_MAX 3072
#define CLAN_VM_SAVE_PACKED_MAX 2048
// 128 g[] entries * 6 bytes + 12-byte header = 780 bytes. 1 KiB is the
// explicit codec bound; the observed 67-entry true-ending blob is 414 bytes.
#define CLAN_VM_PROGRESS_MAX 1024
#define CLAN_VM_PROGRESS_ENTRIES_MAX 128
#define CLAN_VM_ERROR 96

enum { CLAN_MODE_STORY = 0, CLAN_MODE_CHAPTER = 1 };
enum { CLAN_FRAME_TEXT = 0, CLAN_FRAME_CHOICE = 1, CLAN_FRAME_END = 2,
       CLAN_FRAME_ERROR = 3 };

typedef struct {
    uint8_t type;
    const char *speaker;
    const char *text;
    uint8_t choice_count;
    const char *choice_text[CLAN_VM_CHOICES];
    int32_t choice_value[CLAN_VM_CHOICES];
} clannad_frame_t;

typedef struct {
    uint8_t mode;
    uint8_t flags;          // bit0: save was taken while a choice was pending
    uint8_t body_count;
    uint8_t stack_depth;
    uint16_t chapter;
    uint16_t root;
    uint16_t cursor;
    uint16_t page;
    uint16_t choice_sel;
    uint16_t background;
    uint16_t bodies[3];
    uint32_t saved_at;
    char preview[CLAN_VM_PREVIEW];
    char chapter_name[CLAN_VM_NAME];
} clannad_save_info_t;

typedef struct {
    uint32_t key;           // (kind << 20 | index) + 1; 0 = empty
    uint8_t type;           // 0 = int, 1 = string
    uint8_t pad[3];
    int32_t value;
    uint16_t str_off;
    uint16_t str_len;
} clannad_var_t;

typedef struct {
    clannad_pack_t pack;
    uint8_t *raw;
    uint32_t raw_cap;
    uint16_t loaded_block;
    bool block_loaded;
    uint16_t block_ops;
    uint16_t block_text_size;
    uint16_t block_extra_size;

    uint16_t chapter;
    uint16_t root;
    uint16_t cursor;
    uint16_t background;
    uint16_t bodies[3];
    uint8_t body_count;
    uint8_t mode;
    bool ended;
    uint8_t stack_depth;
    struct {
        uint16_t chapter;
        uint16_t cursor;
    } stack[CLAN_VM_STACK_MAX];

    clannad_var_t vars[CLAN_VM_VAR_SLOTS];
    uint16_t var_count;
    char strings[CLAN_VM_STRING_POOL];
    uint16_t string_used;
    struct {
        int32_t value;
        uint8_t ticking;
    } counters[CLAN_VM_COUNTERS];

    char choice_buf[CLAN_VM_CHOICES][CLAN_VM_CHOICE_TEXT];
    char error[CLAN_VM_ERROR];
} clannad_vm_t;

// 打开并校验包；raw 是调用方提供的块解压缓冲（>= CLAN_VM_RAW_CAP）。
bool clannad_vm_init(clannad_vm_t *vm, const uint8_t *pack_data, uint32_t pack_size,
                     uint8_t *raw, uint32_t raw_cap, char *why, size_t cap);

// 新游戏 / 章节入口。mode 决定是否写全局通关记录（见 app）。
bool clannad_vm_new(clannad_vm_t *vm, uint16_t scene_id, uint8_t mode);

// 解释到下一个 text / choice / end；内部处理跳转与 return。
clannad_frame_t clannad_vm_run(clannad_vm_t *vm);
// 当前 text 或 choice 已消费，前进一条 op。
bool clannad_vm_next(clannad_vm_t *vm);
bool clannad_vm_choose(clannad_vm_t *vm, uint8_t index);

// 变量访问（g[] 通关记录 / 测试）。
bool clannad_vm_var_int(const clannad_vm_t *vm, uint8_t kind, uint32_t index, int32_t *out);
bool clannad_vm_var_set_int(clannad_vm_t *vm, uint8_t kind, uint32_t index, int32_t value);

// 存档：raw 编码 -> 固定头 + 稀疏变量 + 计数器 + 调用栈 + 预览 + CRC32。
size_t clannad_vm_save_encode(const clannad_vm_t *vm, uint16_t page, uint16_t choice_sel,
                              uint32_t saved_at, const char *chapter_name,
                              uint8_t *out, size_t cap);
// 完整校验（不改 VM）。成功后 info 可用于列表显示。
bool clannad_vm_save_validate(clannad_vm_t *vm, const uint8_t *blob, size_t len,
                              clannad_save_info_t *info);
// 应用已校验存档；失败不改状态（返回 false 时状态保持原样）。
bool clannad_vm_save_apply(clannad_vm_t *vm, const uint8_t *blob, size_t len);

// 通关记录（只存 g:*）。apply 用全局值覆盖当前状态；encode 从当前状态导出。
size_t clannad_vm_progress_encode(const clannad_vm_t *vm, uint8_t *out, size_t cap);
bool clannad_vm_progress_validate(const uint8_t *blob, size_t len);
bool clannad_vm_progress_apply(clannad_vm_t *vm, const uint8_t *blob, size_t len);
int32_t clannad_progress_light_count(const uint8_t *blob, size_t len);
bool clannad_progress_after_unlocked(const uint8_t *blob, size_t len);
bool clannad_progress_true_ending(const uint8_t *blob, size_t len);

// 当前章节名（rootChapter 的显示名）。
const char *clannad_vm_chapter_name(const clannad_vm_t *vm);
const char *clannad_vm_scene_source(const clannad_vm_t *vm);

// 调试/审计用：当前块指针。
const uint8_t *clannad_vm_raw(const clannad_vm_t *vm);
