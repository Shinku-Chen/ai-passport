// main/gal_text.h —— UTF-8 安全的分页/宽度计算(不依赖 LVGL,宿主机可测)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 等宽 CJK 字体的近似宽度:ASCII 半角 = 像素/2,其余(中文/全角/标点)= 像素。
int gal_text_cp_width(uint32_t cp, int font_px);

// 把 UTF-8 文本按给定像素宽、行数分页,返回指定页的显示文本(含手工换行)。
// page_count 返回总页数(可为 NULL)。成功返回 true;页不存在返回 false。
bool gal_text_page(const char *text, int page, int width_px, int font_px, int max_lines,
                   char *out, size_t cap, int *page_count);

// 同上,并返回该页消费的源字节区间 [start,end)。区间连续覆盖全部源文本,
// 用于“分页后按源区间拼回全文”的无损校验;不依赖页面里插入的换行。
bool gal_text_page_ex(const char *text, int page, int width_px, int font_px, int max_lines,
                      char *out, size_t cap, int *page_count,
                      size_t *start_out, size_t *end_out);

int gal_text_page_count(const char *text, int width_px, int font_px, int max_lines);

// 复制前 cps 个码位(不切断 UTF-8),用于打字机与预览。
int gal_text_prefix(const char *text, int cps, char *out, size_t cap);
int gal_text_cp_count(const char *text);
