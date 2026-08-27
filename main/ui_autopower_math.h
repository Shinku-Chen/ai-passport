// main/ui_autopower_math.h —— 「无操作自动深睡」的判定纯逻辑,与 ESP-IDF 隔离。
// 便于宿主测试覆盖(见 tests/test_ui_autopower_math.c)。
#pragma once

#include <stdint.h>
#include <stdbool.h>

// 无操作超时阈值(毫秒):超过这段时间没有按键活动即进入深睡。
#define AUTOPOWER_IDLE_TIMEOUT_MS  (2 * 60 * 1000UL)   // 2 分钟

// 判断「距上次活动 elapsed_ms 毫秒后,是否已无操作超时(应关机)」。
//   elapsed_ms < 0 视为计时异常,保守返回 false(不关机)。
bool ui_autopower_idle_expired(int64_t elapsed_ms);

// 归一化:把原始毫秒差值掐到 [0, INT64_MAX],供调用方安全使用。
int64_t ui_autopower_clamp_elapsed(int64_t elapsed_ms);
