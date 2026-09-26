// main/starry_pack.h —— 《星空列车与白的旅行》资源包的只读解析层。
//
// 包由 tools/starry_pack.py 生成,整体放在 Flash 里由固件直接读取:没有解压、没有
// 运行时 JSON 解析。本文件只做"字节 -> 结构"的纯映射,不依赖 ESP-IDF,因此可以
// 在宿主机上跑单元测试(tests/test_starry_model.c 用真实包走整条剧情线)。
//
// 字节序与字段布局必须与 tools/starry_pack.py 一致:
//   SEC_TEXT    UTF-8 blob,字符串按 (offset,length) 寻址,不保证 NUL 结尾
//   SEC_NAME    { off u32, len u16, pad u16 }
//   SEC_CHAPTER { id, first_scene, scene_count, first_dlg, dlg_count, next } 全 u16
//   SEC_SCENE   { bg u16, choice_count u8, pad u8, first_dlg u16, dlg_count u16,
//                 choice_name[2] u16, choice_target[2] u16 } = 16 字节
//                 choice_target 是"全局场景下标"(不是章节下标)
//   SEC_DLG     { text_off u32, text_len u16, name u16, sprite u16, flags u8,
//                 jump u8, arg u16 } = 14 字节
//   SEC_BG      { off u32, len u32, w u16, h u16, flags u16 }(off 相对 SEC_BG 数据区)
//   SEC_FG      { color_off, color_len, mask_off, mask_len(全 u32), w u16, h u16,
//                 x u16, y u16, owner u16 } = 26 字节(off 相对 SEC_FG 数据区)
//                 color 是无损 RGB565(小端两字节),mask 是 4bpp(每行字节对齐,
//                 高半字节在左)。设备端直接从 Flash 逐行 blit,不需要立绘解码缓冲。
//                 owner 是这张立绘归属的角色(name 表下标),0xFFFF = 归属不明
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STARRY_PACK_MAGIC "SSRPK001"
// 版本 2:立绘段由「JPEG + 1bpp 遮罩」改为「无损 RGB565 + 4bpp 遮罩」
// (与 ATRI 阅读器的画面区合成一致)。剧本/背景/场景等段的语义与字节布局不变。
#define STARRY_PACK_VERSION 2u

// 0xFFFF 在 chapter.next / scene.bg / dialogue.name / dialogue.sprite 里表示"没有"。
#define STARRY_NONE 0xFFFFu
// 背景表 0 号固定是标题画面(脚本不引用它),与 tools/starry_pack.py 的 TITLE_SOURCE 对应。
#define STARRY_BG_TITLE 0u
// 场景没写背景:沿用上一张(源脚本里目前没有这种场景,留作兼容)。
#define STARRY_BG_KEEP 0xFFFEu
// 立绘沿用上一句(dialogue 未指定 sprite)。
#define STARRY_FG_KEEP 0xFFFFu

// dialogue.flags
#define STARRY_DLG_END (1u << 0)        // arg = 结局名(name 表下标)
#define STARRY_DLG_TO_SCENE (1u << 1)   // 本句后跳转到 scene + jump
#define STARRY_DLG_BRANCH (1u << 2)     // 选择分支:数据里未使用,保留语义

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
} starry_pack_t;

typedef struct {
    uint16_t id;           // 源脚本编号(1..40,缺 7),仅用于展示与诊断
    uint16_t first_scene;  // 全局场景下标
    uint16_t scene_count;
    uint16_t first_dlg;    // 全局对白下标
    uint16_t dlg_count;
    uint16_t next;         // 下一章;STARRY_NONE = 无
} starry_chapter_t;

typedef struct {
    uint16_t bg;                // 背景图下标;STARRY_NONE = 黑屏,STARRY_BG_KEEP = 沿用
    uint8_t choice_count;       // 0 = 普通场景,>0 = 选项场景
    uint16_t first_dlg;         // 全局对白下标
    uint16_t dlg_count;
    uint16_t choice_name[2];    // name 表下标
    uint16_t choice_target[2];  // 全局场景下标
} starry_scene_t;

typedef struct {
    uint32_t text_off;
    uint16_t text_len;
    uint16_t name;    // name 表下标;STARRY_NONE = 无名
    uint16_t sprite;  // 立绘下标;STARRY_FG_KEEP = 沿用上一句
    uint8_t flags;
    uint8_t jump;
    uint16_t arg;
} starry_dialogue_t;

// 背景 flags:CG / 纯色幕这类画面本来就有内容(或本来就该是空的),不再往上叠立绘。
#define STARRY_BG_FLAG_NO_SPRITE (1u << 0)

typedef struct {
    const uint8_t *jpeg;
    uint32_t jpeg_len;
    uint16_t w;
    uint16_t h;
    bool no_sprite;
} starry_bg_t;

typedef struct {
    const uint8_t *color;  // RGB565 小端两字节,行优先,直接读 Flash
    uint32_t color_len;
    const uint8_t *mask;   // 4bpp,每行字节对齐,高半字节在左;stride = (w+1)/2
    uint32_t mask_len;
    uint16_t w;
    uint16_t h;
    uint16_t x;            // 屏幕坐标
    uint16_t y;
    uint16_t owner;        // 归属角色(name 表下标);STARRY_NONE = 归属不明
} starry_fg_t;

// 校验魔数/版本/尺寸并建立各段视图。失败返回 false(不改动 pack)。
bool starry_pack_open(starry_pack_t *pack, const uint8_t *data, uint32_t size);

void starry_pack_chapter(const starry_pack_t *pack, uint16_t index, starry_chapter_t *out);
// 章节的源脚本编号(1..40,缺 7);越界返回 0。存档列表与章节过场卡都要用它。
uint16_t starry_pack_chapter_id(const starry_pack_t *pack, uint16_t index);
void starry_pack_scene(const starry_pack_t *pack, uint16_t index, starry_scene_t *out);
void starry_pack_dialogue(const starry_pack_t *pack, uint16_t index, starry_dialogue_t *out);

// 把字符串复制进 out 并以 NUL 结尾;超长时按 UTF-8 字符边界截断。
// capacity 为 out 的总字节数(含结尾 NUL)。返回写入的字节数(不含 NUL)。
size_t starry_pack_text(const starry_pack_t *pack, uint32_t off, uint32_t len, char *out,
                        size_t capacity);
size_t starry_pack_name(const starry_pack_t *pack, uint16_t id, char *out, size_t capacity);

bool starry_pack_bg(const starry_pack_t *pack, uint16_t id, starry_bg_t *out);
bool starry_pack_fg(const starry_pack_t *pack, uint16_t id, starry_fg_t *out);

// 按源脚本编号找章节下标;找不到返回 -1。
int starry_pack_find_chapter(const starry_pack_t *pack, uint16_t id);
// 全局场景下标落在哪一章;返回章节下标,越界返回 -1,并通过 scene_out 给出章内下标。
int starry_pack_chapter_of_scene(const starry_pack_t *pack, uint16_t scene, uint16_t *scene_out);
