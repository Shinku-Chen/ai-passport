// tests/test_senren_model.c —— 用真实剧本包/图片包跑数据层的宿主测试。
//
// 只依赖 senren_pack.c / senren_model.c(标准 C + zlib),不需要 ESP-IDF。
// 包不在仓库时打印 SKIP 并以 0 退出,保证没带包的 checkout 也能过门禁。
#include "senren_model.h"
#include "senren_pack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition, ...)                                       \
    do {                                                            \
        if (!(condition)) {                                         \
            failures++;                                             \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);             \
            printf(__VA_ARGS__);                                    \
            printf("\n");                                           \
        }                                                           \
    } while (0)

static uint8_t *read_file(const char *path, uint32_t *size_out)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size <= 0) {
        fclose(file);
        return NULL;
    }
    rewind(file);
    uint8_t *data = (uint8_t *)malloc((size_t)size);
    if (data == NULL || fread(data, 1, (size_t)size, file) != (size_t)size) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *size_out = (uint32_t)size;
    return data;
}

static void test_pack(const senren_pack_t *pack)
{
    CHECK(pack->entry_count == 785, "图片包条目数 = %u,期望 785", (unsigned)pack->entry_count);

    // 三个名字空间都能按名字查到,且找不到的名字明确失败
    senren_asset_t asset;
    CHECK(senren_pack_find(pack, SENREN_POOL_BG, "空_青空", &asset), "找不到背景 空_青空");
    CHECK(asset.kind == SENREN_KIND_BG, "空_青空 类型 = %u", (unsigned)asset.kind);
    CHECK(asset.w == 240 && asset.h == 320, "背景尺寸 = %ux%u", (unsigned)asset.w, (unsigned)asset.h);

    CHECK(senren_pack_find(pack, SENREN_POOL_CH, "mz01_1", &asset), "找不到立绘 mz01_1");
    CHECK(asset.kind == SENREN_KIND_SPRITE, "mz01_1 类型 = %u", (unsigned)asset.kind);
    CHECK(asset.h == 320, "立绘高度 = %u,期望 320", (unsigned)asset.h);

    CHECK(senren_pack_find(pack, SENREN_POOL_EV, "sd001ed", &asset), "找不到 SD sd001ed");
    CHECK(asset.kind == SENREN_KIND_SD, "sd001ed 类型 = %u", (unsigned)asset.kind);

    CHECK(senren_pack_find(pack, SENREN_POOL_EV, "ev414b", &asset), "找不到事件图 ev414b");
    CHECK(asset.kind == SENREN_KIND_CG || asset.kind == SENREN_KIND_CG_DIFF, "ev414b 类型 = %u",
          (unsigned)asset.kind);

    CHECK(!senren_pack_find(pack, SENREN_POOL_EV, "no_such_name", &asset), "不存在的名字应该查不到");

    // 补丁条目的载荷必须能拆成掩码 + 像素两段
    static const char *const patches[] = { "ev414c", "ev414d", "ev414e", "ev215a" };
    for (size_t index = 0; index < sizeof(patches) / sizeof(patches[0]); index++) {
        if (!senren_pack_find(pack, SENREN_POOL_EV, patches[index], &asset)) {
            continue;
        }
        if (asset.kind != SENREN_KIND_CG_DIFF) {
            continue;
        }
        const uint8_t *mask = NULL;
        const uint8_t *pixels = NULL;
        uint32_t mask_len = 0;
        uint32_t pixels_len = 0;
        CHECK(senren_patch_parts(&asset, &mask, &mask_len, &pixels, &pixels_len),
              "%s 补丁载荷拆不开", patches[index]);
        CHECK(mask_len > 0 && pixels_len > 0, "%s 补丁某一段为空", patches[index]);
        CHECK(asset.dw > 0 && asset.dh > 0, "%s 补丁矩形为空", patches[index]);
        CHECK(asset.dx + asset.dw <= 240 && asset.dy + asset.dh <= 320, "%s 补丁超出画布",
              patches[index]);
    }
}

static void test_scn(const senren_scn_t *scn)
{
    CHECK(scn->chunk_count == 112, "剧本块数 = %u,期望 112", (unsigned)scn->chunk_count);
    CHECK(scn->char_count > 3000 && scn->char_count < 4000, "字符表大小 = %u",
          (unsigned)scn->char_count);
    CHECK(scn->speaker_count > 100, "说话人字典 = %u", (unsigned)scn->speaker_count);
    CHECK(scn->bg_count > 50, "背景字典 = %u", (unsigned)scn->bg_count);
    CHECK(scn->event_count > 500, "事件图字典 = %u", (unsigned)scn->event_count);
    CHECK(scn->ending_count > 0, "结局字典 = %u", (unsigned)scn->ending_count);
    CHECK(scn->flag_count == 44, "标志位 = %u,期望 44", (unsigned)scn->flag_count);

    char name[SENREN_NAME_MAX];
    CHECK(senren_scn_speaker(scn, 1, name, sizeof(name)) > 0, "说话人 1 解不出来");
    CHECK(senren_scn_background(scn, 0, name, sizeof(name)) > 0, "背景 0 解不出来");
    CHECK(senren_scn_ending(scn, 0, name, sizeof(name)) > 0, "结局 0 解不出来");
}

static void test_reading(const senren_scn_t *scn)
{
    static senren_player_t player;
    const senren_layout_t layout = { 26, 5 };
    CHECK(senren_player_start(&player, scn, 0, 0, 0, &layout), "开始阅读失败");

    // 一路推进,应当先看到章节卡,再看到正文;正文与背景名都不该是空的
    bool saw_chapter = false;
    bool saw_text = false;
    for (int step = 0; step < 64 && !saw_text; step++) {
        senren_step_t result = senren_player_advance(&player, scn, &layout);
        if (result == SENREN_STEP_CHAPTER) {
            saw_chapter = true;
            CHECK(player.chapter_title[0] != '\0', "章节卡没有标题");
        } else if (result == SENREN_STEP_TEXT) {
            saw_text = true;
            CHECK(player.text[0] != '\0', "正文为空");
            CHECK(player.page[0] != '\0', "当前页为空");
            CHECK(player.page_count >= 1, "页数 = %u", (unsigned)player.page_count);
            CHECK(player.bg[0] != '\0', "背景名为空(应该已经切过背景)");
            char buffer[SENREN_TEXT_BUFFER];
            CHECK(senren_player_page_text(&player, buffer, sizeof(buffer)) > 0, "取当前页失败");
            CHECK(senren_player_speaker(&player, buffer, sizeof(buffer)) >= 0, "取说话人失败");
        } else if (result == SENREN_STEP_STUCK) {
            CHECK(false, "推进卡住了");
            break;
        }
    }
    CHECK(saw_chapter, "前 64 步里没有遇到章节卡");
    CHECK(saw_text, "前 64 步里没有读到正文");

    // 连续推进 200 步不应崩溃,页号也应保持在有效范围内
    for (int step = 0; step < 200; step++) {
        senren_step_t result = senren_player_advance(&player, scn, &layout);
        if (result == SENREN_STEP_STUCK || result == SENREN_STEP_ENDING) {
            break;
        }
        CHECK(player.page_index < player.page_count || player.page_count == 0,
              "页号越界: %u/%u", (unsigned)player.page_index, (unsigned)player.page_count);
    }

    // 存档往返
    senren_save_t saved;
    senren_save_from_player(&player, &saved);
    uint8_t encoded[512];
    size_t length = senren_save_encode(&saved, encoded, sizeof(encoded));
    CHECK(length > 0, "存档编码失败");
    senren_save_t restored;
    memset(&restored, 0, sizeof(restored));
    CHECK(senren_save_decode(&restored, encoded, length), "存档解码失败");
    CHECK(restored.chunk == saved.chunk && restored.node == saved.node,
          "存档位置不一致: %u/%u vs %u/%u", (unsigned)restored.chunk, (unsigned)restored.node,
          (unsigned)saved.chunk, (unsigned)saved.node);
    CHECK(restored.chapter == saved.chapter, "存档章节不一致");
    CHECK(strcmp(restored.bg, saved.bg) == 0, "存档背景名不一致");
    CHECK(memcmp(restored.flags, saved.flags, sizeof(saved.flags)) == 0, "存档标志位不一致");
}

static void test_layout(void)
{
    CHECK(senren_char_units('A') == 1, "半角宽度算错");
    CHECK(senren_char_units(0x6C49) == 2, "汉字宽度算错");
    CHECK(senren_char_units(0x3002) == 2, "句号宽度算错");

    uint32_t offsets[16];
    CHECK(senren_text_lines("abcdefgh", 4, offsets, 16) == 2, "8 个半角字符按每行 4 格应为 2 行");
    CHECK(offsets[1] == 4, "第二行起点 = %u,期望 4", (unsigned)offsets[1]);

    // 行首不能是句号:断点应往前挪一格
    const char *text = "一二三。";
    int lines = senren_text_lines(text, 6, offsets, 16);
    CHECK(lines == 2, "「一二三。」按每行 6 格应为 2 行,实际 %d", lines);
    CHECK(offsets[1] == 6, "句号不能落在行首(第二行起点 = %u)", (unsigned)offsets[1]);

    // 单句分页:每行 13 字、每页 5 行 -> 60 个字应当切成一页以上
    char long_text[SENREN_TEXT_BUFFER];
    size_t written = 0;
    for (int index = 0; index < 70 && written + 3 + 1 < sizeof(long_text); index++) {
        written += (size_t)snprintf(long_text + written, sizeof(long_text) - written, "字");
    }
    int pages = senren_text_pages(long_text, 26, 5, offsets, 16);
    CHECK(pages == 2, "70 个字按 13 字/行 5 行/页应为 2 页,实际 %d", pages);
}

int main(void)
{
    uint32_t pack_size = 0;
    uint32_t scn_size = 0;
    uint8_t *pack_data = read_file("main/senren_data/senren_pack.bin", &pack_size);
    uint8_t *scn_data = read_file("main/senren_data/senren_scn.bin", &scn_size);
    if (pack_data == NULL || scn_data == NULL) {
        printf("SKIP: 仓库里没有 main/senren_data/*.bin(先跑 tools/senren_fetch_source.py 与打包器)\n");
        free(pack_data);
        free(scn_data);
        return 0;
    }

    senren_pack_t pack;
    if (!senren_pack_open(&pack, pack_data, pack_size)) {
        printf("FAIL: 图片包打不开\n");
        return 1;
    }
    senren_scn_t scn;
    if (!senren_scn_open(&scn, scn_data, scn_size)) {
        printf("FAIL: 剧本包打不开\n");
        return 1;
    }

    test_pack(&pack);
    test_scn(&scn);
    test_reading(&scn);
    test_layout();

    free(pack_data);
    free(scn_data);
    if (failures == 0) {
        printf("senren 数据层测试: PASS\n");
        return 0;
    }
    printf("senren 数据层测试: FAIL(%d 项)\n", failures);
    return 1;
}
