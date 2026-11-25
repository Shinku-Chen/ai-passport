#pragma once
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
typedef int BaseType_t;
typedef unsigned TickType_t;
typedef struct test_task *TaskHandle_t;
typedef struct test_sem *SemaphoreHandle_t;
typedef void (*TaskFunction_t)(void *);
#define pdTRUE 1
#define pdPASS 1
#define pdFALSE 0
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) (ms)
#define portMUX_TYPE int
#define portMUX_INITIALIZER_UNLOCKED 0
#define taskENTER_CRITICAL(mux) ((void)(mux))
#define taskEXIT_CRITICAL(mux) ((void)(mux))
