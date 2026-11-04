// tests/app_stubs/esp_log.h —— 日志替身(默认静默,EDEN_STUB_VERBOSE=1 时打印)。
#pragma once
#include <stdio.h>
#include <stdlib.h>
#define ESP_LOGE(tag, fmt, ...) eden_stub_log("E", tag, fmt)
#define ESP_LOGW(tag, fmt, ...) eden_stub_log("W", tag, fmt)
#define ESP_LOGI(tag, fmt, ...) eden_stub_log("I", tag, fmt)
#define ESP_LOGD(tag, fmt, ...) eden_stub_log("D", tag, fmt)
static inline void eden_stub_log(const char *level, const char *tag, const char *fmt)
{
    if (getenv("EDEN_STUB_VERBOSE") != NULL) {
        fprintf(stderr, "[%s %s] %s\n", level, tag, fmt);
    }
}
