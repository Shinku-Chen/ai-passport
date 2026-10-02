#include "oc_version.h"

#include <stdio.h>
#include <string.h>

// 从字符串开头读连续数字,返回读到的位数(0 = 不是数字)
static size_t digits(const char *s)
{
    size_t n = 0;
    while (s[n] >= '0' && s[n] <= '9') {
        n++;
    }
    return n;
}

bool oc_version_major(const char *version, char *out, size_t out_size)
{
    if (version == NULL || out == NULL || out_size == 0) {
        return false;
    }
    while (*version == ' ' || *version == '\t') {
        version++;
    }
    size_t a = digits(version);
    if (a == 0 || version[a] != '.') {
        return false;
    }
    size_t b = digits(version + a + 1);
    if (b == 0) {
        return false;
    }
    // 三段及以上的版本号(_x_y_._z_)只取前两段;超出缓冲则截断
    int n = snprintf(out, out_size, "%.*s.%.*s", (int)a, version, (int)b, version + a + 1);
    return n > 0 && (size_t)n < out_size;
}

bool oc_version_same_major(const char *a, const char *b)
{
    char ma[16];
    char mb[16];
    if (!oc_version_major(a, ma, sizeof(ma)) || !oc_version_major(b, mb, sizeof(mb))) {
        return true;   // 信息不足:不判不同,避免假告警
    }
    return strcmp(ma, mb) == 0;
}
