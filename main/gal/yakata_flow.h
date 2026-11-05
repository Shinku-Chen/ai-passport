#pragma once
#include <stdint.h>
#define YAKATA_VAR_COUNT 8
// 0: portrait, 1: 90px landscape, 2: compact 80px landscape panel.
// Stored in the existing NVS header reserve; older layouts replay the line.
#define YAKATA_TEXT_LAYOUT_ID 2u
extern const uint8_t yakata_event_chapters[];
extern const uint8_t yakata_event_cgs[];
extern const uint16_t yakata_chapter_events[];
extern const void *const yakata_cg_images[];
extern const uint16_t yakata_cg_offsets[];
int yakata_exit(int scene, int32_t variables[YAKATA_VAR_COUNT]);
int yakata_choose(int choice, int option, int32_t variables[YAKATA_VAR_COUNT]);
