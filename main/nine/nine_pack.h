// Read-only, bounded 9-nine pack access. Binary layout is nine_format.h.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "nine_format.h"

#define NINE_KEY_MAX 128u
#define NINE_NODE_NONE 0xffffu
#define NINE_TEXT_MAX 2048u
#define NINE_TEXT_BLOCK_MAX 4096u
#define NINE_CHOICE_MAX 16u

typedef struct {
    uint32_t name, title, chapter;
    uint32_t first_frame, frame_count;
    uint32_t first_effect, effect_count;
    uint32_t first_choice, choice_count;
    uint32_t first_next, next_count;
} nine_node_t;
typedef struct {
    uint16_t key;
    uint8_t op; // NINE_COND_* RPN tokens
    int32_t value;
} nine_condition_t;
typedef struct { uint16_t key; uint8_t add; int32_t value; } nine_effect_t;
typedef struct {
    uint16_t to;
    uint32_t label, first_condition, condition_count, first_effect, effect_count;
} nine_edge_t;
typedef struct {
    uint8_t kind; // 0 text, 1 opening movie
    uint32_t visual, speaker;
    uint32_t text_block;
    uint32_t text_offset;
    uint16_t text_length;
} nine_frame_t;
typedef struct { uint32_t title, preview; uint16_t node; } nine_chapter_t;
typedef struct { uint16_t key; uint32_t name, description; } nine_ending_t;

typedef struct {
    const uint8_t *data;
    uint32_t size, episode, identity;
    uint32_t node_count, frame_count, condition_count, effect_count;
    uint32_t choice_count, next_count, key_count;
    uint32_t chapter_count, ending_count, gallery_count, visual_count;
    uint16_t start, loop_key, clear_key;
    bool filter_choices;
    // Private section offsets. Public for a static, allocation-free handle.
    uint32_t offsets[24], sizes[24], counts[24];
    uint8_t text_cache[NINE_TEXT_BLOCK_MAX];
    uint32_t cached_block;
} nine_pack_t;

bool nine_pack_open(nine_pack_t *pack, const uint8_t *data, size_t size);
const char *nine_pack_string(const nine_pack_t *pack, uint32_t offset);
const char *nine_pack_title(const nine_pack_t *pack);
const char *nine_pack_key(const nine_pack_t *pack, uint16_t key);
bool nine_pack_key_persistent(const nine_pack_t *pack, uint16_t key);
int nine_pack_find_key(const nine_pack_t *pack, const char *name);
int nine_pack_find_node(const nine_pack_t *pack, const char *name);
bool nine_pack_node(const nine_pack_t *pack, uint16_t index, nine_node_t *out);
bool nine_pack_frame(const nine_pack_t *pack, uint32_t index, nine_frame_t *out);
// Text points into one bounded cache, invalidated by the next text call.
const char *nine_pack_text(nine_pack_t *pack, const nine_frame_t *frame);
bool nine_pack_condition(const nine_pack_t *pack, uint32_t index, nine_condition_t *out);
bool nine_pack_effect(const nine_pack_t *pack, uint32_t index, nine_effect_t *out);
bool nine_pack_edge(const nine_pack_t *pack, bool choice, uint32_t index, nine_edge_t *out);
bool nine_pack_chapter(const nine_pack_t *pack, uint32_t index, nine_chapter_t *out);
bool nine_pack_ending(const nine_pack_t *pack, uint32_t index, nine_ending_t *out);
bool nine_pack_visual(const nine_pack_t *pack, uint32_t index, nine_visual_record_t *out);
bool nine_pack_layer(const nine_pack_t *pack, uint32_t index, nine_layer_record_t *out);
bool nine_pack_gallery(const nine_pack_t *pack, uint32_t index, nine_gallery_record_t *out);
uint32_t nine_pack_gallery_count(const nine_pack_t *pack);
bool nine_pack_visual_contains(const nine_pack_t *pack, uint32_t visual, uint32_t image);

// Shared serialized inflate workspace, callers must serialize on the UI task.
size_t nine_inflate(void *dst, size_t cap, const uint8_t *src, size_t size);
