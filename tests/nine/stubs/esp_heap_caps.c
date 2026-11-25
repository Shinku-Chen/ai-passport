#include "esp_heap_caps.h"
#include <stdlib.h>
void *heap_caps_malloc(size_t size, uint32_t caps) { (void)caps; return malloc(size); }
void heap_caps_free(void *ptr) { free(ptr); }
uint32_t esp_get_free_heap_size(void) { return 200000; }
size_t heap_caps_get_largest_free_block(uint32_t caps) { (void)caps; return 100000; }
