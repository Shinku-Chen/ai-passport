#pragma once
#include "FreeRTOS.h"
static inline SemaphoreHandle_t xSemaphoreCreateBinary(void) { return (SemaphoreHandle_t)1; }
static inline int xSemaphoreTake(SemaphoreHandle_t s, TickType_t t) { (void)s; (void)t; return pdTRUE; }
static inline int xSemaphoreGiveFromISR(SemaphoreHandle_t s, BaseType_t *woken) { (void)s; (void)woken; return pdTRUE; }
static inline void vSemaphoreDelete(SemaphoreHandle_t s) { (void)s; }
