#pragma once
#include <stdint.h>
typedef int BaseType_t;
typedef unsigned int TickType_t;
typedef void *SemaphoreHandle_t;
#define pdFALSE 0
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
