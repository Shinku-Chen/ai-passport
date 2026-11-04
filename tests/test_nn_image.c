// tests/test_nn_image.c —— JPEG 直写画布 + 1bpp 遮罩合成的宿主测试。
//
// 覆盖:
//   1. 背景解到画布中央(168x240 @ x=76):条带内有像素,条带外保持纯黑;
//   2. 立绘解到画布上时,遮罩为 0 的像素必须保持底色不被覆盖;
//   3. 越界绘制(负坐标 / 超出右下角)不崩、不越界写,可见部分仍然画出来;
//   4. 损坏的 JPEG 返回 false,且不改变画布内容。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nn_image.h"
#include "nn_pack.h"

#define CW 320
#define CH 240
#define DEFAULT_PACK "assets/nn/nn_pack.bin"
#define ART_X 76

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

static uint8_t *load_file(const char *path, size_t *size)
{
    FILE *fh = fopen(path, "rb");
    if (!fh) return NULL;
    fseek(fh, 0, SEEK_END);
    long n = ftell(fh);
    fseek(fh, 0, SEEK_SET);
    if (n <= 0) { fclose(fh); return NULL; }
    uint8_t *buf = malloc((size_t)n);
    if (!buf) { fclose(fh); return NULL; }
    if (fread(buf, 1, (size_t)n, fh) != (size_t)n) { free(buf); fclose(fh); return NULL; }
    fclose(fh);
    *size = (size_t)n;
    return buf;
}

static int count_non_black(const uint16_t *px, int w, int x0, int y0, int x1, int y1)
{
    int n = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            if (px[(size_t)y * w + x] != 0) n++;
        }
    }
    return n;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : DEFAULT_PACK;
    size_t size = 0;
    uint8_t *blob = load_file(path, &size);
    if (!blob) {
        printf("cannot read %s\n", path);
        return 1;
    }
    nn_pack_t pack;
    if (!nn_pack_init(&pack, blob, size)) {
        printf("pack did not parse\n");
        free(blob);
        return 1;
    }

    uint16_t *canvas = calloc((size_t)CW * CH, sizeof(uint16_t));
    CHECK(canvas != NULL, "canvas alloc");
    if (!canvas) { free(blob); return 1; }

    // ---- 1. 背景:fit 到 168x240,画在 x=76
    // 取“字节最多”的整屏图：包里包含 bg_black / bg_white 这种纯色图，
    // 用第一张会让“条带内有像素”的断言失去意义。
    const nn_image_t *bg = NULL;
    const nn_image_t *sprite = NULL;
    for (uint32_t i = 0; i < pack.image_count; ++i) {
        const nn_image_t *img = &pack.images[i];
        // 背景与 CG 都是 168x240：两者都能拿来测“整屏绘制”，取字节最多的那张
        if ((img->kind == NN_IMG_BG || img->kind == NN_IMG_CG || img->kind == NN_IMG_TITLE) &&
            img->w == 168 && img->h == 240 && (!bg || img->jpeg_len > bg->jpeg_len)) {
            bg = img;
        }
        if (img->kind == NN_IMG_SPRITE && (!sprite || img->jpeg_len > sprite->jpeg_len)) {
            sprite = img;
        }
    }
    CHECK(bg != NULL, "no 168x240 background image in the pack");
    CHECK(sprite != NULL, "no sprite image in the pack");
    if (!bg || !sprite) { free(canvas); free(blob); return 1; }

    const uint8_t *bg_data = nn_pack_image_data(&pack, bg);
    CHECK(nn_jpeg_draw(canvas, CW, CH, bg_data, bg->jpeg_len, ART_X, 0, NULL, 0),
          "background decode failed");
    const int inside = count_non_black(canvas, CW, ART_X, 0, ART_X + bg->w, CH);
    const int outside_left = count_non_black(canvas, CW, 0, 0, ART_X, CH);
    const int outside_right = count_non_black(canvas, CW, ART_X + bg->w, 0, CW, CH);
    CHECK(inside > (int)(bg->w * CH / 2), "background strip mostly black: %d px", inside);
    CHECK(outside_left == 0, "pixels left of the art strip were written: %d", outside_left);
    CHECK(outside_right == 0, "pixels right of the art strip were written: %d", outside_right);
    printf("  background %ux%u at x=%d: %d painted px, bars clean\n",
           bg->w, bg->h, ART_X, inside);

    // ---- 2. 立绘:遮罩为 0 的像素不能覆盖底色
    const uint8_t *sp_data = nn_pack_image_data(&pack, sprite);
    CHECK(sp_data != NULL, "sprite data null");
    const uint8_t *mask = sp_data + sprite->jpeg_len;
    const int stride = (int)((sprite->w + 7) / 8);
    CHECK(sprite->mask_len == (uint32_t)(stride * sprite->h), "sprite mask size mismatch");

    // 先铺一层纯色底,再叠立绘
    for (int i = 0; i < CW * CH; ++i) canvas[i] = 0x1234;
    const int sx = (CW - sprite->w) / 2;
    const int sy = CH - sprite->h;
    CHECK(nn_jpeg_draw(canvas, CW, CH, sp_data, sprite->jpeg_len, sx, sy, mask, stride),
          "sprite decode failed");

    int mask_on = 0, mask_off = 0, painted_on = 0, kept_off = 0;
    for (int y = 0; y < sprite->h; ++y) {
        const uint8_t *mrow = mask + (size_t)y * stride;
        for (int x = 0; x < sprite->w; ++x) {
            const int bit = (mrow[x >> 3] >> (7 - (x & 7))) & 1;
            const uint16_t px = canvas[(size_t)(sy + y) * CW + (sx + x)];
            if (bit) {
                mask_on++;
                if (px != 0x1234) painted_on++;
            } else {
                mask_off++;
                if (px == 0x1234) kept_off++;
            }
        }
    }
    CHECK(painted_on > mask_on / 2, "masked-on pixels mostly not painted (%d/%d)", painted_on, mask_on);
    CHECK(kept_off == mask_off, "masked-off pixels were overwritten (%d/%d kept)",
          kept_off, mask_off);
    printf("  sprite %ux%u: mask on %d px (painted %d), mask off %d px (all kept)\n",
           sprite->w, sprite->h, mask_on, painted_on, mask_off);

    // ---- 3. 越界绘制:负坐标与右下溢出都只画可见部分
    for (int i = 0; i < CW * CH; ++i) canvas[i] = 0;
    CHECK(nn_jpeg_draw(canvas, CW, CH, bg_data, bg->jpeg_len, -50, -40, NULL, 0),
          "negative-offset decode failed");
    CHECK(count_non_black(canvas, CW, 0, 0, CW, CH) > 0, "negative offset painted nothing");
    for (int i = 0; i < CW * CH; ++i) canvas[i] = 0;
    CHECK(nn_jpeg_draw(canvas, CW, CH, bg_data, bg->jpeg_len, CW - 20, CH - 20, NULL, 0),
          "overflow-offset decode failed");
    CHECK(count_non_black(canvas, CW, CW - 20, CH - 20, CW, CH) > 0,
          "overflow offset painted nothing");
    printf("  out-of-bounds draws clipped without corruption\n");

    // ---- 4. 损坏数据:返回 false 且画布不变
    uint8_t *broken = malloc(bg->jpeg_len);
    CHECK(broken != NULL, "alloc");
    if (broken) {
        memcpy(broken, bg_data, bg->jpeg_len);
        memset(broken, 0, 64);   // 打掉 SOI/头部
        for (int i = 0; i < CW * CH; ++i) canvas[i] = 0x00FF;
        CHECK(!nn_jpeg_draw(canvas, CW, CH, broken, bg->jpeg_len, 0, 0, NULL, 0),
              "corrupt JPEG accepted");
        int changed = 0;
        for (int i = 0; i < CW * CH; ++i) if (canvas[i] != 0x00FF) changed++;
        CHECK(changed == 0, "corrupt JPEG modified the canvas (%d px)", changed);
        free(broken);
    }


    // ---- 5. 分带 cover：背景/CG/标题三带都铺满，无黑边；标题右对齐保留 logo 侧
    {
        #define BAND_H 80
        uint16_t band[BAND_H * CW];
        const nn_image_t *bgv = NULL, *cgv = NULL, *titv = NULL;
        for (uint32_t i = 0; i < pack.image_count; ++i) {
            const nn_image_t *im = &pack.images[i];
            if (im->kind == NN_IMG_BG && im->w == 168 && (!bgv || im->jpeg_len > bgv->jpeg_len))
                bgv = im;
            if (im->kind == NN_IMG_CG && (!cgv || im->jpeg_len > cgv->jpeg_len)) cgv = im;
            if (im->kind == NN_IMG_TITLE) titv = im;
        }
        CHECK(bgv && cgv && titv, "cover fixtures missing");
        const nn_image_t *items[3] = {bgv, cgv, titv};
        const char *names[3] = {"bg(168x240)", "cg", "title(320x240)"};
        for (int k = 0; k < 3; ++k) {
            const nn_image_t *im = items[k];
            if (!im) continue;
            const uint8_t *jpg = nn_pack_image_data(&pack, im);
            int worst = 0;
            for (int b = 0; b < 3; ++b) {
                for (int i2 = 0; i2 < BAND_H * CW; ++i2) band[i2] = 0;
                const int anchor = im->kind == NN_IMG_TITLE ? 1 : 0;
                CHECK(nn_jpeg_draw_fit_band(band, CW, BAND_H, CH, jpg, im->jpeg_len, false,
                                            b * BAND_H, anchor, im->kind == NN_IMG_CG),
                      "cover band decode failed");
                // 四边：左右两列 + 本带首/末行
                int empty = 0;
                for (int y = 0; y < BAND_H; ++y) {
                    if (band[y * CW + 0] == 0) empty++;
                    if (band[y * CW + CW - 1] == 0) empty++;
                }
                for (int x = 0; x < CW; ++x) {
                    if (band[0 * CW + x] == 0) empty++;
                    if (band[(BAND_H - 1) * CW + x] == 0) empty++;
                }
                if (empty > worst) worst = empty;
            }
            CHECK(worst == 0, "%s has %d empty border px after cover", names[k], worst);
            printf("  cover %-14s bands 0/1/2: borders fully painted\n", names[k]);
        }
        // 标题右对齐：右侧 1/4（x>=240）必须有内容（logo 留在右侧）
        if (titv) {
            const uint8_t *jpg = nn_pack_image_data(&pack, titv);
            int right = 0;
            for (int b = 0; b < 2; ++b) {   // 取上两条带（logo 在上半部）
                for (int i2 = 0; i2 < BAND_H * CW; ++i2) band[i2] = 0;
                (void)nn_jpeg_draw_fit_band(band, CW, BAND_H, CH, jpg, titv->jpeg_len, false,
                                            b * BAND_H, 1, 0);
                right += count_non_black(band, CW, 240, 0, CW, BAND_H);
            }
            printf("  title right quarter: %d px of logo\n", right);
            CHECK(right > 2000, "title right quarter is empty (%d px)", right);
        }
    }

    free(canvas);
    free(blob);
    if (failures) {
        printf("nn_image: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("nn_image: PASS\n");
    return 0;
}
