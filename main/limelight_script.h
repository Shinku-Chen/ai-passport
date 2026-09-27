// main/limelight_script.h —— 剧本包(LLSPK001)的只读视图。
//
// 剧本包由 tools/limelight_script_pack.py 生成,格式见该工具的文件头注释:
//   68,229 条对白按 250 条一块、每块独立 raw-deflate;另有名字池、章节表、选项表。
// 设备端用 ESP32-C3 ROM 里的 tinfl_decompress 解压(main/limelight_inflate.c),
// 因此本层把解压做成回调 —— 宿主测试可以喂 --stored 打的包,不需要解压器。
//
// 本层不碰 ESP-IDF / LVGL,宿主可直接跑测试。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LIME_SCRIPT_MAGIC "LLSPK001"
#define LIME_SCRIPT_VERSION 1u

// 对白记录标志(与打包器的 FLAG_* 一致)。
#define LIME_DLG_HAS_BG (1u << 0)
#define LIME_DLG_HAS_SPRITE (1u << 1)
#define LIME_DLG_HAS_CG (1u << 2)
#define LIME_DLG_HAS_SPEAKER (1u << 3)
#define LIME_DLG_HAS_TEXT (1u << 4)
#define LIME_DLG_Z1 (1u << 5)      // 演出 z=1
#define LIME_DLG_Z2 (1u << 6)      // 演出 z=2
#define LIME_DLG_IS_CHOICE (1u << 7)

#define LIME_NAME_NONE 0xFFFFu

typedef struct {
    uint8_t flags;        // LIME_DLG_*
    uint16_t bg;          // 背景名下标(无 = LIME_NAME_NONE)
    uint16_t sprite;      // 立绘名下标
    uint16_t cg;          // CG/演出图名下标
    uint16_t speaker;     // 说话人名下标(章节标签是 "[CHAPTER…]" 这种说话人)
    const char *text;     // UTF-8,指向块缓存;无正文时为 NULL
    uint16_t text_len;
} lime_dialogue_t;

// 选项:i 从 0 起,每项 6 字节 { name u16, target u32 }。
typedef struct {
    uint16_t name;
    uint32_t target;
} lime_choice_option_t;

// 分块解压回调:src 是块内的压缩字节,dst 容量 dst_cap(≥ max_chunk_raw);
// 成功时写 *out_len 并返回 true。stored 块不需要该回调。
typedef bool (*lime_inflate_fn)(const uint8_t *src, uint32_t src_len, uint8_t *dst,
                                uint32_t dst_cap, uint32_t *out_len);

typedef struct {
    const uint8_t *blob;
    uint32_t size;
    uint32_t entries;            // 对白总条数(id 1..entries 连续)
    uint16_t chunk_entries;      // 每块条数
    uint32_t max_chunk_raw;      // 单块解压后的最大字节数(缓冲按它申请)
    uint32_t chunks;

    const uint8_t *chunk_tab;    // { off u32, size u32, first_id u32, count u16, flags u16 }
    uint32_t chunk_tab_len;
    const uint8_t *text_sec;     // 分块数据
    uint32_t text_sec_len;

    const uint8_t *name_tab;     // { off u32, len u16, pad u16 }
    uint32_t name_count;
    const uint8_t *name_text;
    uint32_t name_text_len;

    const uint8_t *chapter_tab;  // { first_id u32, name u16 }
    uint32_t chapter_count;
    const uint8_t *choice_tab;   // { id u32, count u8, pad u8, [name u16, target u32] × count }
    uint32_t choice_count;
    uint32_t choice_tab_len;

    // 块缓存(由调用方提供)与当前驻留的块。
    uint8_t *cache;
    uint32_t cache_cap;
    uint32_t cache_len;          // 解压后长度
    uint32_t cache_first_id;     // 0 = 无缓存
    lime_inflate_fn inflate;
} lime_script_t;

// 打开剧本包。blob 必须长期有效。失败返回 false。
bool lime_script_open(lime_script_t *script, const uint8_t *blob, uint32_t size);

// 设置分块解压器(设备端接 ROM tinfl;宿主测试用 --stored 包可以不设)。
void lime_script_set_inflate(lime_script_t *script, lime_inflate_fn inflate);

// 设置块缓存(必须 ≥ max_chunk_raw)。不设置则每次取对白都会失败。
void lime_script_set_cache(lime_script_t *script, uint8_t *buffer, uint32_t capacity);

// 对白总条数(id 1..entries 连续)与每块条数。
uint32_t lime_script_entries(const lime_script_t *script);
uint16_t lime_script_chunk_entries(const lime_script_t *script);

// 取第 id 条对白(1 起)。必要时解压所在块,记录里的 text 指向缓存。
// id 越界、缺缓存、解压失败都返回 false。
bool lime_script_dialogue(lime_script_t *script, uint32_t id, lime_dialogue_t *out);

// 名字池查询:返回 UTF-8 文本与长度(不含 NUL)。下标非法返回 false。
bool lime_script_name(const lime_script_t *script, uint16_t index, const char **text,
                      uint16_t *len);

// 章节表:源数据里说话人以 "[CHAPTER" 开头的条目就是章节起点。
uint32_t lime_script_chapters(const lime_script_t *script);
bool lime_script_chapter(const lime_script_t *script, uint32_t index, uint32_t *first_id,
                         uint16_t *name_index);
// 当前 id 落在第几章(0 起);在首章之前返回 0。
uint32_t lime_script_chapter_of(const lime_script_t *script, uint32_t id);

// 选项表。找不到该 id 的选项返回 false。
uint32_t lime_script_choices(const lime_script_t *script);
bool lime_script_choice(const lime_script_t *script, uint32_t index, uint32_t *id,
                        uint8_t *count, lime_choice_option_t *options, uint8_t capacity);
// 按 id 查选项(用于"当前这条是不是选项")。找不到返回 false。
bool lime_script_choice_for(const lime_script_t *script, uint32_t id, uint8_t *count,
                            lime_choice_option_t *options, uint8_t capacity);
