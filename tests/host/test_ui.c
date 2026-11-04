// tests/host/test_ui.c —— 真实 gal_app_key/tick + 薄 LVGL 对象 stub 的 UI 回归。
//
// 覆盖 Lead 指定的阻断点：
//   1. UP 长按 → RELEASE 必须停止快进（tick 不再推进），长按后补发的 CLICK 被吞掉
//   2. 标题 7 项可滚动到「关于」，行不与该页副标题重叠
//   3. 「继续阅读」无自动存档时提示"无自动进度"，不再与读档入口重复
//   4. 各列表页的行对象确实被创建（菜单/存读档 8 槽/设置 3 项）
#define PAPER_UI_TEST_HOOKS 1
#include "bsp_button.h"
#include "esp_heap_caps.h"
#include "gal_app.h"
#include "lvgl.h"
#include "paper_gfx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const uint8_t *paper_host_img;
extern uint32_t paper_host_img_len;
extern const uint8_t *paper_host_scn;
extern uint32_t paper_host_scn_len;

const uint8_t *paper_host_img;
uint32_t paper_host_img_len;
const uint8_t *paper_host_scn;
uint32_t paper_host_scn_len;

static int g_fail;
static int g_checks;
#define CHECK(cond, ...)                                                                 \
    do {                                                                                 \
        g_checks++;                                                                      \
        if (!(cond)) {                                                                    \
            g_fail++;                                                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                   \
            printf(__VA_ARGS__);                                                          \
            printf("\n");                                                                 \
        }                                                                                \
    } while (0)

static uint8_t *read_file(const char *path, uint32_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    *len = (uint32_t)n;
    return buf;
}

static gal_app_t s_app;

static void goto_reader(void);
static void goto_title_for_test(void);

static void press(int btn, int ev)
{
    gal_key_t k = {(uint8_t)btn, (uint8_t)ev};
    gal_app_key(&s_app, &k);
}

static void click(int btn)
{
    // 真实驱动（iot_button.c）：短按是 PRESS → RELEASE → （消抖后）CLICK
    press(btn, BSP_BTN_PRESS);
    press(btn, BSP_BTN_RELEASE);
    press(btn, BSP_BTN_CLICK);
}

static void long_press(int btn)
{
    // 长按：PRESS → LONG → RELEASE，驱动不会补发 CLICK
    press(btn, BSP_BTN_PRESS);
    press(btn, BSP_BTN_LONG);
    press(btn, BSP_BTN_RELEASE);
}

static const lv_obj_t *row_with_text(const char *text)
{
    return lv_stub_find_text(text);
}

static int contains_any(const char *needle)
{
    int n = lv_stub_count();
    for (int i = 0; i < n; i++) {
        const lv_obj_t *o = lv_stub_at(i);
        if (o && strstr(o->text, needle) != NULL) {
            return 1;
        }
    }
    return 0;
}

static void test_title_scroll_and_layout(void)
{
    // 回到标题：连续长按 OK 直到标题页
    for (int i = 0; i < 6 && s_app.page != GAL_PAGE_TITLE; i++) {
        long_press(BSP_BTN_OK);
    }
    CHECK(s_app.page == GAL_PAGE_TITLE, "应回到标题页（当前 %d）", (int)s_app.page);
    // 回标题必须重画标题背景（否则画布留着上一帧剧情图）：id 固定为图书馆背景 472。
    CHECK(gal_app_test_title_image == 472, "标题背景图 id=%u（应为 472）",
          (unsigned)gal_app_test_title_image);

    const lv_obj_t *title = row_with_text("纸上的魔法使");
    CHECK(title != NULL, "标题文字存在");
    const lv_obj_t *sub = row_with_text("紙の上の魔法使い");
    CHECK(sub != NULL, "副标题存在");
    if (title && sub) {
        CHECK(sub->y >= title->y + 16, "副标题不与主标题重叠 (title.y=%d sub.y=%d)", title->y,
              sub->y);
    }

    // 下移到「关于」（第 7 项），必须可见
    for (int i = 0; i < 6; i++) {
        click(BSP_BTN_DOWN);
    }
    CHECK(s_app.sel == 6, "光标应到第 7 项（当前 %d）", s_app.sel);
    const lv_obj_t *about = row_with_text("关于");
    CHECK(about != NULL, "滚动后「关于」必须被创建");
    if (about && sub && about->parent) {
        int ry = about->parent->y;
        int rh = about->parent->h;
        CHECK(ry >= sub->y + 16, "行不与副标题重叠 (row.y=%d sub.y=%d)", ry, sub->y);
        CHECK(ry + rh <= 240, "行必须在屏幕内 (y=%d h=%d)", ry, rh);
    }
    // 回到第一项
    for (int i = 0; i < 6; i++) {
        click(BSP_BTN_UP);
    }
    CHECK(s_app.sel == 0, "光标回到第一项");
}

static void test_continue_without_autosave(void)
{
    // 标题第 2 项 = 继续阅读；没有自动存档时应给出"无自动进度"
    click(BSP_BTN_DOWN);
    CHECK(s_app.sel == 1, "光标在继续阅读");
    click(BSP_BTN_OK);
    CHECK(s_app.page == GAL_PAGE_TITLE, "无进度时留在标题页");
    CHECK(strcmp(s_app.notice, "无自动进度") == 0, "提示无自动进度（实际=%.20s）", s_app.notice);
    click(BSP_BTN_UP);
}

static void test_reader_fast_forward_release(void)
{
    click(BSP_BTN_OK);   // 开始游戏
    CHECK(s_app.page == GAL_PAGE_READER, "进入正文页（当前 %d）", (int)s_app.page);
    // 阅读页四硬指标：名字在框外（<BOX_Y）、页码独占框内右上、正文末行不出框。
    // 面板 152..240；名字在框外、页码在框外右上、正文贴面板顶 +4。
    CHECK(s_app.lbl_name->y + 21 <= 152 && s_app.lbl_page != NULL &&
              s_app.lbl_page->y + 18 <= 152 && s_app.lbl_text->y == 156,
          "name above box / page outside box / text at panel_top+4");
    CHECK(s_app.lbl_text->y + 4 * 19 <= 238, "last text line must stay above 238");
    s_app.speed = GAL_SPEED_INSTANT;

    // 走到一句已显示完的对白
    for (int i = 0; i < 40 && (s_app.typing || s_app.page != GAL_PAGE_READER); i++) {
        gal_app_tick(&s_app, 40);
    }

    // 长按 UP 快进：当前页必须瞬显（不逐字）
    s_app.typing = true;                 // 人为制造打字中
    s_app.typed = 0;
    press(BSP_BTN_UP, BSP_BTN_LONG);
    CHECK(s_app.fast_forward, "长按后进入快进");
    CHECK(!s_app.typing, "快进时不能还在逐字");
    CHECK(s_app.typed == gal_app_test_page_len(&s_app), "typed == 页长（%d vs %d）",
          (int)s_app.typed, gal_app_test_page_len(&s_app));
    gal_app_tick(&s_app, 1200);
    CHECK(s_app.fast_forward, "快进仍在进行");

    // 松手：必须停止快进
    press(BSP_BTN_UP, BSP_BTN_RELEASE);
    CHECK(!s_app.fast_forward, "RELEASE 必须停止快进");
    uint16_t sc_at = s_app.game.scene_id;
    uint32_t cur_at = s_app.game.cursor;
    char txt_at[64];
    snprintf(txt_at, sizeof(txt_at), "%s", s_app.text);
    gal_app_tick(&s_app, 3000);
    CHECK(s_app.game.scene_id == sc_at && s_app.game.cursor == cur_at &&
              strcmp(s_app.text, txt_at) == 0,
          "松手后 tick 不得推进（scene %u->%u cursor %u->%u）", sc_at, s_app.game.scene_id,
          (unsigned)cur_at, (unsigned)s_app.game.cursor);

    // 长按后驱动不会补发 CLICK；若误发也必须被吞掉（suppress_click 在 LONG 置位）
    press(BSP_BTN_UP, BSP_BTN_LONG);
    uint16_t s0 = s_app.game.scene_id;
    uint32_t c0 = s_app.game.cursor;
    press(BSP_BTN_UP, BSP_BTN_CLICK);   // 非驱动行为：验证防御仍然生效
    CHECK(s_app.game.scene_id == s0 && s_app.game.cursor == c0,
          "长按后误发的 CLICK 不得推进（scene %u/%u cursor %u/%u）", s0, s_app.game.scene_id,
          (unsigned)c0, (unsigned)s_app.game.cursor);
    press(BSP_BTN_UP, BSP_BTN_RELEASE);
}

// 自动存档：手动槽 0 不被覆盖；自动槽 8 记录更后面的位置；标题「继续阅读」确实接上
static void goto_reader(void)
{
    for (int i = 0; i < 6 && s_app.page != GAL_PAGE_READER; i++) {
        if (s_app.page == GAL_PAGE_MENU) {
            click(BSP_BTN_UP);          // 光标回第 1 项「继续阅读」
            for (int k = 0; k < 8; k++) {
                click(BSP_BTN_UP);
            }
            click(BSP_BTN_OK);
        } else if (s_app.page == GAL_PAGE_TITLE) {
            for (int k = 0; k < 8; k++) {
                click(BSP_BTN_UP);
            }
            click(BSP_BTN_OK);          // 开始游戏
        } else if (s_app.page == GAL_PAGE_CHOICE) {
            click(BSP_BTN_OK);          // 选项页短按 = 确认第一项，进入正文
        } else {
            long_press(BSP_BTN_OK);     // 子页面长按回来源
        }
    }
}

static void test_autosave_paths(void)
{
    goto_reader();
    CHECK(s_app.page == GAL_PAGE_READER, "回到阅读页（当前 %d）", (int)s_app.page);

    // 手动保存到槽 0（菜单第 2 项 → 保存页第 1 槽）；短 OK 现在是“下一句”，进菜单用长 OK
    long_press(BSP_BTN_OK);
    CHECK(s_app.page == GAL_PAGE_MENU, "打开菜单");
    click(BSP_BTN_DOWN);
    click(BSP_BTN_OK);
    CHECK(s_app.page == GAL_PAGE_SAVE, "进入保存页");
    click(BSP_BTN_OK);
    CHECK(paper_game_slot_exists(0), "手动槽 0 已保存");

    static paper_game_t chk;
    CHECK(paper_game_init(&chk), "chk init");
    CHECK(paper_game_load(&chk, 0), "读回手动槽 0");
    uint16_t manual_scene = chk.scene_id;
    uint32_t manual_cursor = chk.cursor;

    long_press(BSP_BTN_OK);             // 保存页 → 菜单
    click(BSP_BTN_UP);                  // 光标回第 1 项
    for (int k = 0; k < 8; k++) {
        click(BSP_BTN_UP);
    }
    click(BSP_BTN_OK);                  // 继续阅读
    CHECK(s_app.page == GAL_PAGE_READER, "回到阅读页");

    // 推进若干句后落自动档
    for (int i = 0; i < 6; i++) {
        gal_app_tick(&s_app, 200);
        click(BSP_BTN_UP);
    }
    gal_app_flush(&s_app);
    CHECK(paper_game_slot_exists(PAPER_SLOT_AUTO), "自动槽 8 已写入");

    CHECK(paper_game_load(&chk, 0), "再次读回手动槽 0");
    CHECK(chk.scene_id == manual_scene && chk.cursor == manual_cursor,
          "自动存档不得覆盖手动槽 0（scene %u/%u cursor %u/%u）", manual_scene, chk.scene_id,
          (unsigned)manual_cursor, (unsigned)chk.cursor);

    CHECK(paper_game_load(&chk, PAPER_SLOT_AUTO), "读回自动槽 8");
    uint32_t auto_cursor = chk.cursor;
    uint16_t auto_scene = chk.scene_id;
    CHECK(auto_scene != manual_scene || auto_cursor != manual_cursor,
          "自动槽应记录更后面的位置");

    // 回标题：「菜单 → 第 8 项 返回标题」
    long_press(BSP_BTN_OK);
    CHECK(s_app.page == GAL_PAGE_MENU, "打开菜单");
    for (int k = 0; k < 9; k++) {
        click(BSP_BTN_DOWN);
    }
    click(BSP_BTN_OK);
    CHECK(s_app.page == GAL_PAGE_TITLE, "回到标题页（当前 %d）", (int)s_app.page);

    // 继续阅读 → 必须落在自动存档那句
    click(BSP_BTN_DOWN);
    click(BSP_BTN_OK);
    CHECK(s_app.page == GAL_PAGE_READER, "继续阅读进入正文");
    CHECK(s_app.game.scene_id == auto_scene,
          "继续阅读场景应为自动档场景（%u/%u）", auto_scene, s_app.game.scene_id);
    CHECK(s_app.game.text_op >= 0 && (uint32_t)s_app.game.text_op == auto_cursor,
          "继续阅读应回到自动档那一句（text_op %d / cursor %u）", s_app.game.text_op,
          (unsigned)auto_cursor);
}

static int utf8_valid(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        int n;
        if (*p < 0x80) {
            n = 1;
        } else if ((*p >> 5) == 6) {
            n = 2;
        } else if ((*p >> 4) == 14) {
            n = 3;
        } else if ((*p >> 3) == 30) {
            n = 4;
        } else {
            return 0;
        }
        for (int i = 1; i < n; i++) {
            if ((p[i] & 0xC0) != 0x80) {
                return 0;
            }
        }
        p += n;
    }
    return 1;
}

// 打字机：每一帧的标签必须是合法 UTF-8，且是整句的前缀；打完必须等于整句
static void goto_title_for_test(void)
{
    gal_app_test_goto_title(&s_app);
}

static void test_typewriter_utf8(void)
{
    goto_reader();
    s_app.speed = GAL_SPEED_SLOW;
    // 重新载入当前句，让打字从头开始
    gal_app_test_reload_line(&s_app);
    const char *full = s_app.text;
    int bad_utf8 = 0, not_prefix = 0, frames = 0, finished = 0;
    for (int i = 0; i < 400; i++) {
        gal_app_tick(&s_app, 10);
        if (!s_app.lbl_text) {
            break;
        }
        const char *shown = s_app.lbl_text->text;
        frames++;
        if (!utf8_valid(shown)) {
            bad_utf8++;
        }
        size_t n = strlen(shown);
        if (strncmp(shown, full, n) != 0) {
            not_prefix++;
        }
        if (!s_app.typing) {
            finished = strcmp(shown, full) == 0;
            break;
        }
    }
    printf("  打字机: %d 帧，非法 UTF-8=%d，非前缀=%d，最终全等=%d\n", frames, bad_utf8,
           not_prefix, finished);
    CHECK(bad_utf8 == 0, "打字过程中不得出现非法 UTF-8");
    CHECK(not_prefix == 0, "显示内容必须始终是整句前缀");
    CHECK(finished == 1, "打字结束必须等于整句");
}

// 返回矩阵：每个子页面都要回到真实来源
static void test_return_matrix(void)
{
    // 标题 → 各子页 → 回标题
    const int title_items[5] = {2, 3, 4, 5, 6};   // 读取存档/CG鉴赏/章节/设置/关于
    const char *names[5] = {"读档", "鉴赏", "章节", "设置", "关于"};
    for (int k = 0; k < 5; k++) {
        for (int i = 0; i < 8 && s_app.page != GAL_PAGE_TITLE; i++) {
            goto_title_for_test();
        }
        CHECK(s_app.page == GAL_PAGE_TITLE, "回标题");
        for (int i = 0; i < 8; i++) {
            click(BSP_BTN_UP);
        }
        for (int i = 0; i < title_items[k]; i++) {
            click(BSP_BTN_DOWN);
        }
        click(BSP_BTN_OK);
        CHECK(s_app.page != GAL_PAGE_TITLE, "进入%s", names[k]);
        long_press(BSP_BTN_OK);
        CHECK(s_app.page == GAL_PAGE_TITLE, "%s 长按应回标题（实际 %d）", names[k],
              (int)s_app.page);
    }

    // 正文 → 菜单 → 各子页 → 回菜单（短 OK 已是“下一句”，进菜单用长 OK）
    goto_reader();
    long_press(BSP_BTN_OK);
    CHECK(s_app.page == GAL_PAGE_MENU, "菜单");
    const int menu_items[6] = {1, 2, 3, 4, 5, 6};
    const char *mnames[6] = {"保存", "读档", "历史", "设置", "鉴赏", "章节"};
    for (int k = 0; k < 6; k++) {
        for (int i = 0; i < 8; i++) {
            click(BSP_BTN_UP);
        }
        for (int i = 0; i < menu_items[k]; i++) {
            click(BSP_BTN_DOWN);
        }
        click(BSP_BTN_OK);
        CHECK(s_app.page != GAL_PAGE_MENU, "进入%s", mnames[k]);
        long_press(BSP_BTN_OK);
        CHECK(s_app.page == GAL_PAGE_MENU, "%s 长按应回菜单（实际 %d）", mnames[k],
              (int)s_app.page);
    }
    long_press(BSP_BTN_OK);   // 菜单 → 阅读页

    // 正文 DOWN → 历史 → 长按回正文
    goto_reader();
    click(BSP_BTN_DOWN);
    CHECK(s_app.page == GAL_PAGE_HISTORY, "DOWN 进历史（实际 %d）", (int)s_app.page);
    long_press(BSP_BTN_OK);
    CHECK(s_app.page == GAL_PAGE_READER, "历史长按应回正文（实际 %d）", (int)s_app.page);
}

// 坏图语义：绘制失败必须返回 false、不解锁该 CG 组
static void test_draw_failure_semantics(void)
{
    // 找一个属于 CG 组的 JPEG
    int target = -1, group = -1;
    for (uint32_t i = 0; i < s_app.pack->entry_count; i++) {
        if (s_app.pack->entries[i].kind != PAPER_IMG_JPEG) {
            continue;
        }
        int g = paper_image_cg_group(s_app.pack, (uint16_t)i);
        if (g >= 0) {
            target = (int)i;
            group = g;
            break;
        }
    }
    CHECK(target >= 0, "找到一张属于 CG 组的背景图");
    if (target < 0) {
        return;
    }
    if (paper_game_cg_unlocked(&s_app.game, group)) {
        return;   // 已解锁就不再作为失败用例
    }
    // 在内存里把该 JPEG 数据改坏（只改副本，不动磁盘包）
    uint8_t *img = (uint8_t *)paper_host_img;
    const paper_img_entry_t *e = &s_app.pack->entries[target];
    // 破坏 JPEG 的 SOI(0xFFD8)，保证解码器必然失败（改中间字节不一定会挂）
    uint8_t s0 = img[e->data_off], s1 = img[e->data_off + 1];
    img[e->data_off] = 0x00;
    img[e->data_off + 1] = 0x00;
    bool ok = gal_app_test_draw_frame(&s_app, (uint16_t)target);

    img[e->data_off] = s0;
    img[e->data_off + 1] = s1;
    CHECK(!ok, "坏 JPEG 必须让绘制失败");
    CHECK(!paper_game_cg_unlocked(&s_app.game, group), "绘制失败不得解锁 CG 组");
}

// 选项页：长按 OK 开菜单、保存/读档、返回仍是同一选项
static void test_choice_menu_roundtrip(void)
{
    CHECK(gal_app_test_goto_choice(&s_app), "走到第一处选项");
    int choice_idx = s_app.choice_index;
    int sel = s_app.sel;
    long_press(BSP_BTN_OK);
    CHECK(s_app.page == GAL_PAGE_MENU, "选项页长按 OK 应开菜单（实际 %d）", (int)s_app.page);
    click(BSP_BTN_OK);   // 继续阅读
    CHECK(s_app.page == GAL_PAGE_CHOICE, "菜单返回必须回到选项页（实际 %d）", (int)s_app.page);
    CHECK(s_app.choice_index == choice_idx && s_app.sel == sel, "选项/光标应保持");

    // 在选项处保存（菜单 → 保存进度 → 槽 1 → 长按回菜单 → 继续回选项）
    long_press(BSP_BTN_OK);
    CHECK(s_app.page == GAL_PAGE_MENU, "选项页再开菜单");
    for (int i = 0; i < 8; i++) {
        click(BSP_BTN_UP);
    }
    click(BSP_BTN_DOWN);
    click(BSP_BTN_OK);
    CHECK(s_app.page == GAL_PAGE_SAVE, "选项处可进入保存页");
    click(BSP_BTN_OK);
    CHECK(paper_game_slot_exists(0), "选项处保存成功");
    long_press(BSP_BTN_OK);
    CHECK(s_app.page == GAL_PAGE_MENU, "保存页长按回菜单（实际 %d）", (int)s_app.page);
    for (int i = 0; i < 8; i++) {
        click(BSP_BTN_UP);   // 光标回到「继续阅读」
    }
    click(BSP_BTN_OK);   // 菜单「继续阅读」= 回来源
    CHECK(s_app.page == GAL_PAGE_CHOICE, "保存后返回仍是选项页（实际 %d）", (int)s_app.page);
    CHECK(s_app.choice_index == choice_idx, "返回后仍是同一个选项");

    // 在选项处读档：离开到标题再读档，应回到同一选项
    long_press(BSP_BTN_OK);
    for (int i = 0; i < 8; i++) {
        click(BSP_BTN_UP);
    }
    click(BSP_BTN_DOWN);
    click(BSP_BTN_DOWN);
    click(BSP_BTN_OK);
    CHECK(s_app.page == GAL_PAGE_LOAD, "进入读档页（实际 %d）", (int)s_app.page);
    click(BSP_BTN_OK);
    CHECK(s_app.page == GAL_PAGE_CHOICE, "读档后回到选项页（实际 %d）", (int)s_app.page);
    CHECK(s_app.choice_index == choice_idx, "读档后是同一个选项");
}

// 立绘/补丁失败语义：坏 BLOCK、坏 PATCH、分配失败都不得解锁、要有可见错误
static void test_sprite_failure_semantics(void)
{
    // 找一张 BLOCK 与一张 PATCH
    int blk = -1, pat = -1;
    for (uint32_t i = 0; i < s_app.pack->entry_count; i++) {
        if (s_app.pack->entries[i].kind == PAPER_IMG_BLOCK && blk < 0) {
            blk = (int)i;
        }
        if (s_app.pack->entries[i].kind == PAPER_IMG_PATCH && pat < 0) {
            pat = (int)i;
        }
    }
    CHECK(blk >= 0 && pat >= 0, "包内应有 BLOCK 与 PATCH");
    uint8_t *img = (uint8_t *)paper_host_img;

    // 坏 BLOCK：破坏调色板前的块长度表（把第一块长度改 0）
    const paper_img_entry_t *e = &s_app.pack->entries[blk];
    uint32_t lens_off = e->data_off + 16 + 4 * 0;   // 占位：实际偏移见下
    (void)lens_off;
    // 直接把整块载荷清零（不可解压）
    uint8_t saved_hdr[16];
    memcpy(saved_hdr, img + e->data_off, 16);
    memset(img + e->data_off, 0, 16);
    bool ok_blk = gal_app_test_draw_sprites(&s_app, (uint16_t)blk, 0xFFFF, 0xFFFF);
    memcpy(img + e->data_off, saved_hdr, 16);
    CHECK(!ok_blk, "坏 BLOCK 必须让整屏绘制失败");
    CHECK(s_app.draw_error, "失败必须立即可见（draw_error）");

    // 坏 PATCH：把补丁的 mask_len 改大，触发长度校验失败
    const paper_img_entry_t *p = &s_app.pack->entries[pat];
    const uint8_t *pay = paper_image_payload(s_app.pack, p);
    size_t pay_off = (size_t)(pay - img);
    uint8_t m0 = img[pay_off], m1 = img[pay_off + 1], m2 = img[pay_off + 2], m3 = img[pay_off + 3];
    img[pay_off] = 0xFF;
    img[pay_off + 1] = 0xFF;
    img[pay_off + 2] = 0xFF;
    img[pay_off + 3] = 0x7F;
    bool ok_pat = gal_app_test_draw_sprites(&s_app, (uint16_t)pat, 0xFFFF, 0xFFFF);
    img[pay_off] = m0;
    img[pay_off + 1] = m1;
    img[pay_off + 2] = m2;
    img[pay_off + 3] = m3;
    CHECK(!ok_pat, "坏 PATCH 必须让整屏绘制失败");

    // 补丁缓冲分配失败：先释放缓存，再限制单次分配
    paper_gfx_test_release_patch();
    host_heap_set_limit(1024);
    bool ok_alloc = gal_app_test_draw_sprites(&s_app, (uint16_t)pat, 0xFFFF, 0xFFFF);
    host_heap_set_limit(0);
    paper_gfx_test_release_patch();
    CHECK(!ok_alloc, "补丁缓冲分配失败必须报失败（不得静默缺立绘）");

    // 恢复后应重新成功
    CHECK(gal_app_test_draw_sprites(&s_app, (uint16_t)blk, (uint16_t)pat, 0xFFFF),
          "恢复数据后应能成功绘制");
}

// 全部 4 处选项 × 2 分支：必须能用真实按键确认并继续
static void test_all_choice_branches(void)
{
    char text_seen[4][2][64];
    memset(text_seen, 0, sizeof(text_seen));
    for (int n = 0; n < 4; n++) {
        for (int pick = 0; pick < 2; pick++) {
            CHECK(gal_app_test_goto_choice_nth(&s_app, n), "走到第 %d 处选项", n);
            CHECK(s_app.page == GAL_PAGE_CHOICE, "处于选项页");
            int ci = s_app.choice_index;
            const paper_choice_item_t *it = paper_choice_item(s_app.pack, ci, pick);
            CHECK(it != NULL, "选项项存在");
            if (!it) {
                continue;
            }
            if (pick == 1) {
                click(BSP_BTN_DOWN);
            }
            // 短按 OK 必须确认（而不是开菜单）
            click(BSP_BTN_OK);
            CHECK(s_app.page != GAL_PAGE_MENU, "短 OK 不得开菜单（第 %d 处）", n);
            CHECK(s_app.page == GAL_PAGE_READER, "确认后回到正文（第 %d 处，实际 %d）", n,
                  (int)s_app.page);
            CHECK(s_app.game.choice_op == -1, "选项必须被消费（choice_op 清空）");
            snprintf(text_seen[n][pick], sizeof(text_seen[n][pick]), "%s", s_app.text);
        }
        // 两个分支的后续台词应不同（源脚本里目标不同）
        if (text_seen[n][0][0] && text_seen[n][1][0]) {
            CHECK(strcmp(text_seen[n][0], text_seen[n][1]) != 0,
                  "第 %d 处两个分支的后续台词应不同", n);
        }
    }
}

// 非零选项光标：选第二项 → 长 OK 菜单 → 保存/读档/设置往返 → 继续，光标仍在第二项
static void test_choice_cursor_survives_menu(void)
{
    CHECK(gal_app_test_goto_choice_nth(&s_app, 0), "走到第 1 处选项");
    if (s_app.choice_count < 2) {
        return;
    }
    click(BSP_BTN_DOWN);   // 选第二项
    CHECK(s_app.choice_sel == 1, "光标在第二项（实际 %d）", s_app.choice_sel);

    long_press(BSP_BTN_OK);
    CHECK(s_app.page == GAL_PAGE_MENU, "长 OK 开菜单");
    long_press(BSP_BTN_OK);   // 长 OK 返回
    CHECK(s_app.page == GAL_PAGE_CHOICE, "菜单长 OK 回到选项页（实际 %d）", (int)s_app.page);
    CHECK(s_app.choice_sel == 1, "返回后光标仍在第二项（实际 %d）", s_app.choice_sel);

    // 菜单 → 保存页 → 回菜单 → 继续
    long_press(BSP_BTN_OK);
    click(BSP_BTN_DOWN);      // 保存进度
    click(BSP_BTN_OK);
    CHECK(s_app.page == GAL_PAGE_SAVE, "进入保存页");
    long_press(BSP_BTN_OK);
    for (int i = 0; i < 8; i++) {
        click(BSP_BTN_UP);
    }
    click(BSP_BTN_OK);        // 继续阅读
    CHECK(s_app.page == GAL_PAGE_CHOICE, "保存页往返后回到选项页");
    CHECK(s_app.choice_sel == 1, "往返后光标仍在第二项（实际 %d）", s_app.choice_sel);

    // 设置页往返
    long_press(BSP_BTN_OK);
    for (int i = 0; i < 8; i++) {
        click(BSP_BTN_UP);
    }
    for (int i = 0; i < 4; i++) {
        click(BSP_BTN_DOWN);   // 设置
    }
    click(BSP_BTN_OK);
    CHECK(s_app.page == GAL_PAGE_SETTINGS, "进入设置页");
    long_press(BSP_BTN_OK);
    CHECK(s_app.page == GAL_PAGE_MENU, "设置页回菜单");
    for (int i = 0; i < 8; i++) {
        click(BSP_BTN_UP);
    }
    click(BSP_BTN_OK);
    CHECK(s_app.page == GAL_PAGE_CHOICE && s_app.choice_sel == 1, "设置往返后仍是第二项");

    // 最终必须能确认
    click(BSP_BTN_OK);
    CHECK(s_app.page == GAL_PAGE_READER && s_app.game.choice_op == -1, "第二项最终可确认");
}

static void test_menu_lists(void)
{
    goto_reader();
    // 打开菜单（长 OK）
    long_press(BSP_BTN_OK);
    CHECK(s_app.page == GAL_PAGE_MENU, "OK 打开菜单（当前 %d）", (int)s_app.page);
    CHECK(row_with_text("保存进度") != NULL, "菜单项已创建");
    // 滚到菜单末项（返回标题）后再断言可见
    for (int i = 0; i < 7; i++) {
        click(BSP_BTN_DOWN);
    }
    CHECK(row_with_text("返回标题") != NULL, "菜单末项滚动后可见");
    long_press(BSP_BTN_OK);   // 长按返回阅读页
    long_press(BSP_BTN_OK);            // 重新打开菜单（光标重置为 0）
    CHECK(row_with_text("保存进度") != NULL, "菜单重新打开");

    // 保存页：8 个槽位
    click(BSP_BTN_DOWN);   // 保存进度
    click(BSP_BTN_OK);
    CHECK(s_app.page == GAL_PAGE_SAVE, "进入保存页");
    CHECK(contains_any("存档 1"), "存档槽 1 行已创建");
    // 下移到最后一个槽（第 8 个）
    for (int i = 0; i < 7; i++) {
        click(BSP_BTN_DOWN);
    }
    CHECK(s_app.sel == 7, "光标到第 8 槽（当前 %d）", s_app.sel);
    CHECK(contains_any("存档 8"), "第 8 槽可见");

    // 返回菜单 → 设置（3 项，末项"返回"可见）
    press(BSP_BTN_OK, BSP_BTN_LONG);
    press(BSP_BTN_OK, BSP_BTN_RELEASE);
    CHECK(s_app.page == GAL_PAGE_MENU, "长按 OK 返回菜单");
    for (int i = 0; i < 8; i++) {
        click(BSP_BTN_UP);   // 回到菜单首项（光标位置会被保留）
    }
    for (int i = 0; i < 4; i++) {
        click(BSP_BTN_DOWN);   // 设置是第 5 项
    }
    CHECK(contains_any("设置"), "设置项可见");
    click(BSP_BTN_OK);
    CHECK(s_app.page == GAL_PAGE_SETTINGS, "进入设置页");
    CHECK(contains_any("文字速度"), "设置首项可见");
    CHECK(contains_any("返回"), "设置末项可见");
}

// 返回栈必须随“进入正文/结局”清零：否则多次“菜单→章节选择→选章”会把栈填满，
// 之后历史页长按 OK 会弹到菜单而不是正文（审计 P1）。
static void test_back_stack_cleared_on_new_scene(void)
{
    goto_reader();
    CHECK(s_app.page == GAL_PAGE_READER, "回到阅读页（当前 %d）", (int)s_app.page);
    CHECK(s_app.page_depth == 0, "正文页的返回栈应为空（当前 %d）", s_app.page_depth);

    for (int cycle = 0; cycle < 3; cycle++) {
        long_press(BSP_BTN_OK);            // 正文 -> 菜单（短 OK 现在是下一句）
        CHECK(s_app.page == GAL_PAGE_MENU, "第 %d 轮：打开菜单", cycle + 1);
        for (int i = 0; i < 6; i++) click(BSP_BTN_DOWN);   // 章节选择（第 7 项）
        click(BSP_BTN_OK);
        CHECK(s_app.page == GAL_PAGE_CHAPTER, "第 %d 轮：进入章节选择（当前 %d）", cycle + 1,
              (int)s_app.page);
        click(BSP_BTN_OK);                 // 从第一章开始
        CHECK(s_app.page == GAL_PAGE_READER, "第 %d 轮：选章回到正文（当前 %d）", cycle + 1,
              (int)s_app.page);
        CHECK(s_app.page_depth == 0, "第 %d 轮：进入正文后返回栈必须清空（当前 %d）",
              cycle + 1, s_app.page_depth);
    }

    // 三轮之后：正文 DOWN 进历史，长按 OK 必须一层弹回正文（不能弹到菜单）
    click(BSP_BTN_DOWN);
    CHECK(s_app.page == GAL_PAGE_HISTORY, "进入历史页（当前 %d）", (int)s_app.page);
    long_press(BSP_BTN_OK);
    CHECK(s_app.page == GAL_PAGE_READER, "历史长按 OK 应回正文（实际 %d）", (int)s_app.page);
    CHECK(s_app.page_depth == 0, "回到正文后返回栈应为空（当前 %d）", s_app.page_depth);
}

// 非正文页没有 lbl_hint：notice 必须有一条常驻提示行，否则“空槽/已保存”写了也看不见。
static void test_notice_visible_outside_reader(void)
{
    goto_reader();
    CHECK(s_app.page == GAL_PAGE_READER, "回到阅读页");

    // 读档页选一个空槽：必须看到“该槽位没有存档”
    long_press(BSP_BTN_OK);
    CHECK(s_app.page == GAL_PAGE_MENU, "打开菜单");
    for (int i = 0; i < 2; i++) click(BSP_BTN_DOWN);       // 读取存档
    click(BSP_BTN_OK);
    CHECK(s_app.page == GAL_PAGE_LOAD, "进入读档页");
    CHECK(!paper_game_slot_exists(7), "前置条件：槽 8 应为空");
    for (int i = 0; i < 7; i++) click(BSP_BTN_DOWN);
    click(BSP_BTN_OK);
    CHECK(strcmp(s_app.notice, "该槽位没有存档") == 0, "空槽提示已写入（实际=%.20s）",
          s_app.notice);
    CHECK(contains_any("该槽位没有存档"), "空槽提示必须在读档页可见");

    // 保存页保存成功：必须看到“已保存”，并且过期后自动消失
    long_press(BSP_BTN_OK);
    CHECK(s_app.page == GAL_PAGE_MENU, "读档页长按回菜单");
    click(BSP_BTN_UP);                                     // 菜单光标停在 2，回到 1「保存进度」
    click(BSP_BTN_OK);
    CHECK(s_app.page == GAL_PAGE_SAVE, "进入保存页（实际 %d）", (int)s_app.page);
    for (int i = 0; i < 6; i++) click(BSP_BTN_DOWN);       // 槽 7
    click(BSP_BTN_OK);
    CHECK(paper_game_slot_exists(6), "槽 7 已写入");
    CHECK(contains_any("已保存"), "保存成功提示必须在保存页可见");
    gal_app_tick(&s_app, 2100);
    CHECK(!contains_any("已保存"), "提示过期后必须从提示行消失");
}

// tjpgd 的 BGR565 必须换回 RGB565：纯红不能变紫。
static void check_cg_color_order(void)
{
    CHECK(paper_gfx_test_swap(0x001F) == 0xF800, "tjpgd red -> rgb565 red (0x%04x)",
          paper_gfx_test_swap(0x001F));
    CHECK(paper_gfx_test_swap(0xF800) == 0x001F, "tjpgd blue -> rgb565 blue");
    CHECK(paper_gfx_test_swap(0x07E0) == 0x07E0, "green unchanged");
}

int main(int argc, char **argv)
{
    const char *img = argc > 1 ? argv[1] : "main/paper_data/paper_img.bin";
    const char *scn = argc > 2 ? argv[2] : "main/paper_data/paper_scn.bin";
    uint32_t il = 0, sl = 0;
    paper_host_img = read_file(img, &il);
    paper_host_scn = read_file(scn, &sl);
    paper_host_img_len = il;
    paper_host_scn_len = sl;
    if (!paper_host_img || !paper_host_scn) {
        printf("FAIL 读不到资源包\n");
        return 2;
    }
    lv_stub_reset();
    CHECK(gal_app_init(&s_app), "app init");
    CHECK(s_app.page == GAL_PAGE_TITLE, "启动进标题页");
    CHECK(s_app.pack->scene_count == 47, "包已加载");

    test_title_scroll_and_layout();
    check_cg_color_order();
    test_continue_without_autosave();
    test_reader_fast_forward_release();
    test_autosave_paths();
    test_typewriter_utf8();
    test_return_matrix();
    test_all_choice_branches();
    test_choice_cursor_survives_menu();
    test_choice_menu_roundtrip();
    test_draw_failure_semantics();
    test_sprite_failure_semantics();
    test_menu_lists();
    test_back_stack_cleared_on_new_scene();
    test_notice_visible_outside_reader();

    printf("\n%s: %d 项检查, %d 项失败\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
