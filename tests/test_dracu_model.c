// tests/test_dracu_model.c —— 用真实剧本包跑数据层的宿主测试。
//
// 只依赖 dracu_scn.c / dracu_model.c / dracu_inflate.c(标准 C),不需要 ESP-IDF。
// 包不在仓库时打印 SKIP 并以 0 退出,保证没带包的 checkout 也能过门禁。
#include "dracu_model.h"

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

static const dracu_layout_t kLayout = { .units_per_line = 26, .lines_per_page = 5 };

// 一直选第一个选项 / 一路推进,直到结局或超过步数上限。
static dracu_step_t walk(dracu_player_t *player, dracu_scn_t *scn, uint32_t *steps_out)
{
    uint32_t steps = 0;
    for (; steps < 400000; steps++) {
        dracu_step_t step = dracu_player_advance(player, scn, &kLayout);
        if (step == DRACU_STEP_CHOICE) {
            CHECK(dracu_player_choose(player, scn, 0, &kLayout), "选项选择失败(page %u)",
                  (unsigned)player->page);
        } else if (step == DRACU_STEP_ENDING || step == DRACU_STEP_STUCK) {
            *steps_out = steps;
            return step;
        }
    }
    *steps_out = steps;
    return DRACU_STEP_STUCK;
}

int main(void)
{
    uint32_t scn_size = 0;
    uint8_t *scn_data = read_file("main/dracu_data/dracu_scn.bin", &scn_size);
    if (scn_data == NULL) {
        printf("SKIP: 缺少 main/dracu_data/dracu_scn.bin(先用 tools/dracu_scn_pack.py 生成)\n");
        return 0;
    }

    dracu_scn_t scn;
    CHECK(dracu_scn_open(&scn, scn_data, scn_size), "剧本包打不开");
    static uint8_t scratch[DRACU_BLOCK_BUFFER];
    dracu_scn_attach(&scn, scratch, sizeof(scratch));
    printf("剧本包: %u 页 / %u 块(页表 %u)/ 字符 %u / 字符串 %u / 选项 %u / 章节 %u\n",
           (unsigned)scn.page_count, (unsigned)scn.block_count, (unsigned)scn.page_block_count,
           (unsigned)scn.char_count, (unsigned)scn.string_count, (unsigned)scn.choice_count,
           (unsigned)scn.chapter_count);

    // 1) 第一页的正文必须能解出来
    dracu_page_t first;
    CHECK(dracu_scn_page(&scn, 1, &first), "第 1 页读不出来");
    char sample[DRACU_TEXT_BUFFER];
    dracu_scn_text(&scn, first.text_off, first.text_len, sample, sizeof(sample));
    CHECK(strcmp(sample, "未知的风景，在车窗外流动着。") == 0, "第 1 页正文不符: %s", sample);
    CHECK(first.bg != DRACU_SCN_NONE8, "第 1 页应当有背景");

    // 2) 从第 1 页读到结局(一直选第一项)
    dracu_player_t player;
    dracu_player_reset(&player);
    CHECK(dracu_player_start(&player, &scn, 1, &kLayout), "从第 1 页开始失败");
    uint32_t steps = 0;
    dracu_step_t step = walk(&player, &scn, &steps);
    CHECK(step == DRACU_STEP_ENDING, "全选第一项没能走到结局(step %d, %u 步, 页 %u: %s)",
          (int)step, (unsigned)steps, (unsigned)player.page, dracu_get_error());
    char ending[DRACU_NAME_MAX] = { 0 };
    if (player.end_name != DRACU_SCN_NONE16) {
        dracu_scn_string(&scn, player.end_name, ending, sizeof(ending));
    }
    printf("全选第一项: %u 步 -> 结局 %s(页 %u)\n", (unsigned)steps, ending,
           (unsigned)player.page);
    CHECK(ending[0] != '\0', "结局名应当能解出来");

    // 3) 章节表:页号升序,且能按页号找到所属章节
    uint32_t previous_page = 0;
    for (uint32_t index = 0; index < dracu_scn_chapter_count(&scn); index++) {
        dracu_chapter_t chapter;
        CHECK(dracu_scn_chapter(&scn, index, &chapter), "章节 %u 读不出来", (unsigned)index);
        CHECK(chapter.page >= previous_page, "章节表必须按页号升序");
        previous_page = chapter.page;
    }
    uint32_t chapter = dracu_chapter_of_page(&scn, 1);
    CHECK(chapter == 0, "第 1 页应当落在第一个章节里(实际 %u)", (unsigned)chapter);
    chapter = dracu_chapter_of_page(&scn, scn.page_count);
    CHECK(chapter != DRACU_CHAPTER_NONE, "最后一页应当能找到所属章节");

    // 4) 跳过章节:应当跳到下一个章节的起始页
    dracu_player_reset(&player);
    CHECK(dracu_player_start(&player, &scn, 1, &kLayout), "重新开始失败");
    dracu_chapter_t second;
    CHECK(dracu_scn_chapter(&scn, 1, &second), "第 2 个章节读不出来");
    step = dracu_player_skip_chapter(&player, &scn, &kLayout);
    CHECK(step == DRACU_STEP_PAGE || step == DRACU_STEP_CHOICE, "跳过章节返回了 %d", (int)step);
    CHECK(player.page == second.page, "跳过章节落到了 %u,期望 %u", (unsigned)player.page,
          (unsigned)second.page);

    // 5) 存档往返
    dracu_save_t save;
    dracu_save_from_player(&player, &save);
    uint8_t encoded[256];
    size_t encoded_len = dracu_save_encode(&save, encoded, sizeof(encoded));
    CHECK(encoded_len > 0, "存档编码失败");
    dracu_save_t restored;
    CHECK(dracu_save_decode(&restored, encoded, encoded_len), "存档解码失败");
    CHECK(restored.page == save.page && restored.chapter == save.chapter, "存档往返不一致");
    CHECK(memcmp(restored.choice, save.choice, sizeof(save.choice)) == 0, "选择历史往返不一致");
    // 读档回到同一页:正文应当一致
    dracu_player_t loaded;
    dracu_player_reset(&loaded);
    CHECK(dracu_player_resume(&loaded, &scn, restored.page, restored.choice, &kLayout),
          "读档失败");
    char before[DRACU_TEXT_BUFFER];
    char after[DRACU_TEXT_BUFFER];
    dracu_player_text(&player, before, sizeof(before));
    dracu_player_text(&loaded, after, sizeof(after));
    CHECK(strcmp(before, after) == 0, "读档后的正文与存档前不一致");

    // 6) 分页:超长句子必须切成多屏,且每屏都能取出来
    dracu_player_reset(&player);
    CHECK(dracu_player_start(&player, &scn, 1, &kLayout), "分页用例开始失败");
    uint32_t long_page = 0;
    for (uint32_t number = 1; number <= scn.page_count; number++) {
        dracu_page_t record;
        if (!dracu_scn_page(&scn, number, &record)) {
            continue;
        }
        if (record.text_len > 90) {
            long_page = number;
            break;
        }
    }
    CHECK(long_page != 0, "没有找到超长句子");
    if (long_page != 0) {
        dracu_player_reset(&player);
        CHECK(dracu_player_start(&player, &scn, long_page, &kLayout), "长句页开始失败");
        CHECK(player.page_count >= 2, "长句应当切成至少两屏(page %u 只切了 %u)",
              (unsigned)long_page, (unsigned)player.page_count);
        for (uint16_t index = 0; index < player.page_count; index++) {
            char page_text[DRACU_TEXT_BUFFER];
            size_t length = dracu_player_page_text(&player, page_text, sizeof(page_text));
            CHECK(length > 0, "第 %u 屏是空的", (unsigned)index);
            if (index + 1 < player.page_count) {
                CHECK(dracu_player_advance(&player, &scn, &kLayout) == DRACU_STEP_TEXT ||
                          player.page != long_page,
                      "翻屏失败");
            }
        }
    }

    free(scn_data);
    if (failures == 0) {
        printf("PASS: 数据层宿主测试全部通过\n");
        return 0;
    }
    printf("FAILED: %d 项\n", failures);
    return 1;
}
