#pragma once
#include <stddef.h>
#define MALLOC_CAP_INTERNAL (1u << 0)
#define MALLOC_CAP_8BIT     (1u << 1)
void *heap_caps_malloc(size_t size, unsigned caps);
void heap_caps_free(void *ptr);
size_t heap_stub_used(void);
size_t heap_stub_peak(void);
size_t heap_stub_largest(void);
void heap_stub_reset(void);
