// tests/test_summer_app_image.c —— app 层坏立绘回滚 + 恢复重试回归。
//
// 找到一个真实正文停点（vm.body != 0），把该立绘记录的 JPEG len 砍半，让真实
// TJpgDec 在解码中途失败；启动生产 app，断言失败帧是完整背景（不是半张立绘），
// 然后恢复码流并走“保存→读取”同一停点，断言同一资源重试成功。
#include "summer_app.h"
#include "summer_model.h"
#include "summer_ui.h"

#include "bsp_button.h"
#include "lvgl.h"
#include "nvs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const lv_font_t gal_cjk_16 = { 0 };

#ifndef SUMMER_PACK_PATH
#define SUMMER_PACK_PATH "main/summer_data/summer_pack.bin"
#endif

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        failures++; \
    } \
} while (0)

static uint8_t *read_all(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) {
        fclose(f);
        return NULL;
    }
    uint8_t *p = malloc((size_t)n);
    if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) {
        free(p);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return p;
}

static void key(summer_app_t *app, int btn, int ev)
{
    summer_key_t k = { (uint8_t)btn, (uint8_t)ev };
    summer_app_key(app, &k);
}

static void long_press(summer_app_t *app, int btn)
{
    key(app, btn, BSP_BTN_PRESS);
    key(app, btn, BSP_BTN_LONG);
    key(app, btn, BSP_BTN_RELEASE);
    key(app, btn, BSP_BTN_CLICK);   // 驱动补发的 CLICK 必须被吞掉
}

static void short_press(summer_app_t *app, int btn)
{
    key(app, btn, BSP_BTN_PRESS);
    key(app, btn, BSP_BTN_RELEASE);
    key(app, btn, BSP_BTN_CLICK);
}

static uint32_t section_offset(const uint8_t *blob, int type)
{
    const uint32_t count = (uint32_t)blob[12] | ((uint32_t)blob[13] << 8);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *s = blob + 20 + i * 16;
        const uint32_t t = (uint32_t)s[0] | ((uint32_t)s[1] << 8);
        if (t == (uint32_t)type) {
            return (uint32_t)s[4] | ((uint32_t)s[5] << 8) | ((uint32_t)s[6] << 16) |
                   ((uint32_t)s[7] << 24);
        }
    }
    return 0;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

int main(void)
{
    size_t size = 0;
    uint8_t *good = read_all(SUMMER_PACK_PATH, &size);
    if (!good) {
        fprintf(stderr, "SKIP: cannot read %s\n", SUMMER_PACK_PATH);
        return 77;
    }
    summer_pack_t pack;
    const char *err = NULL;
    CHECK(summer_pack_open(&pack, good, (uint32_t)size, &err), "pack: %s", err ? err : "?");

    // Find a real dialogue stop with a portrait body.
    summer_vm_t scan;
    summer_vm_init(&scan);
    CHECK(summer_vm_start_story(&scan, &pack), "scan start");
    uint16_t target_scene = 0;
    uint32_t target_op = 0;
    uint16_t target_body = 0;
    uint16_t target_bg = 0;
    for (int i = 0; i < 4000; ++i) {
        const summer_step_t st = summer_vm_step(&scan, &pack, 256);
        if (st.kind == SUMMER_STEP_TEXT) {
            if (scan.body != 0) {
                target_scene = scan.scene_id;
                target_op = scan.op_index;
                target_body = scan.body;
                target_bg = scan.bg;
                break;
            }
            if (!summer_vm_advance(&scan, &pack)) break;
        } else if (st.kind == SUMMER_STEP_CHOICE) {
            int items[32];
            const int n = summer_vm_visible_choices(&scan, &pack, st.group, items, 32);
            if (n <= 0 || !summer_vm_choose(&scan, &pack, st.group, items[0])) break;
        } else if (st.kind == SUMMER_STEP_MAP) {
            summer_map_choice_t map[40];
            const int n = summer_vm_preview_map(&scan, &pack, st.group, map, 40);
            if (n <= 0 || !summer_vm_choose_map(&scan, &pack, st.group, map[0].item_index)) break;
        } else {
            break;
        }
    }
    CHECK(target_body != 0, "no dialogue body found");
    if (!target_body) {
        free(good);
        return 1;
    }
    printf("target scene=%u op=%u bg=%u body=%u\n", target_scene, target_op, target_bg,
           target_body);

    // Reference background (good pack) for the target stop.
    static uint16_t reference[SUMMER_ART_W * SUMMER_ART_H];
    summer_canvas_t ref_canvas;
    CHECK(summer_image_bind(&ref_canvas, reference), "ref bind");
    CHECK(summer_image_scene(&ref_canvas, &pack, target_bg, 0), "ref background");

    // Corrupt the target portrait record's JPEG length (mid-stream truncation).
    uint8_t *bad = malloc(size);
    CHECK(bad != NULL, "bad pack alloc");
    if (!bad) {
        free(good);
        return 1;
    }
    memcpy(bad, good, size);
    summer_pack_t bad_pack;
    CHECK(summer_pack_open(&bad_pack, bad, (uint32_t)size, &err), "bad pack: %s", err ? err : "?");
    const summer_image_t *target_image = summer_image(&bad_pack, target_body);
    CHECK(target_image != NULL, "target image");
    uint32_t original_len = 0;
    uint8_t *record = NULL;
    if (target_image) {
        const uint32_t index = (uint32_t)(target_image - bad_pack.images);
        uint8_t *records = bad + section_offset(bad, 8);
        record = records + index * 24;
        original_len = rd32(record + 4);
        CHECK(original_len > 64, "target len %u", original_len);
        wr32(record + 4, original_len / 2);
    }

    stub_nvs_reset();
    static summer_app_t app;
    CHECK(summer_app_init(&app, bad, (uint32_t)size), "app init");
    CHECK(app.pack_ok, "app pack");
    short_press(&app, BSP_BTN_OK);  // title -> story
    CHECK(app.page == APP_PAGE_GAME, "app story page %d", app.page);

    // Advance until the app reaches the target stop and renders it.  Follow
    // the same first-choice strategy as the target scan.
    for (int i = 0; i < 4000; ++i) {
        if (app.page == APP_PAGE_GAME && app.vm.scene_id == target_scene &&
            app.vm.op_index == target_op) {
            break;
        }
        if (app.page == APP_PAGE_CHOICE || app.page == APP_PAGE_MAP) {
            short_press(&app, BSP_BTN_OK);
        } else if (app.page == APP_PAGE_GAME) {
            short_press(&app, BSP_BTN_UP);
        } else {
            break;
        }
    }
    CHECK(app.page == APP_PAGE_GAME && app.vm.scene_id == target_scene &&
          app.vm.op_index == target_op, "target stop not reached (%u:%u)", app.vm.scene_id,
          app.vm.op_index);
    CHECK(app.image_error, "bad portrait did not report an image error");
    CHECK(strstr(app.notice, "图片读取失败") != NULL, "failure notice: %s", app.notice);
    CHECK(memcmp(app.pixels, reference, sizeof(reference)) == 0,
          "failure frame is not the complete background");
    CHECK(app.shown_body != target_body, "failed body was cached as shown");

    // Restore the stream, then save/load the same stop; app_maps must retry the
    // same resource and succeed.
    if (record) wr32(record + 4, original_len);
    // Open menu (long OK), save to slot 1, load slot 1.
    long_press(&app, BSP_BTN_OK);
    CHECK(app.page == APP_PAGE_MENU, "menu expected, got %d", app.page);
    app.menu_sel = 2;
    short_press(&app, BSP_BTN_OK);  // save
    CHECK(app.page == APP_PAGE_SLOTS, "save slots expected, got %d", app.page);
    short_press(&app, BSP_BTN_OK);  // slot 1
    CHECK(app.page == APP_PAGE_MENU || app.page == APP_PAGE_GAME,
          "after save page %d", app.page);
    if (app.page == APP_PAGE_MENU) {
        app.menu_sel = 3;
        short_press(&app, BSP_BTN_OK);  // load
        CHECK(app.page == APP_PAGE_SLOTS, "load slots expected, got %d", app.page);
        short_press(&app, BSP_BTN_OK);  // slot 1
    }
    CHECK(!app.image_error, "retry still reports image error");
    CHECK(app.shown_body == target_body, "retry did not show target body (%u)", app.shown_body);
    int diff = 0;
    for (int i = 0; i < SUMMER_ART_W * SUMMER_ART_H; ++i) {
        if (app.pixels[i] != reference[i]) diff++;
    }
    CHECK(diff > 1000, "retry frame did not contain the portrait (%d)", diff);

    free(bad);
    free(good);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_summer_app_image: PASS\n");
    return 0;
}
