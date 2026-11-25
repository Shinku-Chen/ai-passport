// Exact bounded graph semantics from the Watch reader (src/common/engine.js),
// independent of ESP-IDF and LVGL. This is an interpreter for the already
// converted graph, not a reimplementation of the original PC script VM.
#pragma once
#include "nine_pack.h"

#define NINE_HISTORY_MAX 24u

typedef enum { NINE_VM_ERROR = 0, NINE_VM_TEXT, NINE_VM_OP,
               NINE_VM_CHOICE, NINE_VM_END } nine_vm_kind_t;

typedef struct {
    uint16_t node;
    uint32_t index;
    bool preview;
    int32_t flag[NINE_KEY_MAX], progress[NINE_KEY_MAX];
    uint8_t flag_set[NINE_KEY_MAX / 8], progress_set[NINE_KEY_MAX / 8];
} nine_state_t;

typedef struct {
    nine_vm_kind_t kind;
    uint16_t node;
    uint32_t index, frame;
    uint8_t choice_count;
    uint32_t choices[NINE_CHOICE_MAX];   // edge indices after exact filtering/fallback
    const char *error;
} nine_view_t;

// A checkpoint is packed to (only) the variables that were set, so a 72-variable
// episode does not spend a kilobyte per history entry. Storage is allocated at
// init from key_count and bounded by NINE_HISTORY_MAX.
typedef struct {
    nine_pack_t *pack;
    nine_state_t state;
    nine_view_t view;
    uint8_t *history;
    size_t history_entry;
    uint8_t history_count;
    bool progress_dirty;   // a persistent effect changed (for throttled NVS write)
} nine_vm_t;

bool nine_vm_init(nine_vm_t *vm, nine_pack_t *pack);
void nine_vm_deinit(nine_vm_t *vm);
// node=NINE_NODE_NONE means graph.start. New game clears run flags, retains the
// permanent progress map (loop_my/clear_true/titleBg/ending ids).
bool nine_vm_start(nine_vm_t *vm, uint16_t node, bool preview);
const nine_view_t *nine_vm_locate(nine_vm_t *vm);
const nine_view_t *nine_vm_advance(nine_vm_t *vm);
const nine_view_t *nine_vm_choose(nine_vm_t *vm, uint32_t visible_index);
const nine_view_t *nine_vm_previous(nine_vm_t *vm);
const nine_view_t *nine_vm_skip_chapter(nine_vm_t *vm);
bool nine_vm_current_frame(const nine_vm_t *vm, nine_frame_t *out);
int32_t nine_vm_condition_value(const nine_vm_t *vm, uint16_t key);
bool nine_vm_matches(const nine_vm_t *vm, uint32_t first, uint32_t count);
// Display helpers (read-only).
int32_t nine_vm_progress_value(const nine_vm_t *vm, uint16_t key);
bool nine_vm_ending_reached(const nine_vm_t *vm, const char *id);
bool nine_vm_progress_dirty(const nine_vm_t *vm);
void nine_vm_clear_progress_dirty(nine_vm_t *vm);

// Version/episode/pack identity + all run flags and progress + CRC32 + page.
// A save never rolls back the newest permanent progress: decoding overlays the
// live progress map unless the saved record is an isolated chapter preview.
size_t nine_vm_save_encode(const nine_vm_t *vm, uint16_t page, uint8_t *out, size_t cap);
bool nine_vm_save_decode(nine_vm_t *vm, const uint8_t *data, size_t size, uint16_t *page);
bool nine_vm_save_peek(const nine_pack_t *pack, const uint8_t *data, size_t size,
                       uint16_t *node, uint32_t *index, uint16_t *page, bool *preview);
// Permanent progress is a separate record; manual/auto state never rolls it back.
size_t nine_vm_progress_encode(const nine_vm_t *vm, uint8_t *out, size_t cap);
bool nine_vm_progress_decode(nine_vm_t *vm, const uint8_t *data, size_t size);
