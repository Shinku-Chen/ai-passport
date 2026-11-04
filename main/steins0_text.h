// main/steins0_text.h —— 纯逻辑:UTF-8 取字、PUA 宽度、禁则分页、存档与解锁位。
//
// 不依赖 ESP-IDF/LVGL,宿主机测试直接编译。宽度表由 tools/steins0_font.py
// 从转换源的 glyph-widths.js 生成(main/steins0_widths.h),字体生成器使用同一份
// 数字,所以分页结果与 LVGL 里的实际字宽一致。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SG0_TEXT_LINES 3
#define SG0_TEXT_MAX_PAGES 16
#define SG0_TEXT_BUFFER 384
#define SG0_SAVE_BYTES 432
#define SG0_SAVE_V2_BYTES 428
#define SG0_SAVE_CORE_BYTES 64
#define SG0_SAVE_PREVIEW 19
#define SG0_SAVE_RINE_BITS 1408
#define SG0_SAVE_RINE_BYTES ((SG0_SAVE_RINE_BITS + 7) / 8)
#define SG0_SAVE_VERSION 3u
#define SG0_UNLOCK_BYTES 34
#define SG0_UNLOCK_VERSION 1u
#define SG0_CHAPTER_BITS 104
#define SG0_CHAPTER_BYTES ((SG0_CHAPTER_BITS + 7) / 8)
#define SG0_GALLERY_BITS 106
#define SG0_GALLERY_BYTES ((SG0_GALLERY_BITS + 7) / 8)

typedef struct {
    uint8_t pages;
    uint16_t page_start[SG0_TEXT_MAX_PAGES + 1];         // 绝对字节偏移
    uint8_t line_count[SG0_TEXT_MAX_PAGES];
    uint16_t line_start[SG0_TEXT_MAX_PAGES][SG0_TEXT_LINES + 1];  // 绝对字节偏移
} sg0_pages_t;

// UTF-8: 返回 pos 处码点并推进 pos;非法序列按 U+FFFD 单字节前进。
uint32_t sg0_utf8_next(const char *s, size_t len, size_t *pos);
// 返回 pos 前一个码点的起始字节。
size_t sg0_utf8_prev(const char *s, size_t pos);
// 按字体 advance(1/16 px)推进。
uint16_t sg0_advance(uint32_t cp);
// 把码点写入 UTF-8 缓冲(最多 4 字节),返回字节数。
size_t sg0_utf8_encode(uint32_t cp, char *out);

// 源正文清理:去掉对话外层「」、把 PUA 空格换成空格、压缩空白。
// 返回输出字节数(不含 NUL);cap 不足时截断在码点边界。
size_t sg0_text_clean(const char *src, size_t len, char *dst, size_t cap);

// 分页:width_adv 为行宽(1/16 px),lines_per_page 通常为 SG0_TEXT_LINES。
// 行首禁则:PUA 标点 id 190..231 与 702/703、Unicode 标点不落行首。
bool sg0_text_layout(const char *text, uint16_t width_adv, uint8_t lines_per_page,
                     sg0_pages_t *out);

// 判断码点是否禁止出现在行首。
bool sg0_no_line_start(uint32_t cp);

// ---------------------------------------------------------------- 存档
typedef struct {
    uint8_t node;
    uint16_t cursor;
    uint8_t page;
    uint8_t mode;        // 0 = story, 1 = chapter
    uint8_t endings;
    uint8_t flags;
    uint8_t group;
    uint32_t date;
    uint8_t preview_len;
    char preview[SG0_SAVE_PREVIEW];
    uint8_t chapters[SG0_CHAPTER_BYTES];
    uint8_t gallery[SG0_GALLERY_BYTES];
} sg0_save_meta_t;

typedef struct {
    sg0_save_meta_t meta;
    uint8_t rine_active[SG0_SAVE_RINE_BYTES];  // RMSG 目录位
    uint8_t rine_read[SG0_SAVE_RINE_BYTES];
    uint16_t rine_replied;
    uint16_t rine_windows;
    uint8_t pending_valid;
    uint8_t pending_node;
    uint8_t last_card;
    uint8_t event_node;       // v3: node owning the event ordinal
    uint16_t event_cursor;    // v3: consumed event ordinal (not story cursor)
    uint8_t event_valid;      // v2/v1 decode sets 0 for derived migration
} sg0_save_t;

void sg0_save_init(sg0_save_t *save, uint8_t node, uint16_t cursor, uint8_t mode);
size_t sg0_save_encode(const sg0_save_t *save, uint8_t *out, size_t cap);
bool sg0_save_decode(sg0_save_t *save, const uint8_t *data, size_t len);
void sg0_save_meta_from(const sg0_save_t *save, sg0_save_meta_t *meta);
bool sg0_save_rine_active(const sg0_save_t *save, uint16_t ref);
bool sg0_save_rine_read(const sg0_save_t *save, uint16_t ref);
void sg0_save_rine_set(sg0_save_t *save, uint16_t ref, bool read);

// ---------------------------------------------------------------- 解锁
typedef struct {
    uint8_t endings;
    uint8_t flags;
    uint8_t chapters[SG0_CHAPTER_BYTES];
    uint8_t gallery[SG0_GALLERY_BYTES];
} sg0_unlock_t;

void sg0_unlock_init(sg0_unlock_t *unlock);
bool sg0_unlock_has_chapter(const sg0_unlock_t *unlock, uint16_t index);
void sg0_unlock_set_chapter(sg0_unlock_t *unlock, uint16_t index);
bool sg0_unlock_has_gallery(const sg0_unlock_t *unlock, uint8_t group);
void sg0_unlock_set_gallery(sg0_unlock_t *unlock, uint8_t group);
bool sg0_unlock_has_ending(const sg0_unlock_t *unlock, uint8_t ending);
void sg0_unlock_set_ending(sg0_unlock_t *unlock, uint8_t ending);
bool sg0_unlock_has_flag(const sg0_unlock_t *unlock, uint8_t flag);
void sg0_unlock_set_flag(sg0_unlock_t *unlock, uint8_t flag);
// 并集合并;返回是否有新增位。
bool sg0_unlock_merge(sg0_unlock_t *dst, const sg0_unlock_t *src);
// 从存档合并“看到的”解锁位(章节/CG/结局);返回是否有新增位。
bool sg0_unlock_merge_save(sg0_unlock_t *unlock, const sg0_save_t *save);
size_t sg0_unlock_encode(const sg0_unlock_t *unlock, uint8_t *out, size_t cap);
bool sg0_unlock_decode(sg0_unlock_t *unlock, const uint8_t *data, size_t len);

uint16_t sg0_crc16(const uint8_t *data, size_t len);
