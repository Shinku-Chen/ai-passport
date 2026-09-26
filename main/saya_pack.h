// main/saya_pack.h —— 《沙耶之歌》资源包的只读解析层。
//
// 包由 tools/saya_pack.py 生成,整体放在 Flash 里由固件直接读取:没有解压、没有
// 运行时 JSON 解析。本文件只做"字节 -> 结构"的纯映射,不依赖 ESP-IDF,因此可以
// 在宿主机上跑单元测试(tests/test_saya_pack.c 用真实包做剧情图校验)。
//
// 字节序与字段布局必须与 tools/saya_pack.py 一致:
//   SEC_TEXT    UTF-8 blob,字符串按 (offset,length) 寻址,不保证 NUL 结尾
//   SEC_NAME    { off u32, len u16, pad u16 }
//   SEC_CHAPTER { id, first_scene, scene_count, first_dlg, dlg_count, next } 全 u16
//   SEC_SCENE   { bg u16, choice_count u8, pad u8, first_dlg u16, dlg_count u16,
//                 choice_name[2] u16, choice_href[2] u16 } = 16 字节
//   SEC_DLG     { text_off u32, text_len u16, name u16, fg u16, flags u8,
//                 jump u8, arg u16 } = 14 字节
//   SEC_BG      { off u32, len u32, w u16, h u16 }(off 相对 SEC_BG 数据区)
//   SEC_FG      { jpeg_off, jpeg_len, mask_off, mask_len(全 u32), w, h(u16) }
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SAYA_PACK_MAGIC "SAYAPK01"
#define SAYA_PACK_VERSION 1u

// 0xFFFF 在 chapter.next / scene.bg / dialogue.name / dialogue.fg 里表示"没有"。
#define SAYA_NONE 0xFFFFu
// 背景表 0 号固定是标题画面(脚本不引用它),与 tools/saya_pack.py 的 TITLE_SOURCE 对应。
#define SAYA_BG_TITLE 0u
// 立绘沿用上一句(dialogue 未指定 fg)。
#define SAYA_FG_KEEP 0xFFFFu

// dialogue.flags
#define SAYA_DLG_END (1u << 0)        // arg = 结局名(name 表下标)
#define SAYA_DLG_BRANCH (1u << 1)     // 选择分支:数据里未使用,保留语义
#define SAYA_DLG_TO_SCENE (1u << 2)   // 本句后跳转到 scene + jump

typedef struct {
    const uint8_t *blob;
    uint32_t blob_size;
    const uint8_t *text;
    uint32_t text_size;
    const uint8_t *names;
    uint32_t name_count;
    const uint8_t *chapters;
    uint32_t chapter_count;
    const uint8_t *scenes;
    uint32_t scene_count;
    const uint8_t *dialogues;
    uint32_t dialogue_count;
    const uint8_t *bgs;        // 索引表
    const uint8_t *bg_data;    // 数据区
    uint32_t bg_count;
    uint32_t bg_data_size;
    const uint8_t *fgs;
    const uint8_t *fg_data;
    uint32_t fg_count;
    uint32_t fg_data_size;
    const uint8_t *meta;
    uint32_t meta_size;
} saya_pack_t;

typedef struct {
    uint16_t id;           // 源脚本编号(1..51),仅用于展示与诊断
    uint16_t first_scene;  // 全局场景下标
    uint16_t scene_count;
    uint16_t first_dlg;    // 全局对白下标
    uint16_t dlg_count;
    uint16_t next;         // 下一章;SAYA_NONE = 无
} saya_chapter_t;

typedef struct {
    uint16_t bg;                // 背景图下标;SAYA_NONE = 黑屏
    uint8_t choice_count;       // 0 = 普通场景,>0 = 选项场景
    uint16_t first_dlg;         // 全局对白下标
    uint16_t dlg_count;
    uint16_t choice_name[2];    // name 表下标
    uint16_t choice_href[2];    // 目标章节下标
} saya_scene_t;

typedef struct {
    uint32_t text_off;
    uint16_t text_len;
    uint16_t name;    // name 表下标;SAYA_NONE = 无名
    uint16_t fg;      // 立绘下标;SAYA_FG_KEEP = 沿用上一句
    uint8_t flags;
    uint8_t jump;
    uint16_t arg;
} saya_dialogue_t;

typedef struct {
    const uint8_t *jpeg;
    uint32_t jpeg_len;
    uint16_t w;
    uint16_t h;
} saya_bg_t;

typedef struct {
    const uint8_t *jpeg;
    uint32_t jpeg_len;
    const uint8_t *mask;   // 1bpp,按行打包,stride = (w+7)/8
    uint32_t mask_len;
    uint16_t w;
    uint16_t h;
} saya_fg_t;

// 校验魔数/版本/尺寸并建立各段视图。失败返回 false(不改动 pack)。
bool saya_pack_open(saya_pack_t *pack, const uint8_t *data, uint32_t size);

void saya_pack_chapter(const saya_pack_t *pack, uint16_t index, saya_chapter_t *out);
void saya_pack_scene(const saya_pack_t *pack, uint16_t index, saya_scene_t *out);
void saya_pack_dialogue(const saya_pack_t *pack, uint16_t index, saya_dialogue_t *out);

// 把字符串复制进 out 并以 NUL 结尾;超长时按 UTF-8 字符边界截断。
// capacity 为 out 的总字节数(含结尾 NUL)。返回写入的字节数(不含 NUL)。
size_t saya_pack_text(const saya_pack_t *pack, uint32_t off, uint32_t len, char *out,
                      size_t capacity);
size_t saya_pack_name(const saya_pack_t *pack, uint16_t id, char *out, size_t capacity);

bool saya_pack_bg(const saya_pack_t *pack, uint16_t id, saya_bg_t *out);
bool saya_pack_fg(const saya_pack_t *pack, uint16_t id, saya_fg_t *out);

// 按源脚本编号找章节下标;找不到返回 -1。
int saya_pack_find_chapter(const saya_pack_t *pack, uint16_t id);
