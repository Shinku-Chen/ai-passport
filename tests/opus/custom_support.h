#pragma once
#include <stddef.h>
#define OVERRIDE_OPUS_ALLOC
#define OVERRIDE_OPUS_FREE
void *test_opus_alloc(size_t size);
void test_opus_free(void *ptr);
#define opus_alloc test_opus_alloc
#define opus_free test_opus_free
