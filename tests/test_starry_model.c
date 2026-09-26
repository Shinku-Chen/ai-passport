// tests/test_starry_model.c —— 阅读器纯逻辑:用真实资源包走完整条剧情线。
//
// 覆盖:资源包解析、宽度模型、分页与逐行换行的一致性、剧情图(章节链/选项/结局)、
// 跳场景规则、存档编解码往返。构建与运行方式见 tools/validate.sh:
//   (1) cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_starry_model.c
//           main/starry_model.c main/starry_pack.c -o test_starry_model
//   (2) ./test_starry_model main/starry_data/starry_pack.bin
#include "starry_model.h"
#include "starry_pack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(cond, ...)                                                              \
    do {                                                                              \
        if (!(cond)) {                                                                \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                      \
            fprintf(stderr, __VA_ARGS__);                                             \
            fprintf(stderr, "\n");                                                    \
            g_failures++;                                                             \
        }                                                                             \
    } while (0)

static uint8_t *load_file(const char *path, uint32_t *out_size)
{
    FILE *fh = fopen(path, "rb");
    if (!fh) {
        fprintf(stderr, "打不开资源包: %s\n", path);
        exit(2);
    }
    fseek(fh, 0, SEEK_END);
    const long size = ftell(fh);
    fseek(fh, 0, SEEK_SET);
    uint8_t *data = (uint8_t *)malloc((size_t)size);
    if (!data || fread(data, 1, (size_t)size, fh) != (size_t)size) {
        fprintf(stderr, "读资源包失败\n");
        exit(2);
    }
    fclose(fh);
    *out_size = (uint32_t)size;
    return data;
}

static void test_pack_basics(const starry_pack_t *pack)
{
    CHECK(pack->chapter_count == 39, "章节数 %u", (unsigned)pack->chapter_count);
    CHECK(pack->scene_count == 1260, "场景数 %u", (unsigned)pack->scene_count);
    CHECK(pack->dialogue_count == 13787, "对白数 %u", (unsigned)pack->dialogue_count);
    CHECK(pack->bg_count >= 100, "背景数 %u", (unsigned)pack->bg_count);
    CHECK(pack->fg_count >= 15, "立绘数 %u", (unsigned)pack->fg_count);

    starry_chapter_t ch;
    int expected_scene = 0;
    for (uint16_t i = 0; i < pack->chapter_count; ++i) {
        starry_pack_chapter(pack, i, &ch);
        CHECK(ch.first_scene == expected_scene, "章节 %u 场景起点 %u != %d", (unsigned)i,
              (unsigned)ch.first_scene, (int)expected_scene);
        if ((uint32_t)i + 1u < pack->chapter_count) {
            CHECK(ch.next == (uint16_t)(i + 1), "章节 %u next=%u", (unsigned)i, (unsigned)ch.next);
        } else {
            CHECK(ch.next == STARRY_NONE, "最后一章不应有 next");
        }
        CHECK(ch.scene_count > 0 && ch.dlg_count > 0, "章节 %u 为空", (unsigned)i);
        expected_scene += ch.scene_count;
    }

    // 背景记录:尺寸必须是整屏,JPEG 段在数据区内。
    starry_bg_t bg;
    CHECK(starry_pack_bg(pack, STARRY_BG_TITLE, &bg), "标题画面缺失");
    CHECK(bg.w == 240 && bg.h == 320, "标题画面尺寸 %ux%u", (unsigned)bg.w, (unsigned)bg.h);
    CHECK(bg.jpeg_len > 0, "标题画面为空");

    // 立绘:全部塞进固件缓冲上限,靠右边、底边贴屏幕底(下半身会落在半透明文本框下面,
    // 所以允许越过 STARRY_BOX_Y,但必须留在屏幕内)。
    // 这里的两个上限跟 main/atri_image.h 的 STARRY_SPRITE_MAX_W / STARRY_SPRITE_H 一致。
    const uint16_t sprite_max_w = 168;
    const uint16_t sprite_max_h = 252;
    for (uint16_t id = 0; id < pack->fg_count; ++id) {
        starry_fg_t fg;
        CHECK(starry_pack_fg(pack, id, &fg), "立绘 %u 读取失败", (unsigned)id);
        CHECK(fg.w > 0 && fg.h > 0, "立绘 %u 尺寸为空", (unsigned)id);
        CHECK(fg.w <= sprite_max_w && fg.h <= sprite_max_h, "立绘 %u 超缓冲 %ux%u",
              (unsigned)id, (unsigned)fg.w, (unsigned)fg.h);
        CHECK(fg.x + fg.w <= 240 && fg.y + fg.h <= 320, "立绘 %u 越界 (%u,%u)+%ux%u",
              (unsigned)id, (unsigned)fg.x, (unsigned)fg.y, (unsigned)fg.w, (unsigned)fg.h);
        CHECK(fg.x + fg.w >= 232, "立绘 %u 没贴右边: 右边界 %u", (unsigned)id,
              (unsigned)(fg.x + fg.w));
        CHECK(fg.y + fg.h >= 300, "立绘 %u 没贴到屏幕底部: 底边 %u", (unsigned)id,
              (unsigned)(fg.y + fg.h));
        // 无损 RGB565:每像素 2 字节;4bpp 遮罩:每行 (w+1)/2 字节。
        CHECK(fg.color_len == (uint32_t)fg.w * fg.h * 2u, "立绘 %u RGB565 长度与尺寸不匹配",
              (unsigned)id);
        CHECK(fg.mask_len == (uint32_t)(((fg.w + 1u) / 2u) * fg.h), "立绘 %u 遮罩长度与尺寸不匹配",
              (unsigned)id);
    }
}

static void test_width_model(void)
{
    CHECK(starry_char_units('A') == 1, "ASCII 应算半角");
    CHECK(starry_char_units(0x4E00u) == 2, "汉字应算全角");
    CHECK(starry_char_units(0x3002u) == 2, "句号应算全角");
    CHECK(starry_char_units(0xFF61u) == 1, "半角片假名应算半角");
    CHECK(starry_char_units(0x3042u) == 2, "平假名应算全角");

    // UTF-8 步进不会切开多字节字符
    const char *text = "a汉b。";
    const size_t len = strlen(text);
    size_t pos = 0;
    size_t steps = 0;
    while (pos < len) {
        const size_t next = starry_utf8_next_boundary(text, len, pos);
        CHECK(next > pos && next <= len, "utf8 步进越界: %zu -> %zu", pos, next);
        pos = next;
        steps++;
    }
    CHECK(steps == 4, "字符数 %zu != 4", steps);

    // 截断缓冲时必须停在 UTF-8 边界上
    uint32_t offs[4] = { 0 };
    const int lines = starry_text_lines("汉汉汉汉", 4, offs, 4);
    CHECK(lines >= 1, "换行行数 %d", lines);
    for (int i = 0; i <= lines && i < 4; ++i) {
        CHECK(offs[i] <= 12, "行偏移越界");
    }
}

static void test_pagination_matches_wrapping(const starry_pack_t *pack)
{
    const starry_layout_t layouts[2] = {
        { STARRY_SMALL_UNITS_PER_LINE, STARRY_SMALL_LINES_PER_PAGE },
        { STARRY_LARGE_UNITS_PER_LINE, STARRY_LARGE_LINES_PER_PAGE },
    };
    char text[STARRY_TEXT_BUFFER];
    uint32_t page_offsets[STARRY_MAX_PAGES + 1];
    uint32_t line_offsets[STARRY_TEXT_MAX_LINES + 1];
    int checked = 0;

    for (uint16_t i = 0; i < pack->dialogue_count; ++i) {
        starry_dialogue_t dlg;
        starry_pack_dialogue(pack, i, &dlg);
        const size_t len = starry_pack_text(pack, dlg.text_off, dlg.text_len, text, sizeof(text));
        if (len == 0) continue;
        for (size_t l = 0; l < 2; ++l) {
            const int pages = starry_text_pages(text, layouts[l].units_per_line,
                                                layouts[l].lines_per_page, page_offsets,
                                                STARRY_MAX_PAGES + 1);
            CHECK(pages >= 1 && pages <= STARRY_MAX_PAGES, "对白 %u 页数 %d", (unsigned)i, pages);
            CHECK(page_offsets[0] == 0, "首页起点应为 0");
            CHECK(page_offsets[pages] == len, "末页终点应为文本长度 %zu(实际 %u)", len,
                  (unsigned)page_offsets[pages]);
            for (int p = 0; p < pages; ++p) {
                CHECK(page_offsets[p] <= page_offsets[p + 1], "页偏移非递增");
                // 每一页自身按同一套规则换行时,行数不得超过一屏。
                // 注意要先截成独立字符串:否则会把后面的正文也算进这一页。
                char page[STARRY_TEXT_BUFFER];
                size_t take = page_offsets[p + 1] - page_offsets[p];
                if (take >= sizeof(page)) take = sizeof(page) - 1;
                memcpy(page, text + page_offsets[p], take);
                page[take] = '\0';
                const int lines = starry_text_lines(page, layouts[l].units_per_line, line_offsets,
                                                    STARRY_TEXT_MAX_LINES);
                CHECK(lines <= layouts[l].lines_per_page,
                      "对白 %u 第 %d 页有 %d 行,超过每屏 %d 行", (unsigned)i, p, lines,
                      layouts[l].lines_per_page);
            }
            checked++;
        }
    }
    CHECK(checked > 0, "没有检查到任何对白");
    printf("  分页自检: %d 个 (对白, 版面) 组合\n", checked);
}

static void test_story_graph(const starry_pack_t *pack)
{
    starry_layout_t layout = { STARRY_SMALL_UNITS_PER_LINE, STARRY_SMALL_LINES_PER_PAGE };
    starry_player_t player;
    CHECK(starry_player_start(&player, pack, 0, &layout), "第一章打不开");

    uint16_t chapter_ids[64];
    int chapter_count = 0;
    chapter_ids[chapter_count++] = starry_pack_chapter_id(pack, 0);

    int choices = 0;
    int text_steps = 0;
    int scene_steps = 0;
    int sprite_steps = 0;       // 真正会画出立绘的步数
    int hide_cg = 0;            // 因为画面本身是 CG/纯色幕而收起的立绘
    int hide_narration = 0;     // 因为这一句没有说话人而收起的立绘
    int hide_other = 0;         // 因为说话人不是立绘归属角色而收起的立绘
    int sprite_ownerless = 0;   // 归属不明却被源数据引用的立绘
    bool seen_owner[64] = { false };   // 画出来的立绘覆盖多少个不同归属角色
    int repaints = 0;           // 需要整屏重画(背景或立绘变化)的步数
    uint16_t prev_bg = 0xFFFF, prev_sprite = 0xFFFF;
    bool first_step = true;
    bool seen_sprite[64] = { false };
    long guard = 0;
    starry_step_t step = STARRY_STEP_TEXT;
    while (!player.ended && guard++ < 400000) {
        step = starry_player_advance(&player, pack, &layout);
        // 统计整屏重画次数:渲染器在"背景或立绘变化"时会重新解码背景并整屏推屏。
        {
            const uint16_t shown_now = starry_player_visible_sprite(&player, pack);
            if (first_step || player.bg != prev_bg || shown_now != prev_sprite) repaints++;
            prev_bg = player.bg;
            prev_sprite = shown_now;
            first_step = false;
        }
        // 立绘可见性规则:凡是会画出来的立绘,必须"本人正在说话"、当前画面不是 CG/纯色幕。
        {
            const uint16_t shown = starry_player_visible_sprite(&player, pack);
            if (shown == STARRY_NONE && player.sprite != STARRY_NONE && !player.ended) {
                starry_bg_t hidden_bg;
                char who[64] = { 0 };
                starry_player_speaker(&player, pack, who, sizeof(who));
                if (starry_pack_bg(pack, player.bg, &hidden_bg) && hidden_bg.no_sprite) hide_cg++;
                else if (!who[0]) hide_narration++;
                else hide_other++;
            }
            if (shown != STARRY_NONE) {
                sprite_steps++;
                CHECK(shown < pack->fg_count, "可见立绘下标越界 %u", (unsigned)shown);
                seen_sprite[shown % 64] = true;
                starry_fg_t fg;
                CHECK(starry_pack_fg(pack, shown, &fg), "可见立绘 %u 读不到", (unsigned)shown);
                if (fg.owner == STARRY_NONE) sprite_ownerless++;
                if (fg.owner < 64) seen_owner[fg.owner] = true;
                starry_bg_t bg;
                CHECK(starry_pack_bg(pack, player.bg, &bg), "背景 %u 读不到", (unsigned)player.bg);
                CHECK(!bg.no_sprite, "CG/纯色幕上仍画了立绘: 背景 %u", (unsigned)player.bg);
                if (!player.at_choice) {
                    char owner[64] = { 0 };
                    char speaker[64] = { 0 };
                    starry_pack_name(pack, fg.owner, owner, sizeof(owner));
                    starry_player_speaker(&player, pack, speaker, sizeof(speaker));
                    CHECK(speaker[0] != 0,
                          "没有说话人却画了立绘: 立绘 %u 背景 %u", (unsigned)shown,
                          (unsigned)player.bg);
                    CHECK(strcmp(owner, speaker) == 0,
                          "立绘 %u 属于 %s,却由 %s 说话时出现", (unsigned)shown, owner, speaker);
                }
            }
        }
        switch (step) {
        case STARRY_STEP_TEXT: text_steps++; break;
        case STARRY_STEP_SCENE: scene_steps++; break;
        case STARRY_STEP_CHAPTER:
            CHECK(chapter_count < 64, "章节数超缓冲");
            chapter_ids[chapter_count++] = starry_pack_chapter_id(pack, player.chapter);
            break;
        case STARRY_STEP_CHOICE:
            choices++;
            CHECK(player.at_choice, "选项步骤应置 at_choice");
            CHECK(starry_player_choose(&player, pack, 0, &layout), "选项 0 无法选择");
            break;
        case STARRY_STEP_ENDING: break;
        default:
            CHECK(0, "推进卡住: chapter %u scene %u", (unsigned)player.chapter,
                  (unsigned)player.scene);
            break;
        }
        if (step == STARRY_STEP_ENDING) break;
    }

    CHECK(player.ended, "应抵达结局");
    CHECK(choices == 1, "本作只有一处选项,实际 %d", choices);
    // 剧本共 13787 句,但源数据的 6 处 toScenes 跳转会跳过若干场景(约 1800 句),
    // 选项场景也不逐句走完,所以对白步数低于总句数是预期行为。
    CHECK(text_steps > 11000, "推进的对白步数 %d 偏少", text_steps);
    CHECK(scene_steps > 1000, "推进的场景步数 %d 偏少", scene_steps);
    CHECK(text_steps + scene_steps + chapter_count > 12500, "总步数 %d 偏少",
          text_steps + scene_steps + chapter_count);
    CHECK(chapter_count == 39, "走过的章节数 %d != 39", chapter_count);
    for (int i = 0; i < chapter_count; ++i) {
        const uint16_t expected = (uint16_t)(i + 1 + (i >= 6 ? 1 : 0));   // 源脚本缺 7
        CHECK(chapter_ids[i] == expected, "第 %d 章编号 %u != %u", i, (unsigned)chapter_ids[i],
              (unsigned)expected);
    }
    char end_name[64] = { 0 };
    starry_pack_name(pack, player.end_name, end_name, sizeof(end_name));
    CHECK(strcmp(end_name, "FIN") == 0, "结局名 %s != FIN", end_name);
    CHECK(sprite_ownerless == 0, "有 %d 步画出了归属不明的立绘", sprite_ownerless);
    CHECK(sprite_steps > 3000, "会画出立绘的步数 %d 偏少(规则可能误杀)", sprite_steps);
    int sprite_kinds = 0;
    for (int i = 0; i < 64; ++i) sprite_kinds += seen_sprite[i] ? 1 : 0;
    CHECK(sprite_kinds > 5, "整条线只用到 %d 张立绘,过少", sprite_kinds);
    // 立绘可见性依赖 fg.owner:解析层漏读会让 owner 全部塌缩成 0(诺瓦),
    // 从而只放行一个角色的台词,立绘步数从约 4 千跌到约 1 千——这里直接盯住它。
    int owner_kinds = 0;
    for (int i = 0; i < 64; ++i) owner_kinds += seen_owner[i] ? 1 : 0;
    CHECK(owner_kinds >= 5, "画出的立绘只覆盖 %d 个归属角色,owner 解析可能失效", owner_kinds);
    printf("  剧情图自检: %d 章 / %d 对白步 / %d 场景步 / %d 立绘步 / 结局 %s\n", chapter_count,
           text_steps, scene_steps, sprite_steps, end_name);
    printf("  立绘归属角色数: %d(owner 解析自检)\n", owner_kinds);
    printf("  立绘收起: CG/纯色幕 %d 步 / 旁白 %d 步 / 非归属角色 %d 步\n", hide_cg, hide_narration,
           hide_other);
    printf("  需要整屏重画的步数: %d / %d(每次重画 = 一次背景解码,约 166ms)\n", repaints,
           text_steps + scene_steps);
}

static void test_save_roundtrip(const starry_pack_t *pack)
{
    starry_layout_t layout = { STARRY_SMALL_UNITS_PER_LINE, STARRY_SMALL_LINES_PER_PAGE };
    starry_player_t player;
    CHECK(starry_player_start(&player, pack, 3, &layout), "第 4 章打不开");
    for (int i = 0; i < 12; ++i) {
        if (starry_player_advance(&player, pack, &layout) == STARRY_STEP_CHOICE) {
            CHECK(starry_player_choose(&player, pack, 1, &layout), "选项 1 无法选择");
            break;
        }
    }

    starry_save_t save;
    starry_save_from_player(&player, &save);
    uint8_t blob[64];
    const size_t len = starry_save_encode(&save, blob, sizeof(blob));
    CHECK(len > 0, "存档编码失败");
    starry_save_t back;
    CHECK(starry_save_decode(&back, blob, len), "存档解码失败");
    CHECK(back.chapter == save.chapter && back.scene == save.scene &&
              back.dialogue == save.dialogue && back.bg == save.bg && back.sprite == save.sprite &&
              back.choice_len == save.choice_len,
          "存档往返不一致");
    CHECK(memcmp(back.choice_pick, save.choice_pick, sizeof(save.choice_pick)) == 0,
          "选择历史不一致");

    starry_player_t loaded;
    CHECK(starry_player_load(&loaded, pack, &back, &layout), "存档载入失败");
    CHECK(loaded.chapter == player.chapter && loaded.scene == player.scene &&
              loaded.dialogue == player.dialogue,
          "载入位置不一致");
    CHECK(loaded.bg == player.bg && loaded.sprite == player.sprite, "载入背景/立绘不一致");

    // 越界存档:章节点不存在时必须拒绝,场景越界时停在场景开头。
    starry_save_t bad = back;
    bad.chapter = (uint16_t)(pack->chapter_count + 5);
    CHECK(!starry_player_load(&loaded, pack, &bad, &layout), "越界章节应载入失败");
    bad = back;
    bad.dialogue = 60000;
    CHECK(starry_player_load(&loaded, pack, &bad, &layout), "越界对白应降级为场景开头");
    CHECK(loaded.dialogue == 0, "越界对白未归零: %u", (unsigned)loaded.dialogue);
}

static void test_skip_rules(const starry_pack_t *pack)
{
    starry_layout_t layout = { STARRY_SMALL_UNITS_PER_LINE, STARRY_SMALL_LINES_PER_PAGE };
    starry_player_t player;

    // 普通场景可以跳过
    CHECK(starry_player_start(&player, pack, 0, &layout), "第一章打不开");
    const uint16_t scene_before = player.scene;
    CHECK(starry_player_skip_scene(&player, pack, &layout), "普通场景应可跳过");
    CHECK(player.scene == (uint16_t)(scene_before + 1), "跳过后场景号未前进");

    // 章节最后一幕不允许跳过
    starry_chapter_t ch;
    starry_pack_chapter(pack, 0, &ch);
    CHECK(starry_player_start(&player, pack, 0, &layout), "第一章打不开");
    CHECK(starry_player_load(&player, pack,
                             &(starry_save_t){ .chapter = 0,
                                               .scene = (uint16_t)(ch.scene_count - 1),
                                               .bg = STARRY_BG_TITLE,
                                               .sprite = STARRY_NONE },
                             &layout),
          "定位到末幕失败");
    CHECK(!starry_player_skip_scene(&player, pack, &layout), "末幕不该允许跳过");

    // 选项场景不允许跳过
    starry_player_t chooser;
    CHECK(starry_player_start(&chooser, pack, 2, &layout), "第 3 章打不开");
    bool found_choice = false;
    for (int i = 0; i < 40 && !found_choice; ++i) {
        if (chooser.at_choice) {
            found_choice = true;
            break;
        }
        const starry_step_t s = starry_player_advance(&chooser, pack, &layout);
        if (s == STARRY_STEP_CHOICE) found_choice = true;
    }
    CHECK(found_choice, "第 3 章里应能走到选项");
    CHECK(!starry_player_skip_scene(&chooser, pack, &layout), "选项上不该允许跳过");
    // 跳过章节:从任意位置(含选项上)直接进下一章第一幕;最后一章拒绝。
    CHECK(starry_player_skip_chapter(&chooser, pack, &layout), "选项上应能跳过整章");
    CHECK(chooser.chapter == 3, "跳过章节后应落在下一章,实际 %u", (unsigned)chooser.chapter);
    CHECK(chooser.scene == 0 && chooser.dialogue == 0, "跳过章节应停在下一章第一幕");
    starry_layout_t last = layout;
    starry_player_t tail;
    CHECK(starry_player_start(&tail, pack, (uint16_t)(pack->chapter_count - 1), &last),
          "末章打不开");
    CHECK(!starry_player_skip_chapter(&tail, pack, &layout), "最后一章不该允许跳过");
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "main/starry_data/starry_pack.bin";
    uint32_t size = 0;
    uint8_t *blob = load_file(path, &size);

    starry_pack_t pack;
    if (!starry_pack_open(&pack, blob, size)) {
        fprintf(stderr, "资源包不合法: %s(%u 字节)\n", path, (unsigned)size);
        return 2;
    }
    printf("资源包 %s: %u 字节\n", path, (unsigned)size);

    test_pack_basics(&pack);
    test_width_model();
    test_pagination_matches_wrapping(&pack);
    test_story_graph(&pack);
    test_save_roundtrip(&pack);
    test_skip_rules(&pack);

    free(blob);
    if (g_failures) {
        fprintf(stderr, "starry_model: %d 项失败\n", g_failures);
        return 1;
    }
    printf("starry_model: PASS\n");
    return 0;
}
