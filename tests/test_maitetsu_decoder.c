// tests/test_maitetsu_decoder.c —— 生产媒体适配层的真实 LVGL 解码器集成测试。
//
// 不使用计数桩：初始化真实 LVGL，调用生产的 maitetsu_media 注册的
// info / open / get_area / close，交错解码不同宽高的图像（背景、补丁、CG、
// 缩略图），按返回的区域与 stride 拼回整图，再与 pack 直解结果和源构图
// 采样（fixture）逐点对比。最后在已初始化的包上破坏一条条带，确认失败会
// 传到 maitetsu_media_faulted() 且不再产出像素。
//
// host 32/64 位均可；生产目标为 32 位 RISC-V（区域的 int32 语义一致）。
#include "lvgl.h"
#include "lvgl_private.h"
#include "maitetsu_media.h"
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
    fseek(file, 0, SEEK_END);
    const long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (length <= 0) {
        fclose(file);
        return NULL;
    }
    uint8_t *data = malloc((size_t)length);
    if (!data) {
        fclose(file);
        return NULL;
    }
    if (fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *size = (size_t)length;
    return data;
}

// 用生产的 LVGL 解码器把一个描述符解到 canvas（RGB565，行距 = width）。
static bool decode_via_lvgl(const lv_image_dsc_t *src, uint16_t *canvas, int canvas_stride,
                            int expect_w, int expect_h, int *bands_seen)
{
    lv_image_decoder_args_t args;
    memset(&args, 0, sizeof(args));
    lv_image_decoder_dsc_t dsc;
    if (lv_image_decoder_open(&dsc, src, &args) != LV_RESULT_OK) {
        printf("FAIL: decoder open\n");
        return false;
    }
    bool ok = true;
    if (dsc.header.w != (uint32_t)expect_w || dsc.header.h != (uint32_t)expect_h) {
        printf("FAIL: header %ux%u expected %dx%d\n", (unsigned)dsc.header.w,
               (unsigned)dsc.header.h, expect_w, expect_h);
        ok = false;
    }
    if (dsc.header.cf != LV_COLOR_FORMAT_RGB565) {
        printf("FAIL: header color format %d\n", (int)dsc.header.cf);
        ok = false;
    }
    lv_area_t full = { 0, 0, expect_w - 1, expect_h - 1 };
    lv_area_t area = { LV_COORD_MIN, LV_COORD_MIN, LV_COORD_MIN, LV_COORD_MIN };
    int32_t last_y = -1;
    int areas = 0;
    while (lv_image_decoder_get_area(&dsc, &full, &area) == LV_RESULT_OK) {
        if (!dsc.decoded) {
            printf("FAIL: area without decoded buffer\n");
            ok = false;
            break;
        }
        if (area.x1 != 0 || area.x2 != expect_w - 1) {
            printf("FAIL: area x range %d..%d\n", (int)area.x1, (int)area.x2);
            ok = false;
        }
        if (area.y1 != last_y + 1 || area.y2 < area.y1 || area.y2 >= expect_h) {
            printf("FAIL: area y range %d..%d after %d\n", (int)area.y1, (int)area.y2,
                   (int)last_y);
            ok = false;
        }
        const uint32_t stride = dsc.decoded->header.stride;
        if (stride != (uint32_t)expect_w * 2u) {
            printf("FAIL: decoded stride %u\n", (unsigned)stride);
            ok = false;
        }
        const uint32_t rows = dsc.decoded->header.h;
        if (rows != (uint32_t)(area.y2 - area.y1 + 1)) {
            printf("FAIL: decoded rows %u vs area\n", (unsigned)rows);
            ok = false;
        }
        const uint16_t *pixels = (const uint16_t *)dsc.decoded->data;
        for (uint32_t row = 0; row < rows && ok; ++row) {
            if (area.y1 + (int32_t)row >= expect_h) {
                ok = false;
                break;
            }
            memcpy(canvas + (size_t)(area.y1 + (int32_t)row) * canvas_stride,
                   pixels + (size_t)row * expect_w, (size_t)expect_w * sizeof(uint16_t));
        }
        last_y = area.y2;
        areas++;
    }
    if (last_y != expect_h - 1) {
        printf("FAIL: decoded rows end at %d\n", (int)last_y);
        ok = false;
    }
    lv_image_decoder_close(&dsc);
    if (bands_seen) *bands_seen = areas;
    return ok;
}

// pack 直解：独立于 LVGL 适配层的同一条带解码路径，用来逐像素核对适配层。
static bool decode_via_pack(uint16_t image, uint16_t *canvas, int stride, int expect_w,
                            int expect_h)
{
    const maitetsu_image_t *info = maitetsu_pack_image(image);
    if (!info) return false;
    for (uint8_t band = 0; band < info->bands; ++band) {
        maitetsu_band_t view;
        if (!maitetsu_pack_band(image, band, &view)) return false;
        if (view.width != expect_w) return false;
        const uint16_t top = (uint16_t)(band * MAITETSU_BAND_ROWS);
        for (uint16_t row = 0; row < view.rows; ++row) {
            if (top + row >= expect_h) return false;
            memcpy(canvas + (size_t)(top + row) * stride,
                   view.pixels + (size_t)row * view.width,
                   (size_t)view.width * sizeof(uint16_t));
        }
    }
    return true;
}

static uint16_t ref565(int r, int g, int b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static uint16_t scene_patch_image(uint16_t scene, uint8_t patch, uint16_t *x, uint16_t *y)
{
    const maitetsu_scene_t *row = maitetsu_media_scene(scene);
    if (!row || patch >= row->patch_count) return 0xFFFF;
    *x = maitetsu_patches[row->first_patch + patch].x;
    *y = maitetsu_patches[row->first_patch + patch].y;
    return maitetsu_patches[row->first_patch + patch].image;
}

static void check_fixture_samples(const char *fixture, uint16_t *bg, uint16_t *patch)
{
    FILE *file = fopen(fixture, "r");
    if (!file) {
        CHECK(0, "fixture open");
        return;
    }
    char tag[16];
    int scene = -1;
    const maitetsu_scene_t *row = NULL;
    static uint16_t canvas[320 * 180];
    int compared = 0;
    int worst = 0;
    while (fscanf(file, "%15s", tag) == 1) {
        if (strcmp(tag, "scene") == 0) {
            if (fscanf(file, "%d", &scene) != 1) break;
            row = maitetsu_media_scene((uint16_t)scene);
            if (!row) {
                CHECK(0, "fixture scene missing");
                break;
            }
            memset(canvas, 0, sizeof(canvas));
            if (row->bg != MAITETSU_SCENE_NONE) {
                const maitetsu_image_t *info = maitetsu_pack_image(row->bg);
                CHECK(info && info->width == 320 && info->height == 180, "background size");
                CHECK(decode_via_lvgl(maitetsu_media_image(row->bg), bg, 320, 320, 180, NULL),
                      "decode background via LVGL");
                memcpy(canvas, bg, sizeof(canvas));
            }
            for (uint8_t p = 0; p < row->patch_count; ++p) {
                uint16_t px = 0, py = 0;
                const uint16_t image = scene_patch_image((uint16_t)scene, p, &px, &py);
                const maitetsu_image_t *info = maitetsu_pack_image(image);
                CHECK(info != NULL, "patch descriptor");
                if (!info) continue;
                CHECK(decode_via_lvgl(maitetsu_media_image(image), patch, 320,
                                      info->width, info->height, NULL), "decode patch via LVGL");
                for (uint16_t y = 0; y < info->height; ++y) {
                    const uint16_t dy = py + y;
                    if (dy >= 180) break;
                    const uint16_t copy = (px + info->width > 320)
                                              ? (uint16_t)(320 - px) : info->width;
                    // patch 缓冲行距固定 320（decode_via_lvgl 的 canvas_stride）
                    memcpy(canvas + (size_t)dy * 320 + px,
                           patch + (size_t)y * 320, (size_t)copy * sizeof(uint16_t));
                }
            }
            continue;
        }
        if (strcmp(tag, "sample") != 0) break;
        int x = 0, y = 0, r = 0, g = 0, b = 0;
        if (fscanf(file, "%d %d %d %d %d", &x, &y, &r, &g, &b) != 5) break;
        if (scene < 0 || x < 0 || y < 0 || x >= 320 || y >= 180) continue;
        const uint16_t got = canvas[y * 320 + x];
        const uint16_t want = ref565(r, g, b);
        const int dr = abs((int)((got >> 11) & 0x1F) - (int)((want >> 11) & 0x1F));
        const int dg = abs((int)((got >> 5) & 0x3F) - (int)((want >> 5) & 0x3F));
        const int db = abs((int)(got & 0x1F) - (int)(want & 0x1F));
        const int delta = dr > dg ? (dr > db ? dr : db) : (dg > db ? dg : db);
        if (delta > worst) worst = delta;
        if (delta > 12) {
            printf("FAIL: sample scene %d (%d,%d) got %04x want %04x\n", scene, x, y, got, want);
            failures++;
        }
        compared++;
    }
    fclose(file);
    printf("LVGL-decoded scene samples: %d (worst 5-bit delta %d)\n", compared, worst);
    CHECK(compared >= 100, "enough scene samples");
}

// 在已初始化包的副本上破坏一条条带：image CRC 必须让解码失败，并置起 fault。
static void check_fault_propagation(uint8_t *pack, size_t size)
{
    (void)size;
    const uint16_t victim = 758;   // 标题主视觉：前面的测试没有解过它
    const maitetsu_image_t *info = maitetsu_pack_image(victim);
    CHECK(info != NULL, "fault victim descriptor");
    if (!info) return;
    const size_t band_off = (size_t)(pack[36] | (pack[37] << 8) | (pack[38] << 16) |
                                     ((uint32_t)pack[39] << 24));
    const size_t payload_off = (size_t)(pack[40] | (pack[41] << 8) | (pack[42] << 16) |
                                        ((uint32_t)pack[43] << 24));
    const size_t entry = band_off + (size_t)info->first_band * 16u;
    const size_t off = (size_t)(pack[entry] | (pack[entry + 1] << 8) |
                                (pack[entry + 2] << 16) | ((uint32_t)pack[entry + 3] << 24));
    pack[payload_off + off + 40] ^= 0xFF;
    CHECK(!maitetsu_media_faulted(), "no fault before corrupt decode");
    const lv_image_dsc_t *src = maitetsu_media_image(victim);
    CHECK(src != NULL, "victim descriptor");
    if (!src) return;
    lv_image_decoder_args_t args;
    memset(&args, 0, sizeof(args));
    lv_image_decoder_dsc_t dsc;
    if (lv_image_decoder_open(&dsc, src, &args) != LV_RESULT_OK) {
        CHECK(maitetsu_media_faulted(), "open failure is a fault");
        return;
    }
    lv_area_t full = { 0, 0, 319, 179 };
    lv_area_t area = { LV_COORD_MIN, LV_COORD_MIN, LV_COORD_MIN, LV_COORD_MIN };
    const lv_result_t res = lv_image_decoder_get_area(&dsc, &full, &area);
    lv_image_decoder_close(&dsc);
    CHECK(res != LV_RESULT_OK, "corrupt strip is rejected by the decoder");
    CHECK(maitetsu_media_faulted(), "corrupt strip raises the media fault");
    CHECK(maitetsu_media_fault_text()[0] != '\0', "fault text is visible");
}

int main(int argc, char **argv)
{
    const char *pack_path = argc > 1 ? argv[1] : "assets/maitetsu/maitetsu_media.bin";
    const char *fixture = argc > 2 ? argv[2] : "tests/fixtures/maitetsu-scene-samples.txt";
    size_t size = 0;
    uint8_t *pack = read_file(pack_path, &size);
    if (!pack) {
        printf("FAIL: cannot read %s\n", pack_path);
        return 1;
    }
    lv_init();
    CHECK(maitetsu_media_init_from(pack, size), "media init");
    CHECK(maitetsu_media_ready(), "media ready");
    CHECK(maitetsu_media_image_count() == 759, "759 descriptors");

    // 交错解码：背景 320x180 → 补丁（较窄）→ CG 320x180 → 缩略图 64x36 → 回到背景。
    static uint16_t bg[320 * 180];
    static uint16_t bg_again[320 * 180];
    static uint16_t patch[320 * 180];
    static uint16_t cg[320 * 180];
    static uint16_t thumb[320 * 180];

    // 第一个有补丁的场景：背景 + 补丁都从生成的场景表取。
    uint16_t scene_with_patch = 0;
    for (uint16_t i = 0; i < (uint16_t)maitetsu_scene_count; ++i) {
        if (maitetsu_scenes[i].patch_count > 0) {
            scene_with_patch = i;
            break;
        }
    }
    const uint16_t bg_image = maitetsu_scenes[scene_with_patch].bg;
    const uint16_t patch_image = maitetsu_patches[maitetsu_scenes[scene_with_patch].first_patch].image;
    const uint16_t cg_image = maitetsu_cg_images[0];
    const uint16_t thumb_image = maitetsu_cg_thumbs[0];
    const maitetsu_image_t *patch_info = maitetsu_pack_image(patch_image);
    const maitetsu_image_t *thumb_info = maitetsu_pack_image(thumb_image);
    CHECK(patch_info && patch_info->width < 320, "patch is narrower than the screen");
    CHECK(thumb_info && thumb_info->width == 64 && thumb_info->height == 36, "thumb size");

    const lv_image_dsc_t *origin = maitetsu_media_image(bg_image);
    CHECK(origin != NULL, "origin descriptor");
    // 交错：先开背景只取第一段，再完整解补丁/缩略图，然后继续背景。
    {
        lv_image_decoder_args_t args;
        memset(&args, 0, sizeof(args));
        lv_image_decoder_dsc_t dsc;
        CHECK(lv_image_decoder_open(&dsc, origin, &args) == LV_RESULT_OK, "interleave open");
        lv_area_t full = { 0, 0, 319, 179 };
        lv_area_t area = { LV_COORD_MIN, LV_COORD_MIN, LV_COORD_MIN, LV_COORD_MIN };
        CHECK(lv_image_decoder_get_area(&dsc, &full, &area) == LV_RESULT_OK,
              "interleave first area");
        const uint16_t first_rows = (uint16_t)(area.y2 - area.y1 + 1);
        CHECK(dsc.decoded && dsc.decoded->header.stride == 640, "interleave stride");
        memset(bg, 0, sizeof(bg));
        memcpy(bg, dsc.decoded->data, (size_t)first_rows * 320 * sizeof(uint16_t));
        // 另开两个不同宽高的图像，彻底覆盖共享条带缓冲。
        CHECK(decode_via_lvgl(maitetsu_media_image(patch_image), patch, 320,
                              patch_info->width, patch_info->height, NULL),
              "interleaved patch decode");
        CHECK(decode_via_lvgl(maitetsu_media_image(thumb_image), thumb, 320,
                              thumb_info->width, thumb_info->height, NULL),
              "interleaved thumb decode");
        int32_t last_y = area.y2;
        while (lv_image_decoder_get_area(&dsc, &full, &area) == LV_RESULT_OK) {
            CHECK(dsc.decoded && dsc.decoded->header.stride == 640, "area stride 320");
            const uint32_t rows = dsc.decoded->header.h;
            CHECK(area.y1 == last_y + 1, "areas are contiguous");
            memcpy(bg + (size_t)area.y1 * 320, dsc.decoded->data,
                   (size_t)rows * 320 * sizeof(uint16_t));
            last_y = area.y2;
        }
        CHECK(last_y == 179, "background decoded to the last row");
        lv_image_decoder_close(&dsc);
    }

    // 与 pack 直解逐像素比对（同一解码器、不同调用路径）。
    CHECK(decode_via_pack(bg_image, bg_again, 320, 320, 180), "pack direct decode");
    CHECK(memcmp(bg, bg_again, sizeof(bg)) == 0, "LVGL path equals pack path");

    // CG 与缩略图同样走完整路径。
    CHECK(decode_via_lvgl(maitetsu_media_image(cg_image), cg, 320, 320, 180, NULL),
          "CG decode via LVGL");
    CHECK(decode_via_pack(cg_image, bg_again, 320, 320, 180), "CG pack decode");
    CHECK(memcmp(cg, bg_again, sizeof(cg)) == 0, "CG path equals pack path");
    CHECK(decode_via_lvgl(maitetsu_media_image(thumb_image), thumb, 320, 64, 36, NULL),
          "thumb decode via LVGL");

    // 源构图采样：用 LVGL 解出的背景 + 补丁合成，和 fixture 对齐。
    check_fixture_samples(fixture, bg, patch);

    // 生产描述符在 info 回调之前不带像素（UNKNOWN / stride 0 / data_size 0），
    // 所以即使有兜底的 raw 解码器也不会把描述符本身当像素读。
    {
        const lv_image_dsc_t *probe = maitetsu_media_image(bg_image);
        CHECK(probe && probe->header.cf == LV_COLOR_FORMAT_UNKNOWN, "descriptor has no pixel format");
        CHECK(probe && probe->header.stride == 0 && probe->data_size == 0,
              "descriptor carries no pixel extent");
    }
    // 同样形状的伪造描述符必须被所有解码器拒绝（raw 兜底不接受 UNKNOWN）。
    {
        lv_image_decoder_args_t args;
        memset(&args, 0, sizeof(args));
        lv_image_decoder_dsc_t dsc;
        lv_image_dsc_t bogus;
        memset(&bogus, 0, sizeof(bogus));
        bogus.header.magic = LV_IMAGE_HEADER_MAGIC;
        bogus.header.cf = LV_COLOR_FORMAT_UNKNOWN;
        bogus.header.w = 320;
        bogus.header.h = 180;
        bogus.header.stride = 0;
        bogus.data_size = 0;
        bogus.data = (const uint8_t *)&bogus;
        CHECK(lv_image_decoder_open(&dsc, &bogus, &args) != LV_RESULT_OK,
              "foreign descriptor is rejected");
        if (dsc.decoder) lv_image_decoder_close(&dsc);
    }

    check_fault_propagation(pack, size);

    free(pack);
    if (failures) {
        printf("FAIL: %d checks\n", failures);
        return 1;
    }
    printf("PASS: maitetsu LVGL decoder integration\n");
    return 0;
}
