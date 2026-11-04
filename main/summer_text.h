// main/summer_text.h —— 正文分页/折行（纯逻辑，宿主机可测）。
//
// 16px 档：320 宽屏、左右各 8px、行宽 36 个"半角单位"（CJK=2，ASCII=1），
// 一页 3 行。行首禁则：收尾标点不落到下一行行首。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SUMMER_MAX_PAGES 12
#define SUMMER_PAGE_TEXT_CAP 512

typedef struct {
    int units_per_line;   // 36 = 18 个全角字
    int lines_per_page;   // 3
} summer_layout_t;

extern const summer_layout_t SUMMER_LAYOUT_16;
extern const summer_layout_t SUMMER_LAYOUT_20;

// 返回从 pos 开始的下一个 UTF-8 字符起点；非法字节按 1 字节前进。
size_t summer_utf8_next(const char *utf8, size_t len, size_t pos);

// 把整段 UTF-8 切成屏：offsets 至少 max_offsets 项，offsets[0]=0，
// offsets[pages]=len。返回页数（>=1）。
int summer_text_pages(const char *utf8, const summer_layout_t *layout,
                      uint32_t *offsets, int max_offsets);

// 取出第 page 页并插入换行（输出不超过 cap-1 字节）。成功返回 true。
bool summer_text_page_text(const char *utf8, const summer_layout_t *layout, int page,
                           uint32_t *out_start, uint32_t *out_end, char *out, size_t cap);

// 打字机用：从 pos 取下一个 UTF-8 字符的字节长度（1..4）。
size_t summer_utf8_char_len(const char *utf8, size_t len, size_t pos);
