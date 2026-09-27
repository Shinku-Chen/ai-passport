// tests/test_tsxx_model.c —— 用真实资源包驱动阅读器的纯逻辑层。
//
// 覆盖:
//   1. 包结构与各段计数;
//   2. 整张页表逐页校验:分屏数、正文可解码、资源下标合法、选项目标合法;
//   3. 章节点扫描(45 个)与顺序;
//   4. 分支:结局点 / no_next / no_back / 5 个 flag 闸门,以及按指定选择序列
//      走到的女主结局(6 条线一条都不能少);
//   5. 分页排版:强制换行、禁则标点、宽字符计量、超长页分屏;
//   6. 存档编解码往返(含选择历史)与读档后的闸门判定。
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
    // 整张页表逐页过一遍:分屏数、正文可解码、选项目标合法。
    // 推进路线由下面的分支测试覆盖,这里只看"每一页单独拿出来是不是自洽的"。
    tsxx_player_t player;
    char text[TSXX_TEXT_BUFFER];
    uint32_t screens_seen = 0;
    uint32_t choice_stops = 0;
    const uint32_t pages = tsxx_pack_pages(pack);

    for (uint32_t page = 0; page < pages; ++page) {
        CHECK(tsxx_player_jump(&player, pack, page, &LAYOUT_16), "第 %u 页跳不过去",
              (unsigned)page);
        CHECK(player.page == page, "跳页后页号不对");
        CHECK(player.screens <= TSXX_MAX_SCREENS, "第 %u 页分屏数 %u 超出上限",
              (unsigned)page, (unsigned)player.screens);
        CHECK(player.screen < player.screens || player.screens == 0,
              "第 %u 页屏号 %u 超出 %u", (unsigned)page, (unsigned)player.screen,
              (unsigned)player.screens);

        const size_t length = tsxx_player_screen_text(&player, pack, &LAYOUT_16, text,
                                                      sizeof(text));
        CHECK(length < sizeof(text), "第 %u 页正文超出缓冲", (unsigned)page);
        CHECK(player.screens == 0 ? length == 0 : length > 0,
              "第 %u 页第 %u 屏文本长度 %zu 与分屏数 %u 不符",
              (unsigned)page, (unsigned)player.screen, length,
              (unsigned)player.screens);
        screens_seen += player.screens;

        if (player.at_choice) {
            ++choice_stops;
            CHECK(player.choice_count >= 1 && player.choice_count <= 5,
                  "第 %u 页选项数 %u 异常", (unsigned)page, (unsigned)player.choice_count);
            char option[TSXX_TEXT_BUFFER];
            uint32_t target = 0;
            for (uint8_t slot = 0; slot < player.choice_count; ++slot) {
                CHECK(tsxx_pack_choice_option(pack, page, slot, option, sizeof(option),
                                              &target),
                      "第 %u 页第 %u 项读不出来", (unsigned)page, (unsigned)slot);
                CHECK(option[0] != '\0', "第 %u 页第 %u 项文案为空", (unsigned)page,
                      (unsigned)slot);
                CHECK(target < pages, "第 %u 页第 %u 项目标越界", (unsigned)page,
                      (unsigned)slot);
            }
        }
    }
    CHECK(screens_seen > 69000u, "总屏数 %u 偏少", (unsigned)screens_seen);
    // 逐页求和 = 全剧 70,298 屏(含禁则与屏上限)。这里只卡下限,换排版参数不会误报。
    CHECK(choice_stops == 13u, "选择点 %u 个 != 13", (unsigned)choice_stops);
    printf("  页表: %u 页 / %u 屏 / %u 个选择点\n", (unsigned)pages, (unsigned)screens_seen,
           (unsigned)choice_stops);
}

// 一份写死的选择序列:页号是包内 0-based,index 是选项下标。
typedef struct {
    uint32_t page;
    uint8_t index;
} tsxx_plan_t;

// 不在计划里的选项页按第 1 个选项走(与 tests/test_tsxx_branch.py 一致)。
static int plan_lookup(const tsxx_plan_t *plan, size_t count, uint32_t page)
{
    for (size_t i = 0; i < count; ++i) {
        if (plan[i].page == page) {
            return plan[i].index;
        }
    }
    return 0;
}

// 从第 0 页开始推进到结局。返回 true 表示以 TSXX_STEP_END 收住。
// 做过的选择写进 log(页号 0-based、选项号 1-based),最多 capacity 条。
static bool walk_to_ending(const tsxx_pack_t *pack, const tsxx_plan_t *plan, size_t plan_count,
                           tsxx_choice_t *log, uint8_t capacity, uint8_t *log_count,
                           uint32_t *stop_page)
{
    tsxx_player_t player;
    if (!tsxx_player_start(&player, pack, 0, &LAYOUT_16)) {
        return false;
    }
    *log_count = 0;
    for (uint32_t guard = 0; guard < 400000u; ++guard) {
        if (player.at_choice) {
            const int index = plan_lookup(plan, plan_count, player.page);
            if (index < 0 || index >= (int)player.choice_count) {
                return false;
            }
            if (*log_count < capacity) {
                log[*log_count].page = player.page;
                log[*log_count].value = (uint8_t)(index + 1);
            }
            ++*log_count;
            if (!tsxx_player_choose(&player, pack, (uint8_t)index, &LAYOUT_16)) {
                return false;
            }
            continue;
        }
        const tsxx_step_t step = tsxx_player_advance(&player, pack, &LAYOUT_16);
        if (step == TSXX_STEP_STUCK) {
            return false;   // 非选项、非结束却推不动:模型有问题
        }
        if (step == TSXX_STEP_END) {
            *stop_page = player.page;
            return true;
        }
    }
    return false;
}

static void test_branch_tables(const tsxx_pack_t *pack)
{
    char name[TSXX_TEXT_BUFFER];
    uint32_t target = 0;
    uint32_t page = 0;

    // 结局点:末页是"回到主页",第 10070 页是 "END"。
    CHECK(tsxx_pack_end_count(pack) == 15u, "结局点 %u 个 != 15",
          (unsigned)tsxx_pack_end_count(pack));
    CHECK(tsxx_pack_gate_count(pack) == 5u, "闸门 %u 道 != 5",
          (unsigned)tsxx_pack_gate_count(pack));
    CHECK(tsxx_pack_end(pack, 61435u, name, sizeof(name)), "末页不是结局点");
    CHECK(strcmp(name, "回到主页") == 0, "末页结局名是 %s", name);
    CHECK(tsxx_pack_end(pack, 10069u, name, sizeof(name)), "第 10070 页不是结局点");
    CHECK(strcmp(name, "END") == 0, "第 10070 页结局名是 %s", name);
    CHECK(!tsxx_pack_end(pack, 0u, name, sizeof(name)), "第 1 页不该是结局点");
    CHECK(!tsxx_pack_end(pack, tsxx_pack_pages(pack), name, sizeof(name)),
          "越界页不该是结局点");

    // no_next:第 3143 页"推进"跳到第 3386 页(而不是第 3144 页)。
    CHECK(tsxx_pack_next(pack, 3142u, NULL, 0, &target) && target == 3385u,
          "no_next: 第 3143 页 -> %u", (unsigned)target);
    // 没有特殊规则的页返回 false,由调用方顺序推进。
    CHECK(!tsxx_pack_next(pack, 0u, NULL, 0, &target), "第 1 页不该有特殊规则");
    // no_back:第 10072 页"回退"到第 2029 页。
    CHECK(tsxx_pack_prev(pack, 10071u, &page) && page == 2028u, "no_back: %u",
          (unsigned)page);
    CHECK(!tsxx_pack_prev(pack, 0u, &page), "第 1 页不该有回退目标");

    // 闸门 9204(0-based 9203)的两条规则只差选项号,else = 0 表示顺序推进。
    const tsxx_choice_t value2[] = { { 3611u, 2u } };
    const tsxx_choice_t value1[] = { { 3611u, 1u } };
    CHECK(tsxx_pack_next(pack, 9203u, value2, 1, &target) && target == 9204u,
          "闸门 9204 选 2 应到第 9205 页,实际 %u", (unsigned)target);
    CHECK(tsxx_pack_next(pack, 9203u, value1, 1, &target) && target == 9205u,
          "闸门 9204 选 1 应到第 9206 页,实际 %u", (unsigned)target);
    CHECK(!tsxx_pack_next(pack, 9203u, NULL, 0, &target),
          "闸门 9204 没有命中规则时 else=0 应表示顺序推进");

    // 闸门 9341 第一条规则要求 7 个条件全中。
    const tsxx_choice_t amane[] = {
        { 2028u, 2u }, { 3611u, 2u }, { 5247u, 2u }, { 7929u, 2u },
        { 8390u, 2u }, { 9095u, 5u }, { 9204u, 2u },
    };
    CHECK(tsxx_pack_next(pack, 9340u, amane, 7, &target) && target == 9353u,
          "闸门 9341 第一条规则应指向第 9354 页,实际 %u", (unsigned)target);
    CHECK(tsxx_pack_next(pack, 9340u, amane, 6, &target) && target == 9392u,
          "少一个条件就该落到 else(第 9393 页),实际 %u", (unsigned)target);
    CHECK(tsxx_pack_next(pack, 9340u, NULL, 0, &target) && target == 9392u,
          "空历史应落到 else,实际 %u", (unsigned)target);
}

// 6 条女主线各一份指定选择序列(页号 0-based、选项号 1-based,与
// tests/test_tsxx_branch.py 的 HEROINE_ROUTES 一致)。
static const tsxx_choice_t ROUTE_FUMIKA[] = {
    { 2028u, 2u }, { 2962u, 3u }, { 3611u, 1u }, { 5247u, 2u }, { 6001u, 1u },
    { 7929u, 1u }, { 8390u, 1u }, { 9095u, 1u }, { 9205u, 1u }, { 9440u, 1u },
};
static const tsxx_choice_t ROUTE_AMANE[] = {
    { 2028u, 2u }, { 2962u, 1u }, { 3611u, 2u }, { 5247u, 2u }, { 6001u, 1u },
    { 7929u, 2u }, { 8390u, 2u }, { 9095u, 5u }, { 9204u, 2u }, { 9440u, 1u },
};
static const tsxx_choice_t ROUTE_ORIE[] = {
    { 2028u, 2u }, { 2962u, 1u }, { 3611u, 1u }, { 5247u, 2u }, { 6001u, 1u },
    { 7929u, 1u }, { 8390u, 2u }, { 9095u, 4u }, { 9205u, 1u }, { 9440u, 3u },
    { 9883u, 1u },
};

// 按一份选择序列走到底,断言终点页与结局名。
static void check_route(const tsxx_pack_t *pack, const char *plan_name,
                        const tsxx_choice_t *expected, uint8_t count, uint32_t expected_page,
                        const char *expected_name)
{
    tsxx_plan_t plan[TSXX_MAX_HISTORY];
    CHECK(count <= TSXX_MAX_HISTORY, "路线 %s 的选择太多", plan_name);
    for (uint8_t i = 0; i < count; ++i) {
        plan[i].page = expected[i].page;
        plan[i].index = (uint8_t)(expected[i].value - 1u);
    }
    tsxx_choice_t log[TSXX_MAX_HISTORY];
    uint8_t log_count = 0;
    uint32_t stop = 0;
    CHECK(walk_to_ending(pack, plan, count, log, TSXX_MAX_HISTORY, &log_count, &stop),
          "路线 %s 没有走到结局", plan_name);
    CHECK(log_count == count, "路线 %s 做过的选择 %u 个 != %u", plan_name, (unsigned)log_count,
          (unsigned)count);
    for (uint8_t i = 0; i < count && i < log_count; ++i) {
        CHECK(log[i].page == expected[i].page && log[i].value == expected[i].value,
              "路线 %s 第 %u 个选择 (%u, %u) != (%u, %u)", plan_name, (unsigned)i,
              (unsigned)log[i].page, (unsigned)log[i].value, (unsigned)expected[i].page,
              (unsigned)expected[i].value);
    }
    CHECK(stop == expected_page, "路线 %s 停在第 %u 页,期望第 %u 页", plan_name,
          (unsigned)stop, (unsigned)expected_page);
    char name[TSXX_TEXT_BUFFER];
    CHECK(tsxx_pack_end(pack, stop, name, sizeof(name)), "路线 %s 的终点不是结局点", plan_name);
    CHECK(strcmp(name, expected_name) == 0, "路线 %s 的结局是 %s,期望 %s", plan_name, name,
          expected_name);
    printf("  路线 %-10s → 第 %u 页「%s」\n", plan_name, (unsigned)stop, name);
}

static void test_heroine_routes(const tsxx_pack_t *pack)
{
    // 全选第 1 项:第 2028 页选完走进 BAD END,不再走完全表。
    tsxx_choice_t log[TSXX_MAX_HISTORY];
    uint8_t log_count = 0;
    uint32_t stop = 0;
    CHECK(walk_to_ending(pack, NULL, 0, log, TSXX_MAX_HISTORY, &log_count, &stop),
          "默认路线没有走到结局");
    CHECK(log_count == 1u && log[0].page == 2028u && log[0].value == 1u,
          "默认路线做过的选择不对(第 %u 个)", (unsigned)log_count);
    CHECK(stop == 10558u, "默认路线停在第 %u 页", (unsigned)stop);

    // 6 条女主线不能有一条走不进去 —— 这正是移植版原先完全缺失的部分。
    check_route(pack, "Fumika end", ROUTE_FUMIKA,
                (uint8_t)(sizeof(ROUTE_FUMIKA) / sizeof(ROUTE_FUMIKA[0])), 60803u,
                "Fumika end");
    check_route(pack, "Amane end", ROUTE_AMANE,
                (uint8_t)(sizeof(ROUTE_AMANE) / sizeof(ROUTE_AMANE[0])), 29605u,
                "Amane end");
    check_route(pack, "Orie end", ROUTE_ORIE,
                (uint8_t)(sizeof(ROUTE_ORIE) / sizeof(ROUTE_ORIE[0])), 55949u,
                "Orie end");
}

static void test_ending_stops_the_walk(const tsxx_pack_t *pack)
{
    tsxx_player_t player;
    // 结局点读完之后 advance 必须给出 END,并且不再往下走。
    CHECK(tsxx_player_start(&player, pack, 10069u, &LAYOUT_16), "无法从结局页开始");
    for (uint32_t i = 0; i < 2000u && player.screens > 1u && player.screen + 1u < player.screens;
         ++i) {
        CHECK(tsxx_player_advance(&player, pack, &LAYOUT_16) == TSXX_STEP_SCREEN,
              "结局页翻屏异常");
    }
    CHECK(tsxx_player_advance(&player, pack, &LAYOUT_16) == TSXX_STEP_END, "结局页没有给出 END");
    CHECK(player.ended, "结局后 ended 没置位");
    CHECK(tsxx_player_advance(&player, pack, &LAYOUT_16) == TSXX_STEP_STUCK,
          "结局之后还能继续推进");

    // 选项页上"推进"必须被禁用。
    CHECK(tsxx_player_start(&player, pack, 2028u, &LAYOUT_16), "无法从选项页开始");
    CHECK(player.at_choice, "第 2029 页应是选项页");
    CHECK(tsxx_player_advance(&player, pack, &LAYOUT_16) == TSXX_STEP_STUCK,
          "选项页上不该能推进");
}

static void test_jump_and_save(const tsxx_pack_t *pack)
{
    tsxx_player_t player;
    CHECK(tsxx_player_jump(&player, pack, 61435u, &LAYOUT_16), "跳到末页失败");
    CHECK(player.page == 61435u && !player.ended, "跳转后状态不对");

    // 跳到一个已知的选项点(源数据第 2029 页 → 0-based 2028)。
    CHECK(tsxx_player_jump(&player, pack, 2028u, &LAYOUT_16), "跳到选项页失败");
    CHECK(player.at_choice, "第 2028 页应是选项点");
    CHECK(player.choice_count == 2u, "第 2028 页选项数 %u != 2",
          (unsigned)player.choice_count);

    // 存档往返:位置 + 选择历史。
    tsxx_save_t save;
    memset(&save, 0, sizeof(save));
    save.page = 1234u;
    save.screen = 2u;
    save.history_count = 2u;
    save.history[0].page = 2028u;
    save.history[0].value = 2u;
    save.history[1].page = 3611u;
    save.history[1].value = 1u;
    uint8_t encoded[9u + TSXX_MAX_HISTORY * 5u];
    const size_t length = tsxx_save_encode(&save, encoded, sizeof(encoded));
    CHECK(length == 9u + 2u * 5u, "存档长度 %zu 不对", length);
    tsxx_save_t back;
    CHECK(tsxx_save_decode(&back, encoded, length), "存档解码失败");
    CHECK(back.page == 1234u && back.screen == 2u, "存档往返的页/屏不一致");
    CHECK(back.history_count == 2u, "存档往返的历史条数 %u", (unsigned)back.history_count);
    CHECK(back.history[0].page == 2028u && back.history[0].value == 2u &&
          back.history[1].page == 3611u && back.history[1].value == 1u,
          "存档往返的历史内容不一致");

    // 旧固件写的 8 字节存档(只有 page/screen)仍能解码,只是没有历史。
    tsxx_save_t legacy;
    CHECK(tsxx_save_decode(&legacy, encoded, 8u), "旧格式存档没被接受");
    CHECK(legacy.page == 1234u && legacy.screen == 2u && legacy.history_count == 0u,
          "旧格式存档解出来不对");
    CHECK(!tsxx_save_decode(&legacy, encoded, 4u), "过短的存档没被拒绝");
    CHECK(!tsxx_save_decode(&legacy, encoded, 9u + 5u),
          "声明 2 项历史但长度只够 1 项,应拒绝");
    CHECK(tsxx_save_encode(&save, encoded, 8u) == 0u, "缓冲太小时应失败");

    // 读档必须把历史一起恢复:否则闸门会拿空历史去判定,把玩家送到别的分支。
    tsxx_save_t at_gate;
    memset(&at_gate, 0, sizeof(at_gate));
    at_gate.page = 9340u;
    at_gate.history_count = 7u;
    const tsxx_choice_t amane[] = {
        { 2028u, 2u }, { 3611u, 2u }, { 5247u, 2u }, { 7929u, 2u },
        { 8390u, 2u }, { 9095u, 5u }, { 9204u, 2u },
    };
    for (uint8_t i = 0; i < 7u; ++i) {
        at_gate.history[i] = amane[i];
    }
    tsxx_player_t loaded;
    CHECK(tsxx_player_load(&loaded, pack, &at_gate, &LAYOUT_16), "读档失败");
    CHECK(loaded.page == 9340u, "读档后页号 %u", (unsigned)loaded.page);
    CHECK(loaded.history_count == 7u, "读档后历史 %u 项", (unsigned)loaded.history_count);
    uint32_t next = 0;
    CHECK(tsxx_pack_next(pack, loaded.page, loaded.history, loaded.history_count, &next) &&
          next == 9353u,
          "读档恢复的历史没有参与闸门判定(得到 %u)", (unsigned)next);

    // 回退:有 no_back 走它,没有就退回上一页。
    tsxx_player_t backwards;
    CHECK(tsxx_player_jump(&backwards, pack, 10071u, &LAYOUT_16), "跳到 no_back 页失败");
    CHECK(tsxx_player_back(&backwards, pack, &LAYOUT_16), "回退失败");
    CHECK(backwards.page == 2028u, "回退到第 %u 页,期望第 2029 页",
          (unsigned)(backwards.page + 1u));
    CHECK(backwards.history_count == 0u, "回退不该改动历史");
    CHECK(tsxx_player_jump(&backwards, pack, 5000u, &LAYOUT_16), "跳页失败");
    CHECK(tsxx_player_back(&backwards, pack, &LAYOUT_16) && backwards.page == 4999u,
          "没有 no_back 的页应退回上一页");
    CHECK(tsxx_player_jump(&backwards, pack, 0u, &LAYOUT_16), "跳到首页失败");
    CHECK(!tsxx_player_back(&backwards, pack, &LAYOUT_16), "首页不该能回退");

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
    test_branch_tables(&pack);
    test_heroine_routes(&pack);
    test_ending_stops_the_walk(&pack);
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
