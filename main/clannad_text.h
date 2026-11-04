// main/clannad_text.h —— 阅读器排版纯逻辑（16px 字模、横屏 320x240）。
#pragma once

#include <stdint.h>

#define CLAN_TEXT_MAX_PAGES 24
#define CLAN_TEXT_LINES 3
#define CLAN_TEXT_UNITS_PER_LINE 38   // 304px / 16px = 19 全角

// UTF-8 下一个码位起点（非法字节按 1 字节推进）。
uint16_t clannad_utf8_next(const char *s, uint16_t len, uint16_t pos);
// 码位数量（BMP；本作无增补平面字符）。
int clannad_utf8_count(const char *s, uint16_t len);

// 按“每行 units 个单位、每页 lines 行”切页；offsets 至少 CLAN_TEXT_MAX_PAGES+1。
// 返回页数（>=1），offsets[0..pages] 为每页起始字节偏移，末项=文本长度。
int clannad_text_pages(const char *utf8, int units_per_line, int lines_per_page,
                       uint16_t *offsets, int max_offsets);

// 把一页（start..end 字节）按同一套单位/禁则排成最多 CLAN_TEXT_LINES 行；
// reveal 用于打字机：只显示 start..reveal 的内容。
void clannad_text_layout(const char *utf8, uint16_t start, uint16_t end, uint16_t reveal,
                         int units_per_line, char lines[CLAN_TEXT_LINES][160]);
