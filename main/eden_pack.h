// main/eden_pack.h —— eden* 资源包的只读解析层。
//
// 包由 tools/eden_pack.py 生成,整块放进 Flash 由固件直接读取:没有解压、没有运行时
// JSON 解析、没有整包载入 RAM。本文件只做"字节 -> 结构"的纯映射,不依赖 ESP-IDF,
// 可以在宿主机上跑单测(tests/test_eden_pack.c)。
//
// 字节序与字段布局必须与 tools/eden_pack.py 完全一致:
//   header  "EDENPK01" | version u16 | header_size u16 | section_count u16
//           | reserved u16 | total_size u32                      (20 B)
//   section { type u32, offset u32, count u32, size u32 }        (16 B × N)
//
//   SEC_TEXT   UTF-8 字符串连排,每条以 NUL 结尾,按下标引用
//   SEC_TXIDX  u32[N+1] 指向 SEC_TEXT 的字节偏移
//   SEC_OPS    12 B 指令记录 { kind u8, a u8, b u16, c u32, d u32 }
//   SEC_SCENE  u32[scenes+1] 场景 -> 指令区间(场景号 1 起,下标 0 不用)
//   SEC_IMAGE  16 B { data_off u32, data_len u32, w u16, h u16, cw u16, ch u16 }
//              w/h 是交给 JPEG 解码器的存储尺寸(8 对齐),cw/ch 是可见内容尺寸
//   SEC_FRAME  6 B { base u16, patch_first u16, patch_count u8, reserved u8 }
//   SEC_PATCH  6 B { image u16, x u16, y u16 }(补丁贴着 (x,y) 盖在底图上)
//   SEC_CHAP   8 B { scene u16, name u16, op u16, reserved u16 }
//   SEC_GAL    6 B { frame u16, group u16, label u16 }
//   SEC_BLOB   载荷区(JPEG 字节)
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EDEN_PACK_MAGIC "EDENPK01"
#define EDEN_PACK_VERSION 1u

// 段类型(与 tools/eden_pack.py 的 SEC_* 一致)。
enum {
    EDEN_SEC_TEXT = 0,
    EDEN_SEC_TXIDX = 1,
    EDEN_SEC_OPS = 2,
    EDEN_SEC_SCENE = 3,
    EDEN_SEC_IMAGE = 4,
    EDEN_SEC_FRAME = 5,
    EDEN_SEC_CHAP = 6,
    EDEN_SEC_GAL = 7,
    EDEN_SEC_BLOB = 8,
    EDEN_SEC_PATCH = 9,
    EDEN_SEC_COUNT = 10,
};

// 剧本指令种类(见 tools/eden_pack.py 的 OP_*)。
enum {
    EDEN_OP_TEXT = 0,
    EDEN_OP_SET = 1,
    EDEN_OP_IF = 2,
    EDEN_OP_GOTO = 3,
    EDEN_OP_JUMP = 4,
    EDEN_OP_END = 5,
};

// 条件比较符。
enum {
    EDEN_CMP_EQ = 0,
    EDEN_CMP_LE = 1,
};

#define EDEN_NO_PATCH 0xFFFFu

typedef struct {
    uint32_t offset;
    uint32_t count;      // 记录条数(count 为 0 的段也合法)
    uint32_t size;       // 段的字节数
} eden_section_t;

typedef struct {
    const uint8_t *base;
    uint32_t size;
    eden_section_t sections[EDEN_SEC_COUNT];
} eden_pack_t;

typedef struct {
    const uint8_t *data;   // JPEG 字节(指向 Flash)
    uint32_t len;
    uint16_t width;        // 存储宽度(8 对齐,解码缓冲按它算)
    uint16_t height;       // 存储高度(8 对齐)
    uint16_t content_w;    // 可见宽度
    uint16_t content_h;    // 可见高度
} eden_pack_image_t;

typedef struct {
    uint16_t base;         // SEC_IMAGE 下标
    uint16_t patch_first;  // 第一条补丁在 SEC_PATCH 里的下标
    uint16_t patch_count;  // 补丁条数(0 = 只画底图)
} eden_frame_t;

typedef struct {
    uint16_t image;
    uint16_t x;
    uint16_t y;
} eden_patch_t;

typedef struct {
    uint16_t scene;        // 场景号(1 起,对应 catalog 的 scene id)
    uint16_t name;         // SEC_TEXT 下标:章节名
    uint16_t op;           // 该章第一句的指令下标(剧情鉴赏从这里开始)
} eden_chapter_t;

typedef struct {
    uint16_t frame;        // SEC_FRAME 下标,CG 全屏与缩略图都用它
    uint16_t group;        // 归组编号(解锁位按组计)
    uint16_t label;        // SEC_TEXT 下标
} eden_gallery_t;

// 一条指令。字段含义随 kind 变化(见 tools/eden_pack.py 的说明):
//   TEXT  b=说话人名下标, c=正文下标, d=画面下标(a 保留为 0)
//   SET   b=变量下标, c=值
//   IF    a=比较符, b=变量下标, c=值, d=目标指令(本场景绝对下标)
//   GOTO  d=目标指令
//   JUMP  c=目标场景号
//   END   -
typedef struct {
    uint8_t kind;
    uint8_t a;
    uint16_t b;
    uint32_t c;
    uint32_t d;
} eden_op_t;

// 校验魔数/版本/长度并建立各段视图。失败返回 false(不改动 pack)。
bool eden_pack_open(eden_pack_t *pack, const void *data, uint32_t size);

// 段视图(内部用,也便于宿主机测试断言段的尺寸)。
bool eden_pack_section(const eden_pack_t *pack, uint8_t type,
                       const uint8_t **data, uint32_t *count, uint32_t *size);

uint16_t eden_pack_scene_count(const eden_pack_t *pack);
uint16_t eden_pack_chapter_count(const eden_pack_t *pack);
uint16_t eden_pack_gallery_count(const eden_pack_t *pack);
uint16_t eden_pack_frame_count(const eden_pack_t *pack);
uint16_t eden_pack_text_count(const eden_pack_t *pack);

// 场景 -> 指令区间。scene 越界返回 false。
bool eden_pack_scene_range(const eden_pack_t *pack, uint16_t scene,
                           uint32_t *first, uint32_t *count);

// 取一条指令。越界返回 false。
bool eden_pack_op(const eden_pack_t *pack, uint32_t index, eden_op_t *out);

// 取一条字符串(包内 NUL 结尾)。越界返回 NULL。
const char *eden_pack_text(const eden_pack_t *pack, uint32_t index);

bool eden_pack_image(const eden_pack_t *pack, uint16_t index, eden_pack_image_t *out);
bool eden_pack_frame(const eden_pack_t *pack, uint16_t index, eden_frame_t *out);
bool eden_pack_patch(const eden_pack_t *pack, uint16_t index, eden_patch_t *out);

// 取画面并做几何校验:底图内容必须放得进 frame_w x frame_h,每条补丁必须整块落在画面内
// 且不超过解码暂存条宽度。坏包在绘制之前就被拒绝,不会走到 memcpy。
bool eden_pack_frame_checked(const eden_pack_t *pack, uint16_t index,
                             uint16_t frame_w, uint16_t frame_h, uint16_t patch_w_max,
                             eden_frame_t *out);
bool eden_pack_chapter(const eden_pack_t *pack, uint16_t index, eden_chapter_t *out);
bool eden_pack_gallery(const eden_pack_t *pack, uint16_t index, eden_gallery_t *out);

// 场景号 -> 场景下标(1 基场景偶尔不连续时用);找不到返回 0。
uint16_t eden_pack_scene_index_of(const eden_pack_t *pack, uint16_t scene);
