// main/gal/gal_unlock.h —— 剧情锁:观看记录与剧情标记(NVS 持久化)。
//
// 三类解锁状态都存成位掩码,跨存档生效:
//   cg       看过 / 到过解锁点的 CG
//   chapter  已解锁的章节(可能有额外 flag 条件)
//   flag     剧情标记(选项写入、过场与结局自动写入),内容层按它决定
//            台词是否出现、选项是否可选、章节是否解锁
//   ending   看过的结局
#pragma once

#include <stdbool.h>
#include <stdint.h>

// 不绑定任何解锁事件 / 不需要任何标记。
#define GAL_UNLOCK_NONE 0xFF

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
bool gal_unlock_has_flag(const gal_unlock_t *in, uint8_t index);
