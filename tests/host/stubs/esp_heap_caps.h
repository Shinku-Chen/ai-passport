#pragma once
#include <stddef.h>
#include <stdint.h>
#define MALLOC_CAP_8BIT 0x1
#define MALLOC_CAP_INTERNAL 0x2
void *heap_caps_malloc(size_t size, uint32_t caps);
void heap_caps_free(void *p);
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);
// 测试注入：0 = 不限制；>0 = 大于该值的分配返回 NULL
void host_heap_set_limit(size_t max_single_alloc);
size_t host_heap_peak(void);
