// RW5 nine pack v1. All integers are unsigned little-endian unless noted.
// Offsets are absolute file offsets. String references are byte offsets in the
// script string pool, UTF-8 NUL terminated. No file contains native pointers.
#pragma once
#include <stdint.h>
#define NINE_FORMAT_VERSION 1u
#define NINE_SCRIPT_MAGIC "NINESC1\0"
#define NINE_IMAGE_MAGIC "NINEIM1\0"
#define NINE_NONE UINT32_C(0xffffffff)
#define NINE_MAX_VARS 96u
#define NINE_TEXT_BLOCK_MAX 8192u
#define NINE_ART_WIDTH 320u
#define NINE_ART_HEIGHT 180u
#define NINE_IMAGE_BLOCK_ROWS 8u
#define NINE_VAR_PERSISTENT 1u
#define NINE_FRAME_TEXT 0u
#define NINE_FRAME_OP 1u
#define NINE_COND_TRUE 0u
#define NINE_COND_EQ 1u
#define NINE_COND_NE 2u
#define NINE_COND_LT 3u
#define NINE_COND_LE 4u
#define NINE_COND_GT 5u
#define NINE_COND_GE 6u
// RPN: AND consumes the previous value operands (value is operand count).
#define NINE_COND_AND 7u
#define NINE_EFFECT_SET 0u
#define NINE_EFFECT_ADD 1u
#define NINE_IMG_JPEG 0u
#define NINE_IMG_INDEXED 1u
#define NINE_AUDIO_OGG_OPUS 0u
#define NINE_AUDIO_IMA_ADPCM 1u
// Mono IMA blocks: 256 samples/block. Each 132-byte block starts with LE
// int16 predictor (sample 0), uint8 step-index, uint8 zero, then 128 bytes
// of sample 1..255 nibbles (low nibble first; last high nibble padding).
// Final block pads repeated last sample. True playback duration uses duration_ms.
// CRC32 is over the entire file with this crc32 member's four bytes zeroed.
typedef struct {
 uint8_t magic[8];
 uint32_t version, file_size, crc32;
 uint32_t episode_str, title_str, source_sha_str, start_node;
 uint32_t node_count, nodes_offset;
 uint32_t frame_count, frames_offset;
 uint32_t block_count, blocks_offset;
 uint32_t visual_count, visuals_offset;
 uint32_t layer_count, layers_offset;
 uint32_t var_count, vars_offset;
 uint32_t condition_count, conditions_offset;
 uint32_t effect_count, effects_offset;
 uint32_t edge_count, edges_offset;
 uint32_t chapter_count, chapters_offset;
 uint32_t ending_count, endings_offset;
 uint32_t gallery_count, gallery_offset;
 uint32_t strings_offset, strings_size;
 uint32_t reserved[29];
} nine_script_header_t; // 256 bytes
typedef struct {
 uint32_t first_frame, frame_count;
 uint32_t first_effect, effect_count;
 uint32_t first_next, next_count;
 uint32_t first_choice, choice_count;
 uint32_t title_str, chapter, id_str, source_str;
} nine_node_record_t; // 48 bytes, completion is appended to node effects.
typedef struct {
 uint32_t kind, visual, text_block, text_offset, text_length, speaker_str;
} nine_frame_record_t; // 24 bytes. OP has no text; visual may be NONE.
typedef struct {
 uint32_t data_offset, compressed_size, raw_size, crc32;
} nine_text_block_record_t; // 16 bytes; zlib stream; raw_size <=8192.
typedef struct { uint32_t first_layer, layer_count; } nine_visual_record_t;
typedef struct {
 uint32_t image;
 int32_t x, y;
 uint32_t width, height, opacity;
} nine_layer_record_t; // 24 bytes; opacity 0..255; image is already cropped.
typedef struct { uint32_t name_str, flags; } nine_var_record_t;
typedef struct { uint32_t op, variable; int32_t value; } nine_condition_record_t;
typedef struct { uint32_t op, variable; int32_t value; } nine_effect_record_t;
typedef struct {
 uint32_t target, first_condition, condition_count;
 uint32_t first_effect, effect_count, label_str;
} nine_edge_record_t; // 24 bytes, target NONE = end; nexts preserve source order.
// Choices use source visibleChoices semantics: filter by condition; if none
// match, offer every choice. Node effects execute before showing choices.
typedef struct { uint32_t name_str, node; } nine_chapter_record_t;
typedef struct { uint32_t name_str, variable, description_str; } nine_ending_record_t;
typedef struct { uint32_t name_str, image, group; } nine_gallery_record_t;
typedef struct {
 uint8_t magic[8];
 uint32_t version, file_size, crc32;
 uint32_t image_count, images_offset;
 uint32_t op_count, op_offset;
 uint32_t audio_count, audio_offset;
 uint32_t title_images[3], title_logo;
 uint32_t reserved[17];
} nine_image_header_t; // 128 bytes
typedef struct {
 uint32_t kind, width, height, data_offset, data_size, crc32, reserved[2];
} nine_image_record_t; // 32 bytes
// INDEXED image data: palette_count, block_rows, block_count (three u32),
// then RGBA8888 palette[palette_count], then block records[block_count].
// Block offsets are absolute file offsets. Each zlib block expands to
// width*min(block_rows,remaining_rows) palette indices (one byte/pixel).
// Palette alpha is applied together with layer opacity. No full-image inflate.
typedef struct { uint32_t data_offset, compressed_size; } nine_image_block_t;
typedef struct { uint32_t image, timestamp_ms, duration_ms; } nine_op_record_t;
typedef struct {
 uint32_t kind, name_str, data_offset, data_size, duration_ms, sample_rate, channels, crc32;
} nine_audio_record_t; // 32 bytes; name_str uses SCRIPT string pool.
