// tests/host/test_pack_negative.c —— 真实 C 坏包边界回归（生产 pq_pack_init 原样链接）。
//
// 只改内存副本，不碰仓库里的资源包。覆盖：
//   1. 正文块 comp_off+comp_len 的 32 位回绕（旧写法会放行并越界读）；
//   2. 正文块在数据段末尾的合法零长边界必须仍被接受；
//   3. 图片/剧本各段起点落在 header 内、或段间重叠必须拒绝；
//   4. 零长段只要求起点合法（>= header），不参与重叠；
//   5. alias 共享同一图片 payload 是合法结构，不能被误拒。
#include "parquet_pack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
static int g_checks;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        g_checks++;                                                                      \
        if (!(cond)) {                                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                       \
            g_fail++;                                                                    \
        }                                                                                \
    } while (0)

#define CHECK_MSG(cond, msg)                                                             \
    do {                                                                                 \
        g_checks++;                                                                      \
        if (!(cond)) {                                                                   \
            printf("FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, #cond, msg);             \
            g_fail++;                                                                    \
        }                                                                                \
    } while (0)

// 包头偏移（与 main/parquet/parquet_pack.c 的解析一致）
#define SCN_H_COUNT 19
#define SCN_OFF_I 46
#define IMG_OFF_H 8
#define IMG_OFF_I 22

static uint16_t rd16(const uint8_t *b, int off)
{
    uint16_t v;
    memcpy(&v, b + off, 2);
    return v;
}

static uint32_t rd32(const uint8_t *b, int off)
{
    uint32_t v;
    memcpy(&v, b + off, 4);
    return v;
}

static void wr16(uint8_t *b, int off, uint16_t v) { memcpy(b + off, &v, 2); }
static void wr32(uint8_t *b, int off, uint32_t v) { memcpy(b + off, &v, 4); }

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *fh = fopen(path, "rb");
    if (!fh) {
        printf("cannot open %s\n", path);
        exit(2);
    }
    fseek(fh, 0, SEEK_END);
    long n = ftell(fh);
    fseek(fh, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, fh) != (size_t)n) {
        printf("short read %s\n", path);
        exit(2);
    }
    fclose(fh);
    *size = (size_t)n;
    return buf;
}

static uint8_t *dup_buf(const uint8_t *src, size_t n)
{
    uint8_t *p = malloc(n);
    memcpy(p, src, n);
    return p;
}

static bool init_pack(const uint8_t *img, size_t img_size, const uint8_t *scn,
                      size_t scn_size, char *err, size_t err_cap)
{
    pq_pack_t pack;
    return pq_pack_init(&pack, img, img_size, scn, scn_size, err, err_cap);
}

static bool init_copy(const uint8_t *img, size_t img_size, const uint8_t *scn,
                      size_t scn_size)
{
    char err[64] = {0};
    return init_pack(img, img_size, scn, scn_size, err, sizeof(err));
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 3) {
        printf("usage: %s <parquet_img.bin> <parquet_scn.bin>\n", argv[0]);
        return 2;
    }
    size_t img_size = 0, scn_size = 0;
    uint8_t *img = read_file(argv[1], &img_size);
    uint8_t *scn = read_file(argv[2], &scn_size);
    if (img_size < 64 || scn_size < 128) {
        printf("packs too short\n");
        return 2;
    }

    char err[96] = {0};
    CHECK(init_pack(img, img_size, scn, scn_size, err, sizeof(err)));
    CHECK(err[0] == 0);

    // alias 共享同一图片 payload 是既有合法结构（立绘共用身体、图片按文件去重）：
    // 多个 sprite 指向同一个 body image 不能被新的段校验误拒。
    {
        const uint16_t sprite_count = rd16(img, IMG_OFF_H + 4);
        const uint32_t off_sprites = rd32(img, IMG_OFF_I + 4);
        int dup = 0;
        for (int i = 0; i < sprite_count && dup == 0; i++) {
            uint16_t a = rd16(img, (int)off_sprites + 16 * i);
            for (int j = i + 1; j < sprite_count; j++) {
                if (rd16(img, (int)off_sprites + 16 * j) == a) {
                    dup++;
                    break;
                }
            }
        }
        CHECK(dup > 0);   // 979 个立绘只存 36 张身体
    }

    const uint32_t off_text_blocks = rd32(scn, SCN_OFF_I + 3 * 4);
    const uint32_t off_text_data = rd32(scn, SCN_OFF_I + 4 * 4);
    const uint32_t off_tags = rd32(scn, SCN_OFF_I + 5 * 4);
    const uint32_t text_data_size = off_tags - off_text_data;
    const uint32_t raw_len_orig = rd16(scn, (int)off_text_blocks + 10);
    CHECK(text_data_size > 64);

    // 1) 32 位回绕：0xFFFFFFF0 + 0x20 = 0x10 <= text_data_size（旧写法会放行）。
    {
        uint8_t *bad = dup_buf(scn, scn_size);
        wr32(bad, (int)off_text_blocks + 4, 0xFFFFFFF0u);
        wr16(bad, (int)off_text_blocks + 8, 0x20);
        wr16(bad, (int)off_text_blocks + 10, 16);
        const uint32_t wrapped = (uint32_t)(0xFFFFFFF0u + 0x20u);
        CHECK(wrapped <= text_data_size);
        char e2[96] = {0};
        CHECK(!init_pack(img, img_size, bad, scn_size, e2, sizeof(e2)));
        CHECK(e2[0] != 0);
        free(bad);
    }

    // 2) 合法边界：块正好落在正文数据段末尾且长度 0，结构上应被接受。
    {
        uint8_t *edge = dup_buf(scn, scn_size);
        wr32(edge, (int)off_text_blocks + 4, text_data_size);
        wr16(edge, (int)off_text_blocks + 8, 0);
        wr16(edge, (int)off_text_blocks + 10, raw_len_orig);
        CHECK(init_copy(img, img_size, edge, scn_size));
        free(edge);
    }

    // 3) 剧本段：段间重叠 / 起点在 header 内 / text_data 与 tags 顺序颠倒。
    {
        uint8_t *bad = dup_buf(scn, scn_size);
        wr32(bad, SCN_OFF_I + 1 * 4, rd32(bad, SCN_OFF_I));   // off_ops = off_scenes
        CHECK(!init_copy(img, img_size, bad, scn_size));
        free(bad);

        bad = dup_buf(scn, scn_size);
        wr32(bad, SCN_OFF_I + 8 * 4, 16);                     // off_routes 指向 header
        CHECK(!init_copy(img, img_size, bad, scn_size));
        free(bad);

        bad = dup_buf(scn, scn_size);
        wr32(bad, SCN_OFF_I + 5 * 4, off_text_data - 4);      // tags 跑到 text_data 前面
        CHECK(!init_copy(img, img_size, bad, scn_size));
        free(bad);
    }

    // 4) 图片段：重叠 / 起点 0 / 数据段在 header 内。
    {
        uint8_t *bad = dup_buf(img, img_size);
        wr32(bad, IMG_OFF_I + 1 * 4, rd32(bad, IMG_OFF_I));    // off_sprites = off_entries
        CHECK(!init_copy(bad, img_size, scn, scn_size));
        free(bad);

        bad = dup_buf(img, img_size);
        wr32(bad, IMG_OFF_I + 6 * 4, 0);                       // off_data = 0
        CHECK(!init_copy(bad, img_size, scn, scn_size));
        free(bad);

        bad = dup_buf(img, img_size);
        wr32(bad, IMG_OFF_I + 3 * 4, 0);                       // off_titles = 0
        CHECK(!init_copy(bad, img_size, scn, scn_size));
        free(bad);
    }

    // 5) 零长段：起点合法则接受，起点在 header 内则拒绝。
    {
        uint8_t *zero_ok = dup_buf(img, img_size);
        wr16(zero_ok, IMG_OFF_H + 10, 0);                      // title_count = 0 → 零长 titles
        CHECK(init_copy(zero_ok, img_size, scn, scn_size));
        free(zero_ok);

        uint8_t *zero_bad = dup_buf(img, img_size);
        wr16(zero_bad, IMG_OFF_H + 10, 0);
        wr32(zero_bad, IMG_OFF_I + 3 * 4, 0);                  // 零长段起点在 header
        CHECK(!init_copy(zero_bad, img_size, scn, scn_size));
        free(zero_bad);
    }

    // 6) 截断/过短的包仍然拒绝（不因新校验漏掉基本边界）。
    CHECK(!init_copy(img, img_size, scn, 100));
    CHECK(!init_copy(img, 40, scn, scn_size));

    printf("%d checks, %d failures\n", g_checks, g_fail);
    free(img);
    free(scn);
    return g_fail ? 1 : 0;
}
