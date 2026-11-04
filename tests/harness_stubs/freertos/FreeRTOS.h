// 宿主桩：只提供分带刷新用到的延时宏。
#pragma once

#include <stdint.h>

#define pdMS_TO_TICKS(ms) ((uint32_t)(ms))
