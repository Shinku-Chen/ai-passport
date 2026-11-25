#include "esp_heap_caps.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define MAGIC 0x48505354u
typedef struct { uint32_t magic; size_t size; } hdr_t;
static size_t s_used, s_peak, s_largest;
void heap_stub_reset(void) { s_used = s_peak = s_largest = 0; }
size_t heap_stub_used(void) { return s_used; }
size_t heap_stub_peak(void) { return s_peak; }
size_t heap_stub_largest(void) { return s_largest; }
void *heap_caps_malloc(size_t size, unsigned caps)
{
    (void)caps;
    hdr_t *h = malloc(sizeof(*h) + size);
    if (!h) return NULL;
    h->magic = MAGIC; h->size = size;
    s_used += size;
    if (s_used > s_peak) s_peak = s_used;
    if (size > s_largest) s_largest = size;
    return h + 1;
}
void heap_caps_free(void *ptr)
{
    if (!ptr) return;
    hdr_t *h = (hdr_t *)ptr - 1;
    if (h->magic != MAGIC) abort();
    h->magic = 0;
    s_used -= h->size;
    free(h);
}
