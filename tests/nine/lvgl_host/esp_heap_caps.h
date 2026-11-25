#pragma once
#include <stddef.h>
#include <stdint.h>
#define MALLOC_CAP_8BIT (1u << 2)
#define MALLOC_CAP_INTERNAL (1u << 11)
void *heap_caps_malloc(size_t size, uint32_t caps);
void heap_caps_free(void *p);
uint32_t esp_get_free_heap_size(void);
size_t heap_caps_get_largest_free_block(uint32_t caps);
void host_heap_fail_next(int count);
int host_heap_alloc_count(void);
