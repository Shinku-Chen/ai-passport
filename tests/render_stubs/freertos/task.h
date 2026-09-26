#pragma once
#include "FreeRTOS.h"
static inline void vTaskDelay(TickType_t t) { (void)t; }
