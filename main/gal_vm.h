// main/gal_vm.h —— 《魔法少女的魔女审判》剧情虚拟机。
//
// 直接解释源 engine.js / scene.js 的指令语义:背景层、立绘实例、焦点、条件、
// call/return、offer/choice 菜单、地图调查、证据提交、反驳词、审判伤害、手册
// 版本、自动存档点。纯 C,不依赖 ESP-IDF/LVGL,可在宿主机上跑完整路线审计。
#pragma once

#include "gal_expr.h"
#include "gal_pack.h"

#include <stdbool.h>
#include <stdint.h>

#define GAL_MAX_SPRITES 96   // source paths can exceed 33; 96 avoids silent loss
#define GAL_MAX_LAYERS 10
#define GAL_MAX_STACK 32
#define GAL_MAX_PENDING 8
#define GAL_MAX_ROWS 32
#define GAL_MAX_BOOK 192
#define GAL_MAX_BODY 3

// 选项行类型
enum {
    GAL_ROW_ITEM = 0,     // 脚本 choice 选项
    GAL_ROW_BRANCH = 1,   // offer 收集的分支
    GAL_ROW_MAP = 2,      // 调查地点
    GAL_ROW_EVIDENCE = 3, // 提交证据
    GAL_ROW_CONTINUE = 4, // 处刑确认 / 点击继续
};

typedef struct {
    uint32_t text_off;    // 文本池偏移(当 text_is_str=0)
    uint16_t text_len;
    uint16_t text_str;    // 字符串表 id(当 text_is_str=1)
    uint16_t to;
    uint16_t end;
    uint16_t value;       // 地图区域下标 / 手册键下标
    uint16_t source_index;
    uint8_t kind;
    uint8_t branch;
    uint8_t visited;
    uint8_t text_is_str;
} gal_row_t;

typedef struct {
    uint16_t actor;
    uint16_t sub;
    uint16_t image;
    int16_t x;
    int16_t y;
    int16_t world_y;
    int16_t scale100;
    uint16_t order;
    uint8_t visible;
    uint8_t offscreen;
    uint8_t used;
} gal_sprite_t;

typedef struct {
    uint16_t actor;
    uint16_t sub;
    uint16_t image;
    uint16_t order;
    uint8_t visible;
    uint8_t used;
} gal_layer_t;

typedef struct {
    uint16_t text_str;    // offer 标签
    uint16_t to;
    uint16_t end;
    uint16_t menu;
    uint8_t used;
} gal_pending_t;

typedef enum {
    GAL_FRAME_TEXT = 0,
    GAL_FRAME_CHOICE,
    GAL_FRAME_END,
    GAL_FRAME_YIELD,
    GAL_FRAME_ERROR,
} gal_frame_type_t;

typedef struct {
    gal_frame_type_t type;
    uint16_t speaker;
    uint16_t actor;
    uint32_t text_off;
    uint16_t text_len;
    uint16_t link_first;
    uint16_t link_count;
    uint8_t debate;
    uint8_t choice_kind;      // GAL_ROW_*: ITEM/BRANCH/MAP/EVIDENCE/CONTINUE
    uint8_t row_first;
    uint8_t row_count;
    uint16_t end_kind;
    const char *error;
} gal_frame_t;

typedef struct {
    const gal_pack_t *pack;
    uint16_t chapter;          // 当前场景下标
    uint16_t root;             // 最近的非 System 场景
    uint16_t cursor;           // 当前场景内指令下标
    uint16_t focus;
    uint16_t speaker;
    uint16_t text_actor;
    uint32_t text_off;
    uint16_t text_len;
    uint16_t link_first;
    uint16_t link_count;
    uint8_t debate;
    uint16_t end_kind;
    uint8_t ended;
    uint8_t trial_fail;
    uint8_t trial;
    uint8_t initial_book;
    uint8_t debug_direct;
    uint8_t progression_story;
    uint8_t legacy_visuals;
    uint8_t autosave_due;
    uint8_t choice_block;
    uint16_t block_end;
    uint16_t block_continuation;
    uint16_t clue_image;
    uint16_t clue_background;
    uint16_t background;
    uint16_t regular_background;
    gal_sprite_t sprites[GAL_MAX_SPRITES];
    // sceneBodies scratch: keeps the 40-entry sort workspace out of the
    // already-tight call stack; not persisted in save files.
    gal_sprite_t scene_entries[GAL_MAX_SPRITES];
    uint8_t sprite_count;
    gal_sprite_t bodies[GAL_MAX_BODY];
    uint8_t body_count;
    gal_layer_t layers[GAL_MAX_LAYERS];
    uint8_t layer_count;
    uint16_t serial;
    gal_val_t vars[GAL_MAX_VARS];
    uint16_t var_count;
    gal_expr_ctx_t expr;
    uint16_t stack[GAL_MAX_STACK][2];
    uint8_t stack_count;
    gal_pending_t pending[GAL_MAX_PENDING];
    uint8_t pending_count;
    uint8_t book[GAL_MAX_BOOK];
    uint8_t book_order[GAL_MAX_BOOK];
    uint16_t book_count;
    uint16_t choices_made;
    // 最近一次交互选项缓存(源 activeChoices)
    uint16_t active_chapter;
    uint16_t active_cursor;
    uint8_t active_valid;
    gal_row_t active_rows[GAL_MAX_ROWS];
    uint8_t active_count;
    // 交互行
    gal_row_t rows[GAL_MAX_ROWS];
    uint8_t row_count;
    // 全局解锁(由本 VM 观察到的内容;由外部合并持久化)
    uint8_t unlock_gallery[22];   // 172 张 CG
    uint8_t unlock_chapter[47];   // 371 个章节入口
    uint16_t scene_epoch;         // 每次换场景 +1
    const char *error;
} gal_vm_t;

void gal_vm_init(gal_vm_t *vm, const gal_pack_t *pack);

// 源 newState(chapter, story):story=true 为新游戏,false 为章节直达。
bool gal_vm_start(gal_vm_t *vm, uint16_t chapter, bool story, bool legacy_visuals,
                  const char **err);

// 从当前 cursor 执行到下一个可见帧。调用方在 YIELD 时继续调用(有界预算)。
gal_frame_type_t gal_vm_read(gal_vm_t *vm, gal_frame_t *frame);

// 正文推进(源 next):cursor++。清空 links。
void gal_vm_advance(gal_vm_t *vm);

// 交互选择。row 为 frame 中 rows 的下标。
bool gal_vm_choose(gal_vm_t *vm, uint8_t row, const char **err);

// 反驳词(源 objectTo)。
bool gal_vm_object_to(gal_vm_t *vm, uint8_t link, const char **err);

// 当前文本/字符串辅助。
const char *gal_vm_frame_text(const gal_vm_t *vm, const gal_frame_t *frame,
                              char *buf, size_t cap);
const char *gal_vm_row_text(const gal_vm_t *vm, const gal_row_t *row, char *buf, size_t cap);

// 手册/地图/鉴赏辅助。
uint16_t gal_vm_book_key_count(const gal_vm_t *vm);
uint8_t gal_vm_book_version(const gal_vm_t *vm, uint16_t key);
const gal_book_t *gal_vm_book_entry(const gal_vm_t *vm, uint16_t key);
const gal_area_t *gal_vm_map_area(const gal_vm_t *vm, uint16_t op_a, uint16_t index);

// 解锁位。
bool gal_vm_gallery_unlocked(const gal_vm_t *vm, uint16_t item);
bool gal_vm_chapter_unlocked(const gal_vm_t *vm, uint16_t chapter_entry);
void gal_vm_merge_unlock(gal_vm_t *vm, const uint8_t *gallery, const uint8_t *chapters);

// 变量便捷访问(用于 HP/进度/调试显示)。
int32_t gal_vm_var_int(const gal_vm_t *vm, const char *name);
uint16_t gal_vm_var_id(const gal_vm_t *vm, const char *name);
