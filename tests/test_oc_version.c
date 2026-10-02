// 版本号比较的主机单测(纯 C,不依赖 ESP-IDF):cc -std=c11 -Wall -Wextra -Werror -Imain
#include "oc_version.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            failures++;                                                        \
        }                                                                      \
    } while (0)

static void check_major(const char *in, const char *want)
{
    char out[16] = "";
    bool ok = oc_version_major(in, out, sizeof(out));
    CHECK(ok);
    CHECK(strcmp(out, want) == 0);
    if (strcmp(out, want) != 0) {
        printf("  major(%s) = %s, want %s\n", in, out, want);
    }
}

int main(void)
{
    // 大版本 = 前两段
    check_major("1.11.2", "1.11");
    check_major("1.11", "1.11");
    check_major("  1.10  ", "1.10");
    check_major("1.11.2.3", "1.11");
    check_major("10.2", "10.2");

    // 解析不出来的一律 false(调用方据此判「信息不足」)
    {
        char out[16];
        CHECK(!oc_version_major("1", out, sizeof(out)));
        CHECK(!oc_version_major("", out, sizeof(out)));
        CHECK(!oc_version_major("x.y", out, sizeof(out)));
        CHECK(!oc_version_major("1.x", out, sizeof(out)));
        CHECK(!oc_version_major(NULL, out, sizeof(out)));
        CHECK(!oc_version_major("1.11", out, 0));
    }

    // 配套判定:App 小版本(shrink)不算不匹配
    CHECK(oc_version_same_major("1.11.1", "1.11"));
    CHECK(oc_version_same_major("1.11.2", "1.11.0"));
    CHECK(oc_version_same_major("1.11", "1.11.3"));
    CHECK(oc_version_same_major("1.11.1", "1.11.1"));

    // 大版本不同 → 不配套
    CHECK(!oc_version_same_major("1.12", "1.11"));
    CHECK(!oc_version_same_major("1.11.2", "1.10"));
    CHECK(!oc_version_same_major("1.9.0", "1.10"));

    // 信息不足 → 判配套(宁可少报,不要假告警)
    CHECK(oc_version_same_major("", "1.11"));
    CHECK(oc_version_same_major(NULL, "1.11"));
    CHECK(oc_version_same_major("1.11", ""));

    if (failures == 0) {
        printf("test_oc_version: all checks passed\n");
        return 0;
    }
    printf("test_oc_version: %d check(s) failed\n", failures);
    return 1;
}
