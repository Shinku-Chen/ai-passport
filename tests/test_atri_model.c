// tests/test_atri_model.c —— 用真实资源包验证阅读器逻辑(纯宿主机测试)。
//
// 用法: test_atri_model <path/to/atri_pack.bin>
// 覆盖:包结构完整性、剧情图可达性(三个结局与选择分流)、分页规则、跳过场景、
//       存档往返、长句不溢出文本缓冲。
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atri_model.h"
#include "atri_pack.h"

// 与 main/atri_ui.h 的排版参数一致:26 单位 = 13 个全角字,一屏 5 行。
static const atri_layout_t LAYOUT = { 26, 5 };

static uint8_t *load_file(const char *path, uint32_t *size)
{
    FILE *fh = fopen(path, "rb");
    if (!fh) {
        fprintf(stderr, "无法打开资源包: %s\n", path);
        exit(2);
    }
    fseek(fh, 0, SEEK_END);
    const long len = ftell(fh);
    fseek(fh, 0, SEEK_SET);
    assert(len > 0);
    uint8_t *buf = malloc((size_t)len);
    assert(buf);
    assert(fread(buf, 1, (size_t)len, fh) == (size_t)len);
    fclose(fh);
    *size = (uint32_t)len;
    return buf;
}

static void test_pack_structure(const atri_pack_t *pack)
{
    assert(pack->chapter_count > 0);
    assert(pack->scene_count > 0);
    assert(pack->dialogue_count > 0);
    assert(pack->bg_count > 0);
    assert(pack->ovl_count > 0);
    assert(pack->name_count > 0);

    for (uint16_t i = 0; i < pack->chapter_count; ++i) {
        atri_chapter_t ch;
        atri_pack_chapter(pack, i, &ch);
        assert(ch.scene_count > 0);
        assert((uint32_t)ch.first_scene + ch.scene_count <= pack->scene_count);
        assert((uint32_t)ch.first_dlg + ch.dlg_count <= pack->dialogue_count);
        if (ch.next != ATRI_NONE) assert(ch.next < pack->chapter_count);
        if (ch.flags & ATRI_CH_HAS_BRANCH) {
            assert(ch.branch_count > 0);
            assert(ch.branch_bad != ATRI_NONE && ch.branch_bad < pack->chapter_count);
        }
    }

    for (uint16_t i = 0; i < pack->scene_count; ++i) {
        atri_scene_t sc;
        atri_pack_scene(pack, i, &sc);
        assert((uint32_t)sc.first_dlg + sc.dlg_count <= pack->dialogue_count);
        if (sc.bg != ATRI_NONE) {
            atri_bg_t bg;
            assert(atri_pack_bg(pack, sc.bg, &bg));
            assert(bg.jpeg_len > 0);
            assert(bg.w == 240 && bg.h == 320);   // 与画面区尺寸一致(整屏)
            assert(bg.jpeg[0] == 0xFF && bg.jpeg[1] == 0xD8);   // JPEG SOI
        }
        if (sc.ovl != ATRI_NONE) {
            atri_ovl_t ovl;
            assert(atri_pack_ovl(pack, sc.ovl, &ovl));
            assert(ovl.w > 0 && ovl.h > 0 && ovl.w <= 240 && ovl.h <= 320);
            // 叠加是无损 RGB565 + 4bpp 遮罩:颜色必须整张齐全,遮罩要么没有要么够长。
            assert(ovl.color_len >= (uint32_t)ovl.w * ovl.h * 2u);
            if (ovl.mask_len) {
                assert(ovl.mask_len >= (uint32_t)((ovl.w + 1) / 2) * ovl.h);
            }
            // 颜色缓冲必须是 2 字节对齐的(按 u16 读)。
            assert(((uintptr_t)ovl.color % 2u) == 0);
            // 叠加必须落在画面区内(源脚本里最右/最下的位置也不能越界太多)。
            assert(sc.ovl_x >= 0 && sc.ovl_x <= 240);
            assert(sc.ovl_y >= 0 && sc.ovl_y <= 320);
        }
        for (uint8_t c = 0; c < sc.choice_count; ++c) {
            assert(sc.choice_name[c] != ATRI_NONE);
            assert(sc.choice_jump[c] > 0);
        }
    }

    for (uint16_t i = 0; i < pack->dialogue_count; ++i) {
        atri_dialogue_t dlg;
        atri_pack_dialogue(pack, i, &dlg);
        assert(dlg.text_off + dlg.text_len <= pack->text_size);
        assert(dlg.text_len < ATRI_TEXT_BUFFER);   // 单句必须放得进文本缓冲
        if (dlg.name != ATRI_NONE) {
            char name[64];
            assert(atri_pack_name(pack, dlg.name, name, sizeof(name)) > 0);
        }
        if (dlg.flags & ATRI_DLG_END) assert(dlg.arg != ATRI_NONE);
    }

    // 标题画面固定在背景表 0 号,TRUE END 叠加固定在叠加表 0 号。
    atri_bg_t title;
    assert(atri_pack_bg(pack, ATRI_BG_TITLE, &title));
    assert(title.w == 240 && title.h == 320);
    atri_ovl_t true_end;
    assert(atri_pack_ovl(pack, ATRI_OVL_TRUE_END, &true_end));
    assert(true_end.w > 0 && true_end.h > 0);

    // 源脚本编号 -> 章节下标。
    assert(atri_pack_find_chapter(pack, 999) == 0);
    assert(atri_pack_find_chapter(pack, 101) == 1);
    assert(atri_pack_find_chapter(pack, 701) == (int)pack->chapter_count - 1);
    assert(atri_pack_find_chapter(pack, 100) == -1);
}

// 沿剧情推进到底,按 picks 里的选择逐个作答(用完则一律选 0)。
typedef struct {
    int chapters_seen;
    int choices_seen;
    int steps;
    atri_step_t last_step;
    uint16_t end_name;
} walk_result_t;

static walk_result_t walk_story(const atri_pack_t *pack, uint16_t start_chapter,
                                const uint8_t *picks, int pick_count)
{
    walk_result_t result = { 0 };
    atri_player_t player;
    assert(atri_player_start(&player, pack, start_chapter, &LAYOUT));
    uint16_t last_chapter = player.chapter;

    for (int i = 0; i < 200000; ++i) {
        if (player.chapter != last_chapter) {
            ++result.chapters_seen;
            last_chapter = player.chapter;
        }
        if (player.at_choice) {
            const uint8_t pick = result.choices_seen < pick_count ? picks[result.choices_seen] : 0;
            atri_scene_t sc;
            assert(atri_player_scene_view(&player, pack, &sc));
            assert(pick < sc.choice_count);
            assert(atri_player_choose(&player, pack, pick, &LAYOUT));
            ++result.choices_seen;
            continue;
        }
        const atri_step_t step = atri_player_advance(&player, pack, &LAYOUT);
        ++result.steps;
        result.last_step = step;
        if (step == ATRI_STEP_ENDING) {
            result.end_name = player.end_name;
            return result;
        }
        if (step == ATRI_STEP_STUCK) {
            fprintf(stderr, "剧情在章节 %u 场景 %u 卡住\n", (unsigned)player.chapter,
                    (unsigned)player.scene);
            assert(0);
        }
        assert(result.steps < 200000);
    }
    assert(0 && "剧情推进步数超限");
    return result;
}

static void test_story_graph(const atri_pack_t *pack)
{
    // 正确选择(源脚本 branch.choices = [1,1,0])走圆满结局。
    const uint8_t good[3] = { 1, 1, 0 };
    const walk_result_t happy = walk_story(pack, 0, good, 3);
    char name[64] = { 0 };
    atri_pack_name(pack, happy.end_name, name, sizeof(name));
    assert(strcmp(name, "Happy Ending") == 0);
    assert(happy.choices_seen == 3);
    assert(happy.chapters_seen >= 30);   // 主线 31 章 + 序章

    // 全选 0 会在分流处掉进悲剧结局。
    const uint8_t bad[3] = { 0, 0, 0 };
    const walk_result_t sad = walk_story(pack, 0, bad, 3);
    atri_pack_name(pack, sad.end_name, name, sizeof(name));
    assert(strcmp(name, "Bad Ending") == 0);

    // 真正的结局章必须能从标题页直接进入,并且有自己的结局。
    const int te = atri_pack_find_chapter(pack, 701);
    assert(te > 0);
    const walk_result_t true_end = walk_story(pack, (uint16_t)te, NULL, 0);
    atri_pack_name(pack, true_end.end_name, name, sizeof(name));
    assert(strncmp(name, "True Ending", 11) == 0);

    // 三个结局章的标志位必须与章节对应。
    atri_chapter_t ch;
    atri_pack_chapter(pack, (uint16_t)atri_pack_find_chapter(pack, 501), &ch);
    assert(ch.flags & ATRI_CH_HAPPY_END);
    atri_pack_chapter(pack, (uint16_t)atri_pack_find_chapter(pack, 601), &ch);
    assert(ch.flags & ATRI_CH_BAD_END);
    atri_pack_chapter(pack, (uint16_t)atri_pack_find_chapter(pack, 701), &ch);
    assert(ch.flags & ATRI_CH_TRUE_END);
    atri_pack_chapter(pack, (uint16_t)atri_pack_find_chapter(pack, 403), &ch);
    assert(ch.flags & ATRI_CH_HAS_BRANCH);
    assert(ch.branch_count == 3);
    assert(ch.branch_pick == 5);   // [1,1,0] -> 0b01 | 0b01 << 2 | 0 << 4
}

static void test_paging(const atri_pack_t *pack)
{
    // 13 个全角字正好一行(26 单位);第 14 个字开始换行。
    uint32_t offsets[ATRI_MAX_PAGES + 1];
    const char *one_line = "一二三四五六七八九十一二三";
    assert(atri_text_pages(one_line, 26, 5, offsets, ATRI_MAX_PAGES + 1) == 1);
    const char *wrap = "一二三四五六七八九十一二三四";
    int pages = atri_text_pages(wrap, 26, 5, offsets, ATRI_MAX_PAGES + 1);
    assert(pages == 1);   // 还在一屏之内(第 14 字换行,但没满 5 行)

    // 6 行 -> 2 页。
    char long_text[400] = { 0 };
    for (int i = 0; i < 13 * 6; ++i) strcat(long_text, "字");
    pages = atri_text_pages(long_text, 26, 5, offsets, ATRI_MAX_PAGES + 1);
    assert(pages == 2);
    assert(offsets[0] == 0);
    // 断点必须落在 UTF-8 字符边界上。
    for (int i = 1; i <= pages; ++i) {
        assert((offsets[i] % 3) == 0);
        assert(offsets[i] <= strlen(long_text));
    }

    // 换行符也算一行的结束。
    assert(atri_text_pages("一\n二", 26, 5, offsets, ATRI_MAX_PAGES + 1) == 1);
    // 半角按 1 单位算:26 个 ASCII 字符占满一行。
    assert(atri_text_pages("abcdefghijklmnopqrstuvwxyz", 26, 5, offsets,
                           ATRI_MAX_PAGES + 1) == 1);

    // 全脚本:每一句都能分页且不超页数上限。
    for (uint16_t i = 0; i < pack->dialogue_count; ++i) {
        atri_dialogue_t dlg;
        atri_pack_dialogue(pack, i, &dlg);
        char text[ATRI_TEXT_BUFFER];
        const size_t len = atri_pack_text(pack, dlg.text_off, dlg.text_len, text, sizeof(text));
        assert(len == dlg.text_len);   // 没有一句会被截断
        const int p = atri_text_pages(text, LAYOUT.units_per_line, LAYOUT.lines_per_page,
                                      offsets, ATRI_MAX_PAGES + 1);
        assert(p >= 1 && p <= ATRI_MAX_PAGES);
    }
}

static void test_choice_and_skip(const atri_pack_t *pack)
{
    // 第一个选项场景:两个选项分别跳到 当前场景+1 / 当前场景+2。
    atri_player_t player;
    assert(atri_player_start(&player, pack, 0, &LAYOUT));
    for (int i = 0; i < 200000 && !player.at_choice; ++i) {
        const atri_step_t step = atri_player_advance(&player, pack, &LAYOUT);
        assert(step != ATRI_STEP_STUCK && step != ATRI_STEP_ENDING);
    }
    assert(player.at_choice);
    const uint16_t choice_scene = player.scene;
    assert(atri_player_choose(&player, pack, 1, &LAYOUT));
    assert(player.scene == choice_scene + 2);
    assert(player.choice_len == 1 && player.choice_pick[0] == 1);
    // 选择之后必须落在普通场景(有对白)。
    assert(!player.at_choice);

    // 跳过场景:普通场景能跳,末句带跳转/结局的场景不能跳。
    assert(atri_player_skip_scene(&player, pack, &LAYOUT));
    const uint16_t after = player.scene;
    (void)after;

    // 选项场景本身不能跳过。
    atri_player_t at_choice;
    assert(atri_player_start(&at_choice, pack, 0, &LAYOUT));
    for (int i = 0; i < 200000 && !at_choice.at_choice; ++i) {
        (void)atri_player_advance(&at_choice, pack, &LAYOUT);
    }
    assert(at_choice.at_choice);
    assert(!atri_player_skip_scene(&at_choice, pack, &LAYOUT));
}

static void test_skip_chapter(const atri_pack_t *pack)
{
    atri_player_t player;

    // 无选项的一章(b101 = 2 号章):跳过会停在下一章开头。
    assert(atri_player_start(&player, pack, 1, &LAYOUT));
    assert(atri_player_skip_chapter(&player, pack, &LAYOUT) == ATRI_STEP_CHAPTER);
    assert(player.chapter == 2 && player.scene == 0 && player.dialogue == 0);
    assert(!player.at_choice && !player.ended);

    // 含选项的一章(b102 = 3 号章):遇到选项必须停下,不能替玩家做选择。
    assert(atri_player_start(&player, pack, 2, &LAYOUT));
    assert(atri_player_skip_chapter(&player, pack, &LAYOUT) == ATRI_STEP_CHOICE);
    assert(player.chapter == 2 && player.at_choice);
    assert(player.choice_len == 0);

    // 最后一章(b701 = 真正的结局):跳过会直接推到结局。
    const int te = atri_pack_find_chapter(pack, 701);
    assert(te > 0);
    assert(atri_player_start(&player, pack, (uint16_t)te, &LAYOUT));
    assert(atri_player_skip_chapter(&player, pack, &LAYOUT) == ATRI_STEP_ENDING);
    assert(player.ended);

    // 参数为空时不崩,返回 STUCK。
    assert(atri_player_skip_chapter(NULL, pack, &LAYOUT) == ATRI_STEP_STUCK);
}

static void test_save_roundtrip(const atri_pack_t *pack)
{
    atri_player_t player;
    assert(atri_player_start(&player, pack, 3, &LAYOUT));
    assert(atri_player_choose(&player, pack, 1, &LAYOUT) == false);   // 不在选项上,拒绝
    for (int i = 0; i < 200000 && !player.at_choice; ++i) {
        const atri_step_t step = atri_player_advance(&player, pack, &LAYOUT);
        assert(step != ATRI_STEP_STUCK);
        if (step == ATRI_STEP_ENDING) break;
    }
    if (player.at_choice) (void)atri_player_choose(&player, pack, 0, &LAYOUT);

    atri_save_t save;
    memset(&save, 0, sizeof(save));
    save.chapter = player.chapter;
    save.scene = player.scene;
    save.dialogue = player.dialogue;
    save.choice_len = player.choice_len;
    memcpy(save.choice_pick, player.choice_pick, sizeof(save.choice_pick));

    uint8_t blob[64];
    const size_t len = atri_save_encode(&save, blob, sizeof(blob));
    assert(len > 0);
    atri_save_t back;
    assert(atri_save_decode(&back, blob, len));
    assert(back.chapter == save.chapter && back.scene == save.scene);
    assert(back.dialogue == save.dialogue && back.choice_len == save.choice_len);
    assert(memcmp(back.choice_pick, save.choice_pick, sizeof(save.choice_pick)) == 0);

    // 坏数据必须被拒绝,而不是产生野状态。
    assert(!atri_save_decode(&back, blob, 4));
    uint8_t broken[64];
    memcpy(broken, blob, len);
    broken[0] ^= 0xFF;
    assert(!atri_save_decode(&back, broken, len));

    // 恢复到存档点:章节/场景/对白必须一致。
    atri_player_t loaded;
    assert(atri_player_load(&loaded, pack, &save, &LAYOUT));
    assert(loaded.chapter == save.chapter && loaded.scene == save.scene);
    assert(loaded.choice_len == save.choice_len);

    // 越界存档要被夹住而不是崩掉。
    atri_save_t bogus = { .chapter = 0xFFFF, .scene = 0xFFFF, .dialogue = 0xFFFF };
    atri_player_t rejected;
    assert(!atri_player_load(&rejected, pack, &bogus, &LAYOUT));
    bogus.chapter = 0;
    bogus.scene = 0;
    bogus.dialogue = 0xFFFF;
    assert(atri_player_load(&rejected, pack, &bogus, &LAYOUT));
}

static void test_pack_rejects_garbage(void)
{
    atri_pack_t pack;
    uint8_t junk[64];
    memset(junk, 0xAB, sizeof(junk));
    assert(!atri_pack_open(&pack, junk, sizeof(junk)));
    assert(!atri_pack_open(&pack, junk, 4));
    assert(!atri_pack_open(NULL, junk, sizeof(junk)));
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "用法: %s <atri_pack.bin>\n", argv[0]);
        return 2;
    }
    uint32_t size = 0;
    uint8_t *blob = load_file(argv[1], &size);

    atri_pack_t pack;
    assert(atri_pack_open(&pack, blob, size));
    printf("资源包: %u 字节,章节 %u / 场景 %u / 对白 %u / 背景 %u / 叠加 %u / 名字 %u\n",
           (unsigned)size, (unsigned)pack.chapter_count, (unsigned)pack.scene_count,
           (unsigned)pack.dialogue_count, (unsigned)pack.bg_count, (unsigned)pack.ovl_count,
           (unsigned)pack.name_count);

    test_pack_rejects_garbage();
    test_pack_structure(&pack);
    test_story_graph(&pack);
    test_paging(&pack);
    test_choice_and_skip(&pack);
    test_skip_chapter(&pack);
    test_save_roundtrip(&pack);

    free(blob);
    printf("ATRI 剧情 / 分页 / 存档逻辑: PASS\n");
    return 0;
}
