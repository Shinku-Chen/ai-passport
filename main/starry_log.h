// main/starry_log.h —— 纯逻辑与渲染模块的日志适配。
//
// starry_gfx.c / starry_font.c / starry_model.c / starry_pack.c,以及
// starry_render.c(宿主机测试用假面板跑 pages 组合),都要能在宿主机上编译,
// 所以不能直接 include esp_log.h。这里按平台二选一:固件走 ESP_LOGx,宿主机走 stderr。
#pragma once

#if defined(ESP_PLATFORM)
#include "esp_log.h"
#define STARRY_LOGI(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#define STARRY_LOGW(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)
#define STARRY_LOGE(tag, fmt, ...) ESP_LOGE(tag, fmt, ##__VA_ARGS__)
#else
#include <stdio.h>
#define STARRY_LOGI(tag, fmt, ...) fprintf(stderr, "[%s] " fmt "\n", tag, ##__VA_ARGS__)
#define STARRY_LOGW(tag, fmt, ...) fprintf(stderr, "[%s] " fmt "\n", tag, ##__VA_ARGS__)
#define STARRY_LOGE(tag, fmt, ...) fprintf(stderr, "[%s] " fmt "\n", tag, ##__VA_ARGS__)
#endif
