// tests/test_tsxx_model.c —— 用真实资源包驱动阅读器的纯逻辑层。
//
// 覆盖:
//   1. 包结构与各段计数;
//   2. 从第 0 页一路推进到页表末尾,逐页检查分屏数、正文可解码、资源下标合法;
//   3. 章节点扫描(45 个)与顺序;
//   4. 选项点:数量、文案非空、目标页合法且可跳转;
//   5. 分页排版:强制换行、禁则标点、宽字符计量、超长页分屏;
//   6. 存档编解码往返。
//
// 不需要 ESP-IDF / LVGL,也不需要编译器之外的依赖。构建命令见 tools/validate.sh:
//   cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_tsxx_model.c
//      main/tsxx_pack.c main/tsxx_model.c -o test_tsxx_model
//   ./test_tsxx_model main/tsxx_data/tsxx_pack.bin

#include "tsxx_model.h"
#include "tsxx_pack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            ++g_failures;                                                     \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);              \
            fprintf(stderr, __VA_ARGS__);                                     \
            fputc('\n', stderr);                                              \
        }                                                                     \
    } while (0)

static uint8_t *read_file(const char *path, uint32_t *size)
{
    FILE *handle = fopen(path, "rb");
    if (handle == NULL) {
        fprintf(stderr, "无法打开资源包 %s\n", path);
        return NULL;
    }
    fseek(handle, 0, SEEK_END);
    const long length = ftell(handle);
    fseek(handle, 0, SEEK_SET);
    if (length <= 0) {
        fclose(handle);
        return NULL;
    }
    uint8_t *data = (uint8_t *)malloc((size_t)length);
    if (data == NULL || fread(data, 1, (size_t)length, handle) != (size_t)length) {
        free(data);
        fclose(handle);
        return NULL;
    }
    fclose(handle);
    *size = (uint32_t)length;
    return data;
}

// 16px 字体、208px 宽正文区:13 个全角单位一行,5 行一屏。
static const tsxx_layout_t LAYOUT_16 = { .units_per_line = 13, .lines_per_page = 5 };

static void test_pack_structure(const tsxx_pack_t *pack)
{
    CHECK(tsxx_pack_pages(pack) == 61436u, "页数 %u != 61436", (unsigned)tsxx_pack_pages(pack));
    CHECK(tsxx_pack_bg_count(pack) == 90u, "背景 %u 张", (unsigned)tsxx_pack_bg_count(pack));
    CHECK(tsxx_pack_sprite_count(pack) == 156u, "立绘 %u 张",
          (unsigned)tsxx_pack_sprite_count(pack));
    CHECK(tsxx_pack_cg_count(pack) == 295u, "事件整帧 %u 张",
          (unsigned)tsxx_pack_cg_count(pack));
    CHECK(tsxx_pack_cg_total(pack) == 19417u, "有事件图的页 %u",
          (unsigned)tsxx_pack_cg_total(pack));

    char name[64];
    CHECK(tsxx_pack_name(pack, TSXX_TABLE_BG, 0, name, sizeof(name)) > 0, "背景名表空");
    CHECK(tsxx_pack_name(pack, TSXX_TABLE_SPEAKER, 0, name, sizeof(name)) > 0,
          "说话人名表空");

    // 第一页的正文必须与源数据一致。
    tsxx_page_t page;
    CHECK(tsxx_pack_page(pack, 0, &page), "第 0 页读不出来");
    char text[TSXX_TEXT_BUFFER];
    tsxx_pack_text(pack, &page, 0, 0, text, sizeof(text));
    CHECK(strcmp(text, "在伸手不见五指的黑暗中。") == 0, "第 0 页正文是 %s", text);

    // 超出范围的页必须被拒绝。
    CHECK(!tsxx_pack_page(pack, tsxx_pack_pages(pack), &page), "越界页没被拒绝");
}

static void test_chapters(const tsxx_pack_t *pack)
{
    tsxx_chapter_t chapters[TSXX_MAX_CHAPTERS];
    const int count = tsxx_chapters_scan(pack, chapters, TSXX_MAX_CHAPTERS);
    CHECK(count == 45, "章节点 %d 个 != 45", count);
    CHECK(count > 0 && chapters[0].page == 0u, "第一个章节点应在第 0 页,实际 %u",
          (unsigned)(count > 0 ? chapters[0].page : 0u));
    for (int i = 1; i < count; ++i) {
        CHECK(chapters[i].page > chapters[i - 1].page, "章节点 %d 页序倒挂", i);
        CHECK(chapters[i].name_id != chapters[i - 1].name_id, "章节点 %d 重复", i);
    }
}

static void test_full_walk(const tsxx_pack_t *pack)
{
    tsxx_player_t player;
    CHECK(tsxx_player_start(&player, pack, 0, &LAYOUT_16), "无法从第 0 页开始");

    char text[TSXX_TEXT_BUFFER];
    uint32_t screens_seen = 0;
    uint32_t choice_stops = 0;
    uint32_t steps = 0;
    tsxx_step_t step = TSXX_STEP_PAGE;

    for (;;) {
        if (player.at_choice) {
            ++choice_stops;
            CHECK(player.choice_count >= 2 && player.choice_count <= 5,
                  "第 %u 页选项数 %u 异常", (unsigned)player.page, (unsigned)player.choice_count);
            char option[TSXX_TEXT_BUFFER];
            uint32_t target = 0;
            for (uint8_t slot = 0; slot < player.choice_count; ++slot) {
                CHECK(tsxx_pack_choice_option(pack, player.page, slot, option,
                                              sizeof(option), &target),
                      "第 %u 页第 %u 项读不出来", (unsigned)player.page, (unsigned)slot);
                CHECK(option[0] != '\0', "第 %u 页第 %u 项文案为空", (unsigned)player.page,
                      (unsigned)slot);
                CHECK(target < tsxx_pack_pages(pack), "第 %u 页第 %u 项目标越界",
                      (unsigned)player.page, (unsigned)slot);
            }
            // 选第一项继续走下去,确保跳转后的状态仍然自洽。
            CHECK(tsxx_player_choose(&player, pack, 0, &LAYOUT_16), "选择失败");
            step = TSXX_STEP_PAGE;
        }

        CHECK(player.screens <= TSXX_MAX_SCREENS, "第 %u 页分屏数 %u 超出上限",
              (unsigned)player.page, (unsigned)player.screens);
        CHECK(player.screen < player.screens || player.screens == 0,
              "第 %u 页屏号 %u 超出 %u", (unsigned)player.page, (unsigned)player.screen,
              (unsigned)player.screens);

        const size_t length = tsxx_player_screen_text(&player, pack, &LAYOUT_16, text,
                                                      sizeof(text));
        CHECK(length < sizeof(text), "第 %u 页正文超出缓冲", (unsigned)player.page);
        CHECK(player.screens == 0 ? length == 0 : length > 0,
              "第 %u 页第 %u 屏文本长度 %zu 与分屏数 %u 不符",
              (unsigned)player.page, (unsigned)player.screen, length,
              (unsigned)player.screens);

        ++screens_seen;

        step = tsxx_player_advance(&player, pack, &LAYOUT_16);
        if (step == TSXX_STEP_END) {
            CHECK(player.ended, "结束时 ended 没置位");
            break;
        }
        CHECK(step != TSXX_STEP_STUCK, "第 %u 页卡住(非选项非结束)", (unsigned)player.page);
        if (++steps > 1000000u) {
            CHECK(0, "推进次数异常,疑似死循环");
            break;
        }
    }

    CHECK(player.page == tsxx_pack_pages(pack) - 1u, "结束时停在第 %u 页",
          (unsigned)player.page);
    // 一路选第一项会走出“主线 + 跳过支线块”的路径:实测 77647 屏、2 个选项点。
    // 这里只卡区间,换一条选法不会误报。
    CHECK(choice_stops >= 1u && choice_stops <= 13u, "走过的选项点 %u 个不合理",
          (unsigned)choice_stops);
    CHECK(screens_seen > 70000u, "总屏数 %u 偏少", (unsigned)screens_seen);
    printf("  剧情线: %u 屏 / %u 个选项点 / 停在末页 %u\n", (unsigned)screens_seen,
           (unsigned)choice_stops, (unsigned)player.page);
}

static void test_jump_and_save(const tsxx_pack_t *pack)
{
    tsxx_player_t player;
    CHECK(tsxx_player_jump(&player, pack, 61435u, &LAYOUT_16), "跳到末页失败");
    CHECK(player.page == 61435u && !player.ended, "跳转后状态不对");

    // 跳到一个已知的选项点(源数据第 2028 页)。
    CHECK(tsxx_player_jump(&player, pack, 2028u, &LAYOUT_16), "跳到选项页失败");
    CHECK(player.at_choice, "第 2028 页应是选项点");
    CHECK(player.choice_count == 2u, "第 2028 页选项数 %u != 2",
          (unsigned)player.choice_count);

    tsxx_save_t save = { .page = 1234u, .screen = 2u };
    uint8_t encoded[16];
    CHECK(tsxx_save_encode(&save, encoded, sizeof(encoded)) == 8u, "存档长度不为 8");
    tsxx_save_t back;
    CHECK(tsxx_save_decode(&back, encoded, 8u), "存档解码失败");
    CHECK(back.page == 1234u && back.screen == 2u, "存档往返不一致");
    CHECK(!tsxx_save_decode(&back, encoded, 4u), "过短的存档没被拒绝");

    // 跳转到章节点后应能继续推进。
    tsxx_chapter_t chapters[TSXX_MAX_CHAPTERS];
    const int count = tsxx_chapters_scan(pack, chapters, TSXX_MAX_CHAPTERS);
    CHECK(count == 45, "章节点数量异常");
    CHECK(tsxx_player_jump(&player, pack, chapters[count - 1].page, &LAYOUT_16),
          "跳到最后章失败");
    CHECK(!player.ended, "跳章后不该处于结束状态");
}

static void test_pagination(void)
{
    uint32_t offsets[TSXX_MAX_SCREENS + 1];
    const tsxx_layout_t narrow = { .units_per_line = 5, .lines_per_page = 2 };

    // 10 个全角字、每行 5 单位(2.5 字)→ 每行 2 字,4 行,每屏 2 行 → 2 屏。
    const char *ten = "一二三四五六七八九十";
    int screens = tsxx_text_screens(ten, &narrow, offsets, TSXX_MAX_SCREENS);
    CHECK(screens == 3, "10 个全角字在 5x2 版面下应为 3 屏,实际 %d", screens);
    CHECK(offsets[0] == 0u, "首屏偏移不为 0");
    CHECK(offsets[screens] == (uint32_t)strlen(ten), "末屏偏移不是文本长度");

    // 强制换行:两行文本在 1 行 1 屏的版面下分成 2 屏。
    const tsxx_layout_t one = { .units_per_line = 20, .lines_per_page = 1 };
    const char *two = "第一行\n第二行";
    screens = tsxx_text_screens(two, &one, offsets, TSXX_MAX_SCREENS);
    CHECK(screens == 2, "带换行的两行应为 2 屏,实际 %d", screens);

    // 空文本不产生屏。
    CHECK(tsxx_text_screens("", &one, offsets, TSXX_MAX_SCREENS) == 0, "空文本不该有屏");

    // 制表宽度:半角 1 单位、全角 2 单位。
    CHECK(tsxx_char_units('A') == 1, "ASCII 宽度不是 1");
    CHECK(tsxx_char_units(0x3002u) == 2, "。宽度不是 2");
    CHECK(tsxx_char_units(0x2026u) == 2, "…宽度不是 2");
    CHECK(tsxx_char_units(0x3042u) == 2, "あ宽度不是 2");
    CHECK(tsxx_char_units(0x4E00u) == 2, "汉字宽度不是 2");

    // 禁则:一行只放 2 个单位时,「。」不能被挤到行首。
    const tsxx_layout_t tight = { .units_per_line = 2, .lines_per_page = 8 };
    const char *closing = "你好。";
    screens = tsxx_text_screens(closing, &tight, offsets, TSXX_MAX_SCREENS);
    CHECK(screens >= 1, "禁则样例没有产出屏");
    if (screens >= 2) {
        const uint32_t start = offsets[1];
        CHECK(strncmp(closing + start, "。", 3) != 0, "行首出现了禁则标点");
    }

    // 屏数上限:超长文本必须把剩余内容并进最后一屏,不能丢字。
    char long_text[512];
    for (size_t i = 0; i + 1 < sizeof(long_text); i += 3) {
        memcpy(long_text + i, "字", 3);
    }
    long_text[sizeof(long_text) - 1] = '\0';
    screens = tsxx_text_screens(long_text, &narrow, offsets, 3);
    CHECK(screens == 3, "受上限约束的屏数应为 3,实际 %d", screens);
    CHECK(offsets[screens] == (uint32_t)strlen(long_text), "截断时丢了尾部的字");
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "main/tsxx_data/tsxx_pack.bin";
    uint32_t size = 0;
    uint8_t *blob = read_file(path, &size);
    if (blob == NULL) {
        return 2;
    }
    tsxx_pack_t pack;
    if (!tsxx_pack_open(&pack, blob, size)) {
        fprintf(stderr, "tsxx_pack_open 失败:%s\n", path);
        free(blob);
        return 2;
    }
    printf("资源包 %s: %u 字节\n", path, (unsigned)size);
    printf("META:\n%s", tsxx_pack_meta(&pack));

    test_pack_structure(&pack);
    test_chapters(&pack);
    test_full_walk(&pack);
    test_jump_and_save(&pack);
    test_pagination();

    free(blob);
    if (g_failures != 0) {
        fprintf(stderr, "tsxx_model: %d 项失败\n", g_failures);
        return 1;
    }
    printf("tsxx_model: 全部通过\n");
    return 0;
}
