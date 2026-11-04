// main/anemoi/anemoi_pack.h —— 《anemoi》资源包只读解析层（纯数据，不依赖 ESP-IDF/LVGL）。
//
// 包由 tools/anemoi_pack.py 生成，整块编进应用镜像（.rodata）。运行时只做
// "字节 -> 结构" 映射：没有解压、没有运行时 JSON 解析、也不把包载入 RAM。
//
// 字节布局（小端）：
//   header  "ANEMPK1\0" | version u16 | header_size u16 | section_count u16
//           | flags u16 | total_size u32 | crc32 u32 | reserved u32      (28 B)
//   section { type u32, offset u32, count u32, size u32 }               (16 B x N)
// 段表见 anem_sec_t / ANEM_SEC_*；每段的 count 与 size 语义随段类型不同，
// 具体含义在 tools/anemoi_pack.py 与 docs/development/engineering/galgame-anemoi.md。
//
// 所有读取都做边界检查：损坏/截断的包只会让访问函数返回 false，不会越界
// （tests/test_anemoi_pack.c 覆盖截断、越界、错版本、CRC 不符）。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANEM_PACK_MAGIC "ANEMPK1"
#define ANEM_PACK_VERSION 1u
#define ANEM_HEADER_SIZE 28u

enum {
    ANEM_SEC_TEXT = 0,
    ANEM_SEC_TXIDX = 1,
    ANEM_SEC_OPS = 2,
    ANEM_SEC_SCENE = 3,
    ANEM_SEC_SCNID = 4,
    ANEM_SEC_ENTRY = 5,
    ANEM_SEC_CHAP = 6,
    ANEM_SEC_VAR = 7,
    ANEM_SEC_EXPR = 8,
    ANEM_SEC_COND = 9,
    ANEM_SEC_CALL = 10,
    ANEM_SEC_SET = 11,
    ANEM_SEC_CHOICE = 12,
    ANEM_SEC_NAMELIST = 13,
    ANEM_SEC_POSE = 14,
    ANEM_SEC_ACTOR = 15,
    ANEM_SEC_IMAGE = 16,
    ANEM_SEC_BLOB = 17,
    ANEM_SEC_REQ = 18,
    ANEM_SEC_HELPER = 19,
    ANEM_SEC_MICRO = 20,
    ANEM_SEC_GAL = 21,
    ANEM_SEC_NAMEMAP = 22,
    ANEM_SEC_META = 23,
    ANEM_SEC_COUNT = 24,
};

// 指令种类（与 tools/anemoi_pack.py 的 OP_* 一致）。
enum {
    ANEM_OP_TEXT = 0,        // a = 说话人文本 id（0xFFFF = 旁白），b = 正文文本 id
    ANEM_OP_BG = 1,          // flags bit0 = CG 层；a = 图像名文本 id；b = 图像下标（0xFFFF = 缺图）
    ANEM_OP_PORTRAIT = 2,    // flags = replace/change/delete；a = NAMELIST 起始下标；b = 名字个数
    ANEM_OP_COPY_BODIES = 3, // flags = 源 copyBodies 的 value
    ANEM_OP_COPY_CG = 4,     // flags = 源 copyCG 的 value
    ANEM_OP_COMMIT = 5,      // 清 changedBodies / changedCG
    ANEM_OP_INIT_TIME = 6,   // flags bit0 = resetPortraits
    ANEM_OP_IF_FALSE = 7,    // a = 条件 id；b = 目标指令
    ANEM_OP_IF_TRUE = 8,     // a = 条件 id；b = 目标指令
    ANEM_OP_GOTO = 9,        // b = 目标指令
    ANEM_OP_CALL = 10,       // b = 目标指令（场景内调用）
    ANEM_OP_RETURN = 11,
    ANEM_OP_JUMP = 12,       // a = 源场景号；b = catalog entry 号；flags bit0 = entry 来自表达式 c
    ANEM_OP_CALL_SCENE = 13, // 同上
    ANEM_OP_CHOICE = 14,     // a = CHOICE 表下标
    ANEM_OP_BOARD = 15,
    ANEM_OP_END = 16,
    ANEM_OP_OMITTED = 17,
    ANEM_OP_SKIP_GAME = 18,  // a = 1 rps / 2 quiz / 3 snake / 4 race
    ANEM_OP_ROUTE_CHECK = 19,
    ANEM_OP_SET = 20,        // a = SET 表下标
    ANEM_OP_NATIVE = 21,     // a = CALL 表下标（表达式字节码）
    ANEM_OP_ARRIVE_CHOICE = 22, // a = 选项值 11；b = 目标指令（arrive 尾声的轮询）
    ANEM_OP_BOUNDARY = 23,   // 超出本卷的跳转（显示分卷边界页）
    ANEM_OP_SET_DYN = 24,    // 动态键赋值：a = base 文本 id，b = 索引表达式，c = 值表达式
};

// 图像载荷种类。
enum {
    ANEM_IMG_JPEG = 0,   // 背景 / CG / 标题图；w/h 为可见内容尺寸
    ANEM_IMG_BLOCK = 1,  // 立绘 / 表情：调色板 + 逐行滤波 + 8 行一块 zlib
    ANEM_IMG_SOLID = 2,  // 纯色（BLOB 里两个字节 RGB565 小端）
};

enum {
    ANEM_POSE_REPLACE = 0,
    ANEM_POSE_CHANGE = 1,
    ANEM_POSE_DELETE = 2,
};

#define ANEM_NONE16 0xFFFFu
#define ANEM_NONE32 0xFFFFFFFFu

#pragma pack(push, 1)
typedef struct {
    uint32_t off;
    uint32_t count;
    uint32_t size;
} anem_section_t;

typedef struct {
    uint8_t kind;
    uint8_t flags;
    uint16_t a;
    uint32_t b;
    uint32_t c;
} anem_op_t;   // 12 B

typedef struct {
    uint16_t name;
    uint16_t dress;
    uint16_t body;
    uint16_t face;
    uint16_t fx;
    uint16_t fy;
    uint16_t fw;
    uint16_t fh;
    uint16_t base_w;
    uint16_t base_h;
    uint16_t lay0;
    uint16_t lay1;
} anem_pose_t;  // 24 B

typedef struct {
    uint16_t name;
    uint16_t default_dress;
    uint16_t flags;
    uint16_t reserved;
} anem_actor_t;  // 8 B

typedef struct {
    uint32_t off;
    uint32_t len;
    uint16_t w;
    uint16_t h;
    uint8_t kind;
    uint8_t flags;
    uint16_t reserved;
} anem_image_t;  // 16 B

typedef struct {
    uint16_t scene;
    uint16_t entry;
    uint32_t op;
} anem_entry_t;  // 8 B

typedef struct {
    uint16_t scene;
    uint16_t name;
    uint16_t day;
    uint16_t source;
    uint32_t first_op;
} anem_chap_t;   // 12 B

typedef struct {
    uint16_t var;
    uint16_t flags;
    uint32_t expr;
} anem_set_t;    // 8 B

typedef struct {
    uint32_t labels;
    uint32_t enabled;
    uint32_t target;
    uint32_t reserved;
} anem_choice_t; // 16 B

typedef struct {
    uint16_t name;
    uint16_t params;
    uint32_t micro_off;
    uint16_t micro_count;
    uint16_t scope;      // 0xFFFF = 全局；否则仅该源场景可见
} anem_helper_t; // 12 B

typedef struct {
    uint8_t kind;
    uint8_t flags;
    uint16_t a;
    uint32_t b;
    uint32_t c;
} anem_micro_t;  // 12 B（helper 体内的指令）

typedef struct {
    uint16_t image;
    uint16_t group;
    uint16_t label;
    uint16_t reserved;
} anem_gal_t;    // 8 B

typedef struct {
    uint16_t key;
    uint16_t applicant;
    uint16_t body;
    uint16_t flags;
} anem_req_t;    // 8 B

typedef struct {
    uint16_t name;
    uint16_t image;
} anem_namemap_t; // 4 B
#pragma pack(pop)

typedef struct {
    const uint8_t *base;
    uint32_t size;
    uint32_t crc32;
    anem_section_t sec[ANEM_SEC_COUNT];
} anem_pack_t;

// 打开并校验魔数 / 版本 / 段表 / CRC / 段边界。失败返回 false，且不改动调用方数据。
bool anem_pack_open(anem_pack_t *pack, const void *data, size_t size);

// 段视图（测试与固件共用）。
bool anem_pack_section(const anem_pack_t *pack, uint8_t type, const uint8_t **data,
                       uint32_t *count, uint32_t *size);

uint32_t anem_pack_text_count(const anem_pack_t *pack);
uint32_t anem_pack_op_count(const anem_pack_t *pack);
uint32_t anem_pack_scene_count(const anem_pack_t *pack);
uint32_t anem_pack_image_count(const anem_pack_t *pack);
uint32_t anem_pack_pose_count(const anem_pack_t *pack);
uint32_t anem_pack_chapter_count(const anem_pack_t *pack);
uint32_t anem_pack_gallery_count(const anem_pack_t *pack);

// 文本：越界或没有 NUL 结尾返回 NULL。
const char *anem_pack_text(const anem_pack_t *pack, uint16_t id);

// 指令：越界返回 false。
bool anem_pack_op(const anem_pack_t *pack, uint32_t index, anem_op_t *out);

// 场景槽（按镜头顺序）：scene id -> op 区间。scene 不在包内返回 false。
bool anem_pack_scene_range(const anem_pack_t *pack, uint16_t scene, uint32_t *first,
                           uint32_t *count);
// 源 catalog entry -> 全局指令下标；找不到返回 ANEM_NONE32。
uint32_t anem_pack_entry_op(const anem_pack_t *pack, uint16_t scene, uint16_t entry);
// 源场景名（如 "010_プロローグ0701"）-> 源场景号；找不到返回 ANEM_NONE16。
uint16_t anem_pack_scene_by_source(const anem_pack_t *pack, const char *source);

// 章节元数据（全部 264 个源场景都进包，但不一定都有 op 区间）。
bool anem_pack_chapter(const anem_pack_t *pack, uint32_t index, anem_chap_t *out);
bool anem_pack_chapter_for(const anem_pack_t *pack, uint16_t scene, anem_chap_t *out);

// 图像：越界返回 false。
bool anem_pack_image(const anem_pack_t *pack, uint16_t id, anem_image_t *out);
const uint8_t *anem_pack_blob(const anem_pack_t *pack, uint32_t off, uint32_t len);

// 名字 -> 图像（运行时的时间后缀解析走这里）。找不到返回 ANEM_NONE16。
uint16_t anem_pack_image_by_name(const anem_pack_t *pack, const char *name);

// 名字表的精确查找：同时给出名字的 TEXT id 与图像 id。
// 背景时段解析要把“解析后的名字”存进状态（存档/重绘都用它），所以需要 text id。
bool anem_pack_image_entry(const anem_pack_t *pack, const char *name, uint16_t *text_id,
                           uint16_t *image_id);

// 源 exists_image() 语义：裸 base 名如果只是时段变体的别名，答案是不存在。
bool anem_pack_exists_image(const anem_pack_t *pack, const char *name);

// 姿势：按 (名字, dress) 查；回退规则与源 poseKey() 一致：
//   n@d -> <prefix>_01@d -> n@default_dress -> <prefix>_01@default_dress。
// 包内 POSE 按 (名字, dress) 升序排列，查找是二分。
typedef struct {
    uint16_t body;
    uint16_t face;
    uint16_t fx, fy, fw, fh;
    uint16_t base_w, base_h;
    uint16_t lay0, lay1;
} anem_pose_view_t;
bool anem_pack_pose(const anem_pack_t *pack, const char *name, int dress,
                    int default_dress, anem_pose_view_t *out);
bool anem_pack_actor(const anem_pack_t *pack, uint16_t index, anem_actor_t *out);
uint16_t anem_pack_actor_default_dress(const anem_pack_t *pack, const char *actor);

// 表项访问（脚本解释器用）。
bool anem_pack_var(const anem_pack_t *pack, uint16_t index, uint16_t *text_id);
bool anem_pack_set(const anem_pack_t *pack, uint16_t index, anem_set_t *out);
bool anem_pack_choice(const anem_pack_t *pack, uint16_t index, anem_choice_t *out);
bool anem_pack_helper(const anem_pack_t *pack, uint32_t index, anem_helper_t *out);
bool anem_pack_micro(const anem_pack_t *pack, uint16_t index, anem_micro_t *out);
bool anem_pack_gallery(const anem_pack_t *pack, uint16_t index, anem_gal_t *out);
bool anem_pack_request(const anem_pack_t *pack, uint32_t index, anem_req_t *out);
uint32_t anem_pack_expr_size(const anem_pack_t *pack);
const uint8_t *anem_pack_expr_base(const anem_pack_t *pack);
bool anem_pack_expr_at(const anem_pack_t *pack, uint32_t off, const uint8_t **ptr);
bool anem_pack_cond(const anem_pack_t *pack, uint16_t index, uint32_t *expr_off);
bool anem_pack_call(const anem_pack_t *pack, uint16_t index, uint32_t *expr_off);
bool anem_pack_namelist(const anem_pack_t *pack, uint16_t index, uint32_t *expr_off);

// 包内 meta 文本里某个 key 的值（只读指针，可能不 NUL 结尾 -> 用 out_len）。
bool anem_pack_meta(const anem_pack_t *pack, const char *key, const char **value,
                    uint32_t *out_len);

// 结构体必须与包内记录逐字节对应。
_Static_assert(sizeof(anem_section_t) == 12, "section record must be 12 bytes");
_Static_assert(sizeof(anem_op_t) == 12, "op record must be 12 bytes");
_Static_assert(sizeof(anem_pose_t) == 24, "pose record must be 24 bytes");
_Static_assert(sizeof(anem_actor_t) == 8, "actor record must be 8 bytes");
_Static_assert(sizeof(anem_image_t) == 16, "image record must be 16 bytes");
_Static_assert(sizeof(anem_entry_t) == 8, "entry record must be 8 bytes");
_Static_assert(sizeof(anem_chap_t) == 12, "chapter record must be 12 bytes");
_Static_assert(sizeof(anem_set_t) == 8, "set record must be 8 bytes");
_Static_assert(sizeof(anem_choice_t) == 16, "choice record must be 16 bytes");
_Static_assert(sizeof(anem_helper_t) == 12, "helper record must be 12 bytes");
_Static_assert(sizeof(anem_micro_t) == 12, "micro record must be 12 bytes");
_Static_assert(sizeof(anem_gal_t) == 8, "gallery record must be 8 bytes");
_Static_assert(sizeof(anem_req_t) == 8, "request record must be 8 bytes");
_Static_assert(sizeof(anem_namemap_t) == 4, "namemap record must be 4 bytes");

#ifdef __cplusplus
}
#endif
