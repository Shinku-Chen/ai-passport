#pragma once
int fake_log(const char *fmt, ...);
#define ESP_LOGI(tag, fmt, ...) do { (void)(tag); (void)fake_log(fmt, ##__VA_ARGS__); } while (0)
#define ESP_LOGW(tag, fmt, ...) do { (void)(tag); (void)fake_log(fmt, ##__VA_ARGS__); } while (0)
#define ESP_LOGE(tag, fmt, ...) do { (void)(tag); (void)fake_log(fmt, ##__VA_ARGS__); } while (0)
