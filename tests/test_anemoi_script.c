// tests/test_anemoi_script.c —— 生产解释器遍历真实包：路线、选择、存档、native 覆盖。
#include "anemoi_pack.h"
#include "anemoi_script.h"

#include "anemoi_test_util.h"

#include <stdio.h>
#include <string.h>

static anem_script_t s_script;

static int walk_common(const anem_pack_t *pack)
{
    ANEM_CHECK(anem_script_init(&s_script, pack));
    anem_script_new(&s_script, 2, true);
    int lines = 0, choices = 0, boards_completed = 0;
    bool board_seen = false;
    bool ended = false, boundary = false;
    struct {
        uint32_t cursor;
        uint32_t count;
    } choice_tries[16] = { { 0, 0 } };
    for (int i = 0; i < 3000000; i++) {
        anem_frame_t frame = anem_script_read(&s_script);
        if (frame.kind == ANEM_FRAME_LINE) {
            ANEM_CHECK(frame.text != ANEM_NONE16);
            ANEM_CHECK(anem_pack_text(pack, frame.text) != NULL);
            lines++;
            anem_script_next(&s_script);
        } else if (frame.kind == ANEM_FRAME_CHOICE) {
            choices++;
            ANEM_CHECK(frame.count > 0);
            const anem_choice_item_t *item = anem_script_choice_item(&s_script, 0);
            ANEM_CHECK(item != NULL && anem_script_text(&s_script, item->label)[0] != 0);
            // 委托任务会回到同一个选项重申“再来一次/结束”：同一 cursor 每来一次
            // 就换下一个可选项，直到流程离开这个循环（真实 UI 由玩家显式选择）。
            uint8_t pick = 0;
            for (int k = 0; k < 16; k++) {
                if (choice_tries[k].cursor == s_script.st.cursor) {
                    choice_tries[k].count++;
                    pick = (uint8_t)(choice_tries[k].count % frame.count);
                    break;
                }
                if (choice_tries[k].cursor == 0) {
                    choice_tries[k].cursor = s_script.st.cursor;
                    choice_tries[k].count = 0;
                    break;
                }
            }
            ANEM_CHECK(anem_script_choose(&s_script, pick));
        } else if (frame.kind == ANEM_FRAME_BOARD) {
            board_seen = true;
            ANEM_CHECK(frame.count > 0);
            const anem_choice_item_t *item = anem_script_choice_item(&s_script, 0);
            ANEM_CHECK(item != NULL && item->board);
            ANEM_CHECK(anem_script_text(&s_script, item->label)[0] != 0);
            ANEM_CHECK(item->detail == 0 ||
                       anem_script_text(&s_script, item->detail)[0] != 0);
            // 点第一项：进入介绍信（本卷收录）或落到分卷边界，两者都合法；
            // 这里只确认“不会静默失败”——失败时必须带边界原因。
            const bool jumped = anem_script_choose(&s_script, 0);
            boards_completed++;
            if (!jumped) {
                ANEM_CHECK(anem_script_error(&s_script) != NULL);
                boundary = true;
                break;
            }
            break;
        } else if (frame.kind == ANEM_FRAME_END) {
            ended = true;
            break;
        } else if (frame.kind == ANEM_FRAME_BOUNDARY) {
            boundary = true;
            break;
        } else {
            ANEM_CHECK_MSG(false, "解释器错误: %s (cursor=%u)", anem_script_notice(&s_script),
                           (unsigned)s_script.st.cursor);
        }
    }
    printf("  common walk: lines=%d choices=%d boards=%d ended=%d boundary=%d\n", lines,
           choices, boards_completed, ended ? 1 : 0, boundary ? 1 : 0);
    ANEM_CHECK(lines > 4000);
    ANEM_CHECK(choices >= 8);
    ANEM_CHECK(board_seen && boards_completed >= 1);
    ANEM_CHECK(boundary || ended || boards_completed >= 1);
    return 0;
}

static int check_chapters(const anem_pack_t *pack)
{
    static anem_script_t script;
    int started = 0;
    for (uint16_t scene = 3; scene <= 25; scene++) {
        ANEM_CHECK(anem_script_init(&script, pack));
        ANEM_CHECK_MSG(anem_script_start_chapter(&script, scene, 0), "scenario %u", scene);
        bool text_found = false;
        for (int i = 0; i < 5000; i++) {
            anem_frame_t frame = anem_script_read(&script);
            if (frame.kind == ANEM_FRAME_LINE) {
                text_found = true;
                break;
            }
            if (frame.kind == ANEM_FRAME_CHOICE || frame.kind == ANEM_FRAME_BOARD) {
                text_found = true;
                break;
            }
            if (frame.kind == ANEM_FRAME_END || frame.kind == ANEM_FRAME_BOUNDARY) {
                break;
            }
            if (frame.kind == ANEM_FRAME_ERROR) {
                break;
            }
        }
        ANEM_CHECK_MSG(text_found, "场景 %u 起始后没有正文", scene);
        started++;
    }
    printf("  chapters started=%d\n", started);
    return 0;
}

static int check_save_mid_choice(const anem_pack_t *pack)
{
    static anem_script_t script;
    ANEM_CHECK(anem_script_init(&script, pack));
    anem_script_new(&script, 2, true);
    for (int i = 0; i < 200000; i++) {
        anem_frame_t frame = anem_script_read(&script);
        if (frame.kind == ANEM_FRAME_LINE) {
            anem_script_next(&script);
            continue;
        }
        if (frame.kind == ANEM_FRAME_CHOICE) {
            break;
        }
        if (frame.kind != ANEM_FRAME_CHOICE) {
            if (frame.kind == ANEM_FRAME_BOARD || frame.kind == ANEM_FRAME_ERROR ||
                frame.kind == ANEM_FRAME_BOUNDARY || frame.kind == ANEM_FRAME_END) {
                return 0;  // 走到委托板之前没有选择，返回即可（walk 已覆盖）
            }
        }
    }
    ANEM_CHECK(script.choice_count > 0);
    const uint16_t cursor = script.st.cursor;
    const uint8_t count = script.choice_count;
    // 状态快照
    static anem_state_t snapshot;
    snapshot = script.st;
    ANEM_CHECK(anem_script_choose(&script, 1 % count));
    ANEM_CHECK(script.st.cursor != cursor || script.choice_count == 0);
    // 恢复
    script.st = snapshot;
    script.choice_count = 0;
    anem_frame_t again = anem_script_read(&script);
    ANEM_CHECK(again.kind == ANEM_FRAME_CHOICE);
    ANEM_CHECK(again.count == count);
    ANEM_CHECK(script.st.cursor == cursor);
    return 0;
}

static int check_natives(const anem_pack_t *pack)
{
    const char *value = NULL;
    uint32_t length = 0;
    ANEM_CHECK(anem_pack_meta(pack, "natives", &value, &length));
    char buffer[1024];
    ANEM_CHECK(length < sizeof(buffer));
    memcpy(buffer, value, length);
    buffer[length] = 0;
    static anem_script_t script;
    ANEM_CHECK(anem_script_init(&script, pack));
    anem_script_new(&script, 2, true);
    int checked = 0;
    char *save = NULL;
    for (char *name = strtok_r(buffer, ",", &save); name != NULL;
         name = strtok_r(NULL, ",", &save)) {
        anem_value_t args[3] = { { 0, 0, ANEM_VAL_NUM, 0 }, { 0, 0, ANEM_VAL_NUM, 0 },
                                 { 0, 0, ANEM_VAL_NUM, 0 } };
        anem_value_t receiver = { 0, 0, ANEM_VAL_NUM, 0 };
        anem_value_t out;
        script.failed = false;
        script.notice[0] = 0;
        (void)anem_script_native(&script, name, args, 0, &receiver, &out);
        const char *notice = anem_script_notice(&script);
        ANEM_CHECK_MSG(strstr(notice, "尚未支持") == NULL, "native 未实现: %s", name);
        checked++;
    }
    printf("  natives checked=%d\n", checked);
    return 0;
}

static int check_route_check(const anem_pack_t *pack)
{
    static anem_script_t script;
    ANEM_CHECK(anem_script_init(&script, pack));
    anem_script_new(&script, 2, true);
    // 没有累计次数：不满足任何路线条件
    ANEM_CHECK(!anem_script_route_check(&script));
    ANEM_CHECK(script.route_result == 0);
    // 找到本卷实际用到的路线累计变量（a:13 / a:14 之一），设到阈值以上。
    const char *candidates[] = { "a:13", "a:14", "a:12", "a:11" };
    uint16_t id = 0xFFFF;
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (anem_var_id_by_name(pack, candidates[i], &id)) {
            break;
        }
        id = 0xFFFF;
    }
    if (id != 0xFFFF) {
        ANEM_CHECK(anem_script_var_set_num(&script, id, 9));
        (void)anem_script_route_check(&script);
        // 命中任一分支时 route_result 必须置 1（具体 entry 由源表决定）
        ANEM_CHECK(script.route_result == 1);
        uint16_t label = 0;
        if (anem_var_id_by_name(pack, "$jump_label_no", &label)) {
            ANEM_CHECK(anem_script_var_get(&script, label).num >= 100);
        }
    }
    return 0;
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
    int rc = walk_common(&pack);
    if (rc == 0) rc = check_chapters(&pack);
    if (rc == 0) rc = check_save_mid_choice(&pack);
    if (rc == 0) rc = check_natives(&pack);
    if (rc == 0) rc = check_route_check(&pack);
    free(blob);
    if (rc == 0) {
        printf("test_anemoi_script: PASS\n");
    }
    return rc;
}
