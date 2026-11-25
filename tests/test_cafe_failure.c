// tests/test_cafe_failure.c —— 真实坏 pack：正文 CRC、图像 CRC、坏 JPEG 的 app 级失败传播。
#include "cafe_app.h"
#include "cafe_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #cond); return 1; } } while (0)

static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const unsigned char *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void wr32(unsigned char *p, uint32_t v)
{ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

typedef struct { uint32_t type, off, size, count, crc, table_off; } sec_t;

static sec_t find_sec(const unsigned char *p, uint32_t type)
{
    sec_t s = {0};
    const uint32_t n = rd32(p + 20);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t t = rd32(p + 128 + i * 24);
        if (t == type) {
            s.type = t; s.off = rd32(p + 128 + i * 24 + 4);
            s.size = rd32(p + 128 + i * 24 + 8);
            s.count = rd32(p + 128 + i * 24 + 12);
            s.crc = rd32(p + 128 + i * 24 + 16);
            s.table_off = 128 + i * 24;
            return s;
        }
    }
    return s;
}

static unsigned char *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f); *size = (size_t)n; return b;
}

static void tap(cafe_app_t *app, uint8_t key, uint32_t t)
{
    cafe_app_key(app, key, true, t);
    cafe_app_key(app, key, false, t + 40);
}

static int run_case(const unsigned char *original, size_t size, int mode)
{
    unsigned char *pack = malloc(size);
    CHECK(pack);
    memcpy(pack, original, size);
    CHECK(cafe_pack_set_data(pack, size));

    const sec_t blocks = find_sec(pack, 16);
    const sec_t text = find_sec(pack, 17);
    const sec_t images = find_sec(pack, 18);
    const sec_t imgdata = find_sec(pack, 19);
    const sec_t assets = find_sec(pack, 14);
    CHECK(blocks.size && text.size && images.size && imgdata.size && assets.size);
    CHECK(rd16(pack + blocks.off) == 2);   // n2 block
    const uint32_t block_off = rd32(pack + blocks.off + 8);
    const uint32_t block_comp = rd32(pack + blocks.off + 12);
    CHECK(block_comp > 16);

    const uint16_t bg_image_id = rd16(pack + assets.off);   // a0 -> image id
    CHECK(bg_image_id < images.count);
    const unsigned char *img0 = pack + images.off + (size_t)bg_image_id * 36;
    const uint32_t img0_off = rd32(img0 + 8);
    const uint32_t img0_len = rd32(img0 + 12);
    const uint32_t img0_mask_off = rd32(img0 + 16);
    const uint32_t img0_mask_len = rd32(img0 + 20);
    CHECK(img0_len > 16);

    if (mode == 0) {
        // 正文压缩数据坏：文本块 CRC/解压失败。
        pack[text.off + block_off + 10] ^= 0x5A;
    } else if (mode == 1) {
        // 图像数据坏但 record CRC 未更新：image verify 失败。
        pack[imgdata.off + img0_off + 10] ^= 0x5A;
    } else {
        // 坏 JPEG（SOI 破坏）但把 image record 与 images section CRC 都改对：
        // 只有真实 JPEG 解码失败，不走 CRC 失败路径。
        pack[imgdata.off + img0_off] = 0x00;
        uint32_t crc = cafe_crc32(0, pack + imgdata.off + img0_off, img0_len);
        crc = cafe_crc32(crc, pack + imgdata.off + img0_mask_off, img0_mask_len);
        wr32((unsigned char *)img0 + 32, crc);
        const uint32_t sec_crc = cafe_crc32(0, pack + images.off, images.size);
        wr32(pack + images.table_off + 16, sec_crc);
    }
    CHECK(cafe_pack_set_data(pack, size));

    cafe_ui_t ui;
    cafe_bands_t bands;
    memset(&ui, 0, sizeof(ui));
    memset(&bands, 0, sizeof(bands));
    CHECK(cafe_image_bands_init(&bands));
    cafe_app_t app;
    CHECK(cafe_app_init(&app, &ui, &bands));
    app.now_ms = 1000;
    app.last_activity_ms = 1000;
    const uint32_t unlock_before = app.unlock.progress;
    tap(&app, CAFE_KEY_OK, 1000);    // start -> title
    CHECK(app.page == CAFE_PAGE_TITLE);
    tap(&app, CAFE_KEY_OK, 1100);    // new game -> n2[0]
    CHECK(app.resource_fault);
    CHECK(app.modal_open);
    CHECK(app.page == CAFE_PAGE_GAME || app.page == CAFE_PAGE_TITLE);
    CHECK(app.vm.node == 2 && app.vm.index == 0);
    CHECK(!app.progress_dirty);
    CHECK(app.unlock.progress == unlock_before);

    const uint16_t node = app.vm.node, index = app.vm.index;
    tap(&app, CAFE_KEY_OK, 1200);    // retry, fails again
    CHECK(app.resource_fault);
    CHECK(app.vm.node == node && app.vm.index == index);
    cafe_app_tick(&app, 2000, 5000); // 自动/快进/存档不得越过故障
    CHECK(app.vm.node == node && app.vm.index == index);
    CHECK(!app.progress_dirty);
    CHECK(app.unlock.progress == unlock_before);
    CHECK(!app.unlock_dirty);

    tap(&app, CAFE_KEY_DOWN, 1300);  // 选择“返回标题”
    tap(&app, CAFE_KEY_OK, 1400);
    CHECK(app.page == CAFE_PAGE_TITLE);
    CHECK(!app.resource_fault);
    cafe_save_t saved;
    CHECK(!cafe_store_load_auto(&saved));   // 失败位置没有写成成功进度

    cafe_image_bands_free(&bands);
    free(pack);
    return 0;
}

int main(int argc, char **argv)
{
    (void)argc;
    size_t size = 0;
    unsigned char *pack = read_file(argv[1], &size);
    CHECK(pack);
    for (int mode = 0; mode < 3; ++mode) {
        CHECK(run_case(pack, size, mode) == 0);
        printf("failure mode %d PASS\n", mode);
    }
    free(pack);
    printf("cafe failure propagation PASS\n");
    return 0;
}
