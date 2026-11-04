/*
 * tests/test_eden_image.c —— 真实 eden_image.c 的解码失败/回退路径。
 *
 * 编译的是生产代码 main/eden_image.c,esp_jpeg 与 LVGL 用 tests/app_stubs 的替身,
 * 替身可以按"图片长度"注入解码失败。这里验证的是几何校验与失败语义本身:
 *   1. 正常帧:底图铺满画布,补丁块覆盖在 (x,y);
 *   2. 补丁解码失败:返回 false,画布回退到完整底图(不能留半张补丁);
 *   3. 底图解码失败:返回 false,画布清黑(不留上一帧的残片);
 *   4. 补丁坐标越界(坏包):几何校验直接拒绝,画布保持上一帧;
 *   5. JPEG 头与包内声明尺寸不符(坏包):同样拒绝且不碰画布。
 *
 * 这套用例与 Python/Pillow 的"数据等价"对照是两件事:那边证明包里的字节是源图的
 * 正确重编码,这边证明运行时的失败路径。真实 esp_jpeg 的解码结果只能上真机验证。
 */
#include "eden_image.h"

#include "eden_pack.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>

void eden_stub_reset(void);
void eden_stub_jpeg_reset(void);
void eden_stub_jpeg_fail(bool all, unsigned fail_len);

static int failures;
static int checks;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        checks++;                                                           \
        if (!(cond)) {                                                      \
            failures++;                                                     \
            fprintf(stderr, "FAIL %s:%d ", __FILE__, __LINE__);             \
            fprintf(stderr, __VA_ARGS__);                                   \
            fputc('\n', stderr);                                            \
        }                                                                   \
    } while (0)

#define BASE_LEN 40u
#define PATCH_LEN 60u
#define PATCH_W 40u
#define PATCH_H 30u

// ---------------------------------------------------------------- 合成包
typedef struct {
    uint8_t text[64];
    size_t text_len;
    uint32_t txidx[4];
    uint8_t ops[4 * 12];
    uint8_t scene[3 * 8];
    uint8_t images[2 * 16];
    uint8_t frames[2 * 6];
    uint8_t chapters[1 * 8];
    uint8_t gallery[1 * 6];
    uint8_t patches[1 * 6];
    uint8_t blob[256];
    size_t blob_len;
} small_pack_t;

static void put32(uint8_t *dst, uint32_t value)
{
    for (int byte = 0; byte < 4; byte++) {
        dst[byte] = (uint8_t)((value >> (8 * byte)) & 0xFF);
    }
}

// patch_x 允许越界,用来构造"坏包"。
static void build_small_pack(small_pack_t *p, uint16_t patch_x, uint16_t image_w)
{
    memset(p, 0, sizeof(*p));
    const char *speaker = "亮";
    const char *line1 = "第一句";
    const char *line2 = "第二句";
    size_t at = 0;
    p->txidx[0] = 0;
    memcpy(p->text + at, speaker, strlen(speaker) + 1);
    at += strlen(speaker) + 1;
    p->txidx[1] = (uint32_t)at;
    memcpy(p->text + at, line1, strlen(line1) + 1);
    at += strlen(line1) + 1;
    p->txidx[2] = (uint32_t)at;
    memcpy(p->text + at, line2, strlen(line2) + 1);
    at += strlen(line2) + 1;
    p->txidx[3] = (uint32_t)at;
    p->text_len = at;

    // 两张图:底图 264x168(内容 264x165),补丁 image_w x168(内容 PATCH_W x PATCH_H)
    const uint32_t base_off = 0;
    p->blob[p->blob_len++] = (uint8_t)(264 & 0xFF);
    p->blob[p->blob_len++] = (uint8_t)(264 >> 8);
    p->blob[p->blob_len++] = (uint8_t)(168 & 0xFF);
    p->blob[p->blob_len++] = (uint8_t)(168 >> 8);
    while (p->blob_len < BASE_LEN) {
        p->blob[p->blob_len++] = 0x11;
    }
    const uint32_t patch_off = (uint32_t)p->blob_len;
    p->blob[p->blob_len++] = (uint8_t)(image_w & 0xFF);
    p->blob[p->blob_len++] = (uint8_t)(image_w >> 8);
    p->blob[p->blob_len++] = (uint8_t)(168 & 0xFF);
    p->blob[p->blob_len++] = (uint8_t)(168 >> 8);
    while (p->blob_len < patch_off + PATCH_LEN) {
        p->blob[p->blob_len++] = 0x22;
    }

    put32(p->images + 0, base_off);
    put32(p->images + 4, BASE_LEN);
    p->images[8] = 264 & 0xFF;
    p->images[9] = 264 >> 8;
    p->images[10] = 168 & 0xFF;
    p->images[11] = 168 >> 8;
    p->images[12] = 264 & 0xFF;
    p->images[13] = 264 >> 8;
    p->images[14] = 165 & 0xFF;
    p->images[15] = 0;

    put32(p->images + 16, patch_off);
    put32(p->images + 20, PATCH_LEN);
    p->images[24] = (uint8_t)(image_w & 0xFF);
    p->images[25] = (uint8_t)(image_w >> 8);
    p->images[26] = 168 & 0xFF;
    p->images[27] = 168 >> 8;
    p->images[28] = (uint8_t)(PATCH_W & 0xFF);
    p->images[29] = (uint8_t)(PATCH_W >> 8);
    p->images[30] = (uint8_t)(PATCH_H & 0xFF);
    p->images[31] = (uint8_t)(PATCH_H >> 8);

    // 画面 0:只有底图;画面 1:底图 + 一个补丁
    p->frames[0] = 0;
    p->frames[2] = 0;
    p->frames[3] = 0;
    p->frames[4] = 0;
    p->frames[6] = 0;    // 画面 1 的 base = 0
    p->frames[8] = 0;    // patch_first
    p->frames[9] = 0;
    p->frames[10] = 1;   // patch_count

    p->patches[0] = 1;
    p->patches[2] = (uint8_t)(patch_x & 0xFF);
    p->patches[3] = (uint8_t)(patch_x >> 8);
    p->patches[4] = 10;
    p->patches[5] = 0;

    // 场景:两条 text 指令都指向画面 0/1
    p->ops[0] = 0;                     // TEXT
    p->ops[2] = 0;                     // speaker = 0
    put32(p->ops + 4, 1);
    put32(p->ops + 8, 0);              // frame 0
    p->ops[12] = 0;
    p->ops[14] = 0;
    put32(p->ops + 16, 2);
    put32(p->ops + 20, 1);             // frame 1
    put32(p->scene + 8, 0);
    put32(p->scene + 12, 2);
    p->chapters[0] = 1;
    p->gallery[0] = 0;
}

static size_t emit_small(const small_pack_t *p, uint8_t *out)
{
    struct { uint32_t kind; const void *data; size_t size; size_t count; } parts[] = {
        { EDEN_SEC_TEXT, p->text, p->text_len, 3 },
        { EDEN_SEC_TXIDX, p->txidx, 16, 4 },
        { EDEN_SEC_OPS, p->ops, 24, 2 },
        { EDEN_SEC_SCENE, p->scene, 24, 3 },
        { EDEN_SEC_IMAGE, p->images, 32, 2 },
        { EDEN_SEC_FRAME, p->frames, 12, 2 },
        { EDEN_SEC_CHAP, p->chapters, 8, 1 },
        { EDEN_SEC_GAL, p->gallery, 6, 1 },
        { EDEN_SEC_BLOB, p->blob, p->blob_len, p->blob_len },
        { EDEN_SEC_PATCH, p->patches, 6, 1 },
    };
    const size_t part_count = sizeof(parts) / sizeof(parts[0]);
    const size_t header = 20 + 10 * 16;
    size_t offset = header;
    size_t places[16];
    for (size_t index = 0; index < part_count; index++) {
        places[index] = offset;
        offset += parts[index].size;
        offset += (4 - (offset % 4)) % 4;
    }
    memset(out, 0, offset);
    memcpy(out, EDEN_PACK_MAGIC, 8);
    out[8] = (uint8_t)EDEN_PACK_VERSION;
    out[9] = 0;
    out[10] = (uint8_t)(header & 0xFF);
    out[11] = (uint8_t)((header >> 8) & 0xFF);
    out[12] = 10;
    put32(out + 16, (uint32_t)offset);
    for (size_t index = 0; index < part_count; index++) {
        uint8_t *record = out + 20 + index * 16;
        put32(record, parts[index].kind);
        put32(record + 4, (uint32_t)places[index]);
        put32(record + 8, (uint32_t)parts[index].count);
        put32(record + 12, (uint32_t)parts[index].size);
        memcpy(out + places[index], parts[index].data, parts[index].size);
    }
    return offset;
}

// ---------------------------------------------------------------- 夹具
static uint16_t s_canvas[EDEN_CANVAS_W * EDEN_CANVAS_H];
static uint16_t s_scratch[EDEN_PATCH_W * EDEN_PATCH_H];
static eden_image_t s_image;

static uint16_t base_pixel(void)
{
    return (uint16_t)(((264 & 0xFF) << 8) | (168 & 0xFF));
}

static uint16_t patch_pixel(uint16_t image_w)
{
    return (uint16_t)(((image_w & 0xFF) << 8) | (168 & 0xFF));
}

static bool setup(eden_pack_t *pack, uint8_t *blob, small_pack_t *fixture, uint16_t patch_x,
                  uint16_t image_w)
{
    eden_stub_reset();
    eden_stub_jpeg_reset();
    build_small_pack(fixture, patch_x, image_w);
    const size_t size = emit_small(fixture, blob);
    if (!eden_pack_open(pack, blob, (uint32_t)size)) {
        return false;
    }
    lv_obj_t *screen = lv_screen_active();
    if (!eden_image_init(&s_image, screen, s_canvas, s_scratch)) {
        return false;
    }
    eden_image_set_pack(&s_image, pack);
    return true;
}

// ---------------------------------------------------------------- 用例
static void test_success_and_patch(void)
{
    eden_pack_t pack;
    uint8_t blob[4096];
    small_pack_t fixture;
    CHECK(setup(&pack, blob, &fixture, 10, 104), "初始化失败");

    CHECK(eden_image_draw(&s_image, 0), "无补丁帧应绘制成功");
    CHECK(s_canvas[0] == base_pixel(), "底图应铺满画布");

    CHECK(eden_image_draw(&s_image, 1), "带补丁帧应绘制成功");
    const uint16_t in_patch = s_canvas[10 * EDEN_CANVAS_W + 10];
    const uint16_t outside = s_canvas[0];
    CHECK(in_patch == patch_pixel(104), "补丁区域应被覆盖(%04x)", in_patch);
    CHECK(outside == base_pixel(), "补丁之外应保持底图(%04x)", outside);
}

static void test_patch_decode_failure_falls_back(void)
{
    eden_pack_t pack;
    uint8_t blob[4096];
    small_pack_t fixture;
    CHECK(setup(&pack, blob, &fixture, 10, 104), "初始化失败");
    CHECK(eden_image_draw(&s_image, 0), "先画一张正常帧");

    eden_stub_jpeg_fail(false, PATCH_LEN);
    CHECK(!eden_image_draw(&s_image, 1), "补丁解码失败必须返回 false");
    CHECK(s_canvas[0] == base_pixel(), "失败后应回退到完整底图");
    CHECK(s_canvas[10 * EDEN_CANVAS_W + 10] == base_pixel(),
          "失败后不能留下半张补丁");
    eden_stub_jpeg_fail(false, 0);
}

static void test_base_decode_failure_clears(void)
{
    eden_pack_t pack;
    uint8_t blob[4096];
    small_pack_t fixture;
    CHECK(setup(&pack, blob, &fixture, 10, 104), "初始化失败");
    CHECK(eden_image_draw(&s_image, 1), "先画一张正常帧");

    eden_stub_jpeg_fail(false, BASE_LEN);
    CHECK(!eden_image_draw(&s_image, 0), "底图解码失败必须返回 false");
    CHECK(s_canvas[0] == 0 && s_canvas[EDEN_CANVAS_W * 100 + 200] == 0,
          "底图失败应清黑,不能留上一帧的残片");
    eden_stub_jpeg_fail(false, 0);
}

static void test_bad_patch_geometry_rejected(void)
{
    eden_pack_t pack;
    uint8_t blob[4096];
    small_pack_t fixture;
    // 补丁 x=260 + 宽 40 > 264:坏包
    CHECK(setup(&pack, blob, &fixture, 260, 104), "初始化失败");
    CHECK(eden_image_draw(&s_image, 0), "先画一张正常帧");
    const uint16_t before = s_canvas[0];
    CHECK(!eden_image_draw(&s_image, 1), "补丁越界必须被几何校验拒绝");
    CHECK(s_canvas[0] == before, "坏包被拒时不能破坏当前画面");
}

static void test_header_mismatch_rejected(void)
{
    eden_pack_t pack;
    uint8_t blob[4096];
    small_pack_t fixture;
    CHECK(setup(&pack, blob, &fixture, 10, 104), "初始化失败");
    // 包声明补丁宽 104,但把 JPEG 头里的宽度改成 96:头预检必须拒绝
    fixture.blob[BASE_LEN] = 96;
    fixture.blob[BASE_LEN + 1] = 0;
    const size_t size = emit_small(&fixture, blob);
    CHECK(eden_pack_open(&pack, blob, (uint32_t)size), "改头后包结构仍应可解析");
    eden_image_set_pack(&s_image, &pack);
    CHECK(eden_image_draw(&s_image, 0), "先画一张正常帧");
    CHECK(!eden_image_draw(&s_image, 1), "JPEG 头与包声明不符应被拒绝");
}


// CG 全屏分带：4 条 320x60 拼成 320x240，四边无黑边（cover），且每带 y 区间正确。
static void test_cg_bands(void)
{
    eden_pack_t pack;
    uint8_t blob[4096];
    small_pack_t fixture;
    CHECK(setup(&pack, blob, &fixture, 10, 104), "初始化失败");
    CHECK(eden_image_draw(&s_image, 0), "底图应绘制成功");

    static uint16_t band[EDEN_CG_BAND_W * EDEN_CG_BAND_H];
    CHECK(EDEN_CG_BAND_W * EDEN_CG_BAND_H * 2 == 38400, "band buffer must be 38,400 B");
    for (int b = 0; b < EDEN_CG_BANDS; b++) {
        memset(band, 0xA5, sizeof(band));
        CHECK(eden_image_cg_band(&s_image, band, b), "band %d 生成失败", b);
        int empty = 0;
        for (int x = 0; x < EDEN_CG_BAND_W; x++) {
            if (band[x] == 0) empty++;                                    // 首行
            if (band[(EDEN_CG_BAND_H - 1) * EDEN_CG_BAND_W + x] == 0) empty++;  // 末行
        }
        for (int y = 0; y < EDEN_CG_BAND_H; y++) {
            if (band[y * EDEN_CG_BAND_W] == 0) empty++;                   // 左列
            if (band[y * EDEN_CG_BAND_W + EDEN_CG_BAND_W - 1] == 0) empty++;    // 右列
        }
        CHECK(empty == 0, "band %d 四边有 %d 个空白像素", b, empty);
        CHECK(band[0] != 0xA5, "band %d 没有被写入", b);
    }
}

int main(void)
{
    test_success_and_patch();
    test_patch_decode_failure_falls_back();
    test_base_decode_failure_clears();
    test_bad_patch_geometry_rejected();
    test_header_mismatch_rejected();
    test_cg_bands();
    if (failures != 0) {
        fprintf(stderr, "%d/%d 项检查失败\n", failures, checks);
        return 1;
    }
    printf("eden image: OK (%d checks)\n", checks);
    return 0;
}
