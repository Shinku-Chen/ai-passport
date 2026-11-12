
// tests/test_anemoi_app.c —— 生产状态机 + 生产视图模型：屏幕、行、坐标、按键语义。
#include "anemoi_app.h"
#include "anemoi_ui.h"

#include "anemoi_test_util.h"

#include <stdio.h>
#include <string.h>

static void test_workspace_boot(void) { (void)anem_store_workspace_init(); }

static unsigned char s_storage[48][4608];
static size_t s_length[48];
static char s_keys[48][32];
static int s_count;
static bool s_fail_write;

static bool fake_read(const char *key, void *out, size_t size, size_t *out_size)
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_keys[i], key) == 0 && s_length[i] == size) {
            memcpy(out, s_storage[i], size);
            if (out_size) {
                *out_size = size;
            }
            return true;
        }
    }
    return false;
}

static bool fake_write(const char *key, const void *data, size_t size)
{
    if (s_fail_write) {
        return false;
    }
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_keys[i], key) == 0) {
            memcpy(s_storage[i], data, size);
            s_length[i] = size;
            return true;
        }
    }
    if (s_count >= 48) {
        return false;
    }
    snprintf(s_keys[s_count], sizeof(s_keys[s_count]), "%s", key);
    memcpy(s_storage[s_count], data, size);
    s_length[s_count] = size;
    s_count++;
    return true;
}

static const anem_store_io_t s_io = { .read = fake_read, .write = fake_write };

static anem_app_t s_app;
static anem_view_t s_view;
static uint32_t s_now = 1000;

static void click(uint8_t key)
{
    anem_app_key(&s_app, key, ANEM_EV_PRESS, s_now);
    s_now += 40;
    anem_app_key(&s_app, key, ANEM_EV_RELEASE, s_now);
    s_now += 10;
    anem_app_key(&s_app, key, ANEM_EV_CLICK, s_now);
    s_now += 10;
}

static void long_press(uint8_t key)
{
    anem_app_key(&s_app, key, ANEM_EV_PRESS, s_now);
    s_now += 600;
    anem_app_key(&s_app, key, ANEM_EV_LONG, s_now);
    s_now += 20;
    anem_app_key(&s_app, key, ANEM_EV_RELEASE, s_now);
    s_now += 20;
}

static void tick(uint32_t ms)
{
    s_now += ms;
    anem_app_tick(&s_app, s_now);
}

static int start_game(const uint8_t *pack, size_t size)
{
    ANEM_CHECK(anem_app_init(&s_app, pack, size));
    s_app.draw_art = false;  // 逻辑回归不重复解码真实图像（图像由 test_anemoi_image 覆盖）
    anem_store_bind(&s_io);
    anem_app_refresh_store(&s_app, s_now);
    anem_app_view(&s_app, &s_view);
    ANEM_CHECK(s_view.screen == ANEM_SCR_TITLE);
    ANEM_CHECK(s_app.title_image_normal != ANEM_NONE16);
    ANEM_CHECK(s_app.title_image_anemoi != ANEM_NONE16);
    ANEM_CHECK(s_app.title_image_eternicle != ANEM_NONE16);
    ANEM_CHECK(s_app.title_image == s_app.title_image_normal);
    ANEM_CHECK(s_view.art_image == s_app.title_image);
    ANEM_CHECK(s_view.row_count == 6);
    ANEM_CHECK(strcmp(s_view.row[0].text, "开始游戏") == 0);
    ANEM_CHECK(s_view.row[0].selected);
    // 短按确定：开始游戏
    click(ANEM_KEY_OK);
    tick(300);  // 打字机先露出几个字
    anem_app_view(&s_app, &s_view);
    ANEM_CHECK(s_view.screen == ANEM_SCR_READ);
    ANEM_CHECK(s_view.show_art);
    ANEM_CHECK(s_view.line_count > 0);
    ANEM_CHECK(s_view.header[0] != 0);
    ANEM_CHECK(s_view.pages >= 1);
    return 0;
}

static int check_title_levels(void)
{
    s_app.screen = ANEM_SCR_TITLE;
    s_app.progress_ready = true;
    s_app.progress.level = 4;
    anem_app_refresh_store(&s_app, s_now);
    ANEM_CHECK(s_app.title_image == s_app.title_image_anemoi);
    s_app.progress.level = 2;
    anem_app_refresh_store(&s_app, s_now);
    ANEM_CHECK(s_app.title_image == s_app.title_image_eternicle);
    s_app.progress.level = 0;
    anem_app_refresh_store(&s_app, s_now);
    ANEM_CHECK(s_app.title_image == s_app.title_image_normal);
    s_app.screen = ANEM_SCR_READ;
    return 0;
}

static int check_key_semantics(void)
{
    // PRESS 不得推进（只有延迟 CLICK 生效）
    const uint32_t before = s_app.script.st.cursor;
    anem_app_key(&s_app, ANEM_KEY_UP, ANEM_EV_PRESS, s_now);
    ANEM_CHECK(s_app.script.st.cursor == before);
    // 长按 UP = 快进（PRESS -> LONG 开始），松开即停（RELEASE）
    anem_app_key(&s_app, ANEM_KEY_UP, ANEM_EV_PRESS, s_now);
    s_now += 600;
    anem_app_key(&s_app, ANEM_KEY_UP, ANEM_EV_LONG, s_now);
    ANEM_CHECK(s_app.ff_held);
    tick(600);
    ANEM_CHECK(s_app.script.st.cursor != before);
    s_now += 20;
    anem_app_key(&s_app, ANEM_KEY_UP, ANEM_EV_RELEASE, s_now);
    ANEM_CHECK(!s_app.ff_held);
    const uint32_t after_ff = s_app.script.st.cursor;
    tick(600);
    ANEM_CHECK(s_app.script.st.cursor == after_ff);  // 已松开：不再推进
    // 长按 OK 打开菜单；返回后正文不前进
    const uint32_t at_menu = s_app.script.st.cursor;
    long_press(ANEM_KEY_OK);
    anem_app_view(&s_app, &s_view);
    ANEM_CHECK(s_view.screen == ANEM_SCR_MENU);
    ANEM_CHECK(s_view.row_count == 6);
    ANEM_CHECK(strcmp(s_view.row[0].text, "返回游戏") == 0);
    click(ANEM_KEY_OK);  // 返回游戏
    anem_app_view(&s_app, &s_view);
    ANEM_CHECK(s_view.screen == ANEM_SCR_READ);
    ANEM_CHECK(s_app.script.st.cursor == at_menu);
    return 0;
}

static void advance_to_choice(void)
{
    for (int i = 0; i < 20000; i++) {
        anem_app_view(&s_app, &s_view);
        if (s_view.screen == ANEM_SCR_CHOICE) {
            return;
        }
        if (s_view.screen != ANEM_SCR_READ) {
            return;
        }
        anem_app_tick(&s_app, s_now);   // 打完字
        s_now += 4000;
        click(ANEM_KEY_UP);
    }
}

static int check_choice_and_menu(void)
{
    advance_to_choice();
    anem_app_view(&s_app, &s_view);
    ANEM_CHECK(s_view.screen == ANEM_SCR_CHOICE);
    ANEM_CHECK(s_view.row_count >= 2);
    ANEM_CHECK(s_view.row[0].selected);
    ANEM_CHECK(s_view.row[0].y < s_view.row[1].y);
    click(ANEM_KEY_DOWN);
    anem_app_view(&s_app, &s_view);
    ANEM_CHECK(s_view.row[1].selected);
    // 菜单往返：待选与光标保持
    long_press(ANEM_KEY_OK);
    anem_app_view(&s_app, &s_view);
    ANEM_CHECK(s_view.screen == ANEM_SCR_MENU);
    long_press(ANEM_KEY_OK);  // 长按返回
    anem_app_view(&s_app, &s_view);
    ANEM_CHECK(s_view.screen == ANEM_SCR_CHOICE);
    ANEM_CHECK(s_app.choice_sel == 1);
    // 显式确认
    const uint32_t cursor = s_app.script.st.cursor;
    click(ANEM_KEY_OK);
    anem_app_view(&s_app, &s_view);
    ANEM_CHECK(s_view.screen == ANEM_SCR_READ);
    ANEM_CHECK(s_app.script.st.cursor != cursor || s_app.script.choice_count > 0);
    return 0;
}

static int check_save_failure(void)
{
    // 保存失败：storage 不可用时提示错误，且不留下“已保存”的槽位
    long_press(ANEM_KEY_OK);
    click(ANEM_KEY_DOWN);  // 存档
    click(ANEM_KEY_OK);    // 进入存档页
    anem_app_view(&s_app, &s_view);
    ANEM_CHECK(s_view.screen == ANEM_SCR_SAVE);
    ANEM_CHECK(s_view.row_count == 8);
    click(ANEM_KEY_DOWN);  // 自动档 -> 存档 01
    s_fail_write = true;
    click(ANEM_KEY_OK);
    anem_app_view(&s_app, &s_view);
    ANEM_CHECK(s_app.notice_kind == 2);
    ANEM_CHECK(!s_app.slot_meta[1].valid);
    s_fail_write = false;
    // 再存一次（成功）：槽位出现，且能读回
    click(ANEM_KEY_OK);
    anem_app_view(&s_app, &s_view);
    ANEM_CHECK(s_app.slot_meta[1].valid);
    // 长按返回菜单 -> 返回游戏
    long_press(ANEM_KEY_OK);
    anem_app_view(&s_app, &s_view);
    ANEM_CHECK(s_view.screen == ANEM_SCR_MENU);
    click(ANEM_KEY_OK);
    anem_app_view(&s_app, &s_view);
    ANEM_CHECK(s_view.screen == ANEM_SCR_READ);
    return 0;
}

static int check_auto_stops_at_choice(void)
{
    // 自动播放：打开后碰到选择必须停
    advance_to_choice();
    anem_app_view(&s_app, &s_view);
    if (s_view.screen != ANEM_SCR_CHOICE) {
        return 0;
    }
    s_app.auto_enabled = true;
    for (int i = 0; i < 200; i++) {
        tick(500);
    }
    anem_app_view(&s_app, &s_view);
    ANEM_CHECK(s_view.screen == ANEM_SCR_CHOICE);
    return 0;
}

static int check_gallery(void)
{
    // 回到标题 -> 鉴赏 -> CG 列表
    s_app.screen = ANEM_SCR_TITLE;
    click(ANEM_KEY_DOWN);
    click(ANEM_KEY_DOWN);
    click(ANEM_KEY_DOWN);  // 鉴赏
    click(ANEM_KEY_OK);
    anem_app_view(&s_app, &s_view);
    ANEM_CHECK(s_view.screen == ANEM_SCR_GALLERY_MENU);
    click(ANEM_KEY_OK);
    anem_app_view(&s_app, &s_view);
    ANEM_CHECK(s_view.screen == ANEM_SCR_GALLERY);
    ANEM_CHECK(s_view.row_count > 0);
    ANEM_CHECK(s_view.row[0].text[0] != 0);
    click(ANEM_KEY_OK);
    anem_app_view(&s_app, &s_view);
    ANEM_CHECK(s_view.screen == ANEM_SCR_CG);
    long_press(ANEM_KEY_OK);
    anem_app_view(&s_app, &s_view);
    ANEM_CHECK(s_view.screen == ANEM_SCR_GALLERY);
    return 0;
}

int main(int argc, char **argv)
{
    test_workspace_boot();
    const char *path = argc > 1 ? argv[1] : "main/anemoi_data/anemoi_pack.bin";
    size_t size = 0;
    unsigned char *blob = anem_test_read(path, &size);
    if (blob == NULL) {
        fprintf(stderr, "SKIP: 读不到资源包 %s\n", path);
        return 77;
    }
    int rc = start_game(blob, size);
    if (rc == 0) rc = check_title_levels();
    if (rc == 0) rc = check_key_semantics();
    if (rc == 0) rc = check_choice_and_menu();
    if (rc == 0) rc = check_save_failure();
    if (rc == 0) rc = check_auto_stops_at_choice();
    if (rc == 0) rc = check_gallery();
    free(blob);
    if (rc == 0) {
        printf("test_anemoi_app: PASS\n");
    }
    return rc;
}
