// 宿主机用 esp_log.h:错误打到 stderr,info/debug 静默。
#pragma once
#include <stdio.h>
#define ESP_LOGE(tag, fmt, ...) fprintf(stderr, "E %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) fprintf(stderr, "W %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) do { if (0) fprintf(stderr, "%s" fmt, tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGD(tag, fmt, ...) do { if (0) fprintf(stderr, "%s" fmt, tag, ##__VA_ARGS__); } while (0)
