/*
 * tests/test_eden_app.c —— 真实应用层的宿主机回归(P0 / P1)。
 *
 * 编译的是生产代码:main/eden_app.c + eden_ui.c + eden_image.c + eden_pack.c +
 * eden_script.c + eden_store.c + gal_settings.c,LVGL/NVS/JPEG/BSP 用 tests/app_stubs
 * 的替身。断言全部落在"真实调用产生了什么":
 *   - eden_ui 对象可见性 / 文本(不是复制页面逻辑)
 *   - 注入 NVS 提交失败后,旧存档仍在、仍可读
 *   - 注入 JPEG 解码失败后,同帧再渲染不解锁 CG;恢复后才解锁
 *   - 快进:短按只推一句、长按后按其它键/松手/换页即取消
 *   - 所有 UI 写操作都在 bsp_lvgl_lock 之内
 *   - 解锁位写失败会退避重试,恢复后重启能找回
 *
 * 构建见 tools/validate.sh:加 -DEDEN_APP_HOST_TEST -Itests/app_stubs。
 */
#include "eden_app.h"

#include "bsp_button.h"
#include "eden_pack.h"
#include "eden_store.h"
#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 来自替身实现的测试钩子
void eden_stub_reset(void);
void eden_stub_timer_tick(void);
void eden_stub_advance_us(int64_t delta);
void eden_stub_nvs_reset(void);
void eden_stub_nvs_fail(bool fail_set, bool fail_commit);
void eden_stub_nvs_fail_init(int error);
int eden_stub_nvs_commit_attempts(void);
int eden_stub_nvs_commit_success(void);
bool eden_stub_nvs_present(const char *ns, const char *key);
bool eden_stub_nvs_write_raw(const char *ns, const char *key, const void *data, size_t size);
void eden_stub_jpeg_reset(void);
void eden_stub_jpeg_fail(bool all, unsigned fail_len);
int eden_stub_ui_violations(void);
int eden_stub_landscape(void);
bool eden_stub_visible_container(int index);
lv_obj_stub_t *eden_stub_object_count(int *count);
const char *eden_stub_text_of(lv_obj_stub_t *obj);

// 字体对象:生产固件里由 assets/fonts 生成,宿主机只用来占位。
const lv_font_t gal_cjk_16;
const lv_font_t gal_cjk_20;

static int failures;
static int checks;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        checks++;                                                           \
        if (!(cond)) {                                                      \
            failures++;                                                     \
            fprintf(stderr, "FAIL %s:%d ", __FILE__, __LINE__);             \
            fprintf(stderr, __VA_ARGS__);                                   \
            fputc('\n', stderr);                                            \
        }                                                                   \
    } while (0)

// ---------------------------------------------------------------- 合成资源包
#define TEXT_CAP 4096
#define OPS_CAP 256
#define IMG_CAP 8
#define FRAME_CAP 8

typedef struct {
    uint8_t blob[16384];
    size_t size;

    char text[TEXT_CAP];
    size_t text_len;
    uint32_t txidx[64];
    int text_count;

    uint8_t ops[OPS_CAP * 12];
    int op_count;
    uint32_t scene_start[8];
    uint32_t scene_count[8];
    int max_scene;

    uint8_t images[IMG_CAP * 16];
    int image_count;
    uint8_t blob_data[2048];
    size_t blob_len;

    uint8_t frames[FRAME_CAP * 6];
    int frame_count;
    uint8_t patches[8 * 6];
    int patch_count;

    uint8_t chapters[8 * 8];
    int chapter_count;
    uint8_t gallery[8 * 6];
    int gallery_count;
} pack_builder_t;

static int intern(pack_builder_t *b, const char *value)
{
    // 线性查重就够了(测试里字符串很少)。
    uint32_t offset = 0;
    for (int index = 0; index < b->text_count; index++) {
        const char *existing = b->text + offset;
        if (strcmp(existing, value) == 0) {
            return index;
        }
        offset = b->txidx[index + 1];
    }
    const size_t length = strlen(value);
    memcpy(b->text + b->text_len, value, length + 1);
    b->text_len += length + 1;
    b->txidx[b->text_count] = (uint32_t)(b->text_len - length - 1);
    b->text_count++;
    b->txidx[b->text_count] = (uint32_t)b->text_len;
    return b->text_count - 1;
}

static void add_op(pack_builder_t *b, uint8_t kind, uint8_t a, uint16_t bb, uint32_t c, uint32_t d)
{
    uint8_t *dst = b->ops + (size_t)b->op_count * 12;
    dst[0] = kind;
    dst[1] = a;
    dst[2] = (uint8_t)(bb & 0xFF);
    dst[3] = (uint8_t)(bb >> 8);
    for (int byte = 0; byte < 4; byte++) {
        dst[4 + byte] = (uint8_t)((c >> (8 * byte)) & 0xFF);
        dst[8 + byte] = (uint8_t)((d >> (8 * byte)) & 0xFF);
    }
    b->op_count++;
}

// 加一张"JPEG":前 4 字节是宽高,后接填充字节,总长度 = length(替身按长度注入失败)。
static int add_image(pack_builder_t *b, uint16_t w, uint16_t h, uint16_t cw, uint16_t ch,
                     uint16_t length)
{
    const int index = b->image_count++;
    uint8_t *record = b->images + index * 16;
    const uint32_t offset = (uint32_t)b->blob_len;
    b->blob_data[b->blob_len++] = (uint8_t)(w & 0xFF);
    b->blob_data[b->blob_len++] = (uint8_t)(w >> 8);
    b->blob_data[b->blob_len++] = (uint8_t)(h & 0xFF);
    b->blob_data[b->blob_len++] = (uint8_t)(h >> 8);
    while (b->blob_len < offset + length) {
        b->blob_data[b->blob_len++] = 0x5A;
    }
    memset(record, 0, 16);
    for (int byte = 0; byte < 4; byte++) {
        record[byte] = (uint8_t)((offset >> (8 * byte)) & 0xFF);
        record[4 + byte] = (uint8_t)((length >> (8 * byte)) & 0xFF);
    }
    record[8] = (uint8_t)(w & 0xFF);
    record[9] = (uint8_t)(w >> 8);
    record[10] = (uint8_t)(h & 0xFF);
    record[11] = (uint8_t)(h >> 8);
    record[12] = (uint8_t)(cw & 0xFF);
    record[13] = (uint8_t)(cw >> 8);
    record[14] = (uint8_t)(ch & 0xFF);
    record[15] = (uint8_t)(ch >> 8);
    return index;
}

static int add_frame(pack_builder_t *b, int base, int patch_first, int patch_count)
{
    const int index = b->frame_count++;
    uint8_t *record = b->frames + index * 6;
    record[0] = (uint8_t)(base & 0xFF);
    record[1] = (uint8_t)(base >> 8);
    record[2] = (uint8_t)(patch_first & 0xFF);
    record[3] = (uint8_t)(patch_first >> 8);
    record[4] = (uint8_t)patch_count;
    record[5] = 0;
    return index;
}

static void add_patch(pack_builder_t *b, int image, uint16_t x, uint16_t y)
{
    uint8_t *record = b->patches + b->patch_count * 6;
    record[0] = (uint8_t)(image & 0xFF);
    record[1] = (uint8_t)(image >> 8);
    record[2] = (uint8_t)(x & 0xFF);
    record[3] = (uint8_t)(x >> 8);
    record[4] = (uint8_t)(y & 0xFF);
    record[5] = (uint8_t)(y >> 8);
    b->patch_count++;
}

// 合成剧本:两章,第一章 4 句(含一句带补丁画面),第二章 2 句。
// 每句的正文都不同,便于用文本判断"到底推进到哪一句"。
typedef struct {
    int scene1_lines;
    int scene2_lines;
    uint16_t frame_plain;      // 无补丁画面
    uint16_t frame_patched;    // 带补丁画面
    unsigned base_len;
    unsigned patch_len;
} fixture_t;

static void build_pack(pack_builder_t *b, fixture_t *fixture)
{
    memset(b, 0, sizeof(*b));

    const int speaker_a = intern(b, "阿亮");
    const int speaker_b = intern(b, "");
    // 正文:每句唯一
    const int line_idx[6] = {
        intern(b, "第一句台词"),
        intern(b, "第二句台词"),
        intern(b, "第三句台词"),
        intern(b, "第四句台词"),
        intern(b, "第五句:第二章"),
        intern(b, "第六句:第二章"),
    };

    const unsigned base_len = 40;
    const unsigned patch_len = 60;
    const int base_img = add_image(b, 264, 168, 264, 165, (uint16_t)base_len);
    const int patch_img = add_image(b, 104, 168, 40, 30, (uint16_t)patch_len);
    const int frame_plain = add_frame(b, base_img, 0, 0);
    add_patch(b, patch_img, 10, 10);
    const int frame_patched = add_frame(b, base_img, 0, 1);

    fixture->frame_plain = (uint16_t)frame_plain;
    fixture->frame_patched = (uint16_t)frame_patched;
    fixture->base_len = base_len;
    fixture->patch_len = patch_len;

    // 场景 1:4 句;第 3 句用带补丁画面。
    b->scene_start[1] = 0;
    add_op(b, 0, 0, (uint16_t)speaker_a, (uint32_t)line_idx[0], (uint32_t)frame_plain);
    add_op(b, 0, 0, (uint16_t)speaker_b, (uint32_t)line_idx[1], (uint32_t)frame_plain);
    add_op(b, 0, 0, (uint16_t)speaker_a, (uint32_t)line_idx[2], (uint32_t)frame_patched);
    add_op(b, 0, 0, (uint16_t)speaker_b, (uint32_t)line_idx[3], (uint32_t)frame_plain);
    b->scene_count[1] = 4;
    fixture->scene1_lines = 4;

    // 场景 2:2 句 + end
    b->scene_start[2] = 4;
    add_op(b, 0, 0, (uint16_t)speaker_a, (uint32_t)line_idx[4], (uint32_t)frame_plain);
    add_op(b, 0, 0, (uint16_t)speaker_b, (uint32_t)line_idx[5], (uint32_t)frame_plain);
    add_op(b, 5, 0, 0, 0, 0);   // END
    b->scene_count[2] = 3;
    fixture->scene2_lines = 2;
    b->max_scene = 2;

    // 章节表:顺序 1 -> 2
    uint8_t *chapter = b->chapters;
    chapter[0] = 1;
    chapter[4] = 0;
    chapter[8] = 2;
    chapter[12] = 4;
    b->chapter_count = 2;

    // 鉴赏:8 条(每页 6 条 → 两页)。前两条对应两个画面且会被解锁,
    // 其余只用于翻页/锁定显示。
    static const char *labels[8] = { "CG甲", "CG乙", "CG丙", "CG丁", "CG戊", "CG己",
                                     "CG庚", "CG辛" };
    uint8_t *gal = b->gallery;
    for (int index = 0; index < 8; index++) {
        const int frame = (index % 2 == 0) ? frame_plain : frame_patched;
        const int label = intern(b, labels[index]);
        uint8_t *record = gal + index * 6;
        record[0] = (uint8_t)(frame & 0xFF);
        record[1] = (uint8_t)(frame >> 8);
        record[2] = (uint8_t)index;      // group
        record[3] = 0;
        record[4] = (uint8_t)(label & 0xFF);
        record[5] = (uint8_t)(label >> 8);
    }
    b->gallery_count = 8;
}

// 把 builder 序列化成 EDENPK01。
static void emit_pack(pack_builder_t *b, uint8_t *out, size_t *out_size)
{
    struct {
        uint32_t kind;
        const void *data;
        size_t size;
        size_t count;
    } parts[] = {
        { EDEN_SEC_TEXT, b->text, b->text_len, (size_t)b->text_count },
        { EDEN_SEC_TXIDX, b->txidx, (size_t)(b->text_count + 1) * 4,
          (size_t)(b->text_count + 1) },
        { EDEN_SEC_OPS, b->ops, (size_t)b->op_count * 12, (size_t)b->op_count },
        { EDEN_SEC_SCENE, b->scene_start, (size_t)(b->max_scene + 1) * 8,
          (size_t)(b->max_scene + 1) },
        { EDEN_SEC_IMAGE, b->images, (size_t)b->image_count * 16, (size_t)b->image_count },
        { EDEN_SEC_FRAME, b->frames, (size_t)b->frame_count * 6, (size_t)b->frame_count },
        { EDEN_SEC_CHAP, b->chapters, (size_t)b->chapter_count * 8, (size_t)b->chapter_count },
        { EDEN_SEC_GAL, b->gallery, (size_t)b->gallery_count * 6, (size_t)b->gallery_count },
        { EDEN_SEC_BLOB, b->blob_data, b->blob_len, b->blob_len },
        { EDEN_SEC_PATCH, b->patches, (size_t)b->patch_count * 6, (size_t)b->patch_count },
    };
    // SCENE 表用 start/count 两个 u32 表达;这里直接铺成 [start, start+count]。
    uint8_t scene_table[8 * 8];
    memset(scene_table, 0, sizeof(scene_table));
    for (int scene = 1; scene <= b->max_scene; scene++) {
        for (int byte = 0; byte < 4; byte++) {
            scene_table[scene * 8 + byte] =
                (uint8_t)((b->scene_start[scene] >> (8 * byte)) & 0xFF);
            const uint32_t end = b->scene_start[scene] + b->scene_count[scene];
            scene_table[scene * 8 + 4 + byte] = (uint8_t)((end >> (8 * byte)) & 0xFF);
        }
    }
    parts[3].data = scene_table;
    parts[3].size = (size_t)(b->max_scene + 1) * 8;
    parts[3].count = (size_t)(b->max_scene + 1);

    const size_t part_count = sizeof(parts) / sizeof(parts[0]);
    const size_t header = 20 + 10 * 16;
    size_t offset = header;
    size_t places[16];
    for (size_t index = 0; index < part_count; index++) {
        places[index] = offset;
        offset += parts[index].size;
        offset += (4 - (offset % 4)) % 4;
    }
    memset(out, 0, offset);
    memcpy(out, EDEN_PACK_MAGIC, 8);
    out[8] = (uint8_t)(EDEN_PACK_VERSION & 0xFF);
    out[9] = (uint8_t)(EDEN_PACK_VERSION >> 8);
    out[10] = (uint8_t)(header & 0xFF);
    out[11] = (uint8_t)((header >> 8) & 0xFF);
    out[12] = 10;
    for (int byte = 0; byte < 4; byte++) {
        out[16 + byte] = (uint8_t)((offset >> (8 * byte)) & 0xFF);
    }
    for (size_t index = 0; index < part_count; index++) {
        uint8_t *record = out + 20 + index * 16;
        for (int byte = 0; byte < 4; byte++) {
            record[byte] = (uint8_t)((parts[index].kind >> (8 * byte)) & 0xFF);
            record[4 + byte] = (uint8_t)(((uint32_t)places[index] >> (8 * byte)) & 0xFF);
            record[8 + byte] = (uint8_t)(((uint32_t)parts[index].count >> (8 * byte)) & 0xFF);
            record[12 + byte] = (uint8_t)(((uint32_t)parts[index].size >> (8 * byte)) & 0xFF);
        }
        memcpy(out + places[index], parts[index].data, parts[index].size);
    }
    *out_size = offset;
}

// ---------------------------------------------------------------- UI 观测
// 当前是否能看到某段文本(本身与祖先都没被隐藏)。
static bool text_visible(const char *needle)
{
    int count = 0;
    lv_obj_stub_t *objects = eden_stub_object_count(&count);
    for (int index = 0; index < count; index++) {
        if (objects[index].kind == 1 && strstr(objects[index].text, needle) != NULL) {
            const lv_obj_stub_t *cursor = &objects[index];
            bool hidden = false;
            while (cursor != NULL) {
                if ((cursor->flags & LV_OBJ_FLAG_HIDDEN) != 0) {
                    hidden = true;
                    break;
                }
                cursor = cursor->parent;
            }
            if (!hidden) {
                return true;
            }
        }
    }
    return false;
}

static const char *visible_line_text(void)
{
    static char buffer[256];
    int count = 0;
    lv_obj_stub_t *objects = eden_stub_object_count(&count);
    for (int index = 0; index < count; index++) {
        if (objects[index].kind == 1 && strstr(objects[index].text, "句") != NULL &&
            eden_stub_visible(&objects[index])) {
            snprintf(buffer, sizeof(buffer), "%s", objects[index].text);
            return buffer;
        }
    }
    return "";
}

// ---------------------------------------------------------------- 按键驱动
// 把打字机跑完(定时器回调一次输出一个字)。
static void settle(void)
{
    for (int index = 0; index < 80; index++) {
        eden_stub_timer_tick();
    }
}

static void key(int btn, int ev)
{
    eden_key_t k = { (uint8_t)btn, (uint8_t)ev };
    eden_app_key(&k);
}

static void click(int btn)
{
    key(btn, BSP_BTN_PRESS);
    key(btn, BSP_BTN_RELEASE);
    key(btn, BSP_BTN_CLICK);
    settle();
}

static void long_press(int btn)
{
    key(btn, BSP_BTN_PRESS);
    key(btn, BSP_BTN_LONG);
    key(btn, BSP_BTN_RELEASE);
    settle();
}

// 标题菜单光标归位:先上到顶(到顶会停住),再下 index 次。
static void title_select(int index)
{
    for (int i = 0; i < 6; i++) {
        click(BSP_BTN_UP);
    }
    for (int i = 0; i < index; i++) {
        click(BSP_BTN_DOWN);
    }
}

// 单调推进的时钟:应用内部按 now_ms 差值判断进度,回绕会让"刚发生过"变成"很久以前"。
static uint32_t s_now_ms;

static void tick_ms(unsigned ms)
{
    for (unsigned elapsed = 0; elapsed < ms; elapsed += 50) {
        eden_stub_advance_us(50 * 1000);
        s_now_ms += 50;
        eden_app_tick(s_now_ms);
    }
}

#define BTN(b) ((b) == 0 ? BSP_BTN_UP : (b) == 1 ? BSP_BTN_DOWN : BSP_BTN_OK)

// ---------------------------------------------------------------- 夹具
static pack_builder_t s_builder;
static uint8_t s_pack[16384];
static size_t s_pack_size;
static fixture_t s_fixture;

static bool boot(void)
{
    s_now_ms = 0;
    eden_stub_reset();
    eden_stub_nvs_reset();
    eden_stub_jpeg_reset();
    build_pack(&s_builder, &s_fixture);
    emit_pack(&s_builder, s_pack, &s_pack_size);
    return eden_app_init_with_pack(s_pack, (uint32_t)s_pack_size);
}

// 从标题开始新游戏,停在第一章第一句。
static void start_game(void)
{
    click(BSP_BTN_OK);              // START -> TITLE
    title_select(0);                // 开始游戏
    click(BSP_BTN_OK);
}

// ---------------------------------------------------------------- P0 页面来源
static void test_page_origin(void)
{
    CHECK(boot(), "初始化失败");
    click(BSP_BTN_OK);   // START -> TITLE
    CHECK(text_visible("开始游戏"), "标题页菜单不可见");
    // 阅读页四硬指标（名字框外 / 页码面板内且不压正文 / 末行不出框）在进入正文后成立。
    (void)0;
    // 标题背景：进标题页必须画第 0 帧（复用对话页 264x168 画布，无新分配）。
    CHECK(eden_app_test_title_frame == 0, "标题背景帧=%u（应为 0）",
          (unsigned)eden_app_test_title_frame);

    // 标题 -> 设置 -> 长按 OK,必须回标题(不是空正文)。
    title_select(3);       // 设置
    click(BSP_BTN_OK);
    CHECK(text_visible("文字速度"), "设置页没打开");
    long_press(BSP_BTN_OK);
    CHECK(text_visible("开始游戏"), "设置返回后应回到标题,实际不在标题页");
    CHECK(!text_visible("第一句台词"), "设置返回后不应进入正文");

    // 标题 -> 鉴赏 -> 长按 OK 回标题
    title_select(4);       // 鉴赏
    click(BSP_BTN_OK);
    CHECK(text_visible("CG 鉴赏"), "鉴赏入口没打开");
    long_press(BSP_BTN_OK);
    CHECK(text_visible("开始游戏"), "鉴赏返回后应回到标题");

    // 正文 -> 菜单 -> 设置 -> 长按 OK 回菜单 -> 再长按回正文
    title_select(0);       // 开始游戏
    click(BSP_BTN_OK);
    CHECK(text_visible("第一句台词"), "新游戏没有显示第一句");
    long_press(BSP_BTN_OK);            // 菜单
    CHECK(text_visible("返回剧情"), "菜单没打开");
    click(BSP_BTN_DOWN);
    click(BSP_BTN_DOWN);
    click(BSP_BTN_DOWN);
    click(BSP_BTN_DOWN);   // 设置
    click(BSP_BTN_OK);
    CHECK(text_visible("文字速度"), "菜单里的设置没打开");
    long_press(BSP_BTN_OK);
    CHECK(text_visible("返回剧情"), "设置应回到菜单");
    long_press(BSP_BTN_OK);
    CHECK(text_visible("第一句台词"), "菜单应回到正文");
}

// ---------------------------------------------------------------- P1a 写失败保旧档
// 题面:覆盖写失败时不能把 RAM 里的旧档清掉。走真实路径:存一次 -> 注入 commit 失败再存
// -> 失败提示可见 -> 旧档预览仍在 -> 立刻读旧档能回到旧位置。
static void test_save_failure_keeps_old_slot(void)
{
    CHECK(boot(), "初始化失败");
    start_game();
    CHECK(strcmp(visible_line_text(), "第一句台词") == 0, "第一句没出现: %s",
          visible_line_text());
    // 阅读页四硬指标：名字框外、页码在面板内且不压正文、末行不出框。
    CHECK(eden_app_test_reader_geometry(), "reader geometry (name/page/last line)");

    // 第一次保存(空槽,直接写)
    long_press(BSP_BTN_OK);   // 菜单
    click(BSP_BTN_DOWN);      // 存档
    click(BSP_BTN_OK);
    CHECK(text_visible("存档 1"), "存档页没打开");
    click(BSP_BTN_OK);
    CHECK(text_visible("已保存"), "首次保存应成功");
    CHECK(eden_stub_nvs_present("edensave", "slots"), "NVS 里应有存档记录");

    // 回到正文,推进到第二句
    long_press(BSP_BTN_OK);   // 存档 -> 菜单
    long_press(BSP_BTN_OK);   // 菜单 -> 正文
    click(BSP_BTN_UP);
    CHECK(strcmp(visible_line_text(), "第二句台词") == 0, "应推进到第二句: %s",
          visible_line_text());

    // 覆盖保存,但让 nvs_commit 失败(走真实 eden_store_nvs.c 路径)
    eden_stub_nvs_fail(false, true);
    long_press(BSP_BTN_OK);
    click(BSP_BTN_DOWN);
    click(BSP_BTN_OK);
    click(BSP_BTN_OK);        // 覆盖确认
    click(BSP_BTN_OK);        // 真正写入 -> 失败
    CHECK(text_visible("保存失败"), "写失败必须提示失败");
    CHECK(!text_visible("已保存"), "写失败不能显示已保存");

    // 旧档必须还在 RAM 里:槽位预览显示第一句,而不是"空"
    CHECK(!text_visible("存档 1 · 空"), "写失败后旧档不应变成空槽");
    CHECK(text_visible("第一句台词"), "槽位预览应仍是旧档内容");

    // 已提交区(flash 里的旧记录)也不能被失败的 commit 破坏:
    // 读完 NVS 里的记录,槽 0 应仍是第一句的位置。
    {
        eden_slot_t reloaded[EDEN_SAVE_SLOTS];
        memset(reloaded, 0, sizeof(reloaded));
        CHECK(eden_store_load_slots(reloaded), "失败后应仍能读到已提交的旧记录");
        CHECK(reloaded[0].valid == 1, "已提交记录应仍有效");
        CHECK(strstr(reloaded[0].preview, "第一句台词") != NULL,
              "已提交记录应仍是旧档(实际预览: %s)", reloaded[0].preview);
    }

    // 立即读回旧档:存档页 -> 菜单 -> 读档
    eden_stub_nvs_fail(false, false);
    long_press(BSP_BTN_OK);   // 保存页 -> 菜单
    CHECK(text_visible("返回剧情"), "从存档页返回应回到菜单");
    click(BSP_BTN_DOWN);      // 读档
    click(BSP_BTN_OK);
    CHECK(text_visible("存档 1"), "读档页没打开");
    click(BSP_BTN_OK);
    CHECK(strcmp(visible_line_text(), "第一句台词") == 0, "旧档应能读回第一句: %s",
          visible_line_text());
}

// ---------------------------------------------------------------- P1b 解码失败
static void goto_gallery_from_game(void)
{
    long_press(BSP_BTN_OK);   // 菜单
    click(BSP_BTN_DOWN);
    click(BSP_BTN_DOWN);
    click(BSP_BTN_DOWN);
    click(BSP_BTN_DOWN);
    click(BSP_BTN_DOWN);      // 回到标题
    click(BSP_BTN_OK);
    CHECK(text_visible("开始游戏"), "应回到标题");
    title_select(4);          // 鉴赏
    click(BSP_BTN_OK);
    click(BSP_BTN_OK);        // CG 鉴赏
}

static void back_to_title_from_gallery(void)
{
    long_press(BSP_BTN_OK);   // CG 列表 -> 鉴赏
    long_press(BSP_BTN_OK);   // 鉴赏 -> 标题
}

// 带补丁的画面解码失败时:不解锁 CG;同帧再渲染仍不解锁;故障解除后成功解码才解锁。
static void test_draw_failure_does_not_unlock(void)
{
    CHECK(boot(), "初始化失败");
    eden_stub_jpeg_fail(false, s_fixture.patch_len);   // 只让补丁解码失败
    start_game();
    CHECK(strcmp(visible_line_text(), "第一句台词") == 0, "第一句没出现: %s",
          visible_line_text());
    click(BSP_BTN_UP);
    click(BSP_BTN_UP);   // 第三句:带补丁画面,解码失败
    CHECK(strcmp(visible_line_text(), "第三句台词") == 0, "第三句没出现: %s",
          visible_line_text());
    eden_app_flush();    // 让"继续游戏"能回到这一句

    // 鉴赏:group 0(已成功解码)可见,group 1(解码失败)仍是问号
    goto_gallery_from_game();
    CHECK(text_visible("CG甲"), "已成功解码的 group 0 应显示标签");
    CHECK(text_visible("？？？"), "解码失败的 group 1 不应解锁");

    // 回到标题再"继续游戏":同一帧重新渲染(仍失败),仍不能解锁
    back_to_title_from_gallery();
    title_select(1);     // 继续游戏
    click(BSP_BTN_OK);
    CHECK(strcmp(visible_line_text(), "第三句台词") == 0, "继续游戏应回到第三句: %s",
          visible_line_text());
    goto_gallery_from_game();
    CHECK(text_visible("？？？"), "同帧再渲染后仍不能解锁");

    // 故障解除 -> 重新渲染同一帧 -> 解锁
    eden_stub_jpeg_fail(false, 0);
    back_to_title_from_gallery();
    title_select(1);
    click(BSP_BTN_OK);
    CHECK(strcmp(visible_line_text(), "第三句台词") == 0, "再次继续游戏应回到第三句: %s",
          visible_line_text());
    goto_gallery_from_game();
    CHECK(text_visible("CG乙"), "故障解除并成功解码后应解锁");
    // 其余 6 组故意没有剧情触发,保持问号是正确行为。
    CHECK(text_visible("？？？"), "未触发的组应保持未解锁");
}

// ---------------------------------------------------------------- P1c 快进
static void test_fast_forward_cancel(void)
{
    CHECK(boot(), "初始化失败");
    start_game();
    CHECK(text_visible("第一句台词"), "第一句没出现");

    // 短按 PRESS->RELEASE->CLICK:只推进一句
    click(BSP_BTN_UP);
    CHECK(text_visible("第二句台词"), "短按应推进到第二句");

    // 长按快进 -> 当前句必须瞬显（不再逐字）；之后每次推进也直接整句。
    key(BSP_BTN_UP, BSP_BTN_PRESS);
    key(BSP_BTN_UP, BSP_BTN_LONG);
    CHECK(!eden_app_test_typing(), "快进时必须整句瞬显");
    // 按 DOWN 进历史 -> 松手 -> 回正文后不能再继续推进
    tick_ms(400);                      // 快进推进若干句
    click(BSP_BTN_DOWN);               // 进历史(同时取消快进)
    CHECK(text_visible("历史"), "应进入历史页");
    key(BSP_BTN_UP, BSP_BTN_RELEASE);  // 在历史页松手
    click(BSP_BTN_OK);                 // 回正文
    char before[256];
    snprintf(before, sizeof(before), "%s", visible_line_text());
    CHECK(before[0] != 0, "返回正文后应能看到当前台词");
    tick_ms(600);   // 若快进没被取消,这里会继续推进
    const char *after = visible_line_text();
    CHECK(strcmp(before, after) == 0, "从历史返回后快进应已取消(%s -> %s)", before, after);
}

// ---------------------------------------------------------------- P1d 锁
static void test_ui_only_inside_lock(void)
{
    CHECK(boot(), "初始化失败");
    const int before = eden_stub_ui_violations();
    start_game();
    long_press(BSP_BTN_OK);
    click(BSP_BTN_DOWN);
    long_press(BSP_BTN_OK);
    click(BSP_BTN_UP);
    CHECK(eden_stub_ui_violations() == before,
          "有 %d 次 UI 写操作发生在 LVGL 锁之外", eden_stub_ui_violations() - before);
}

// ---------------------------------------------------------------- P0r CG 导航栈
// 列表内移动/翻页不能压栈;大图关闭必须出栈回列表;返回要一次一层。
static void test_cg_navigation_stack(void)
{
    CHECK(boot(), "初始化失败");
    start_game();
    CHECK(strcmp(visible_line_text(), "第一句台词") == 0, "第一句没出现");
    goto_gallery_from_game();
    CHECK(text_visible("CG甲") || text_visible("？？？"), "CG 列表没打开");

    // 同页移动多次:不能把来源栈填满
    for (int index = 0; index < 4; index++) {
        click(BSP_BTN_DOWN);
    }
    for (int index = 0; index < 2; index++) {
        click(BSP_BTN_UP);
    }
    CHECK(text_visible("CG 鉴赏"), "移动光标后仍应在 CG 列表");

    // 跨页:8 条 → 第 2 页(光标落到 7/8 条)
    click(BSP_BTN_DOWN);
    click(BSP_BTN_DOWN);
    click(BSP_BTN_DOWN);
    click(BSP_BTN_DOWN);
    click(BSP_BTN_DOWN);
    click(BSP_BTN_DOWN);
    CHECK(text_visible("CG 鉴赏  2/2 页"), "应翻到第 2 页(实际: %s)",
          visible_line_text());
    for (int index = 0; index < 12; index++) {
        click(BSP_BTN_UP);          // 一直上到第 1 页第 1 项(到顶会停住)
    }
    CHECK(text_visible("CG 鉴赏  1/2 页"), "应退回第 1 页");

    // 开关大图 10 次:每次都是一进一出,不能累积栈层
    for (int loop = 0; loop < 10; loop++) {
        click(BSP_BTN_OK);          // 进大图
        CHECK(!text_visible("CG 鉴赏  1/2 页"), "大图页不应显示列表标题");
        long_press(BSP_BTN_OK);     // 关大图
        CHECK(text_visible("CG 鉴赏  1/2 页"), "第 %d 次关闭大图应回到 CG 列表", loop + 1);
    }

    // 一次长 OK 退到鉴赏,再一次退到标题(不能消耗多次返回)
    long_press(BSP_BTN_OK);
    CHECK(text_visible("章节鉴赏"), "第一次长 OK 应回到鉴赏入口(实际: %s)", visible_line_text());
    long_press(BSP_BTN_OK);
    CHECK(text_visible("开始游戏"), "第二次长 OK 应回到标题");
}

// ---------------------------------------------------------------- P1e 解锁落盘
// 首次 dirty 会安排 2s 后的重试窗口。这里真的跨过窗口、统计失败尝试数、验证指数退避,
// 再恢复 IO 并从 NVS 记录重新加载,确认 CG / 章节 / 结局位都能找回。
static void test_unlock_retry_backoff_and_reload(void)
{
    CHECK(boot(), "初始化失败");
    eden_stub_nvs_fail(false, true);   // 从头就让解锁写失败
    start_game();
    CHECK(strcmp(visible_line_text(), "第一句台词") == 0, "第一句没出现");
    CHECK(!eden_stub_nvs_present("edenulck", "bits"), "写失败时不应有已提交记录");

    const int base_attempts = eden_stub_nvs_commit_attempts();
    tick_ms(1000);                     // 首次窗口(2s)未到
    CHECK(eden_stub_nvs_commit_attempts() == base_attempts,
          "首次重试窗口内不应写 flash(多了 %d 次)",
          eden_stub_nvs_commit_attempts() - base_attempts);
    tick_ms(1200);                     // 跨过 2s 窗口 -> 应有 1 次失败尝试
    CHECK(eden_stub_nvs_commit_attempts() > base_attempts, "跨过首次窗口后应重试一次");
    const int after_first = eden_stub_nvs_commit_attempts();
    tick_ms(1000);                     // 退避已翻倍到 4s,这 1s 内不应再试
    CHECK(eden_stub_nvs_commit_attempts() == after_first,
          "退避窗口内不应重试(多了 %d 次)",
          eden_stub_nvs_commit_attempts() - after_first);

    // 恢复 IO:下一个窗口写成功
    eden_stub_nvs_fail(false, false);
    tick_ms(4000);
    CHECK(eden_stub_nvs_present("edenulck", "bits"), "恢复后解锁位应已落盘");
    CHECK(eden_stub_nvs_commit_success() >= 1, "应至少有一次成功提交");

    // 从记录重新加载(CG / 章节)
    eden_unlock_t reloaded;
    memset(&reloaded, 0xAA, sizeof(reloaded));
    CHECK(eden_store_load_unlock(&reloaded), "应能从 NVS 读回解锁记录");
    CHECK(eden_unlock_has_cg(&reloaded, 0), "CG 组 0 应已解锁");
    CHECK(eden_unlock_has_chapter(&reloaded, 0), "第 1 章应已解锁");
}

// ---------------------------------------------------------------- 任意页面都要落盘
// 标题 / CG 列表 / 结局页都没有正文(line_valid 为假),持久化重试不能依赖页面。
static void test_unlock_flush_on_every_page(void)
{
    // --- 标题页 ---
    CHECK(boot(), "初始化失败");
    eden_stub_nvs_fail(false, true);
    start_game();
    long_press(BSP_BTN_OK);            // 菜单
    click(BSP_BTN_DOWN);
    click(BSP_BTN_DOWN);
    click(BSP_BTN_DOWN);
    click(BSP_BTN_DOWN);
    click(BSP_BTN_DOWN);               // 回到标题
    click(BSP_BTN_OK);
    CHECK(text_visible("开始游戏"), "应停在标题页");
    int before = eden_stub_nvs_commit_attempts();
    eden_stub_nvs_fail(false, false);
    tick_ms(3000);
    CHECK(eden_stub_nvs_commit_attempts() > before, "标题页也必须重试解锁落盘");
    CHECK(eden_stub_nvs_present("edenulck", "bits"), "标题页上应已落盘");

    // --- CG 列表页 ---
    CHECK(boot(), "初始化失败");
    eden_stub_nvs_fail(false, true);
    start_game();
    goto_gallery_from_game();
    CHECK(text_visible("CG 鉴赏"), "应停在 CG 列表");
    before = eden_stub_nvs_commit_attempts();
    tick_ms(3000);
    CHECK(eden_stub_nvs_commit_attempts() > before, "CG 列表页也必须重试解锁落盘");
    eden_stub_nvs_fail(false, false);
    tick_ms(3000);
    CHECK(eden_stub_nvs_present("edenulck", "bits"), "CG 列表页上应已落盘");

    // --- 结局页 ---
    CHECK(boot(), "初始化失败");
    eden_stub_nvs_fail(false, true);
    start_game();
    for (int index = 0; index < 4; index++) {
        click(BSP_BTN_UP);             // 第 1 章读完后进入章节过场
    }
    CHECK(text_visible("按 OK 继续"), "第 1 章结束应显示章节过场");
    click(BSP_BTN_OK);                 // 过场结束 -> 第 5 句
    click(BSP_BTN_UP);                 // 第 6 句
    click(BSP_BTN_UP);                 // end 指令 -> 结局页
    CHECK(text_visible("TRUE END"), "应到达结局页");
    before = eden_stub_nvs_commit_attempts();
    tick_ms(3000);
    CHECK(eden_stub_nvs_commit_attempts() > before, "结局页也必须重试解锁落盘");
    eden_stub_nvs_fail(false, false);
    tick_ms(3000);
    CHECK(eden_stub_nvs_present("edenulck", "bits"), "结局页上应已落盘");
    eden_unlock_t reloaded;
    memset(&reloaded, 0, sizeof(reloaded));
    CHECK(eden_store_load_unlock(&reloaded), "结局记录应可读回");
    CHECK(eden_unlock_has_ending(&reloaded, 0), "结局位应已记录");
}

// ---------------------------------------------------------------- P0r 鉴赏 ↔ 章节
// 章节鉴赏页的“返回”必须是出栈（回鉴赏入口），不能是 show_gallery() 再压一层：
// 否则 鉴赏→章节→长OK→鉴赏→长OK→章节 … 永远回不到标题。
static void test_gallery_chapter_round_trip(void)
{
    CHECK(boot(), "初始化失败");
    start_game();
    CHECK(text_visible("第一句台词"), "新游戏没显示第一句");
    goto_gallery_from_game();          // 正文 -> 菜单 -> 标题 -> 鉴赏 -> CG 列表
    long_press(BSP_BTN_OK);            // CG 列表 -> 鉴赏
    CHECK(text_visible("章节鉴赏"), "鉴赏入口没回来");
    click(BSP_BTN_DOWN);               // 鉴赏 -> 章节鉴赏
    click(BSP_BTN_OK);
    CHECK(text_visible("章节鉴赏"), "章节鉴赏页没打开");
    CHECK(text_visible("已读") || text_visible("未读"), "章节行没渲染");

    // 第一次长 OK：回鉴赏入口（不是又进章节页；鉴赏入口自身就有“章节鉴赏”这一行，
    // 所以用章节页独有的“已读”标记来区分）。
    long_press(BSP_BTN_OK);
    CHECK(text_visible("CG 鉴赏") && !text_visible("已读"),
          "章节鉴赏长OK 应回到鉴赏入口(实际: %s)", visible_line_text());
    // 第二次长 OK：回标题，必须真的能出去
    long_press(BSP_BTN_OK);
    CHECK(text_visible("开始游戏"), "鉴赏入口长OK 应回到标题");

    // 重复来回 10 次：栈不能被填满/污染，最后一次仍能一层层退出
    for (int loop = 0; loop < 10; loop++) {
        title_select(4);
        click(BSP_BTN_OK);             // 标题 -> 鉴赏
        click(BSP_BTN_DOWN);
        click(BSP_BTN_OK);             // 鉴赏 -> 章节鉴赏
        long_press(BSP_BTN_OK);        // 章节 -> 鉴赏
        long_press(BSP_BTN_OK);        // 鉴赏 -> 标题
    }
    CHECK(text_visible("开始游戏"), "来回 10 次后仍应能回到标题");
}

// ---------------------------------------------------------------- P0r CG 大图页脚
// eden_ui_set_footer(EDEN_PAGE_CG, …) 之前是空操作（该页从未创建 footer 标签）。
static void test_cg_page_footer_visible(void)
{
    CHECK(boot(), "初始化失败");
    start_game();
    click(BSP_BTN_UP);
    click(BSP_BTN_UP);                 // 第三句解锁 CG 甲
    CHECK(strcmp(visible_line_text(), "第三句台词") == 0, "第三句没出现: %s",
          visible_line_text());
    goto_gallery_from_game();
    CHECK(text_visible("CG甲"), "group 0 应已解锁");
    click(BSP_BTN_OK);                 // 打开大图
    CHECK(text_visible("CG 01"), "大图页没打开(实际: %s)", visible_line_text());
    CHECK(text_visible("OK 返回缩略图"), "大图页必须有返回提示");
    click(BSP_BTN_OK);                 // 关闭大图
    CHECK(text_visible("CG 鉴赏"), "关闭大图应回到 CG 列表");
}


// 设置行单行且够宽；姓名不被切、底边不压面板。
static void test_settings_rows_and_name(void)
{
    CHECK(boot(), "初始化失败");
    start_game();
    // 游戏内：长 OK 进菜单（短 OK 已是“下一句”），菜单第 3 项 = 设置
    long_press(BSP_BTN_OK);        // 菜单（6 项：返回剧情/存档/读档/历史/设置/回标题）
    for (int i = 0; i < 4; ++i) click(BSP_BTN_DOWN);   // 落到“设置”
    click(BSP_BTN_OK);
    CHECK(eden_app_test_settings_rows(), "设置行必须单行且宽度 >= 200");
    CHECK(eden_app_test_name_label(), "姓名高度/底边不合法");
}

int main(void)
{
    test_page_origin();
    test_save_failure_keeps_old_slot();
    test_draw_failure_does_not_unlock();
    test_fast_forward_cancel();
    test_settings_rows_and_name();
    test_ui_only_inside_lock();
    test_cg_navigation_stack();
    test_gallery_chapter_round_trip();
    test_cg_page_footer_visible();
    test_unlock_retry_backoff_and_reload();
    test_unlock_flush_on_every_page();

    if (failures != 0) {
        fprintf(stderr, "%d/%d 项检查失败\n", failures, checks);
        return 1;
    }
    printf("eden app: OK (%d checks)\n", checks);
    return 0;
}
