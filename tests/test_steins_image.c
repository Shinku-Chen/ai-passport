// tests/test_steins_image.c —— 真实 TJpgDec + 真实资源包的解码/几何回归。
//
// 覆盖:
//   * 包里每一个资源都能被真实 JPEG 解码器解出来,且解码器报的尺寸与表里一致
//   * 立绘遮罩:透明区不写像素,不透明区写像素(用一张四角透明的图验证)
//   * 视口裁剪:目标矩形出界时只写画布内的部分,不越界
//   * 层顺序:后画的层覆盖先画的层
//   * 故障注入:截断 JPEG、声明尺寸与 JPEG 头不符、非法资源下标都必须失败,
//     并且失败时不把画布标记为"已画出"
#if defined(STEINS_GATE_NEGATIVE)
// 门禁负例用:故意编译失败,验证 validate.sh 会把"图片测试编译失败"当成失败,
// 而不是只打印一行 FAIL 就继续往下走。
#error "STEINS_GATE_NEGATIVE: intentional compile failure"
#endif

#include "steins_image.h"
#include "steins_pack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(cond, ...)                            \
    do {                                            \
        if (!(cond)) {                              \
            fprintf(stderr, "FAIL %s:%d ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);           \
            fprintf(stderr, "\n");                  \
            ++g_failures;                           \
        }                                           \
    } while (0)

static uint8_t *load_file(const char *path, uint32_t *size_out)
{
    FILE *fh = fopen(path, "rb");
    if (!fh) return NULL;
    fseek(fh, 0, SEEK_END);
    const long size = ftell(fh);
    fseek(fh, 0, SEEK_SET);
    if (size <= 0) {
        fclose(fh);
        return NULL;
    }
    uint8_t *data = (uint8_t *)malloc((size_t)size);
    if (!data) {
        fclose(fh);
        return NULL;
    }
    if (fread(data, 1, (size_t)size, fh) != (size_t)size) {
        free(data);
        fclose(fh);
        return NULL;
    }
    fclose(fh);
    *size_out = (uint32_t)size;
    return data;
}

static uint16_t s_canvas[STEINS_IMAGE_PIXELS];

static void test_all_assets(const steins_pack_t *pack)
{
    const uint32_t assets = steins_pack_count(pack, STEINS_SEC_ASSET);
    uint32_t sprites = 0;
    uint32_t decoded = 0;
    for (uint32_t i = 0; i < assets; ++i) {
        steins_asset_t asset;
        CHECK(steins_pack_asset(pack, (uint16_t)i, &asset), "asset %u unreadable", (unsigned)i);
        steins_image_error_t err;
        const bool ok = steins_image_validate(pack, (uint16_t)i, &err);
        CHECK(ok, "asset %u failed to decode (reason %u)", (unsigned)i, err.reason);
        if (asset.mask_len) ++sprites;
        if (ok) ++decoded;
    }
    CHECK(decoded == assets, "decoded %u of %u assets", (unsigned)decoded, (unsigned)assets);
    CHECK(sprites > 600, "only %u sprites carry an alpha mask", (unsigned)sprites);
    printf("  assets=%u sprites=%u\n", (unsigned)assets, (unsigned)sprites);
}

static void test_view_render(const steins_pack_t *pack)
{
    steins_image_t image;
    memset(&image, 0, sizeof(image));
    image.pixels = s_canvas;

    const uint32_t views = steins_pack_count(pack, STEINS_SEC_VIEW);
    uint32_t rendered = 0;
    uint32_t empty = 0;
    for (uint32_t v = 0; v < views; ++v) {
        steins_image_error_t err;
        const bool ok = steins_image_show_view(&image, pack, (uint16_t)v, &err);
        CHECK(ok, "view %u failed to render (asset %u reason %u)", (unsigned)v,
              (unsigned)err.asset, err.reason);
        steins_view_t view;
        steins_pack_view(pack, (uint16_t)v, &view);
        if (view.count == 0) {
            ++empty;
        } else if (ok) {
            ++rendered;
        }
    }
    CHECK(rendered + empty == views, "rendered %u + empty %u != %u views", (unsigned)rendered,
          (unsigned)empty, (unsigned)views);
    printf("  views=%u rendered=%u empty=%u\n", (unsigned)views, (unsigned)rendered,
           (unsigned)empty);
}

// 找出一个带遮罩的资源,检查"遮罩位为 0 的像素不被写"。
static void test_mask_and_clip(const steins_pack_t *pack)
{
    const uint32_t assets = steins_pack_count(pack, STEINS_SEC_ASSET);
    uint16_t sprite = STEINS_NONE;
    for (uint32_t i = 0; i < assets; ++i) {
        steins_asset_t asset;
        if (!steins_pack_asset(pack, (uint16_t)i, &asset)) continue;
        if (asset.mask_len && asset.w < 200 && asset.h < 260) {
            sprite = (uint16_t)i;
            break;
        }
    }
    CHECK(sprite != STEINS_NONE, "no masked sprite found");
    if (sprite == STEINS_NONE) return;

    steins_image_t image;
    memset(&image, 0, sizeof(image));
    image.pixels = s_canvas;
    steins_image_clear(&image, 0x1234u);

    steins_asset_t asset;
    steins_pack_asset(pack, sprite, &asset);
    steins_image_error_t err;
    const bool ok = steins_image_draw_asset(&image, pack, sprite, 0, 0, asset.cw, asset.ch,
                                            1000u, &err);
    CHECK(ok, "sprite %u draw failed (reason %u)", (unsigned)sprite, err.reason);

    // 画布上必须同时存在"原底色"和"新像素":既真的画了内容,也没把整块涂满。
    uint32_t painted = 0;
    for (int i = 0; i < STEINS_IMAGE_PIXELS; ++i) {
        if (s_canvas[i] != 0x1234u) ++painted;
    }
    CHECK(painted > 0, "sprite painted nothing");
    CHECK(painted < (uint32_t)asset.cw * asset.ch,
          "sprite covered the whole rect (%u px) despite alpha mask", (unsigned)painted);
    printf("  sprite=%u rect=%ux%u painted=%u\n", (unsigned)sprite, (unsigned)asset.cw,
           (unsigned)asset.ch, (unsigned)painted);

    // 视口裁剪:把同一张图部分画到画布外,不能越界写,也不能整张消失。
    steins_image_clear(&image, 0x1234u);
    const int off_x = -asset.cw / 2;
    const int off_y = STEINS_IMAGE_H - asset.ch / 2;
    const bool clip_ok = steins_image_draw_asset(&image, pack, sprite, off_x, off_y,
                                                 asset.cw, asset.ch, 1000u, &err);
    CHECK(clip_ok, "clipped draw failed (reason %u)", err.reason);
    uint32_t clip_painted = 0;
    for (int i = 0; i < STEINS_IMAGE_PIXELS; ++i) {
        if (s_canvas[i] != 0x1234u) ++clip_painted;
    }
    CHECK(clip_painted > 0 && clip_painted < (uint32_t)STEINS_IMAGE_PIXELS,
          "clipped draw painted %u pixels", (unsigned)clip_painted);
}

// 层顺序:同一个 view 里后画的层必须参与合成,且整帧合成是确定的。
static void test_layer_order(const steins_pack_t *pack)
{
    static uint16_t canvas_a[STEINS_IMAGE_PIXELS];
    static uint16_t canvas_b[STEINS_IMAGE_PIXELS];
    const uint32_t views = steins_pack_count(pack, STEINS_SEC_VIEW);
    uint32_t checked = 0;
    for (uint32_t v = 0; v < views && checked < 3; ++v) {
        steins_view_t view;
        if (!steins_pack_view(pack, (uint16_t)v, &view)) continue;
        if (view.count < 2) continue;

        steins_image_t image;
        memset(&image, 0, sizeof(image));
        steins_image_error_t err;

        // 1) 整帧两次渲染必须逐像素一致(合成确定、不依赖残留画布)。
        image.pixels = canvas_a;
        CHECK(steins_image_show_view(&image, pack, (uint16_t)v, &err), "view %u render",
              (unsigned)v);
        image.pixels = canvas_b;
        CHECK(steins_image_show_view(&image, pack, (uint16_t)v, &err), "view %u re-render",
              (unsigned)v);
        CHECK(memcmp(canvas_a, canvas_b, sizeof(canvas_a)) == 0,
              "view %u composite is not deterministic", (unsigned)v);

        // 2) 少画最后一层,画面必须发生变化 —— 说明它确实在最后覆盖上去。
        image.pixels = canvas_b;
        steins_image_clear(&image, 0x0000u);
        for (uint8_t l = 0; l + 1 < view.count; ++l) {
            steins_layer_t layer;
            steins_pack_layer(pack, (uint16_t)(view.layer_first + l), &layer);
            CHECK(steins_image_draw_asset(&image, pack, layer.asset, layer.x, layer.y,
                                          layer.w, layer.h, layer.opacity, &err),
                  "view %u layer %u draw", (unsigned)v, (unsigned)l);
        }
        CHECK(memcmp(canvas_a, canvas_b, sizeof(canvas_a)) != 0,
              "view %u last layer changed nothing", (unsigned)v);
        printf("  layer order checked on view %u (%u layers)\n", (unsigned)v,
               (unsigned)view.count);
        ++checked;
    }
    CHECK(checked >= 3, "only %u multi-layer views checked", (unsigned)checked);
}

// 真实 JPEG 中途失败:必须得到"确定的失败画面"(全黑),而不是半帧;
// 恢复原数据后同一资源必须成功,且与本轮校验通过的参考帧逐像素一致。
static uint8_t *dup_pack(const uint8_t *data, uint32_t size, steins_pack_t *out)
{
    uint8_t *copy = (uint8_t *)malloc(size);
    if (!copy) return NULL;
    memcpy(copy, data, size);
    if (!steins_pack_open(out, copy, size)) {
        free(copy);
        return NULL;
    }
    return copy;
}

// 把某个资源的 JPEG 声明长度砍半:解码器会解到一半就失败(真实中途失败)。
static void halve_asset_len(uint8_t *copy, const steins_pack_t *pack, uint16_t asset)
{
    const uint32_t off = pack->sec_off[STEINS_SEC_ASSET] + (uint32_t)asset * 40u + 4u;
    const uint32_t len = steins_rd32(copy + off);
    const uint32_t half = len / 2u;
    copy[off] = (uint8_t)(half & 0xFFu);
    copy[off + 1] = (uint8_t)((half >> 8) & 0xFFu);
    copy[off + 2] = (uint8_t)((half >> 16) & 0xFFu);
    copy[off + 3] = (uint8_t)((half >> 24) & 0xFFu);
}

static uint32_t count_nonblack(const uint16_t *canvas)
{
    uint32_t painted = 0;
    for (int i = 0; i < STEINS_IMAGE_PIXELS; ++i) {
        if (canvas[i] != 0u) ++painted;
    }
    return painted;
}

static void test_failure_frame_view(const steins_pack_t *pack, const uint8_t *data,
                                    uint32_t size)
{
    static uint16_t reference[STEINS_IMAGE_PIXELS];
    steins_image_t image;
    memset(&image, 0, sizeof(image));
    image.pixels = s_canvas;
    steins_image_error_t err;

    // 找一个至少两层的 view,把**最后一层**弄坏:前面的层已画上画布,
    // 正是"半帧残影"的场景。
    uint32_t target = UINT32_MAX;
    uint16_t bad_asset = STEINS_NONE;
    for (uint32_t v = 0; v < steins_pack_count(pack, STEINS_SEC_VIEW); ++v) {
        steins_view_t view;
        if (!steins_pack_view(pack, (uint16_t)v, &view)) continue;
        if (view.count < 2) continue;
        steins_layer_t layer;
        if (!steins_pack_layer(pack, (uint16_t)(view.layer_first + view.count - 1), &layer)) {
            continue;
        }
        target = v;
        bad_asset = layer.asset;
        break;
    }
    CHECK(target != UINT32_MAX && bad_asset != STEINS_NONE, "no multi-layer view found");
    if (target == UINT32_MAX) return;

    // 1) 参考帧(A):正常渲染成功,画布有内容。
    CHECK(steins_image_show_view(&image, pack, (uint16_t)target, &err),
          "reference view %u failed (reason %u)", (unsigned)target, err.reason);
    CHECK(count_nonblack(s_canvas) > 0, "reference view painted nothing");
    memcpy(reference, s_canvas, sizeof(reference));

    // 2) 坏包(B):同一 view 的最后一层中途失败 → 画布必须全黑,不能留半帧。
    steins_pack_t broken;
    uint8_t *copy = dup_pack(data, size, &broken);
    CHECK(copy != NULL, "broken pack copy failed");
    if (!copy) return;
    halve_asset_len(copy, &broken, bad_asset);
    steins_asset_t probe;
    CHECK(steins_pack_asset(&broken, bad_asset, &probe), "broken asset unreadable");
    CHECK(probe.len > 0, "broken asset length is zero");
    const bool ok_bad = steins_image_show_view(&image, &broken, (uint16_t)target, &err);
    CHECK(!ok_bad, "truncated JPEG layer reported success");
    CHECK(err.reason == STEINS_IMG_ERR_DECOMP || err.reason == STEINS_IMG_ERR_PREPARE ||
              err.reason == STEINS_IMG_ERR_SIZE || err.reason == STEINS_IMG_ERR_EMPTY,
          "unexpected failure reason %u", err.reason);
    CHECK(count_nonblack(s_canvas) == 0,
          "failed view left %u non-black pixels (half frame)", count_nonblack(s_canvas));
    free(copy);

    // 3) 恢复(A):同一个 view 重新渲染必须成功,并与参考帧逐像素相同。
    CHECK(steins_image_show_view(&image, pack, (uint16_t)target, &err),
          "recovery render failed (reason %u)", err.reason);
    uint32_t diff = 0;
    for (int i = 0; i < STEINS_IMAGE_PIXELS; ++i) {
        if (reference[i] != s_canvas[i]) ++diff;
    }
    CHECK(diff == 0, "recovery frame differs from reference in %u pixels", diff);
    printf("  view %u: A(bad last layer %u)->black->A identical (%u px)\n",
           (unsigned)target, (unsigned)bad_asset, (unsigned)(STEINS_IMAGE_PIXELS - diff));
}

static void test_failure_frame_asset(const steins_pack_t *pack, const uint8_t *data,
                                     uint32_t size)
{
    static uint16_t reference[STEINS_IMAGE_PIXELS];
    steins_image_t image;
    memset(&image, 0, sizeof(image));
    image.pixels = s_canvas;
    steins_image_error_t err;

    // 用鉴赏室第一组的第一张图作为 CG 入口。
    steins_gal_t gal;
    uint16_t asset = STEINS_NONE;
    CHECK(steins_pack_gal(pack, 0, &gal), "gallery group 0 unreadable");
    CHECK(steins_pack_gentry(pack, gal.first, &asset), "gallery entry 0 unreadable");
    CHECK(asset != STEINS_NONE, "gallery entry 0 has no asset");
    if (asset == STEINS_NONE) return;

    CHECK(steins_image_show_asset(&image, pack, asset, true, &err), "CG render failed (%u)",
          err.reason);
    CHECK(count_nonblack(s_canvas) > 0, "CG painted nothing");
    memcpy(reference, s_canvas, sizeof(reference));

    steins_pack_t broken;
    uint8_t *copy = dup_pack(data, size, &broken);
    CHECK(copy != NULL, "broken pack copy failed");
    if (!copy) return;
    halve_asset_len(copy, &broken, asset);
    CHECK(!steins_image_show_asset(&image, &broken, asset, true, &err),
          "broken CG reported success");
    CHECK(count_nonblack(s_canvas) == 0, "broken CG left %u non-black pixels",
          count_nonblack(s_canvas));
    free(copy);

    CHECK(steins_image_show_asset(&image, pack, asset, true, &err), "CG recovery failed (%u)",
          err.reason);
    uint32_t diff = 0;
    for (int i = 0; i < STEINS_IMAGE_PIXELS; ++i) {
        if (reference[i] != s_canvas[i]) ++diff;
    }
    CHECK(diff == 0, "CG recovery differs from reference in %u pixels", diff);
    printf("  CG asset %u: A->black->A identical\n", (unsigned)asset);
}

static void test_failures(const steins_pack_t *pack, const uint8_t *data, uint32_t size)
{
    steins_image_t image;
    memset(&image, 0, sizeof(image));
    image.pixels = s_canvas;
    steins_image_error_t err;

    // 非法下标
    CHECK(!steins_image_validate(pack, (uint16_t)steins_pack_count(pack, STEINS_SEC_ASSET),
                                 &err),
          "out-of-range asset accepted");
    CHECK(err.reason == STEINS_IMG_ERR_PACK, "out-of-range reason %u", err.reason);

    // 声明尺寸与 JPEG 头不符:复制一份包,改掉 ASSET 的 w/h。
    uint8_t *copy = (uint8_t *)malloc(size);
    memcpy(copy, data, size);
    steins_pack_t broken;
    CHECK(steins_pack_open(&broken, copy, size), "copy pack rejected");
    const uint32_t asset_off = broken.sec_off[STEINS_SEC_ASSET];
    // 第一条 ASSET 记录的 w 在 +16,w+? 直接改低 8 位
    copy[asset_off + 16] = (uint8_t)(copy[asset_off + 16] ^ 0x01u);
    steins_asset_t asset;
    CHECK(steins_pack_asset(&broken, 0, &asset), "broken asset unreadable");
    const bool size_ok = steins_image_validate(&broken, 0, &err);
    CHECK(!size_ok, "declared size mismatch accepted");
    CHECK(err.reason == STEINS_IMG_ERR_SIZE, "size mismatch reason %u", err.reason);
    free(copy);

    // 截断 JPEG:把 BLOB 段里第一张图的字节全部写成 0xFF。
    copy = (uint8_t *)malloc(size);
    memcpy(copy, data, size);
    CHECK(steins_pack_open(&broken, copy, size), "copy pack rejected");
    steins_asset_t first;
    CHECK(steins_pack_asset(&broken, 0, &first), "asset 0 unreadable");
    uint8_t *jpeg = copy + broken.sec_off[STEINS_SEC_BLOB] + first.off;
    memset(jpeg, 0xFF, first.len);
    CHECK(!steins_image_validate(&broken, 0, &err), "garbage JPEG accepted");
    CHECK(err.reason == STEINS_IMG_ERR_PREPARE || err.reason == STEINS_IMG_ERR_DECOMP,
          "garbage JPEG reason %u", err.reason);

    // 段越界:把 ASSET 的 len 改大(steins_pack_asset 自己就要拒绝)
    const uint32_t off_field = broken.sec_off[STEINS_SEC_ASSET] + 4;
    copy[off_field] = 0xFFu;
    copy[off_field + 1] = 0xFFu;
    copy[off_field + 2] = 0xFFu;
    copy[off_field + 3] = 0x7Fu;
    CHECK(!steins_pack_asset(&broken, 0, &first), "out-of-range asset length accepted");
    free(copy);
    printf("  failure injection checked\n");
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "assets/steins/steins_pack.bin";
    uint32_t size = 0;
    uint8_t *data = load_file(path, &size);
    if (!data) {
        fprintf(stderr, "cannot read pack: %s\n", path);
        return 2;
    }
    steins_pack_t pack;
    if (!steins_pack_open(&pack, data, size)) {
        fprintf(stderr, "pack rejected\n");
        free(data);
        return 2;
    }
    printf("image regression on %s (%u bytes)\n", path, (unsigned)size);
    test_all_assets(&pack);
    test_view_render(&pack);
    test_mask_and_clip(&pack);
    test_failure_frame_view(&pack, data, size);
    test_failure_frame_asset(&pack, data, size);
    test_layer_order(&pack);
    test_failures(&pack, data, size);
    free(data);
    if (g_failures) {
        printf("FAILED (%d)\n", g_failures);
        return 1;
    }
    printf("OK\n");
    return 0;
}
