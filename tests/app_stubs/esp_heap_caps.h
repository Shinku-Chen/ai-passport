// 宿主桩：堆分配直接走 malloc，caps 忽略。
#pragma once

#include <stddef.h>
#include <stdint.h>

#define MALLOC_CAP_8BIT 0x00000004
#define MALLOC_CAP_INTERNAL 0x00000800

void *heap_caps_malloc(size_t size, uint32_t caps);
void heap_caps_free(void *ptr);
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);
