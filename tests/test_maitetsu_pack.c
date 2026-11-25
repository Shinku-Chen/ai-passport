// tests/test_maitetsu_pack.c —— 媒体包解析、CRC 与条带解码的 host 回归。
//
// 覆盖：全部 3,994 条带逐个解码；条带尺寸与包表一致；同一图像重复解码结果
// 相同；表区/条带区损坏、截断都必须在初始化或解码时被拒绝；场景合成按
// 参考采样点与源图构图对齐（容差吸收 JPEG 损失）。
#include "maitetsu_assets.h"
#include "maitetsu_pack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { failures++; printf("FAIL: %s\n", msg); } } while (0)

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return NULL; }
    const long length = ftell(file);
    if (length <= 0 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return NULL; }
    uint8_t *data = malloc((size_t)length);
    if (!data) { fclose(file); return NULL; }
    if (fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *size = (size_t)length;
    return data;
}

static void test_decode_all(const uint8_t *pack, size_t size)
{
    char error[96];
    CHECK(maitetsu_pack_init(pack, size, error, sizeof(error)), "pack init");
    CHECK(maitetsu_pack_ready(), "pack ready");
    const uint16_t images = maitetsu_pack_image_count();
    CHECK(images == 759, "pack has 759 images");

    uint32_t bands = 0;
    uint32_t rows_total = 0;
    for (uint16_t image = 0; image < images; ++image) {
        const maitetsu_image_t *info = maitetsu_pack_image(image);
        CHECK(info != NULL, "image descriptor valid");
        if (!info) continue;
        CHECK(info->bands == (info->height + MAITETSU_BAND_ROWS - 1) / MAITETSU_BAND_ROWS,
              "band count matches height");
        for (uint8_t band = 0; band < info->bands; ++band) {
            maitetsu_band_t view;
            if (!maitetsu_pack_band(image, band, &view)) {
                printf("FAIL: decode image %u band %u\n", image, band);
                failures++;
                continue;
            }
            const uint16_t expected_rows = (uint16_t)(
                (info->height - band * MAITETSU_BAND_ROWS) < MAITETSU_BAND_ROWS
                    ? info->height - band * MAITETSU_BAND_ROWS
                    : MAITETSU_BAND_ROWS);
            CHECK(view.width == info->width, "band width matches image");
            if (view.rows != expected_rows) {
                printf("FAIL: image %u band %u rows %u expected %u (h %u)\n", image, band,
                       view.rows, expected_rows, info->height);
            }
            CHECK(view.rows == expected_rows, "band rows match image height");
            CHECK(view.pixels != NULL, "band pixels present");
            bands++;
            rows_total += view.rows;
        }
    }
    CHECK(bands == 3994, "decoded every band exactly once");
    CHECK(rows_total > 50000, "decoded rows account for the full pack");
    printf("decoded %u bands, %u rows over %u images\n", (unsigned)bands,
           (unsigned)rows_total, (unsigned)images);
}

static void test_determinism(void)
{
    // 解码同一张图的同一条带两次，字节必须一致；换一张图再回来也必须一致。
    maitetsu_band_t first;
    CHECK(maitetsu_pack_band(0, 0, &first), "decode image 0 band 0");
    static uint16_t copy[MAITETSU_MAX_WIDTH * MAITETSU_BAND_ROWS];
    const size_t bytes = (size_t)first.width * first.rows * sizeof(uint16_t);
    memcpy(copy, first.pixels, bytes);
    maitetsu_band_t other;
    CHECK(maitetsu_pack_band(300, 2, &other), "decode another image band");
    maitetsu_band_t again;
    CHECK(maitetsu_pack_band(0, 0, &again), "decode image 0 band 0 again");
    CHECK(memcmp(copy, again.pixels, bytes) == 0, "repeated decode is identical");
}

static void test_bad_tables(const uint8_t *pack, size_t size)
{
    char error[96];
    uint8_t *copy = malloc(size);
    CHECK(copy != NULL, "copy for corruption test");
    if (!copy) return;
    memcpy(copy, pack, size);
    copy[12] ^= 0x01;   // image_count
    CHECK(!maitetsu_pack_init(copy, size, error, sizeof(error)), "corrupt header rejected");
    CHECK(!maitetsu_pack_ready(), "not ready after corrupt header");

    memcpy(copy, pack, size);
    copy[48] ^= 0x40;   // prefix table
    CHECK(!maitetsu_pack_init(copy, size, error, sizeof(error)), "corrupt table CRC rejected");
    CHECK(strstr(error, "CRC") != NULL, "corrupt table reports CRC error");

    CHECK(!maitetsu_pack_init(pack, size - 1, error, sizeof(error)), "truncated pack rejected");
    CHECK(!maitetsu_pack_init(pack, 16, error, sizeof(error)), "short pack rejected");

    // 条带区损坏：表区 CRC 仍然正确，必须在首次解码该图像时被拒绝。
    CHECK(maitetsu_pack_init(pack, size, error, sizeof(error)), "re-init clean pack");
    const maitetsu_image_t *info = maitetsu_pack_image(5);
    CHECK(info != NULL, "image 5 descriptor");
    const size_t payload_off = (size_t)(
        (pack[40] | (pack[41] << 8) | (pack[42] << 16) | ((uint32_t)pack[43] << 24)));
    const uint8_t *entries = pack + (size_t)(pack[36] | (pack[37] << 8) | (pack[38] << 16) |
                                             ((uint32_t)pack[39] << 24)) +
                            (size_t)info->first_band * 16u;
    const size_t band_off = (size_t)(entries[0] | (entries[1] << 8) | (entries[2] << 16) |
                                     ((uint32_t)entries[3] << 24));
    memcpy(copy, pack, size);
    copy[payload_off + band_off + 8] ^= 0xFF;   // inside the first band of image 5
    CHECK(maitetsu_pack_init(copy, size, error, sizeof(error)), "corrupt band keeps tables valid");
    maitetsu_band_t view;
    CHECK(!maitetsu_pack_band(5, 0, &view), "corrupt band payload rejected by image CRC");
    // 把解码器指回完好的包，再释放损坏副本（init 只保存指针，不复制）。
    CHECK(maitetsu_pack_init(pack, size, error, sizeof(error)), "clean pack restored");
    free(copy);
}

static uint16_t ref565(int r, int g, int b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static const maitetsu_scene_t *scene_row(uint16_t scene)
{
    if (scene >= (uint16_t)maitetsu_scene_count) return NULL;
    const maitetsu_scene_t *row = &maitetsu_scenes[scene];
    if ((uint32_t)row->first_patch + row->patch_count > (uint32_t)maitetsu_patch_count) return NULL;
    return row;
}

static const maitetsu_patch_t *patch_row(uint16_t scene, uint8_t patch)
{
    const maitetsu_scene_t *row = scene_row(scene);
    if (!row || patch >= row->patch_count) return NULL;
    return &maitetsu_patches[row->first_patch + patch];
}

// 按媒体包表把场景合成到 320x180 的 RGB565 缓冲里（与 UI 的图层合成独立实现）。
static bool compose_scene(uint16_t scene, uint16_t *canvas)
{
    const maitetsu_scene_t *row = scene_row(scene);
    if (!row) return false;
    memset(canvas, 0, MAITETSU_MAX_WIDTH * 180 * sizeof(uint16_t));
    if (row->bg != 0xFFFF) {
        const maitetsu_image_t *bg = maitetsu_pack_image(row->bg);
        if (!bg) return false;
        for (uint8_t band = 0; band < bg->bands; ++band) {
            maitetsu_band_t view;
            if (!maitetsu_pack_band(row->bg, band, &view)) return false;
            memcpy(canvas + (size_t)band * MAITETSU_BAND_ROWS * MAITETSU_MAX_WIDTH,
                   view.pixels, (size_t)view.width * view.rows * sizeof(uint16_t));
        }
    }
    for (uint8_t p = 0; p < row->patch_count; ++p) {
        const maitetsu_patch_t *patch = patch_row(scene, p);
        if (!patch) return false;
        const maitetsu_image_t *image = maitetsu_pack_image(patch->image);
        if (!image) return false;
        for (uint8_t band = 0; band < image->bands; ++band) {
            maitetsu_band_t view;
            if (!maitetsu_pack_band(patch->image, band, &view)) return false;
            const int top = patch->y + band * MAITETSU_BAND_ROWS;
            for (uint16_t y = 0; y < view.rows; ++y) {
                const int dst_y = top + y;
                if (dst_y < 0 || dst_y >= 180) continue;
                const int copy_w = view.width;
                if (patch->x + copy_w > MAITETSU_MAX_WIDTH) continue;
                memcpy(canvas + (size_t)dst_y * MAITETSU_MAX_WIDTH + patch->x,
                       view.pixels + (size_t)y * view.width, (size_t)copy_w * sizeof(uint16_t));
            }
        }
    }
    return true;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "assets/maitetsu/maitetsu_media.bin";
    size_t size = 0;
    uint8_t *pack = read_file(path, &size);
    if (!pack) {
        printf("FAIL: cannot read %s\n", path);
        return 1;
    }
    printf("pack %s (%u bytes)\n", path, (unsigned)size);
    test_decode_all(pack, size);
    test_determinism();
    test_bad_tables(pack, size);

    // 场景合成：每个场景都能合成，且同一场景重复合成完全一致。
    static uint16_t canvas[MAITETSU_MAX_WIDTH * 180];
    static uint16_t second[MAITETSU_MAX_WIDTH * 180];
    bool all_scenes = true;
    uint32_t nonblack = 0;
    for (uint16_t scene = 0; scene < (uint16_t)maitetsu_scene_count; ++scene) {
        if (!compose_scene(scene, canvas)) {
            printf("FAIL: compose scene %u\n", scene);
            all_scenes = false;
            failures++;
            continue;
        }
        for (int i = 0; i < MAITETSU_MAX_WIDTH * 180; ++i) {
            if (canvas[i] != 0) nonblack++;
        }
    }
    CHECK(all_scenes, "compose every scene");
    CHECK(nonblack > 50000000u / 4u, "scenes carry real pixels");
    CHECK(compose_scene(0, canvas), "compose scene 0");
    CHECK(compose_scene(0, second), "compose scene 0 again");
    CHECK(memcmp(canvas, second, sizeof(canvas)) == 0, "scene composition is deterministic");

    // 参考采样：由源图合成（Python 独立路径）得到的 RGB 采样点，容差吸收 JPEG 损失。
    const char *fixture = argc > 2 ? argv[2] : "tests/fixtures/maitetsu-scene-samples.txt";
    FILE *samples = fopen(fixture, "r");
    if (!samples) {
        printf("FAIL: cannot read sample fixture %s\n", fixture);
        failures++;
    } else {
        int scene = -1, x = 0, y = 0, r = 0, g = 0, b = 0;
        int compared = 0;
        int worst = 0;
        char tag[16];
        while (fscanf(samples, "%15s", tag) == 1) {
            if (strcmp(tag, "scene") == 0) {
                if (fscanf(samples, "%d", &scene) != 1) break;
                CHECK(compose_scene((uint16_t)scene, canvas), "compose fixture scene");
                continue;
            }
            if (strcmp(tag, "sample") != 0) break;
            if (fscanf(samples, "%d %d %d %d %d", &x, &y, &r, &g, &b) != 5) break;
            if (scene < 0 || x < 0 || y < 0 || x >= MAITETSU_MAX_WIDTH || y >= 180) continue;
            const uint16_t got = canvas[y * MAITETSU_MAX_WIDTH + x];
            const uint16_t want = ref565(r, g, b);
            const int dr = abs((int)((got >> 11) & 0x1F) - (int)((want >> 11) & 0x1F));
            const int dg = abs((int)((got >> 5) & 0x3F) - (int)((want >> 5) & 0x3F));
            const int db = abs((int)(got & 0x1F) - (int)(want & 0x1F));
            const int delta = dr > dg ? (dr > db ? dr : db) : (dg > db ? dg : db);
            if (delta > worst) worst = delta;
            if (delta > 12) {
                printf("FAIL: scene %d (%d,%d) got %04x want %04x\n", scene, x, y, got, want);
                failures++;
            }
            compared++;
        }
        fclose(samples);
        printf("scene samples compared: %d (worst 5-bit delta %d)\n", compared, worst);
        CHECK(compared >= 100, "enough reference samples");
    }

    free(pack);
    if (failures) {
        printf("FAIL: %d checks\n", failures);
        return 1;
    }
    printf("PASS: maitetsu pack\n");
    return 0;
}
