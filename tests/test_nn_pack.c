// tests/test_nn_pack.c —— 资源包校验回归（fail closed）。
//
// 独立审查用探针证明了旧实现在几类坏包上 **fail open**：
//   * nn_pack_init 没有赋值 pack->blob/size，nn_pack_image_data 的二次边界恒失效；
//   * 图像记录按“整段（含记录表）大小”校验，off 可以指到下一段的字节；
//   * 段重叠、段数=32（实际 8）都能通过。
// 这个测试把那些案例接过来，要求**全部被拒绝**，同时真实包必须仍然可读。
//
// 做法：读入真实包 → 复制 → 定向破坏一处 → 期望 nn_pack_init 返回 false。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nn_pack.h"

#define DEFAULT_PACK "assets/nn/nn_pack.bin"
#define SEC_IMAGE 16u
#define SEC_SCENE 17u
#define SEC_TEXT 0u

static int failures = 0;
#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                     \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
            failures++;                                                       \
        }                                                                     \
    } while (0)

static uint8_t *load(const char *p, size_t *n)
{
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long s = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)s);
    if (!b || fread(b, 1, (size_t)s, f) != (size_t)s) { free(b); fclose(f); return NULL; }
    fclose(f);
    *n = (size_t)s;
    return b;
}
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static uint8_t *find_sec(uint8_t *blob, uint32_t sections, uint32_t type)
{
    for (uint32_t i = 0; i < sections; ++i) {
        uint8_t *rec = blob + 20 + 16 * i;
        if (get32(rec) == type) return rec;
    }
    return NULL;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : DEFAULT_PACK;
    size_t n = 0;
    uint8_t *blob = load(path, &n);
    if (!blob) {
        printf("cannot read %s\n", path);
        return 2;
    }

    // ---- 0. 真实包：必须仍然可读，且 blob/size 已绑定
    nn_pack_t pack;
    CHECK(nn_pack_init(&pack, blob, n), "real pack rejected");
    CHECK(pack.blob == blob && pack.size == n, "pack->blob/size not bound");
    CHECK(pack.image_count == 77, "expected 77 images, got %u", pack.image_count);
    CHECK(pack.scene_count == 13, "expected 13 scenes, got %u", pack.scene_count);
    const uint8_t *img0 = nn_pack_image_data(&pack, &pack.images[0]);
    CHECK(img0 != NULL, "first image data pointer is NULL");
    CHECK(img0 >= blob && img0 + pack.images[0].jpeg_len <= blob + n, "image data outside the blob");
    printf("  real pack: %u images / %u scenes, blob+size bound, first image inside\n",
           pack.image_count, pack.scene_count);

    uint8_t *copy = malloc(n);
    CHECK(copy != NULL, "alloc");
    if (!copy) return 1;
    const uint32_t sections = get32(blob + 16);
    uint8_t *isec = find_sec(blob, sections, SEC_IMAGE);
    uint8_t *ssec = find_sec(blob, sections, SEC_SCENE);
    CHECK(isec && ssec, "section table incomplete");
    if (!isec || !ssec) return 1;
    const uint32_t image_off = get32(isec + 4);
    const uint32_t image_count = get32(isec + 8);
    const uint32_t image_bytes = get32(isec + 12);
    const uint32_t data_bytes = image_bytes - image_count * 20u;

    // ---- 1. 图像 off 越过数据区（进入下一段）
    memcpy(copy, blob, n);
    uint8_t *rec = copy + image_off;           // 第一条图像记录
    put32(rec + 0, data_bytes - 1);            // off 合法但与 jlen 相加越界
    CHECK(!nn_pack_init(&pack, copy, n), "image off+jlen past the data area accepted");

    memcpy(copy, blob, n);
    rec = copy + image_off;
    put32(rec + 0, data_bytes);                // off 正好等于数据区大小
    CHECK(!nn_pack_init(&pack, copy, n), "image off == data area size accepted");

    // ---- 2. 段重叠：把 SCENE 段挪到 IMAGE 段内部
    memcpy(copy, blob, n);
    put32(find_sec(copy, sections, SEC_SCENE) + 4, image_off + 8);
    CHECK(!nn_pack_init(&pack, copy, n), "overlapping sections accepted");

    // ---- 3. 段数与已知类型数不符（32 个记录，实际 8）
    memcpy(copy, blob, n);
    put32(copy + 16, 32);
    CHECK(!nn_pack_init(&pack, copy, n), "section count 32 accepted");

    // ---- 4. 段表与数据重叠：TEXT 段指回段表内部
    memcpy(copy, blob, n);
    put32(find_sec(copy, sections, SEC_TEXT) + 4, 20);
    CHECK(!nn_pack_init(&pack, copy, n), "section pointing into the table accepted");

    // ---- 5. 重复段类型
    memcpy(copy, blob, n);
    put32(find_sec(copy, sections, SEC_SCENE), SEC_IMAGE);
    CHECK(!nn_pack_init(&pack, copy, n), "duplicate section type accepted");

    // ---- 6. 截断 / 魔数 / 版本 / 总长
    CHECK(!nn_pack_init(&pack, blob, n - 1), "truncated pack accepted");
    CHECK(!nn_pack_init(&pack, blob, 19), "19-byte pack accepted");
    memcpy(copy, blob, n);
    copy[0] = 'X';
    CHECK(!nn_pack_init(&pack, copy, n), "bad magic accepted");
    memcpy(copy, blob, n);
    put32(copy + 8, 99);
    CHECK(!nn_pack_init(&pack, copy, n), "bad version accepted");
    memcpy(copy, blob, n);
    put32(copy + 12, (uint32_t)n + 16);
    CHECK(!nn_pack_init(&pack, copy, n), "size field mismatch accepted");

    // ---- 7. 记录数乘法溢出：SCENE 段记录数写成 0xFFFFFFF
    memcpy(copy, blob, n);
    put32(find_sec(copy, sections, SEC_SCENE) + 8, 0x0FFFFFFFu);
    CHECK(!nn_pack_init(&pack, copy, n), "record count overflow accepted");

    // ---- 8. 图像段记录数大到记录表本身放不下
    memcpy(copy, blob, n);
    put32(find_sec(copy, sections, SEC_IMAGE) + 8, image_bytes / 20u + 1u);
    CHECK(!nn_pack_init(&pack, copy, n), "image record table overflow accepted");

    // ---- 9. 破坏一个操作码（未知 op）必须被拒绝
    memcpy(copy, blob, n);
    uint8_t *osec = find_sec(copy, sections, 18u);
    CHECK(osec != NULL, "op section missing");
    if (osec) {
        put32(osec + 4, get32(osec + 4));   // no-op，保持结构
        uint8_t *ops = copy + get32(osec + 4);
        ops[0] = 0x7F;                       // 未知类型
        CHECK(!nn_pack_init(&pack, copy, n), "unknown opcode accepted");
    }

    // ---- 10. 未知段类型（31）必须拒绝：NN_SEC_COUNT 只是表容量，不是合法类型上界
    memcpy(copy, blob, n);
    put32(find_sec(copy, sections, 1u) /* META */, 31u);
    CHECK(!nn_pack_init(&pack, copy, n), "unknown section type 31 accepted");

    // ---- 11. 记录内文本引用越界（CHOICE / ENDING / SPEAKER / SCENE）
    uint8_t *csec = find_sec(copy, sections, 20u);
    uint8_t *esec = find_sec(copy, sections, 21u);
    uint8_t *psec = find_sec(copy, sections, 19u);
    uint8_t *ss = find_sec(copy, sections, 17u);
    CHECK(csec && esec && psec && ss, "section table incomplete for text-ref cases");
    if (csec && esec && psec && ss) {
        const uint32_t c_off = get32(csec + 4);
        const uint32_t e_off = get32(esec + 4);
        const uint32_t p_off = get32(psec + 4);
        const uint32_t s_off = get32(ss + 4);

        memcpy(copy, blob, n);
        put32(copy + c_off + 0, 60000);          // CHOICE.text_off
        CHECK(!nn_pack_init(&pack, copy, n), "choice text_off out of range accepted");

        memcpy(copy, blob, n);
        put32(copy + e_off + 0, 60000);          // ENDING.name_off
        CHECK(!nn_pack_init(&pack, copy, n), "ending name_off out of range accepted");

        memcpy(copy, blob, n);
        put32(copy + e_off + 4, 60000);          // ENDING.text_off
        CHECK(!nn_pack_init(&pack, copy, n), "ending text_off out of range accepted");

        memcpy(copy, blob, n);
        put32(copy + p_off + 0, 60000);          // SPEAKER.name_off
        CHECK(!nn_pack_init(&pack, copy, n), "speaker name_off out of range accepted");

        memcpy(copy, blob, n);
        put32(copy + s_off + 2, 60000);          // SCENE.name_off
        CHECK(!nn_pack_init(&pack, copy, n), "scene name_off out of range accepted");

        // 长度改成越过 text 段末尾
        memcpy(copy, blob, n);
        {
            const uint32_t name_off = get32(copy + s_off + 2);
            (void)name_off;
            uint8_t *rec = copy + s_off;
            rec[6] = 0xFF;                       // name_len = 0xFFFF
            rec[7] = 0xFF;
        }
        CHECK(!nn_pack_init(&pack, copy, n), "scene name_len overflow accepted");
    }

    free(copy);
    free(blob);
    if (failures) {
        printf("nn_pack: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("nn_pack: PASS (real pack readable, 17 bad-pack cases rejected)\n");
    return 0;
}
