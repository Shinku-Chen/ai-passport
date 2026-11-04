// tests/test_gal_model.c —— 剧情虚拟机语义测试(合成 pack,宿主机运行)。
//
// 覆盖:顺序执行、BG/立绘槽位、整数变量计数器、条件跳转、回跳、选项动作、
// 条件选项、章节/结局步骤、解锁位图(>32 位)、UTF-8 安全截断。
#include "gal_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        failures++; \
    } \
} while (0)

// 文本 "first\0second\0back\0bad end\0true end\0" 的相对偏移
enum { OFF_FIRST = 0, OFF_SECOND = 6, OFF_BACK = 13, OFF_BAD = 18, OFF_TRUE = 26 };
static const char k_text[] = "first\0second\0back\0bad end\0true end\0";
enum { N_FIRST = 0, N_SECOND = 1, N_BAD = 2, N_TRUE = 3 };

static const gal_line_t k_lines[] = {
    { OFF_FIRST, 5, GAL_NONE },
    { OFF_SECOND, 6, GAL_NONE },
    { OFF_BACK, 4, GAL_NONE },
    { OFF_TRUE, 8, GAL_NONE },
};
static const gal_event_t k_events[] = {
    { GAL_EV_LINE, 0, 0, 0, 0 },            // 0 LINE first
    { GAL_EV_BG, 0, 0, 0, 0 },              // 1 BG 0
    { GAL_EV_SPRITE, GAL_POS_LEFT, 0, GAL_POS_CENTER, 0 },  // 2 立绘槽 0
    { GAL_EV_VAR, 1, GAL_OP_ADD, 1, 0 },    // 3 v1 += 1
    { GAL_EV_LINE, 0, 1, 0, 0 },            // 4 LINE second
    { GAL_EV_GOTO, 0, 7, 0, 0 },            // 5 if v1>=2 -> 7
    { GAL_EV_LINE, 0, 2, 0, 0 },            // 6 LINE back
    { GAL_EV_CHOICE, 0, 0, 0, 0 },          // 7 CHOICE 0
    { GAL_EV_ENDING, 0, 0, 0, 0 },          // 8 ENDING bad
    { GAL_EV_LINE, 0, 2, 0, 0 },            // 9 LINE back(reward)
    { GAL_EV_ENDING, 0, 1, 0, 0 },          // 10 ENDING true
    { GAL_EV_END, 0, 0, 0, 0 },             // 11 END
    { GAL_EV_LINE, 0, 3, 0, 0 },            // 12 LINE true end
    { GAL_EV_GOTO, 0, 0, GAL_NONE, 0 },     // 13 无条件回跳到 0
};
static const gal_choice_t k_choices[] = { { 0, 2, 0, GAL_NONE } };
static const gal_option_t k_options[] = {
    { N_FIRST, 8, GAL_NONE, GAL_NONE, 0, 0 },   // 直接走 Bad End
    { N_SECOND, 9, 0, 0, 1, 0 },                // 条件 v1>=2,动作 v0 += 5
};
static const gal_action_t k_actions[] = { { 0, GAL_OP_ADD, 5 } };
static const gal_action_t k_conds[] = { { 1, GAL_CMP_GE, 2 } };
static const gal_chapter_t k_chapters[] = { { N_FIRST, 0, 14, 0 } };
static const gal_ending_t k_endings[] = {
    { N_BAD, N_BAD, 8, 0, 0 },
    { N_TRUE, N_TRUE, 10, 0, 0 },
};
static const gal_gallery_t k_gallery[] = { { N_FIRST, 0, 8, 0 } };
static const uint8_t k_img[] = { 1, 2, 3, 4, 5, 6, 7, 8 };

typedef struct {
    uint32_t type, offset, count, size;
} sec_t;

static uint8_t g_blob[8192];

static void add_section(uint8_t **cursor, sec_t *out, uint32_t type, const void *data,
                        uint32_t count, uint32_t size)
{
    out->type = type;
    out->offset = (uint32_t)(*cursor - g_blob);
    out->count = count;
    out->size = size;
    if (size) memcpy(*cursor, data, size);
    *cursor += size;
}

static uint32_t build_pack(void)
{
    const uint32_t nsec = 15;
    uint8_t *cursor = g_blob + 20 + 16 * nsec;
    sec_t sec[15];
    gal_line_t name_tbl[4];
    for (int i = 0; i < 4; ++i) {
        const uint32_t offs[4] = { 0, 6, 18, 26 };
        const uint16_t lens[4] = { 5, 6, 7, 8 };
        name_tbl[i].text_off = offs[i];
        name_tbl[i].text_len = lens[i];
        name_tbl[i].speaker = 0;
    }
    uint8_t img[36];
    memset(img, 0, sizeof(img));
    const uint16_t w = 90, h = 240;
    memcpy(img, &w, 2);
    memcpy(img + 2, &h, 2);
    const uint32_t img_base = 0;   // patched below (needs IMGDATA offset)
    (void)img_base;

    add_section(&cursor, &sec[0], GAL_SEC_TEXT, k_text, 0, (uint32_t)sizeof(k_text));
    add_section(&cursor, &sec[1], GAL_SEC_NAME, name_tbl, 4, (uint32_t)sizeof(name_tbl));
    add_section(&cursor, &sec[2], GAL_SEC_EVENT, k_events, 14, (uint32_t)sizeof(k_events));
    add_section(&cursor, &sec[3], GAL_SEC_LINE, k_lines, 4, (uint32_t)sizeof(k_lines));
    add_section(&cursor, &sec[4], GAL_SEC_CHOICE, k_choices, 1, (uint32_t)sizeof(k_choices));
    add_section(&cursor, &sec[5], GAL_SEC_OPTION, k_options, 2, (uint32_t)sizeof(k_options));
    add_section(&cursor, &sec[6], GAL_SEC_ACTION, k_actions, 1, (uint32_t)sizeof(k_actions));
    add_section(&cursor, &sec[7], GAL_SEC_COND, k_conds, 1, (uint32_t)sizeof(k_conds));
    // 图片表:top/bot/mask 都指向 IMGDATA(偏移在写完 IMGDATA 后回填)
    sec[8].type = GAL_SEC_IMAGE;
    sec[8].offset = (uint32_t)(cursor - g_blob);
    sec[8].count = 1;
    sec[8].size = sizeof(img);
    cursor += sizeof(img);
    add_section(&cursor, &sec[9], GAL_SEC_CHAPTER, k_chapters, 1, (uint32_t)sizeof(k_chapters));
    add_section(&cursor, &sec[10], GAL_SEC_ENDING, k_endings, 2, (uint32_t)sizeof(k_endings));
    add_section(&cursor, &sec[11], GAL_SEC_GALLERY, k_gallery, 1, (uint32_t)sizeof(k_gallery));
    add_section(&cursor, &sec[12], GAL_SEC_META, "t", 0, 1);
    add_section(&cursor, &sec[13], GAL_SEC_TITLE, k_gallery, 1, 4);   // title image id 0
    const uint32_t imgdata_off = (uint32_t)(cursor - g_blob);
    add_section(&cursor, &sec[14], GAL_SEC_IMGDATA, k_img, 0, (uint32_t)sizeof(k_img));

    // 图片记录:top/bot/mask 都指向 imgdata 起点
    memcpy(img + 4, &imgdata_off, 4);
    const uint32_t top_len = (uint32_t)sizeof(k_img);
    memcpy(img + 8, &top_len, 4);
    memcpy(img + 12, &imgdata_off, 4);
    memcpy(img + 16, &top_len, 4);
    memcpy(img + 20, &imgdata_off, 4);
    memcpy(img + 24, &top_len, 4);
    memcpy(img + 28, &imgdata_off, 4);
    memcpy(img + 32, &top_len, 4);
    memcpy(g_blob + sec[8].offset, img, sizeof(img));

    memcpy(g_blob, "GALPK001", 8);
    uint32_t version = 1, total = (uint32_t)(cursor - g_blob), count = nsec;
    memcpy(g_blob + 8, &version, 4);
    memcpy(g_blob + 12, &total, 4);
    memcpy(g_blob + 16, &count, 4);
    for (uint32_t i = 0; i < nsec; ++i) memcpy(g_blob + 20 + 16 * i, &sec[i], sizeof(sec_t));
    return total;
}

int main(void)
{
    const uint32_t size = build_pack();
    gal_pack_t pack;
    const char *err = NULL;
    CHECK(gal_pack_open(&pack, g_blob, size, &err), "open: %s", err ? err : "?");
    CHECK(pack.event_count == 14, "events %u", (unsigned)pack.event_count);
    CHECK(pack.line_count == 4, "lines %u", (unsigned)pack.line_count);

    // 1) 顺序:LINE first -> BG -> SPRITE -> VAR -> LINE second
    gal_play_t play;
    gal_play_reset(&play);
    gal_step_t st = gal_play_next(&play, &pack);
    uint16_t speaker = 0;
    CHECK(st.kind == GAL_STEP_LINE && st.index == 0, "first step kind=%d idx=%u", (int)st.kind,
          (unsigned)st.index);
    CHECK(strcmp(gal_play_line(&play, &pack, &speaker), "first") == 0, "first text");
    st = gal_play_next(&play, &pack);
    CHECK(st.kind == GAL_STEP_LINE && st.index == 1, "second line kind=%d", (int)st.kind);
    CHECK(play.bg == 0, "bg %u", (unsigned)play.bg);
    CHECK(play.spr[0] == 0, "sprite %u", (unsigned)play.spr[0]);
    CHECK(play.vars[1] == 1, "counter %d", play.vars[1]);

    // 2) 条件不成立(v1=1 < 2):不跳转,继续到 LINE back
    st = gal_play_next(&play, &pack);
    CHECK(st.kind == GAL_STEP_LINE && st.index == 2, "third line kind=%d idx=%u", (int)st.kind,
          (unsigned)st.index);

    // 3) 选项:条件选项(cond v1>=2)此时不满足,choose 必须被拒绝。
    st = gal_play_next(&play, &pack);
    CHECK(st.kind == GAL_STEP_CHOICE, "choice step kind=%d", (int)st.kind);
    CHECK(gal_cond_ok(&play, &pack, 0) == false, "cond should be false");
    CHECK(!gal_play_choose(&play, &pack, 1), "conditional option must be rejected");
    CHECK(gal_play_choose(&play, &pack, 0), "option 0");
    st = gal_play_next(&play, &pack);
    CHECK(st.kind == GAL_STEP_ENDING && st.index == 0, "bad ending kind=%d idx=%u", (int)st.kind,
          (unsigned)st.index);

    // 4) 计数器到 2 -> 条件跳转 -> 条件选项可选 -> 动作生效 -> 真结局
    gal_play_reset(&play);
    play.vars[1] = 1;
    play.pc = 3;                            // 从 VAR 开始
    st = gal_play_next(&play, &pack);       // VAR -> LINE second
    CHECK(st.kind == GAL_STEP_LINE && st.index == 1, "line second");
    st = gal_play_next(&play, &pack);       // GOTO 成立 -> CHOICE
    CHECK(st.kind == GAL_STEP_CHOICE, "goto choice kind=%d", (int)st.kind);
    CHECK(play.pc == 8, "pc after choice %u", (unsigned)play.pc);
    CHECK(gal_cond_ok(&play, &pack, 0) == true, "cond should hold");
    CHECK(gal_play_choose(&play, &pack, 1), "conditional option");
    CHECK(play.vars[0] == 5, "action var0 %d", play.vars[0]);
    st = gal_play_next(&play, &pack);
    CHECK(st.kind == GAL_STEP_LINE && st.index == 2, "reward line");
    st = gal_play_next(&play, &pack);
    CHECK(st.kind == GAL_STEP_ENDING && st.index == 1, "true ending kind=%d idx=%u", (int)st.kind,
          (unsigned)st.index);

    // 5) 回跳:target < pc 的 GOTO 正常执行。
    gal_play_reset(&play);
    play.pc = 12;
    st = gal_play_next(&play, &pack);
    CHECK(st.kind == GAL_STEP_LINE && st.index == 3, "true end line");
    st = gal_play_next(&play, &pack);
    CHECK(st.kind == GAL_STEP_LINE && st.index == 0, "back jump lands on first line");
    CHECK(play.pc == 1, "pc after back jump %u", (unsigned)play.pc);

    // 6) 解锁位图:Cg 34 位、章节 18、结局 4,都越过 32 位。
    gal_unlock_t unlock;
    gal_unlock_clear(&unlock);
    gal_unlock_cg_set(&unlock, 33);
    gal_unlock_chapter_set(&unlock, 17);
    gal_unlock_ending_set(&unlock, 3);
    CHECK(gal_unlock_cg_get(&unlock, 33), "cg 33");
    CHECK(!gal_unlock_cg_get(&unlock, 32), "cg 32");
    CHECK(gal_unlock_chapter_get(&unlock, 17), "chapter 17");
    CHECK(gal_unlock_ending_get(&unlock, 3), "ending 3");
    CHECK(gal_unlock_count(unlock.cg, GAL_UNLOCK_CG_BYTES) == 1, "cg count");

    // 7) 鉴赏映射:显示哪个 image 就解锁哪一条(不依赖"解锁事件下标")。
    CHECK(gal_gallery_index_for_image(&pack, 0) == 0, "gallery map 0");
    CHECK(gal_gallery_index_for_image(&pack, 3) == -1, "gallery map out of range");
    CHECK(gal_gallery_index_for_image(&pack, GAL_NONE) == -1, "gallery map none");

    // 8) 立绘槽位 -> x:左 6、右屏幕宽-w-6、中居中。
    CHECK(gal_sprite_x(GAL_POS_LEFT, 90, 320) == 6, "left x");
    CHECK(gal_sprite_x(GAL_POS_RIGHT, 90, 320) == 320 - 90 - 6, "right x");
    CHECK(gal_sprite_x(GAL_POS_CENTER, 90, 320) == (320 - 90) / 2, "center x");
    CHECK(gal_sprite_x(GAL_POS_CENTER, 400, 320) == 0, "oversize clamps");

    // 9) 在选项处存档、恢复后仍停在同一个选项(游标语义)。
    {
        gal_play_t saved = play;
        saved.pc = 7;                       // CHOICE 事件
        gal_play_t restored;
        memset(&restored, 0, sizeof(restored));
        restored.pc = saved.pc;
        gal_step_t again = gal_play_next(&restored, &pack);
        CHECK(again.kind == GAL_STEP_CHOICE && again.index == 0, "restore at choice kind=%d",
              (int)again.kind);
        CHECK(gal_play_choose(&restored, &pack, 0), "choose after restore");
        gal_step_t after = gal_play_next(&restored, &pack);
        CHECK(after.kind == GAL_STEP_ENDING, "after restore -> ending");
    }

    // 10) UTF-8 截断不切多字节字符。
    char out[16];
    gal_utf8_truncate(out, "一二三四五", 7);
    CHECK(strcmp(out, "一二") == 0, "truncate '%s'", out);
    gal_utf8_truncate(out, "abc", 10);
    CHECK(strcmp(out, "abc") == 0, "truncate ascii");

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("model ok\n");
    return 0;
}
