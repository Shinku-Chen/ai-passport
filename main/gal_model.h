// main/gal_model.h —— 剧情虚拟机(纯逻辑,可在宿主机测试)。
//
// 一个状态 = 事件下标 + 变量 + 当前画面状态(背景/CG/三个立绘槽位)。
// 事件流里 BG/CG/SPRITE/VAR/GOTO 都是"不打断阅读"的指令,遇到 LINE/CHOICE/
// CHAPTER/ENDING/END 才停下来交给界面。
//
// 跳转(包括 3 处回跳)全部是 EV_GOTO 运行时执行,脚本在打包期就已展平成事件流,
// 不做有限展开,也不跳过任何分支。
#pragma once

#include "gal_pack.h"

#include <stdbool.h>
#include <stdint.h>

#define GAL_UNLOCK_CG_BYTES 5      // 34 张 CG/SD
#define GAL_UNLOCK_CH_BYTES 3      // 18 章
#define GAL_UNLOCK_END_BYTES 1     // 4 个结局

typedef struct {
    uint8_t cg[GAL_UNLOCK_CG_BYTES];
    uint8_t chapter[GAL_UNLOCK_CH_BYTES];
    uint8_t ending[GAL_UNLOCK_END_BYTES];
} gal_unlock_t;

typedef struct {
    uint16_t pc;            // 下一个要执行的事件
    uint16_t chapter;       // 当前章节(章节事件设置)
    uint16_t bg;            // 当前背景图 id(GAL_NONE = 黑屏)
    uint16_t cg;            // 当前 CG id(GAL_NONE = 显示背景)
    uint16_t spr[3];        // 三个立绘槽位(左/中/右)
    int16_t vars[GAL_MAX_VARS];
} gal_play_t;

typedef enum {
    GAL_STEP_LINE = 0,
    GAL_STEP_CHOICE,
    GAL_STEP_CHAPTER,
    GAL_STEP_ENDING,
    GAL_STEP_END,
    GAL_STEP_ERROR,
    GAL_STEP_UNLOCK_END,   // 只解锁结局(不弹结局画面),上层解锁后继续
} gal_step_kind_t;

typedef struct {
    gal_step_kind_t kind;
    uint16_t index;         // LINE/CHOICE/CHAPTER/ENDING 的下标
} gal_step_t;

void gal_play_reset(gal_play_t *play);

// 执行到下一个"停点"。每次前进最多执行 guard 条指令,防止数据异常导致死循环。
gal_step_t gal_play_next(gal_play_t *play, const gal_pack_t *pack);

// 在当前选项组里按【组内序号】(0..opt_count-1)选择;成功返回 true。
// 调用方随后继续 gal_play_next。
bool gal_play_choose(gal_play_t *play, const gal_pack_t *pack, int index);

// 界面上"可见选项"的全局下标列表(已按条件过滤),返回数量。
int gal_choice_visible(const gal_pack_t *pack, const gal_play_t *play, uint16_t group,
                       uint16_t *out, int cap);

// 按【界面可见序号】选择:换算成组内序号后调用 gal_play_choose。
// 这是 UI 唯一应该调用的入口,避免把全局 options 下标当成组内下标。
bool gal_choice_pick(gal_play_t *play, const gal_pack_t *pack, uint16_t group,
                     int visible_index);

// 读取当前停点上可显示的台词(仅 GAL_STEP_LINE 有效)。
const char *gal_play_line(const gal_play_t *play, const gal_pack_t *pack, uint16_t *speaker);

// 条件求值(选项过滤用):cond_id == GAL_NONE 视为成立。
bool gal_cond_ok(const gal_play_t *play, const gal_pack_t *pack, uint16_t cond_id);

// 变量按名字取值/赋值(调试与测试用)。
int gal_var_index(const gal_pack_t *pack, const char *name);
int16_t gal_var_get(const gal_play_t *play, int index);

// ---- 解锁位图 -------------------------------------------------------------
void gal_unlock_clear(gal_unlock_t *u);
bool gal_unlock_cg_get(const gal_unlock_t *u, int index);
void gal_unlock_cg_set(gal_unlock_t *u, int index);
bool gal_unlock_chapter_get(const gal_unlock_t *u, int index);
void gal_unlock_chapter_set(gal_unlock_t *u, int index);
bool gal_unlock_ending_get(const gal_unlock_t *u, int index);
void gal_unlock_ending_set(gal_unlock_t *u, int index);
int gal_unlock_count(const uint8_t *bits, int bytes);
// dst |= src(旧存档合并进全局记录时只增不减)。
void gal_unlock_merge(gal_unlock_t *dst, const gal_unlock_t *src);
bool gal_unlock_any(const gal_unlock_t *u);

// 某个图片 id 对应的鉴赏条目下标(GAL_NONE = 不在鉴赏里)。
// 界面在真正显示 CG 时用它解锁,避免"解锁事件"与实际显示脱节。
int gal_gallery_index_for_image(const gal_pack_t *pack, uint16_t image);

// 立绘槽位 -> 画布 x 坐标(左/中/右),w 为立绘宽度。
int gal_sprite_x(int slot, int w, int screen_w);

// 把一段 UTF-8 安全截断到 max_bytes 以内(不切断多字节字符)。
void gal_utf8_truncate(char *dst, const char *src, int max_bytes);
