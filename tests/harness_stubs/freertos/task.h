// 宿主桩：vTaskDelay 空实现（分带刷新用）。
#pragma once

#include <stdint.h>

void vTaskDelay(uint32_t ticks);
