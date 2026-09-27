// Host-test stub: 只给 BSP 里的 vTaskDelay 提供 tick 换算。
#pragma once
#include <stdint.h>
#define pdMS_TO_TICKS(ms) ((uint32_t)(ms))
