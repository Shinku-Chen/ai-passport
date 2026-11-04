// main/nn_pack.h —— 《男娘恋爱物语》资源包读取层（纯数据,不依赖 ESP-IDF/LVGL）。
//
// 包由 tools/nn_pack.py 生成,以二进制数组的形式编进应用镜像（.rodata,由指令
// cache 直接映射），因此运行时不需要分区查找、解压或 JSON 解析:
//
//   header  : magic "NNPK01\0\0", version u32, total size u32, section count u32
//   section : { type u32, offset u32, count u32, size u32 }
//   SEC_IMAGE(16) { off u32, jpeg_len u32, mask_len u32, w u16, h u16, kind u8, pad u8, pad u16 }
//
// 所有读取都带边界检查:损坏/截断的包只会让操作返回 false,不会越界访问
// (tests/test_nn_pack.c 用截断和畸形包覆盖这条)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NN_PACK_MAGIC "NNPK01"
#define NN_PACK_VERSION 1u

// 段类型（与 tools/nn_pack.py 一致）。
enum {
    NN_SEC_TEXT = 0,
    NN_SEC_META = 1,
    NN_SEC_IMAGE = 16,
    NN_SEC_SCENE = 17,
    NN_SEC_OP = 18,
    NN_SEC_SPEAKER = 19,
    NN_SEC_CHOICE = 20,
    NN_SEC_ENDING = 21,
    NN_SEC_COUNT = 32,
};

// 操作码。
enum {
    NN_OP_SAY = 0,      // a = speaker id（0xFFFF = 旁白），b = 文本偏移，c = 字节长度
    NN_OP_BG = 1,       // a = 图像 id
    NN_OP_EV = 2,       // a = 图像 id（整屏 CG）
    NN_OP_SPRITE = 3,   // a = 图像 id（0xFFFF = 收起立绘）
    NN_OP_CHAPTER = 4,  // b = 文本偏移，c = 字节长度
    NN_OP_JUMP = 5,     // a = 场景，b = 场景内操作下标
    NN_OP_CHOICE = 6,   // a = 选项表首下标，b = 选项数
    NN_OP_END = 7,      // a = 结局 id
    NN_OP_FX = 8,       // a = 特效种类（0 = blackout）
};

#define NN_NONE16 0xFFFFu
#define NN_CHOICE_CONTINUE 0x01u

// 图像种类（与 tools/nn_pack.py 一致）。
enum {
    NN_IMG_BG = 0,          // 背景（fit 到屏幕高度）
    NN_IMG_SPRITE = 1,      // 立绘：JPEG + 1bpp 遮罩
    NN_IMG_THUMB = 2,       // 鉴赏室缩略图（与 CG 同序）
    NN_IMG_CG = 3,          // 事件 CG（fit 到屏幕高度）
    NN_IMG_TITLE = 4,       // 标题图
};

typedef struct {
    uint32_t off;
    uint32_t jpeg_len;
    uint32_t mask_len;
    uint16_t w;
    uint16_t h;
    uint8_t kind;
    uint8_t reserved;
    uint16_t reserved2;
} nn_image_t;   // 20 字节,与包内记录逐字节对应

typedef struct {
    uint16_t first_op;
    uint16_t op_count;
    uint32_t name_off;
    uint16_t name_len;
    uint16_t reserved;
} nn_scene_t;

typedef struct {
    uint8_t type;
    uint8_t arg;
    uint16_t a;
    uint32_t b;
    uint32_t c;
} nn_op_t;

typedef struct {
    uint32_t text_off;
    uint16_t text_len;
    uint16_t scene;
    uint16_t op;
    uint8_t flags;
    uint8_t reserved;
} nn_choice_t;

// 字段顺序按 4 字节对齐排布（两个 u32 在前），否则结构体会被填充到 20 字节。
typedef struct {
    uint32_t name_off;
    uint32_t text_off;
    uint16_t name_len;
    uint16_t text_len;
    uint16_t first_scene;
    uint16_t reserved;
} nn_ending_t;  // 16 字节

typedef struct {
    const uint8_t *blob;
    size_t size;
    const uint8_t *text;
    uint32_t text_size;
    const nn_image_t *images;
    uint32_t image_count;
    const nn_scene_t *scenes;
    uint32_t scene_count;
    const nn_op_t *ops;
    uint32_t op_count;
    const nn_choice_t *choices;
    uint32_t choice_count;
    const nn_ending_t *endings;
    uint32_t ending_count;
    const uint8_t *speakers;   // { name_off u32, name_len u16, pad u16 }
    uint32_t speaker_count;
    const char *meta;
    uint32_t meta_size;
} nn_pack_t;

// 解析并绑定一个包。任何结构性错误（魔数/版本/长度/段重叠/越界）都返回 false。
bool nn_pack_init(nn_pack_t *pack, const uint8_t *blob, size_t size);

// 取 UTF-8 文本（带长度）。偏移越界返回 NULL。
const char *nn_pack_text(const nn_pack_t *pack, uint32_t off, uint32_t len);

// 取图像描述。越界返回 NULL。
const nn_image_t *nn_pack_image(const nn_pack_t *pack, uint16_t id);

// 取图像像素数据指针（JPEG 在 off 处，遮罩紧随其后）。
const uint8_t *nn_pack_image_data(const nn_pack_t *pack, const nn_image_t *img);

// 场景名（如 "chunk001"）。
const char *nn_pack_scene_name(const nn_pack_t *pack, uint32_t scene, uint16_t *len);

// 说话人名（speaker id）。NN_NONE16 或越界返回 NULL。
const char *nn_pack_speaker(const nn_pack_t *pack, uint16_t id, uint16_t *len);

// 包内记录与结构体必须逐字节对应（小端,ESP32-C3 与宿主一致）。
_Static_assert(sizeof(nn_image_t) == 20, "nn_image_t must be 20 bytes");
_Static_assert(sizeof(nn_scene_t) == 12, "nn_scene_t must be 12 bytes");
_Static_assert(sizeof(nn_op_t) == 12, "nn_op_t must be 12 bytes");
_Static_assert(sizeof(nn_choice_t) == 12, "nn_choice_t must be 12 bytes");
_Static_assert(sizeof(nn_ending_t) == 16, "nn_ending_t must be 16 bytes");

#ifdef __cplusplus
}
#endif
