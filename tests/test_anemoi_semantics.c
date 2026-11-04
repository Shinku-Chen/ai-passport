// tests/test_anemoi_semantics.c —— 生产 C 与源语义的定点回归：
//   变量 schema（含 native-only / 动态键）、helper 返回值、背景时段变体、
//   第 31 章三选一委托、第一块委托板的落点。
#include "anemoi_pack.h"
#include "anemoi_script.h"

#include "anemoi_test_util.h"

#include <stdio.h>
#include <string.h>

static anem_script_t s_script;

static uint16_t var_id(const anem_pack_t *pack, const char *name)
{
    uint16_t id = 0xFFFF;
    if (!anem_var_id_by_name(pack, name, &id)) {
        fprintf(stderr, "missing variable %s\n", name);
        return 0xFFFF;
    }
    return id;
}

static uint16_t text_id_of(const anem_pack_t *pack, const char *text)
{
    for (uint32_t i = 0; i < anem_pack_text_count(pack); i++) {
        const char *cand = anem_pack_text(pack, (uint16_t)i);
        if (cand != NULL && strcmp(cand, text) == 0) {
            return (uint16_t)i;
        }
    }
    return 0xFFFF;
}

static int check_var_schema(const anem_pack_t *pack)
{
    const char *required[] = { "a:1", "a:2", "a:3", "a:4", "a:5", "$time_control",
                               "z:201", "z:450", "g:0", "g:30", "a:17" };
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
        ANEM_CHECK_MSG(var_id(pack, required[i]) != 0xFFFF, "VAR 缺少 %s", required[i]);
    }
    for (uint16_t i = 0; i < 512; i++) {
        uint16_t text = 0;
        if (!anem_pack_var(pack, i, &text)) {
            break;
        }
        ANEM_CHECK(anem_pack_text(pack, text) != NULL);
    }
    return 0;
}

static int check_helpers(const anem_pack_t *pack)
{
    ANEM_CHECK(anem_script_init(&s_script, pack));
    anem_script_new(&s_script, 31, false);
    anem_value_t recv = { 0, 0, ANEM_VAL_NUM, 0 };
    anem_value_t out;
    const int expected_rf[] = { 1, 0, 1 };  // $$reverse_flag = abs(flag - 1)
    for (int v = 0; v < 3; v++) {
        anem_value_t args[1] = { { .num = 0, .text = 0, .kind = ANEM_VAL_NUM, .len = 0 } };
        args[0].num = v;
        ANEM_CHECK(anem_script_native(&s_script, "$$reverse_flag", args, 1, &recv, &out));
        ANEM_CHECK_MSG(out.num == expected_rf[v], "reverse_flag(%d)=%d", v, (int)out.num);
    }
    {
        anem_value_t args[1] = { { 0, 0, ANEM_VAL_STR, 0 } };
        args[0].text = text_id_of(pack, "b_bg002");
        ANEM_CHECK(args[0].text != 0xFFFF);
        ANEM_CHECK(anem_script_native(&s_script, "$$chop_bg_blur_name", args, 1, &recv, &out));
        ANEM_CHECK(out.kind == ANEM_VAL_STR);
        ANEM_CHECK(strcmp(anem_script_text(&s_script, out.text), "bg002") == 0);
    }
    {
        const uint16_t base = var_id(pack, "$time_control");
        const uint16_t text = text_id_of(pack, "bg002");
        ANEM_CHECK(text != 0xFFFF);
        for (int t = 0; t < 3; t++) {
            ANEM_CHECK(anem_script_var_set_num(&s_script, base, t));
            anem_value_t args[1] = { { .num = 0, .text = text, .kind = ANEM_VAL_STR,
                                       .len = 0 } };
            ANEM_CHECK(anem_script_native(&s_script, "$$get_bg_time_control_name", args, 1,
                                          &recv, &out));
            char expected[32];
            snprintf(expected, sizeof(expected), "bg002_%02d", t + 1);
            const char *got = anem_script_text(&s_script, out.text);
            ANEM_CHECK_MSG(strcmp(got, expected) == 0, "t=%d got=%s want=%s", t, got, expected);
        }
    }
    {
        const uint16_t text = text_id_of(pack, "0701");
        ANEM_CHECK(text != 0xFFFF);
        anem_value_t args[1] = { { .num = 0, .text = text, .kind = ANEM_VAL_STR,
                                   .len = 0 } };
        ANEM_CHECK(anem_script_native(&s_script, "$$set_date", args, 1, &recv, &out));
        const uint16_t a1 = var_id(pack, "a:1");
        const uint16_t a2 = var_id(pack, "a:2");
        ANEM_CHECK_MSG(anem_script_var_get(&s_script, a1).num == 7, "a:1=%d",
                       (int)anem_script_var_get(&s_script, a1).num);
        ANEM_CHECK(anem_script_var_get(&s_script, a2).num == 1);
    }
    return 0;
}

static int check_bg_variant(const anem_pack_t *pack)
{
    // 源 resolveBackgroundName()：base 名不存在时用 base + 时段后缀。
    // 在 3..6 章里找出第一个解析成 "_03"（$time_control=2）的背景并核对图像。
    for (uint16_t scene = 3; scene <= 6; scene++) {
        ANEM_CHECK(anem_script_init(&s_script, pack));
        ANEM_CHECK(anem_script_start_chapter(&s_script, scene, 0));
        ANEM_CHECK(anem_script_var_set_num(&s_script, var_id(pack, "$time_control"), 2));
        for (int i = 0; i < 4000; i++) {
            anem_frame_t f = anem_script_read(&s_script);
            if (f.kind == ANEM_FRAME_LINE) {
                const char *name = anem_script_text(&s_script, s_script.st.regular_bg);
                if (strstr(name, "_03") == NULL) {
                    anem_script_next(&s_script);
                    continue;
                }
                anem_picture_t pic;
                anem_script_picture(&s_script, &pic);
                uint16_t tid = 0, image = 0;
                ANEM_CHECK(anem_pack_image_entry(pack, name, &tid, &image));
                ANEM_CHECK_MSG(pic.back_image == image, "back_image=%u want=%u name=%s",
                               pic.back_image, image, name);
                printf("  bg variant: scene %u -> %s\n", scene, name);
                return 0;
            }
            if (f.kind == ANEM_FRAME_CHOICE || f.kind == ANEM_FRAME_BOARD ||
                f.kind == ANEM_FRAME_END || f.kind == ANEM_FRAME_ERROR ||
                f.kind == ANEM_FRAME_BOUNDARY) {
                break;
            }
        }
    }
    fprintf(stderr, "no time-of-day background variant reached in scenes 3..6\n");
    return 1;
}

static int check_scene31(const anem_pack_t *pack)
{
    ANEM_CHECK(anem_script_init(&s_script, pack));
    ANEM_CHECK(anem_script_start_chapter(&s_script, 31, 10));
    const uint16_t a125 = var_id(pack, "a:125");
    const uint16_t a126 = var_id(pack, "a:126");
    const uint16_t a127 = var_id(pack, "a:127");
    const int expected_counts[] = { 3, 2, 1 };
    int round = 0;
    bool ended = false;
    for (int i = 0; i < 20000; i++) {
        anem_frame_t f = anem_script_read(&s_script);
        if (f.kind == ANEM_FRAME_LINE) {
            anem_script_next(&s_script);
            continue;
        }
        if (f.kind == ANEM_FRAME_CHOICE) {
            ANEM_CHECK_MSG(round < 3 && f.count == expected_counts[round],
                           "round=%d count=%d", round, f.count);
            const anem_choice_item_t *item = anem_script_choice_item(&s_script, 0);
            ANEM_CHECK(item != NULL && anem_script_text(&s_script, item->label)[0] != 0);
            ANEM_CHECK(anem_script_choose(&s_script, 0));
            round++;
            continue;
        }
        if (f.kind == ANEM_FRAME_END) {
            ended = true;
            break;
        }
        if (f.kind == ANEM_FRAME_BOUNDARY) {
            break;
        }
        if (f.kind == ANEM_FRAME_ERROR) {
            ANEM_CHECK_MSG(false, "scene31 error: %s", anem_script_notice(&s_script));
        }
    }
    ANEM_CHECK(round == 3 && ended);
    ANEM_CHECK(anem_script_var_get(&s_script, a125).num == 1);
    ANEM_CHECK(anem_script_var_get(&s_script, a126).num == 1);
    ANEM_CHECK(anem_script_var_get(&s_script, a127).num == 1);
    return 0;
}

static int check_first_board(const anem_pack_t *pack)
{
    ANEM_CHECK(anem_script_init(&s_script, pack));
    anem_script_new(&s_script, 2, true);
    for (int i = 0; i < 60000; i++) {
        anem_frame_t f = anem_script_read(&s_script);
        if (f.kind == ANEM_FRAME_LINE) {
            anem_script_next(&s_script);
            continue;
        }
        if (f.kind == ANEM_FRAME_CHOICE) {
            ANEM_CHECK(anem_script_choose(&s_script, 0));
            continue;
        }
        if (f.kind == ANEM_FRAME_BOARD) {
            ANEM_CHECK(anem_script_var_get(&s_script, var_id(pack, "a:1")).num == 7);
            ANEM_CHECK(anem_script_var_get(&s_script, var_id(pack, "a:2")).num == 12);
            ANEM_CHECK(anem_script_var_get(&s_script, var_id(pack, "a:3")).num == 1);
            ANEM_CHECK(f.count == 4);
            const char *expected[] = { "030_愛乃0001", "040_淡雪0001", "050_小詠0001",
                                       "060_六花0001" };
            for (int k = 0; k < f.count && k < 4; k++) {
                const anem_choice_item_t *item = anem_script_choice_item(&s_script, (uint8_t)k);
                ANEM_CHECK(item != NULL && item->board);
                ANEM_CHECK(strcmp(anem_pack_text(pack, s_script.board[item->value].scene),
                                  expected[k]) == 0);
            }
            ANEM_CHECK(anem_script_choose(&s_script, 0));
            ANEM_CHECK(s_script.st.chapter == 62);
            return 0;
        }
        if (f.kind == ANEM_FRAME_ERROR || f.kind == ANEM_FRAME_BOUNDARY ||
            f.kind == ANEM_FRAME_END) {
            break;
        }
    }
    fprintf(stderr, "first board not reached\n");
    return 1;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "main/anemoi_data/anemoi_pack.bin";
    size_t size = 0;
    unsigned char *blob = anem_test_read(path, &size);
    if (blob == NULL) {
        fprintf(stderr, "SKIP: 读不到资源包 %s\n", path);
        return 77;
    }
    anem_pack_t pack;
    ANEM_CHECK(anem_pack_open(&pack, blob, size));
    int rc = check_var_schema(&pack);
    if (rc == 0) rc = check_helpers(&pack);
    if (rc == 0) rc = check_bg_variant(&pack);
    if (rc == 0) rc = check_scene31(&pack);
    if (rc == 0) rc = check_first_board(&pack);
    free(blob);
    if (rc == 0) {
        printf("test_anemoi_semantics: PASS\n");
    }
    return rc;
}
