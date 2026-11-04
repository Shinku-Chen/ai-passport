// tests/test_gal_text.c —— 排版纯逻辑测试（宿主运行，不需要设备）。
// 横屏 320x240:16px 档每行 19 字(38 单位)x4 行,20px 档每行 15 字(30 单位)x3 行。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "gal_text.h"

int main(void)
{
    uint32_t offsets[GAL_TEXT_MAX_PAGES + 1];

    // 13 个全角字正好一行(每行 26 个单位):单页。
    assert(gal_text_pages("一二三四五六七八九十十一十", 26, 5, offsets,
                          GAL_TEXT_MAX_PAGES + 1) == 1);

    // 每行 2 个单位、每页 1 行:4 个全角字 = 4 页,偏移按 3 字节递进。
    const int pages = gal_text_pages("中中中中", 2, 1, offsets, GAL_TEXT_MAX_PAGES + 1);
    assert(pages == 4);
    assert(offsets[0] == 0 && offsets[1] == 3 && offsets[2] == 6 && offsets[3] == 9);
    assert(offsets[4] == 12);   // 文本长度

    // 行首禁则：句号挂在行尾不会崩（分页器只暴露页边界，行内构成不在此断言）。
    assert(gal_text_pages("中中中。", 6, 2, offsets, GAL_TEXT_MAX_PAGES + 1) == 1);
    assert(gal_text_pages("中中中。中中", 6, 2, offsets, GAL_TEXT_MAX_PAGES + 1) == 1);

    // 显式换行符强制断行,哪怕本行还有空间。
    const int nl_pages = gal_text_pages("ab\ncd", 20, 1, offsets, GAL_TEXT_MAX_PAGES + 1);
    assert(nl_pages == 2);
    assert(offsets[1] == 3);

    // 空文本算一页;NULL 偏移只取页数。
    assert(gal_text_pages("", 26, 5, offsets, GAL_TEXT_MAX_PAGES + 1) == 1);
    assert(gal_text_pages("中中中中", 2, 1, NULL, 0) == 4);

    puts("gal_text pagination: PASS");
    // 横屏 16px 档:38 单位/行、4 行/页 —— 76 个全角字正好一页。
    {
        // 用真实 UTF-8 汉字更直观
        char text[400] = { 0 };
        for (int i = 0; i < 76; ++i) strcat(text, "中");
        assert(gal_text_pages(text, 38, 4, offsets, GAL_TEXT_MAX_PAGES + 1) == 1);
        strcat(text, "多");   // 第 77 个字 -> 第二页
        assert(gal_text_pages(text, 38, 4, offsets, GAL_TEXT_MAX_PAGES + 1) == 2);
    }
    // 横屏 20px 档:30 单位/行、3 行/页 —— 45 个全角字一页,第 46 个字换页。
    {
        char text[400] = { 0 };
        for (int i = 0; i < 45; ++i) strcat(text, "中");
        assert(gal_text_pages(text, 30, 3, offsets, GAL_TEXT_MAX_PAGES + 1) == 1);
        strcat(text, "多");
        assert(gal_text_pages(text, 30, 3, offsets, GAL_TEXT_MAX_PAGES + 1) == 2);
    }
    // 台词最长 117 字:16px 档 2 页内装得下(38/4 与 30/3 都够)。
    {
        char text[400] = { 0 };
        for (int i = 0; i < 117; ++i) strcat(text, "中");
        assert(gal_text_pages(text, 38, 4, offsets, GAL_TEXT_MAX_PAGES + 1) == 2);
        assert(gal_text_pages(text, 30, 3, offsets, GAL_TEXT_MAX_PAGES + 1) == 3);
    }
    return 0;
}
