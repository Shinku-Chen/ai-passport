/**
 * 版本号比较（纯 C，无 ESP-IDF 依赖，可在主机上跑单测）。
 *
 * 版本体系：**固件发大版本 `X.Y`**（1.10 / 1.11），**App 可以发小版本 `X.Y[.Z]`**（1.11 / 1.11.1）。
 * 设备与手机是否「配套」只看大版本 `X.Y` —— App 的小版本升级不是不匹配，不该在设备屏上弹告警。
 *
 * 历史坑：设备原来拿 `strcmp(app_version, OC_APP_VERSION)` 整串比，App 一发 1.11.1 就会误报。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/**
 * 取版本号的大版本（前两段）写入 out：`1.11.2` → `1.11`，`1.11` → `1.11`。
 * 成功返回 true；版本串为空或前两段不是数字时返回 false（out 内容不作保证）。
 */
bool oc_version_major(const char *version, char *out, size_t out_size);

/**
 * 两个版本是否**大版本相同**（配套）。
 * 任一侧信息不足（空串 / 解析失败）时返回 true —— 宁可判成配套也别乱报不匹配。
 */
bool oc_version_same_major(const char *a, const char *b);
