// main/gal/gal_text.h —— 阅读器的文本排版纯逻辑（与具体作品无关）。
//
// 分页模型：半角算 1 个单位、全角算 2 个；行首禁则标点（、。！？」等）允许轻微
// 超宽挂在行尾，不会出现在下一行行首。纯逻辑、可宿主测试。
#pragma once

#include <stdint.h>

// 单次排版最多支持多少页（超出按需要调大）。
#define GAL_TEXT_MAX_PAGES 8

// 把 utf8 按“每行 units_per_line 个单位、每页 lines_per_page 行”切页。
// offsets[0..pages-1] 回填每页起始字节偏移，offsets[pages] 回填文本长度。
// 返回页数（至少 1）。offsets 可为 NULL（只要页数）。
int gal_text_pages(const char *utf8, int units_per_line, int lines_per_page,
                   uint32_t *offsets, int max_offsets);
