#pragma once
#include <stddef.h>
#include <stdint.h>
#define MALLOC_CAP_8BIT 0x1
#define MALLOC_CAP_INTERNAL 0x2
void *heap_caps_malloc(size_t size, uint32_t caps);
void heap_caps_free(void *p);
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);
void host_heap_set_limit(size_t max_single_alloc);
void host_heap_set_free(size_t free_bytes);
// >=0：只允许成功这么多次 heap_caps_malloc（-1 = 不限），用于测回退路径。
void host_heap_fail_after(int allow);
// >0：这个字节数的分配一律失败（0 = 关闭）。
void host_heap_fail_size(size_t bytes);
size_t host_heap_peak(void);
