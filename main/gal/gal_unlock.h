// main/gal/gal_unlock.h —— 永久解锁:CG / 章节 / 结局(NVS 持久化)。
//
// 永久解锁状态存成位掩码,跨存档生效:
//   cg       看过 / 到过解锁点的 CG
//   chapter  已解锁的章节
//   ending   看过的结局
// flag 是旧版全局故事标记的兼容字段；新代码把周目标记放在
// gal_app.flags 并随 galsave/galprog 保存，不再读写这里。
#pragma once

#include <stdbool.h>
#include <stdint.h>

// 以下 flag 位是旧版全局故事标记的兼容字段；新代码把周目 flag 存在
// gal_app/gal_save 中，不再读写这里。cg/chapter/ending 仍为永久解锁。

// 字段缺省值:
//   flag 相关字段是 uint8(最多 32 个标记) -> GAL_UNLOCK_NONE
//   事件 / 行 / 场景下标是 uint16(最多 65535) -> GAL_EVENT_NONE
#define GAL_UNLOCK_NONE 0xFF
#define GAL_EVENT_NONE 0xFFFF

typedef struct {
    uint32_t cg;
    uint32_t chapter;
    uint32_t flag;
    uint32_t ending;
} gal_unlock_t;

// NVS 准备(失败时解锁状态只在本次运行内有效)。
bool gal_unlock_init(void);

// 读取解锁状态;没有记录时清空并返回 false。
bool gal_unlock_load(gal_unlock_t *out);

// 写回解锁状态。
bool gal_unlock_store(const gal_unlock_t *out);

// 清空内存中的解锁状态。
void gal_unlock_clear(gal_unlock_t *out);

// 授予(幂等)。
void gal_unlock_grant_cg(gal_unlock_t *out, uint8_t index);
void gal_unlock_grant_chapter(gal_unlock_t *out, uint8_t index);
void gal_unlock_grant_ending(gal_unlock_t *out, uint8_t index);
void gal_unlock_set_flag(gal_unlock_t *out, uint8_t index);

// 查询。
bool gal_unlock_has_cg(const gal_unlock_t *in, uint8_t index);
bool gal_unlock_has_chapter(const gal_unlock_t *in, uint8_t index);
bool gal_unlock_has_ending(const gal_unlock_t *in, uint8_t index);
bool gal_unlock_has_any_ending(const gal_unlock_t *in);
bool gal_unlock_has_flag(const gal_unlock_t *in, uint8_t index);

// 周目 flag：显式位操作，供 gal_app / 存档 / 自动进度共用。
bool gal_flags_has(uint32_t flags, uint8_t index);
uint32_t gal_flags_set(uint32_t flags, uint8_t index);
