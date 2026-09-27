// tests/test_limelight_model.c —— 阅读器纯逻辑的守卫测试。
//
// 两层:
//   1. 排版/编码/存档(总是跑):禁则、换行、字符宽度、UTF-8 边界与非法字节、存档往返;
//   2. 真实剧本包(给了路径才跑):推进/翻页/回退/章节/选项跳转/结局。
//      真实剧本包要用 --stored 打(宿主没有 inflate)。
//
// 用法: test_limelight_model [script_pack.bin]

#include "limelight_model.h"
#include "limelight_script.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(cond)                                                            \
    do {                                                                       \
        checks++;                                                              \
        if (!(cond)) {                                                         \
            failures++;                                                        \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
        }                                                                      \
    } while (0)

static const lime_layout_t LAYOUT = { 26, 5 };   // 13 个全角字 x 5 行

// ---------------------------------------------------------------------------
// 排版与编码
// ---------------------------------------------------------------------------

static void test_char_units(void)
{
    CHECK(lime_char_units('A') == 1);
    CHECK(lime_char_units(0x4E2D) == 2);        // 中
    CHECK(lime_char_units(0x3002) == 2);        // 。
    CHECK(lime_char_units(0xFF21) == 2);        // 全角 A
    CHECK(lime_char_units(0xFF71) == 1);        // 半角片假名 ア
    CHECK(lime_char_units(0x30A2) == 2);        // 全角片假名 ア
    CHECK(lime_char_units(0x00B7) == 2);        // 间隔号 ·
    CHECK(lime_char_units(0x2014) == 2);        // 破折号 —
    CHECK(lime_char_units(0x2B695) == 2);       // CJK 扩展 B(剧本里真有)
}

static void test_pagination(void)
{
    uint32_t offsets[LIME_MAX_PAGES + 1];
    char line[128];

    // 13 个全角字正好一行
    int pages = lime_text_pages("一二三四五六七八九十百千万", 26, 5, offsets, 17);
    CHECK(pages == 1 && offsets[1] == (uint32_t)strlen("一二三四五六七八九十百千万"));

    // 26 个全角字 = 2 行,仍在一页内
    pages = lime_text_pages("一二三四五六七八九十百千万一二三四五六七八九十百千万", 26, 5,
                            offsets, 17);
    CHECK(pages == 1);

    // 6 行 -> 2 页(第 2 页 1 行)
    memset(line, 0, sizeof(line));
    for (int i = 0; i < 6; i++) strcat(line, "一二三四五六七八九十百千万");
    pages = lime_text_pages(line, 26, 5, offsets, 17);
    CHECK(pages == 2);
    CHECK(offsets[1] == 13 * 3 * 5);            // 5 行 x 13 字 x 3 字节
    CHECK(offsets[2] == strlen(line));

    // 换行符强制断行:3 个短行 + 换行
    pages = lime_text_pages("甲\n乙\n丙", 26, 5, offsets, 17);
    CHECK(pages == 1);

    // 行尾禁则:第 14 个字是句号时,句号挂在本行行尾而不是换到下一行行首
    uint32_t lines[8];
    const char *kinsoku = "一二三四五六七八九十百千万。";   // 13 字 + 句号 = 14 字
    int count = lime_text_lines(kinsoku, 26, lines, 8);
    CHECK(count == 1);                                   // 不允许把"。"甩到第二行
    CHECK(lines[1] == strlen(kinsoku));

    // 非禁则字符照常换行
    const char *normal = "一二三四五六七八九十百千万亿";     // 14 字
    count = lime_text_lines(normal, 26, lines, 8);
    CHECK(count == 2);
    CHECK(lines[1] == 13 * 3);

    // 空文本:1 页,偏移表首尾都是 0
    pages = lime_text_pages("", 26, 5, offsets, 17);
    CHECK(pages == 1 && offsets[0] == 0 && offsets[1] == 0);

    // 半角字符按 1 单位:26 个 ASCII 正好一行
    pages = lime_text_pages("abcdefghijklmnopqrstuvwxyz", 26, 5, offsets, 17);
    CHECK(pages == 1 && offsets[1] == 26);
}

static void test_utf8(void)
{
    const char *text = "a中\U0002B695";       // 1 + 3 + 4 字节
    const size_t len = strlen(text);
    size_t pos = 0;
    CHECK(lime_utf8_decode(text, len, pos, &pos) == 'a' && pos == 1);
    CHECK(lime_utf8_decode(text, len, pos, &pos) == 0x4E2D && pos == 4);
    CHECK(lime_utf8_decode(text, len, pos, &pos) == 0x2B695 && pos == len);

    // 打字机边界:不会切开多字节字符
    size_t at = 0;
    size_t steps = 0;
    while (at < len && steps < 16) {
        at = lime_utf8_next_boundary(text, len, at);
        steps++;
    }
    CHECK(at == len && steps == 3);

    // 非法字节:返回 U+FFFD 并前进 1 字节(不死循环)
    const char bad[] = { (char)0x80, (char)0xE4, (char)0xB8, (char)0xAD, '\0' };
    pos = 0;
    CHECK(lime_utf8_decode(bad, 4, pos, &pos) == 0xFFFD && pos == 1);
    CHECK(lime_utf8_decode(bad, 4, pos, &pos) == 0x4E2D && pos == 4);
    pos = lime_utf8_next_boundary(bad, 4, 0);
    CHECK(pos == 1);
}

static void test_chapter_number(void)
{
    CHECK(lime_chapter_number("[CHAPTER7-1]", 13) == 7);
    CHECK(lime_chapter_number("CHAPTER0-1", 11) == 0);
    CHECK(lime_chapter_number("CHAPTER12-3", 12) == 12);
    CHECK(lime_chapter_number("序章", 6) == 0);
    CHECK(lime_chapter_number("", 0) == 0);
    CHECK(lime_chapter_number(NULL, 4) == 0);
    CHECK(lime_chapter_number("[CHAPTER999]", 13) == 999);

    // 章节显示为 X-X:章与节都要取到。
    int major = -1, minor = -1;
    CHECK(lime_chapter_pair("CHAPTER7-1", 11, &major, &minor) && major == 7 && minor == 1);
    CHECK(lime_chapter_pair("CHAPTER0-2", 11, &major, &minor) && major == 0 && minor == 2);
    CHECK(lime_chapter_pair("CHAPTER10-3", 12, &major, &minor) && major == 10 && minor == 3);
    CHECK(lime_chapter_pair("[CHAPTER12-4]", 14, &major, &minor) && major == 12 && minor == 4);
    // 只有章号时,节按 0 算。
    CHECK(lime_chapter_pair("[CHAPTER999]", 13, &major, &minor) && major == 999 && minor == 0);
    // 没有章号就不是章节标记。
    CHECK(!lime_chapter_pair("序章", 6, &major, &minor));
    CHECK(!lime_chapter_pair("", 0, &major, &minor));
    CHECK(!lime_chapter_pair(NULL, 4, &major, &minor));
}

static void test_save(void)
{
    lime_save_t save = { .id = 0x00123456u, .page = 7, .choice_len = 3,
                         .choice_pick = { 1, 0, 2, 0, 0, 0, 0, 0 } };
    uint8_t buf[64];
    size_t size = lime_save_encode(&save, buf, sizeof(buf));
    CHECK(size == 10 + LIME_CHOICE_HISTORY);

    lime_save_t back;
    CHECK(lime_save_decode(&back, buf, size));
    CHECK(back.id == save.id && back.page == save.page && back.choice_len == 3);
    CHECK(memcmp(back.choice_pick, save.choice_pick, LIME_CHOICE_HISTORY) == 0);

    // 缓冲不足 / 数据被改坏 / choice_len 越界都要拒绝
    CHECK(lime_save_encode(&save, buf, 4) == 0);
    uint8_t broken[32];
    memcpy(broken, buf, size);
    broken[0] = 'X';
    CHECK(!lime_save_decode(&back, broken, size));
    memcpy(broken, buf, size);
    broken[1] = 9;                                   // 版本不对
    CHECK(!lime_save_decode(&back, broken, size));
    memcpy(broken, buf, size);
    broken[8] = 200;                                 // choice_len 越界
    CHECK(!lime_save_decode(&back, broken, size));
    memcpy(broken, buf, size);
    memset(broken + 2, 0, 4);                        // id = 0
    CHECK(!lime_save_decode(&back, broken, size));
    CHECK(!lime_save_decode(&back, buf, 8));
}

// ---------------------------------------------------------------------------
// 真实剧本包
// ---------------------------------------------------------------------------

static uint8_t *read_file(const char *path, uint32_t *size)
{
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (length <= 0) {
        fclose(file);
        return NULL;
    }
    uint8_t *buffer = (uint8_t *)malloc((size_t)length);
    if (!buffer) {
        fclose(file);
        return NULL;
    }
    size_t got = fread(buffer, 1, (size_t)length, file);
    fclose(file);
    if (got != (size_t)length) {
        free(buffer);
        return NULL;
    }
    *size = (uint32_t)length;
    return buffer;
}

static void test_real_script(const char *path)
{
    uint32_t size = 0;
    uint8_t *blob = read_file(path, &size);
    if (!blob) {
        printf("skip real script pack (%s 读不到)\n", path);
        return;
    }
    static lime_script_t script;
    static uint8_t cache[64 * 1024];
    if (!lime_script_open(&script, blob, size)) {
        CHECK(!"lime_script_open");
        free(blob);
        return;
    }
    lime_script_set_cache(&script, cache, sizeof(cache));

    // 宿主没有 inflate:压缩包取不出对白,直接跳过而不是报一堆假失败。
    lime_dialogue_t probe;
    if (!lime_script_dialogue(&script, 1, &probe)) {
        printf("skip real script pack: %s 取不出对白(需要 --stored 打的包)\n", path);
        free(blob);
        return;
    }

    lime_player_t player;
    CHECK(lime_player_start(&player, &script, 1, &LAYOUT));
    CHECK(player.id == 1 && player.page == 0);
    CHECK(player.chapter == 0);
    CHECK(player.bg != LIME_NAME_NONE);
    CHECK(player.cg == LIME_NAME_NONE);          // 第 1 条是章节标签,没有 CG

    // 连推 500 步:id 单调递增、页码始终落在页数内、每步都有背景或正文
    uint32_t previous = player.id;
    int advanced = 0;
    for (int i = 0; i < 500; i++) {
        lime_step_t step = lime_player_advance(&player, &script, &LAYOUT);
        if (step == LIME_STEP_CHOICE) break;
        CHECK(step != LIME_STEP_STUCK);
        CHECK(player.page < player.page_count);
        CHECK(player.id >= previous);
        if (player.id != previous) {
            advanced++;
            previous = player.id;
        }
    }
    CHECK(advanced > 100);
    CHECK(player.chapter >= 0);

    // 第 3613 条是选项:选项表里有 2 项,选第 2 项跳到 c2t = 3672
    CHECK(lime_player_jump(&player, &script, 3613, &LAYOUT));
    CHECK(player.at_choice && player.choice_count == 2);
    // 停在选项上时 advance 不应前进
    CHECK(lime_player_advance(&player, &script, &LAYOUT) == LIME_STEP_CHOICE);
    CHECK(player.id == 3613);
    uint8_t before_len = player.choice_len;
    CHECK(lime_player_choose(&player, &script, 1, &LAYOUT));
    CHECK(player.id == 3672);
    CHECK(player.choice_len == before_len + 1);
    CHECK(!player.at_choice);
    CHECK(!lime_player_choose(&player, &script, 0, &LAYOUT));   // 已经不在选项上

    // 回退:停在上一句的最后一页
    uint32_t current = player.id;
    CHECK(lime_player_back(&player, &script, &LAYOUT));
    CHECK(player.id == current - 1);
    CHECK(player.page == player.page_count - 1);

    // 超长句必须能翻到最后一页(页数上限不能截断文本)
    uint32_t longest = 0;
    size_t longest_len = 0;
    char text[LIME_TEXT_BUFFER];
    for (uint32_t id = 1; id <= script.entries; id += 97) {
        if (!lime_player_jump(&player, &script, id, &LAYOUT)) continue;
        size_t length = lime_player_text(&player, &script, text, sizeof(text));
        if (length > longest_len) {
            longest_len = length;
            longest = id;
        }
    }
    CHECK(longest_len > 0);
    CHECK(lime_player_jump(&player, &script, longest, &LAYOUT));
    lime_player_text(&player, &script, text, sizeof(text));
    int pages = lime_text_pages(text, LAYOUT.units_per_line, LAYOUT.lines_per_page, NULL, 0);
    CHECK(player.page_count == (pages > LIME_MAX_PAGES ? LIME_MAX_PAGES : pages));
    size_t total = 0;
    for (int i = 0; i < player.page_count; i++) {
        player.page = (uint16_t)i;
        total += lime_player_page_text(&player, &script, &LAYOUT, text, sizeof(text));
    }
    CHECK(total == longest_len);                 // 每一页都拼得上,没有丢字

    // 最后一条:advance 到达结局
    CHECK(lime_player_jump(&player, &script, script.entries, &LAYOUT));
    CHECK(player.ended == 1);
    CHECK(lime_player_advance(&player, &script, &LAYOUT) == LIME_STEP_ENDING);

    // 越界 id 一律夹回合法范围
    CHECK(lime_player_start(&player, &script, 0, &LAYOUT));
    CHECK(player.id == 1);
    CHECK(lime_player_start(&player, &script, 999999, &LAYOUT));
    CHECK(player.id == script.entries);
    CHECK(!lime_player_jump(&player, &script, 0, &LAYOUT));

    // 存档往返(带真实进度)
    CHECK(lime_player_start(&player, &script, 3613, &LAYOUT));
    CHECK(lime_player_choose(&player, &script, 0, &LAYOUT));
    lime_save_t save;
    lime_save_from_player(&player, &save);
    uint8_t buffer[32];
    size_t encoded = lime_save_encode(&save, buffer, sizeof(buffer));
    lime_save_t restored;
    CHECK(lime_save_decode(&restored, buffer, encoded));
    CHECK(restored.id == player.id && restored.choice_len == player.choice_len);
    CHECK(lime_player_jump(&player, &script, restored.id, &LAYOUT));
    CHECK(player.id == save.id);

    free(blob);
}

int main(int argc, char **argv)
{
    test_char_units();
    test_chapter_number();
    test_pagination();
    test_utf8();
    test_save();
    if (argc > 1) {
        test_real_script(argv[1]);
    } else {
        printf("note: 未给真实剧本包路径,只跑了排版/编码/存档用例\n");
    }
    printf("limelight model: checks=%d failures=%d\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
