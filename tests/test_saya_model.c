// tests/test_saya_model.c —— 用真实资源包验证阅读器逻辑(纯宿主机测试)。
//
// 用法: test_saya_model <path/to/saya_pack.bin>
// 覆盖:包结构完整性、剧情图可达性(3 个结局)、分页规则、跳过场景、存档往返。
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "saya_model.h"
#include "saya_pack.h"

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

// 沿剧情一路推进,返回结束时玩家所处的状态;途中记录经过的章节数。
static saya_player_t walk_to_choice(const saya_pack_t *pack, const saya_layout_t *layout,
                                    int max_steps)
{
    saya_player_t player;
    assert(saya_player_start(&player, pack, 0, layout));
    for (int i = 0; i < max_steps; ++i) {
        const saya_step_t step = saya_player_advance(&player, pack, layout);
        if (step == SAYA_STEP_CHOICE || step == SAYA_STEP_ENDING) break;
        assert(step != SAYA_STEP_STUCK);
    }
    return player;
}

static void test_pack_structure(const saya_pack_t *pack)
{
    assert(pack->chapter_count > 0);
    assert(pack->scene_count > 0);
    assert(pack->dialogue_count > 0);
    assert(pack->bg_count > 0);
    assert(pack->fg_count > 0);
    assert(pack->name_count > 0);

    // 章节记录必须自洽:场景/对白区间落在包内。
    for (uint16_t i = 0; i < pack->chapter_count; ++i) {
        saya_chapter_t ch;
        saya_pack_chapter(pack, i, &ch);
        assert(ch.scene_count > 0);
        assert((uint32_t)ch.first_scene + ch.scene_count <= pack->scene_count);
        assert((uint32_t)ch.first_dlg + ch.dlg_count <= pack->dialogue_count);
        if (ch.next != SAYA_NONE) assert(ch.next < pack->chapter_count);
    }

    // 场景记录:对白区间在包内、选项目标有效、背景图可取。
    for (uint16_t i = 0; i < pack->scene_count; ++i) {
        saya_scene_t sc;
        saya_pack_scene(pack, i, &sc);
        assert((uint32_t)sc.first_dlg + sc.dlg_count <= pack->dialogue_count);
        if (sc.bg != SAYA_NONE) {
            saya_bg_t bg;
            assert(saya_pack_bg(pack, sc.bg, &bg));
            assert(bg.jpeg_len > 0 && bg.w > 0 && bg.h > 0);
            assert(bg.jpeg[0] == 0xFF && bg.jpeg[1] == 0xD8);   // JPEG SOI
        }
        for (uint8_t c = 0; c < sc.choice_count; ++c) {
            assert(sc.choice_href[c] < pack->chapter_count);
            assert(sc.choice_name[c] != SAYA_NONE);
        }
    }

    // 对白记录:文本与立绘可解引用。
    for (uint16_t i = 0; i < pack->dialogue_count; ++i) {
        saya_dialogue_t dlg;
        saya_pack_dialogue(pack, i, &dlg);
        assert(dlg.text_off + dlg.text_len <= pack->text_size);
        if (dlg.name != SAYA_NONE) {
            char name[64];
            assert(saya_pack_name(pack, dlg.name, name, sizeof(name)) > 0);
        }
        if (dlg.fg != SAYA_FG_KEEP && dlg.fg != SAYA_NONE) {
            saya_fg_t fg;
            assert(saya_pack_fg(pack, dlg.fg, &fg));
            assert(fg.w > 0 && fg.h > 0);
            assert(fg.mask_len == (uint32_t)(((fg.w + 7) / 8) * fg.h));
        }
        if (dlg.flags & SAYA_DLG_END) assert(dlg.arg != SAYA_NONE);
    }

    // 标题画面固定在背景表 0 号,尺寸必须是画面区尺寸(标题页复用它)。
    saya_bg_t title;
    assert(saya_pack_bg(pack, SAYA_BG_TITLE, &title));
    assert(title.w == 320 && title.h == 136);
    assert(title.jpeg[0] == 0xFF && title.jpeg[1] == 0xD8);

    // 源脚本编号 -> 章节下标:第一章是 0 号,最后一章是 51,不存在的编号返回 -1。
    assert(saya_pack_find_chapter(pack, 1) == 0);
    assert(saya_pack_find_chapter(pack, 51) == (int)pack->chapter_count - 1);
    assert(saya_pack_find_chapter(pack, 12) == -1);   // 源数据里没有第 12 章

    // 每个立绘的遮罩长度必须与裁剪尺寸吻合(1bpp 行打包)。
    for (uint16_t i = 0; i < pack->fg_count; ++i) {
        saya_fg_t fg;
        assert(saya_pack_fg(pack, i, &fg));
        assert(fg.mask_len == (uint32_t)(((fg.w + 7) / 8) * fg.h));
        assert(fg.h <= 136);   // 只保留画面区可见部分
    }
}

static void test_story_graph(const saya_pack_t *pack, const saya_layout_t *layout)
{
    // 第一章 -> 第一次选择(第 10 章)
    saya_player_t player = walk_to_choice(pack, layout, 100000);
    assert(player.at_choice);
    saya_chapter_t ch;
    saya_pack_chapter(pack, player.chapter, &ch);
    assert(ch.id == 10);
    saya_scene_t sc;
    saya_pack_scene(pack, (uint16_t)(ch.first_scene + player.scene), &sc);
    assert(sc.choice_count == 2);

    // 选项 0 -> End 结局
    saya_player_t branch_a = player;
    assert(saya_player_choose(&branch_a, pack, 0, layout));
    int steps = 0;
    while (!branch_a.ended && steps++ < 100000) {
        assert(saya_player_advance(&branch_a, pack, layout) != SAYA_STEP_STUCK);
    }
    assert(branch_a.ended);
    char end_name[64];
    saya_pack_name(pack, branch_a.end_name, end_name, sizeof(end_name));
    assert(strcmp(end_name, "End") == 0);

    // 选项 1 -> 第二次选择(第 20 章)
    saya_player_t branch_b = player;
    assert(saya_player_choose(&branch_b, pack, 1, layout));
    steps = 0;
    while (!branch_b.at_choice && !branch_b.ended && steps++ < 100000) {
        assert(saya_player_advance(&branch_b, pack, layout) != SAYA_STEP_STUCK);
    }
    assert(branch_b.at_choice);
    saya_pack_chapter(pack, branch_b.chapter, &ch);
    assert(ch.id == 20);

    // 选项 1.0 -> BadEnd,1.1 -> MadEnd
    saya_player_t bad = branch_b;
    assert(saya_player_choose(&bad, pack, 0, layout));
    steps = 0;
    while (!bad.ended && steps++ < 100000) {
        assert(saya_player_advance(&bad, pack, layout) != SAYA_STEP_STUCK);
    }
    assert(bad.ended);
    saya_pack_name(pack, bad.end_name, end_name, sizeof(end_name));
    assert(strcmp(end_name, "BadEnd") == 0);

    saya_player_t mad = branch_b;
    assert(saya_player_choose(&mad, pack, 1, layout));
    steps = 0;
    while (!mad.ended && steps++ < 100000) {
        assert(saya_player_advance(&mad, pack, layout) != SAYA_STEP_STUCK);
    }
    assert(mad.ended);
    saya_pack_name(pack, mad.end_name, end_name, sizeof(end_name));
    assert(strcmp(end_name, "MadEnd") == 0);

    // 结局之后继续推进只是重复结局,不会越界。
    assert(saya_player_advance(&mad, pack, layout) == SAYA_STEP_ENDING);
    assert(saya_player_advance(&mad, pack, layout) == SAYA_STEP_ENDING);
}

static void test_pagination(const saya_layout_t *layout)
{
    uint32_t offsets[SAYA_MAX_PAGES + 1];

    // 纯 ASCII:19 字/行、4 行 -> 38 单位/行、152 单位/页。
    const int ascii_pages = saya_text_pages("abcdefghijklmnopqrstuvwxyz0123456789ABCD",
                                            layout->units_per_line, layout->lines_per_page,
                                            offsets, SAYA_MAX_PAGES + 1);
    assert(ascii_pages >= 1);
    assert(offsets[ascii_pages] == strlen("abcdefghijklmnopqrstuvwxyz0123456789ABCD"));

    // 80 个全角字(240 字节):一行 19 字、一屏 4 行 = 每屏 76 字,第二屏放剩下的 4 字。
    char full[1024];
    int n = 0;
    for (int i = 0; i < 80; ++i) n += snprintf(full + n, sizeof(full) - (size_t)n, "字");
    assert(n == 240);
    int pages = saya_text_pages(full, 38, 4, offsets, SAYA_MAX_PAGES + 1);
    assert(pages == 2);
    assert(offsets[0] == 0);
    assert(offsets[1] == 228);   // 76 字 = 228 字节
    assert(offsets[2] == 240);

    // 禁则:第 77 个字是句号时,句号被拉回第 4 行行尾,不落在行首。
    n = 0;
    for (int i = 0; i < 76; ++i) n += snprintf(full + n, sizeof(full) - (size_t)n, "字");
    n += snprintf(full + n, sizeof(full) - (size_t)n, "。");
    n += snprintf(full + n, sizeof(full) - (size_t)n, "后续文字");
    pages = saya_text_pages(full, 38, 4, offsets, SAYA_MAX_PAGES + 1);
    assert(pages == 2);
    assert(offsets[1] == 231);   // 76 字 + 句号 = 77 字 = 231 字节

    // 空文本占一页。
    assert(saya_text_pages("", 38, 4, offsets, SAYA_MAX_PAGES + 1) == 1);
    assert(offsets[0] == 0 && offsets[1] == 0);

    // 换行符强制换行:把每页行数设成 1,一行就是一页。
    assert(saya_text_pages("a\nb", 38, 1, offsets, SAYA_MAX_PAGES + 1) == 2);
    assert(offsets[0] == 0 && offsets[1] == 2 && offsets[2] == 3);
}

static void test_scene_and_save(const saya_pack_t *pack, const saya_layout_t *layout)
{
    saya_player_t player;
    assert(saya_player_start(&player, pack, 0, layout));

    // 跳过场景:第一幕可以跳,跳到下一幕后位置前进。
    const uint16_t scene_before = player.scene;
    assert(saya_player_skip_scene(&player, pack, layout));
    assert(player.scene == scene_before + 1);
    assert(player.dialogue == 0);

    // 存档往返。
    player.fg = SAYA_FG_KEEP == player.fg ? SAYA_NONE : player.fg;
    saya_save_t save = {
        .chapter = player.chapter,
        .scene = player.scene,
        .dialogue = 3,
        .fg = 7,
        .choice_len = 2,
        .choice_pick = { 1, 0, 9, 9, 9, 9, 9, 9 },
    };
    uint8_t blob[64];
    const size_t len = saya_save_encode(&save, blob, sizeof(blob));
    assert(len == 11 + SAYA_CHOICE_HISTORY);
    saya_save_t back;
    assert(saya_save_decode(&back, blob, len));
    assert(back.chapter == save.chapter && back.scene == save.scene);
    assert(back.dialogue == save.dialogue && back.fg == save.fg);
    assert(back.choice_len == save.choice_len);
    for (int i = 0; i < SAYA_CHOICE_HISTORY; ++i) {
        assert(back.choice_pick[i] == (i < (int)save.choice_len ? save.choice_pick[i] : 0));
    }

    // 坏数据要拒绝,而不是读出一堆垃圾。
    uint8_t bad[32];
    memcpy(bad, blob, len);
    bad[0] = 0;
    assert(!saya_save_decode(&back, bad, len));
    assert(!saya_save_decode(&back, blob, 4));
    assert(saya_save_encode(&save, blob, 4) == 0);
}

// 抽查长对白:模型算出的分页边界,必须与 saya_player_page_text 返回的切片一致,
// 而且所有页拼起来等于原文。
static void test_page_text(const saya_pack_t *pack, const saya_layout_t *layout)
{
    int checked = 0;
    for (uint16_t c = 0; c < pack->chapter_count && checked < 20; ++c) {
        saya_chapter_t ch;
        saya_pack_chapter(pack, c, &ch);
        for (uint16_t s = 0; s < ch.scene_count && checked < 20; ++s) {
            saya_scene_t sc;
            saya_pack_scene(pack, (uint16_t)(ch.first_scene + s), &sc);
            for (uint16_t d = 0; d < sc.dlg_count && checked < 20; ++d) {
                saya_player_t p;
                saya_player_reset(&p);
                p.chapter = c;
                p.scene = s;
                p.dialogue = d;
                p.page = 0;
                char full[SAYA_TEXT_BUFFER];
                const size_t full_len = saya_player_text(&p, pack, full, sizeof(full));

                uint32_t offsets[SAYA_MAX_PAGES + 1];
                const int pages = saya_text_pages(full, layout->units_per_line,
                                                  layout->lines_per_page, offsets,
                                                  SAYA_MAX_PAGES + 1);
                if (pages < 2) {
                    // 一页 = 76 个全角字 = 228 字节;放得下就不该切页。
                    assert(full_len <= 240);
                    continue;
                }
                assert(offsets[pages] == full_len);

                char joined[SAYA_TEXT_BUFFER];
                size_t joined_len = 0;
                for (int page = 0; page < pages; ++page) {
                    char buf[SAYA_TEXT_BUFFER];
                    p.page = (uint16_t)page;
                    const size_t got =
                        saya_player_page_text(&p, pack, layout, buf, sizeof(buf));
                    assert(got == offsets[page + 1] - offsets[page]);
                    assert(memcmp(buf, full + offsets[page], got) == 0);
                    assert(joined_len + got < sizeof(joined));
                    memcpy(joined + joined_len, buf, got);
                    joined_len += got;
                }
                assert(joined_len == full_len);
                assert(memcmp(joined, full, full_len) == 0);
                checked++;
            }
        }
    }
    assert(checked > 0);   // 必须真的抽查到长句,否则这个测试等于没跑
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "main/saya_data/saya_pack.bin";
    uint32_t size = 0;
    uint8_t *blob = load_file(path, &size);

    saya_pack_t pack;
    assert(saya_pack_open(&pack, blob, size));

    // 坏包要拒绝。
    {
        saya_pack_t bad;
        uint8_t *copy = malloc(size);
        assert(copy);
        memcpy(copy, blob, size);
        copy[0] = 'X';
        assert(!saya_pack_open(&bad, copy, size));
        memcpy(copy, blob, size);
        copy[12] = (uint8_t)(copy[12] + 1);   // 总长度与实际不符
        assert(!saya_pack_open(&bad, copy, size));
        assert(!saya_pack_open(&bad, blob, 8));
        free(copy);
    }

    const saya_layout_t layout = { .units_per_line = 38, .lines_per_page = 4 };

    test_pack_structure(&pack);
    test_story_graph(&pack, &layout);
    test_pagination(&layout);
    test_scene_and_save(&pack, &layout);
    test_page_text(&pack, &layout);

    free(blob);
    printf("test_saya_model: OK (chapters=%u scenes=%u dialogues=%u bgs=%u fgs=%u)\n",
           (unsigned)pack.chapter_count, (unsigned)pack.scene_count,
           (unsigned)pack.dialogue_count, (unsigned)pack.bg_count, (unsigned)pack.fg_count);
    return 0;
}
