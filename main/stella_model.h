// main/stella_model.h —— 纯逻辑：中文分页、UTF-8 游标、存档编解码、解锁位图。
//
// 这一层不依赖 ESP-IDF / LVGL，宿主机测试直接编译。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STELLA_TEXT_BUFFER 1024
#define STELLA_MAX_PAGES 16
#define STELLA_SAVE_BYTES 20
#define STELLA_SAVE_VERSION 2u
#define STELLA_UNLOCK_SCENE_BYTES 5
#define STELLA_UNLOCK_GALLERY_BYTES 32

// 解锁 flags
#define STELLA_FLAG_MAIN_CLEAR (1u << 0)
#define STELLA_FLAG_AFTER_CLEAR (1u << 1)
#define STELLA_FLAG_MAIN_END (1u << 2)
#define STELLA_FLAG_AFTER_END (1u << 3)
#define STELLA_FLAG_EXTRA_END (1u << 4)

typedef struct {
    int units_per_line;   // CJK = 2 units, ASCII = 1 unit
    int lines_per_page;
} stella_layout_t;

// 存档模式必须显式保存：chapter 模式从章节入口进入，读完 21 章也不能授予
// sf.stella_main_clear；story 模式才走完整周目。
#define STELLA_MODE_STORY 0u
#define STELLA_MODE_CHAPTER 1u

typedef struct {
    uint16_t scene;   // 源场景 id (1..109)
    uint32_t op;      // 场景内 op 下标
    uint16_t page;    // 当前文本 op 的页号
    uint16_t flags;   // 存档时的剧情 flags 快照
    uint8_t mode;     // STELLA_MODE_STORY / STELLA_MODE_CHAPTER
} stella_save_t;

typedef struct {
    uint8_t flags;
    uint8_t scenes[STELLA_UNLOCK_SCENE_BYTES];
    uint8_t gallery[STELLA_UNLOCK_GALLERY_BYTES];
} stella_unlock_t;

extern const stella_layout_t STELLA_LAYOUT_16;
extern const stella_layout_t STELLA_LAYOUT_20;

// 把一整段 UTF-8 切成若干屏。offsets 至少 STELLA_MAX_PAGES+1 项；
// 返回页数（>=1），offsets[0..pages] 是每页起始字节偏移，末项 = 文本长度。
int stella_text_pages(const char *utf8, const stella_layout_t *layout,
                      uint32_t *offsets, int max_offsets);

// 返回从 pos 开始的下一个 UTF-8 字符起点；用于打字机逐字显示。
size_t stella_utf8_next(const char *utf8, size_t len, size_t pos);

// 存档：16 字节，含 magic/version/flags/scene/op/page/CRC16。
size_t stella_save_encode(const stella_save_t *save, uint8_t *out, size_t cap);
bool stella_save_decode(stella_save_t *save, const uint8_t *data, size_t len);

// 40 个场景位 + 256 个鉴赏组位 + 剧情 flags。
void stella_unlock_init(stella_unlock_t *unlock);
bool stella_unlock_has_scene(const stella_unlock_t *unlock, uint16_t scene_id);
void stella_unlock_set_scene(stella_unlock_t *unlock, uint16_t scene_id);
bool stella_unlock_has_gallery(const stella_unlock_t *unlock, uint16_t group);
void stella_unlock_set_gallery(stella_unlock_t *unlock, uint16_t group);
bool stella_unlock_has_flag(const stella_unlock_t *unlock, uint8_t flag);
void stella_unlock_set_flag(stella_unlock_t *unlock, uint8_t flag);
