// Exact bounded graph semantics of the pinned 9-nine- Watch graph, not the
// original PC VM. State storage is sized by the pack's declared variable count
// (15 here, up to 128), so a title with many flags is not squeezed into a fixed
// 8/32-bit mask. All graph hops are capped at 500 and history at 24.
#pragma once
#include "nine_pack.h"

#define NINE_HISTORY_MAX 24u
#define NINE_SAVE_CAPACITY 1120u

typedef enum { NINE_VM_ERROR = 0, NINE_VM_TEXT, NINE_VM_OP,
               NINE_VM_CHOICE, NINE_VM_END } nine_vm_kind_t;
typedef struct {
    uint16_t node;
    uint32_t index;
    bool preview;
    int32_t *flag, *progress;
    uint8_t *flag_set, *progress_set;
} nine_state_t;
// A fixed-size rollback copy (one checkpoint, not 24 history entries).
typedef struct {
    uint16_t node;
    uint32_t index;
    bool preview;
    uint8_t history_count;
    int32_t flag[NINE_KEY_MAX], progress[NINE_KEY_MAX];
    uint8_t flag_set[NINE_KEY_MAX / 8], progress_set[NINE_KEY_MAX / 8];
} nine_checkpoint_t;
typedef struct {
    nine_vm_kind_t kind;
    uint16_t node;
    uint32_t index, frame;
    uint8_t choice_count;
    uint32_t choices[NINE_CHOICE_MAX]; // edge indices, after exact filtering/fallback
    const char *error;
} nine_view_t;
typedef struct {
    nine_pack_t *pack;
    uint8_t *arena;
    size_t stride;
    nine_state_t state;
    nine_state_t history[NINE_HISTORY_MAX];
    uint8_t history_count;
    nine_view_t view;
} nine_vm_t;

bool nine_vm_init(nine_vm_t *vm, nine_pack_t *pack);
void nine_vm_deinit(nine_vm_t *vm);
bool nine_vm_start(nine_vm_t *vm, uint16_t node, bool preview);
// Start a fresh reading session at an arbitrary frame (chapter / CG recall).
// clear_flags follows the source create() semantics for a new session.
bool nine_vm_seek(nine_vm_t *vm, uint16_t node, uint32_t index, bool preview,
                  bool clear_flags);
void nine_vm_checkpoint(const nine_vm_t *vm, nine_checkpoint_t *out);
void nine_vm_restore(nine_vm_t *vm, const nine_checkpoint_t *in);
const nine_view_t *nine_vm_locate(nine_vm_t *vm);
const nine_view_t *nine_vm_advance(nine_vm_t *vm);
const nine_view_t *nine_vm_choose(nine_vm_t *vm, uint32_t visible_index);
const nine_view_t *nine_vm_previous(nine_vm_t *vm);
const nine_view_t *nine_vm_skip_chapter(nine_vm_t *vm);
bool nine_vm_current_frame(const nine_vm_t *vm, nine_frame_t *out);
int32_t nine_vm_condition_value(const nine_vm_t *vm, uint16_t key);
bool nine_vm_matches(const nine_vm_t *vm, uint32_t first, uint32_t count);
// Version/episode/pack identity + all run flags and progress + CRC32 + page.
size_t nine_vm_save_encode(const nine_vm_t *vm, uint16_t page, uint8_t *out, size_t cap);
bool nine_vm_save_decode(nine_vm_t *vm, const uint8_t *data, size_t size, uint16_t *page);
bool nine_vm_save_peek(const nine_pack_t *pack, const uint8_t *data, size_t size,
                       uint16_t *node, uint32_t *index, uint16_t *page, bool *preview);
// Permanent progress is a separate record; manual/auto state never rolls it back.
size_t nine_vm_progress_encode(const nine_vm_t *vm, uint8_t *out, size_t cap);
bool nine_vm_progress_decode(nine_vm_t *vm, const uint8_t *data, size_t size);
