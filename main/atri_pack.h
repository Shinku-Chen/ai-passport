// main/atri_pack.h —— 《ATRI -My Dear Moments-》资源包的只读解析层。
//
// 包由 tools/atri_pack.py 生成,整体放在 Flash 里由固件直接读取:没有解压、没有
// 运行时 JSON 解析。本文件只做"字节 -> 结构"的纯映射,不依赖 ESP-IDF,因此可以在
// 宿主机上跑单元测试(tests/test_atri_model.c 用真实包走完整条剧情线)。
//
// 字节序与字段布局必须与 tools/atri_pack.py 一致:
//   SEC_TEXT    UTF-8 blob,字符串按 (offset,length) 寻址,不保证 NUL 结尾
//   SEC_NAME    { off u32, len u16, pad u16 } = 8 字节
//   SEC_CHAPTER { id u16, flags u8, branch_count u8, first_scene u16, scene_count u16,
//                 first_dlg u16, dlg_count u16, next u16, branch_bad u16,
//                 branch_pick u16, pad u16 } = 20 字节
//   SEC_SCENE   { bg u16, ovl u16, ovl_x i16, ovl_y i16, first_dlg u16, dlg_count u16,
//                 choice_count u8, pad u8, choice_name[2] u16, choice_jump[2] u16 } = 22 字节
//   SEC_DLG     { text_off u32, text_len u16, name u16, chr u16, flags u8, jump u8,
//                 arg u16 } = 14 字节 = 12 字节
//   SEC_BG      { off u32, len u32, w u16, h u16 }(off 相对 SEC_BG 数据区)
//   SEC_OVL     { color_off u32, color_len u32, mask_off u32, mask_len u32, w u16, h u16 }
//   SEC_CHAR    { color_off, color_len, mask_off, mask_len(全 u32), x, y, w, h(u16) }
// 背景是 JPEG(解码目标就是画布本身,不需要额外缓冲);叠加与角色立绘是无损 RGB565 +
// 4bpp 遮罩(设备端堆只有几十 KB,全屏图的 JPEG 解码缓冲拿不出来)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ATRI_PACK_MAGIC "ATRIPK01"
#define ATRI_PACK_VERSION 1u
// 叠加图格式:1 = RGB565 + 4bpp 遮罩(见 tools/atri_pack.py 的文件头说明)。
#define ATRI_OVL_FORMAT_RGB565 1

// 0xFFFF 在 chapter.next/scene.bg/scene.ovl/dialogue.name 里表示"没有"。
#define ATRI_NONE 0xFFFFu
// 背景表 0 号固定是标题画面(脚本不引用它),与 tools/atri_pack.py 的 TITLE_SOURCE 对应。
#define ATRI_BG_TITLE 0u
// 叠加表 0 号固定是 TRUE END 标题叠加(两个结局都达成后标题页显示)。
#define ATRI_OVL_TRUE_END 0u
// 角色立绘是“粘性”的:记录里写 0xFFFF 表示沿用当前立绘,不换;
// 0xFFFE 表示这一屏不画立绘(清空),其余值是立绘下标。
#define ATRI_CHAR_KEEP 0xFFFFu
#define ATRI_CHAR_NONE 0xFFFEu

// chapter.flags
#define ATRI_CH_HAS_BRANCH (1u << 0)   // 本章末按选择历史分流
#define ATRI_CH_BAD_END (1u << 1)      // 悲剧结局章
#define ATRI_CH_TRUE_END (1u << 2)     // 真正的结局章
#define ATRI_CH_HAPPY_END (1u << 3)    // 圆满结局章

// dialogue.flags
#define ATRI_DLG_END (1u << 0)         // arg = 结局名(name 表下标)
#define ATRI_DLG_TO_SCENE (1u << 1)    // 本句是场景末句,之后跳转到 scene + jump
#define ATRI_DLG_BRANCH (1u << 2)      // 本章末句:选择历史命中 next,否则 branch_bad

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
    const uint8_t *bg_data;    // JPEG 数据区
    uint32_t bg_count;
    uint32_t bg_data_size;
    const uint8_t *ovls;
    const uint8_t *ovl_data;
    uint32_t ovl_count;
    uint32_t ovl_data_size;
    const uint8_t *chars;
    const uint8_t *char_data;
    uint32_t char_count;
    uint32_t char_data_size;
    const uint8_t *meta;
    uint32_t meta_size;
} atri_pack_t;

typedef struct {
    uint16_t id;            // 源脚本编号(999/101/.../701),仅用于展示与诊断
    uint8_t flags;          // ATRI_CH_*
    uint8_t branch_count;   // 分流需要的选择项数(0 = 不分流)
    uint16_t first_scene;   // 全局场景下标
    uint16_t scene_count;
    uint16_t first_dlg;     // 全局对白下标
    uint16_t dlg_count;
    uint16_t next;          // 下一章;ATRI_NONE = 无
    uint16_t branch_bad;    // 分流未命中的目标章(通常是非番外结局章)
    uint16_t branch_pick;   // 期望的选择历史:每项 2 bit,低位在前
} atri_chapter_t;

typedef struct {
    uint16_t bg;                // 背景图下标;ATRI_NONE = 黑屏
    uint16_t ovl;               // 叠加图下标;ATRI_NONE = 无
    int16_t ovl_x;              // 叠加绘制位置(画面区坐标)
    int16_t ovl_y;
    uint16_t first_dlg;
    uint16_t dlg_count;
    uint8_t choice_count;       // 0 = 普通场景,>0 = 选项场景
    uint16_t choice_name[2];    // name 表下标
    uint16_t choice_jump[2];    // 相对当前场景的场景偏移
} atri_scene_t;

typedef struct {
    uint32_t text_off;
    uint16_t text_len;
    uint16_t name;    // name 表下标;ATRI_NONE = 旁白
    uint16_t chr;     // 角色立绘下标;ATRI_CHAR_KEEP = 沿用上一张
    uint8_t flags;
    uint8_t jump;
    uint16_t arg;
} atri_dialogue_t;

typedef struct {
    const uint8_t *jpeg;
    uint32_t jpeg_len;
    uint16_t w;
    uint16_t h;
} atri_bg_t;

typedef struct {
    const uint8_t *color; // RGB565 小端两字节,行优先,直接读 Flash
    uint32_t color_len;
    const uint8_t *mask;  // 4bpp,每行字节对齐,高半字节在左;mask_len = 0 表示整图不透明
    uint32_t mask_len;
    uint16_t w;
    uint16_t h;
} atri_ovl_t;

// 角色立绘:全身、透明底,固定画在屏幕坐标 (x, y)。
typedef struct {
    const uint8_t *color;
    uint32_t color_len;
    const uint8_t *mask;
    uint32_t mask_len;
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t h;
} atri_char_t;

// 校验魔数/版本/尺寸并建立各段视图。失败返回 false(不改动 pack)。
bool atri_pack_open(atri_pack_t *pack, const uint8_t *data, uint32_t size);

void atri_pack_chapter(const atri_pack_t *pack, uint16_t index, atri_chapter_t *out);
void atri_pack_scene(const atri_pack_t *pack, uint16_t index, atri_scene_t *out);
void atri_pack_dialogue(const atri_pack_t *pack, uint16_t index, atri_dialogue_t *out);

// 把字符串复制进 out 并以 NUL 结尾;超长时按 UTF-8 字符边界截断。
// capacity 为 out 的总字节数(含结尾 NUL)。返回写入的字节数(不含 NUL)。
size_t atri_pack_text(const atri_pack_t *pack, uint32_t off, uint32_t len, char *out,
                      size_t capacity);
size_t atri_pack_name(const atri_pack_t *pack, uint16_t id, char *out, size_t capacity);

bool atri_pack_bg(const atri_pack_t *pack, uint16_t id, atri_bg_t *out);
bool atri_pack_ovl(const atri_pack_t *pack, uint16_t id, atri_ovl_t *out);
bool atri_pack_char(const atri_pack_t *pack, uint16_t id, atri_char_t *out);

// 按源脚本编号找章节下标;找不到返回 -1。
int atri_pack_find_chapter(const atri_pack_t *pack, uint16_t id);

// 章节下标是否合法。
bool atri_pack_chapter_valid(const atri_pack_t *pack, uint16_t index);
