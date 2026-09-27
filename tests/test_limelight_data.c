// tests/test_limelight_data.c —— 素材包 / 剧本包读取器的守卫测试。
//
// 两层:
//   1. 合成包(总是跑):在内存里搭最小的 LLSPK001 / LLMPK001,覆盖分块边界(块尾、
//      最后一块条数不足)、名字池、章节表、选项表、越界与坏输入的拒绝;
//   2. 真实包(给了路径才跑):核对 tools/limelight_script_pack.py 与
//      tools/limelight_material_pack.py 产物的条数、抽样正文、名字互查。
//      真实剧本包要用 --stored 打(宿主没有 inflate)。
//
// 用法: test_limelight_data [script_pack.bin] [material_pack.bin]

#include "limelight_assets.h"
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

#define SEC_TEXT_ 0u
#define SEC_CHUNK_ 1u
#define SEC_NAME_ 2u
#define SEC_NAMETEXT_ 3u
#define SEC_CHAPTER_ 4u
#define SEC_CHOICE_ 5u
#define SEC_META_ 6u

typedef struct {
    uint8_t blob[8192];
    uint32_t size;
} buffer_t;

static void wr16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value & 0xFF);
    p[1] = (uint8_t)(value >> 8);
}

static void wr32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value & 0xFF);
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

// ---------------------------------------------------------------------------
// 合成剧本包:3 条对白、每块 2 条、2 个名字、1 章、1 处选项
// ---------------------------------------------------------------------------

#define SYN_ENTRIES 3
#define SYN_CHUNK_ENTRIES 2

static const char SYN_TEXT_2[] = "hello 世界";       // 6 + 3 + 3 字节
static const char SYN_NAMES[] = "bgAspk";

static void push_section(buffer_t *buf, uint32_t type, const void *payload, uint32_t len,
                         uint32_t count)
{
    uint32_t off = buf->size;
    if (len) memcpy(buf->blob + off, payload, len);
    buf->size += len;
    while (buf->size % 4) buf->blob[buf->size++] = 0;
    uint8_t *table = buf->blob + 32 + 16 * type;
    wr32(table, type);
    wr32(table + 4, off);
    wr32(table + 8, count);
    wr32(table + 12, len);
}

static uint32_t build_synth_script(buffer_t *buf)
{
    memset(buf, 0, sizeof(*buf));
    memcpy(buf->blob, "LLSPK001", 8);
    wr32(buf->blob + 8, 1);
    wr32(buf->blob + 16, 7);
    wr32(buf->blob + 20, 64);                  // max_chunk_raw
    buf->size = 32 + 16 * 7;

    // 第 1 块(1..2 条)与第 2 块(1 条)
    uint8_t chunk0[64];
    uint32_t n = 0;
    chunk0[n++] = LIME_DLG_HAS_BG | LIME_DLG_HAS_SPEAKER;     // 1: 背景 + 说话人
    wr16(chunk0 + n, 0); n += 2;                             //   bg = "bgA"
    wr16(chunk0 + n, 1); n += 2;                             //   speaker = "spk"
    chunk0[n++] = LIME_DLG_HAS_BG | LIME_DLG_HAS_TEXT;        // 2: 背景 + 正文
    wr16(chunk0 + n, 1); n += 2;                             //   bg = "spk"(用第 2 个名字)
    wr16(chunk0 + n, (uint16_t)(sizeof(SYN_TEXT_2) - 1)); n += 2;
    memcpy(chunk0 + n, SYN_TEXT_2, sizeof(SYN_TEXT_2) - 1);
    n += sizeof(SYN_TEXT_2) - 1;
    uint32_t chunk0_len = n;

    uint8_t chunk1[32];
    n = 0;
    chunk1[n++] = LIME_DLG_HAS_TEXT | LIME_DLG_IS_CHOICE;     // 3: 选项
    wr16(chunk1 + n, 4); n += 2;
    memcpy(chunk1 + n, "pick", 4); n += 4;
    uint32_t chunk1_len = n;

    uint8_t text[512];
    memcpy(text, chunk0, chunk0_len);
    memcpy(text + chunk0_len, chunk1, chunk1_len);
    uint32_t text_off = buf->size;
    push_section(buf, SEC_TEXT_, text, chunk0_len + chunk1_len, 2);
    uint32_t text_len = chunk0_len + chunk1_len;

    uint8_t chunks[2 * 16];
    memset(chunks, 0, sizeof(chunks));
    wr32(chunks, 0);
    wr32(chunks + 4, chunk0_len);
    wr32(chunks + 8, 1);
    wr16(chunks + 12, SYN_CHUNK_ENTRIES);
    wr16(chunks + 14, 1);                       // STORED
    wr32(chunks + 16, chunk0_len);
    wr32(chunks + 20, chunk1_len);
    wr32(chunks + 24, 3);
    wr16(chunks + 28, 1);
    wr16(chunks + 30, 1);                       // STORED
    push_section(buf, SEC_CHUNK_, chunks, sizeof(chunks), 2);

    uint8_t names[2 * 8];
    wr32(names, 0);      wr16(names + 4, 3);    // "bgA"
    wr32(names + 8, 3);  wr16(names + 12, 3);   // "spk"
    push_section(buf, SEC_NAME_, names, sizeof(names), 2);
    push_section(buf, SEC_NAMETEXT_, SYN_NAMES, sizeof(SYN_NAMES) - 1, sizeof(SYN_NAMES) - 1);

    uint8_t chapter[6];
    wr32(chapter, 1);    wr16(chapter + 4, 1);  // 第 1 章从 id 1 开始,标签用 "spk"
    push_section(buf, SEC_CHAPTER_, chapter, sizeof(chapter), 1);

    uint8_t choice[6 + 2 * 6];
    wr32(choice, 3);     choice[4] = 2;         choice[5] = 0;
    wr16(choice + 6, 0); wr32(choice + 8, 1);   // 选项 1 -> id 1
    wr16(choice + 12, 1); wr32(choice + 14, 2); // 选项 2 -> id 2
    push_section(buf, SEC_CHOICE_, choice, sizeof(choice), 1);

    const char meta[] = "generator=synthetic\n";
    push_section(buf, SEC_META_, meta, sizeof(meta) - 1, 0);

    wr32(buf->blob + 12, buf->size);
    (void)text_off;
    (void)text_len;
    return buf->size;
}

static void test_synthetic_script(void)
{
    static buffer_t buf;
    static uint8_t cache[64];
    uint32_t size = build_synth_script(&buf);

    lime_script_t script;
    CHECK(lime_script_open(&script, buf.blob, size));
    CHECK(script.entries == SYN_ENTRIES);
    CHECK(script.chunk_entries == SYN_CHUNK_ENTRIES);
    CHECK(script.chunks == 2);
    CHECK(lime_script_chapters(&script) == 1);
    CHECK(lime_script_choices(&script) == 1);

    // 坏输入:魔数 / 长度 / 版本
    CHECK(!lime_script_open(&script, buf.blob, size - 1));
    uint8_t broken[8192];
    memcpy(broken, buf.blob, size);
    broken[0] = 'X';
    CHECK(!lime_script_open(&script, broken, size));
    memcpy(broken, buf.blob, size);
    wr32(broken + 8, 99);
    CHECK(!lime_script_open(&script, broken, size));

    CHECK(lime_script_open(&script, buf.blob, size));
    // 没有缓存:取对白必须失败
    lime_dialogue_t dlg;
    CHECK(!lime_script_dialogue(&script, 1, &dlg));
    lime_script_set_cache(&script, cache, sizeof(cache));
    CHECK(lime_script_dialogue(&script, 1, &dlg));
    CHECK((dlg.flags & (LIME_DLG_HAS_BG | LIME_DLG_HAS_SPEAKER)) ==
          (LIME_DLG_HAS_BG | LIME_DLG_HAS_SPEAKER));
    CHECK(dlg.text == NULL);
    const char *text = NULL;
    uint16_t len = 0;
    CHECK(lime_script_name(&script, dlg.bg, &text, &len) && len == 3 && memcmp(text, "bgA", 3) == 0);
    CHECK(lime_script_name(&script, dlg.speaker, &text, &len) && len == 3 &&
          memcmp(text, "spk", 3) == 0);
    CHECK(!lime_script_name(&script, 2, &text, &len));

    // 块边界:第 2 条在同一块,第 3 条在下一块(且尾块只有 1 条)
    CHECK(lime_script_dialogue(&script, 2, &dlg));
    CHECK(dlg.text_len == sizeof(SYN_TEXT_2) - 1);
    CHECK(memcmp(dlg.text, SYN_TEXT_2, dlg.text_len) == 0);
    CHECK(lime_script_dialogue(&script, 3, &dlg));
    CHECK((dlg.flags & LIME_DLG_IS_CHOICE) != 0);
    CHECK(dlg.text_len == 4 && memcmp(dlg.text, "pick", 4) == 0);
    CHECK(!lime_script_dialogue(&script, 0, &dlg));
    CHECK(!lime_script_dialogue(&script, SYN_ENTRIES + 1, &dlg));

    // 章节与选项
    uint32_t first_id = 0;
    uint16_t name_index = 0;
    CHECK(lime_script_chapter(&script, 0, &first_id, &name_index) && first_id == 1);
    CHECK(!lime_script_chapter(&script, 1, &first_id, &name_index));
    CHECK(lime_script_chapter_of(&script, 1) == 0);
    CHECK(lime_script_chapter_of(&script, 3) == 0);

    lime_choice_option_t options[4];
    uint8_t count = 0;
    uint32_t id = 0;
    CHECK(lime_script_choice(&script, 0, &id, &count, options, 4));
    CHECK(id == 3 && count == 2);
    CHECK(options[0].target == 1 && options[1].target == 2);
    CHECK(lime_script_choice_for(&script, 3, &count, options, 4) && count == 2);
    CHECK(!lime_script_choice_for(&script, 2, &count, options, 4));
    CHECK(!lime_script_choice(&script, 1, &id, &count, options, 4));

    // 缓存太小:拒绝而不是越界写
    static uint8_t tiny[8];
    lime_script_set_cache(&script, tiny, sizeof(tiny));
    CHECK(!lime_script_dialogue(&script, 1, &dlg));
}

// ---------------------------------------------------------------------------
// 合成素材包:1 张图 + 1 个立绘(4x4 遮罩) + meta 名字表
// ---------------------------------------------------------------------------

static const char SYN_META[] =
    "generator=synthetic\nentries=2\nblobs=2\n[names]\nbg\tbgA\nsprite\tsprA\n";

static uint32_t build_synth_assets(buffer_t *buf)
{
    memset(buf, 0, sizeof(*buf));
    memcpy(buf->blob, "LLMPK001", 8);
    wr32(buf->blob + 8, 1);
    wr32(buf->blob + 12, 3);            // 条目:1 图 + 1 立绘 + meta
    wr32(buf->blob + 16, 2);            // 段数
    wr16(buf->blob + 20, 30);           // quality
    wr16(buf->blob + 22, 240);
    wr16(buf->blob + 24, 320);
    wr16(buf->blob + 26, 214);
    wr32(buf->blob + 28, LIME_ASSET_FLAG_FILTERED | LIME_ASSET_FLAG_BG_CROPPED);

    const uint32_t index_len = 14 * 3 + 4;              // 立绘多一个 jpeg_len
    uint32_t off = 32 + index_len;
    while (off % 4) off++;
    const uint32_t image_off = off;
    const uint8_t image[4] = { 0xFF, 0xD8, 0xFF, 0xD9 };
    memcpy(buf->blob + image_off, image, sizeof(image));
    const uint32_t sprite_off = image_off + 4;
    const uint8_t jpeg[4] = { 0xFF, 0xD8, 0xFF, 0xD9 };
    memcpy(buf->blob + sprite_off, jpeg, sizeof(jpeg));
    const uint32_t mask_off = sprite_off + sizeof(jpeg);
    uint8_t mask[6];
    wr32(mask, 4);                       // raw 长度 = 4 字节(8x4 位图 = 1 字节/行)
    mask[4] = 4;                         // run 4
    mask[5] = 0xF0;                      // 值
    memcpy(buf->blob + mask_off, mask, sizeof(mask));
    const uint32_t meta_off = mask_off + sizeof(mask);
    memcpy(buf->blob + meta_off, SYN_META, sizeof(SYN_META) - 1);
    buf->size = meta_off + sizeof(SYN_META) - 1;

    uint8_t *p = buf->blob + 32;
    wr32(p, image_off);      wr32(p + 4, 4);      wr16(p + 8, 240);  wr16(p + 10, 214);
    p[12] = LIME_ASSET_KIND_IMAGE;  p[13] = 0;
    p += 14;
    wr32(p, sprite_off);     wr32(p + 4, 4 + 6);  wr16(p + 8, 8);    wr16(p + 10, 4);
    p[12] = LIME_ASSET_KIND_SPRITE; p[13] = 0;
    wr32(p + 14, 4);                              // jpeg_len
    p += 18;
    wr32(p, meta_off);       wr32(p + 4, sizeof(SYN_META) - 1);
    wr16(p + 8, 0);          wr16(p + 10, 0);
    p[12] = LIME_ASSET_KIND_META;   p[13] = 0;
    return buf->size;
}

static void test_synthetic_assets(void)
{
    static buffer_t buf;
    uint32_t size = build_synth_assets(&buf);

    lime_assets_t assets;
    CHECK(lime_assets_open(&assets, buf.blob, size));
    CHECK(lime_assets_count(&assets) == 3);
    CHECK(assets.bg_rows == 214 && assets.quality == 30);
    CHECK((assets.flags & LIME_ASSET_FLAG_FILTERED) != 0);

    lime_asset_t entry;
    CHECK(lime_assets_get(&assets, 0, &entry));
    CHECK(entry.kind == LIME_ASSET_KIND_IMAGE && entry.w == 240 && entry.h == 214);
    CHECK(lime_assets_get(&assets, 1, &entry));
    CHECK(entry.kind == LIME_ASSET_KIND_SPRITE && entry.w == 8 && entry.h == 4);
    CHECK(entry.jpeg_len == 4 && entry.len == 10);
    CHECK(!lime_assets_get(&assets, 3, &entry));

    // 名字表:带扩展名也能查到
    CHECK(lime_assets_find(&assets, "bgA") == 0);
    CHECK(lime_assets_find(&assets, "bgA.jpg") == 0);
    CHECK(lime_assets_find(&assets, "sprA") == 1);
    CHECK(lime_assets_find(&assets, "nope") == -1);
    uint16_t len = 0;
    const char *name = lime_assets_name(&assets, 1, &len);
    CHECK(name && len == 4 && memcmp(name, "sprA", 4) == 0);
    const char *family = lime_assets_family(&assets, 1, &len);
    CHECK(family && len == 6 && memcmp(family, "sprite", 6) == 0);
    CHECK(lime_assets_family(&assets, 3, &len) == NULL);          // 越界

    // 遮罩:RLE 解出 4 字节
    uint8_t raw[4];
    uint32_t mask_size = 0;
    const uint8_t *mask = lime_assets_mask(&assets, &entry, &mask_size);
    CHECK(mask && mask_size == 6);
    CHECK(lime_mask_decode(mask, mask_size, entry.w, entry.h, raw, sizeof(raw)));
    CHECK(raw[0] == 0xF0);
    // 尺寸不符 / 长度不符都要拒绝
    CHECK(!lime_mask_decode(mask, mask_size, 16, 4, raw, sizeof(raw)));
    CHECK(!lime_mask_decode(mask, 3, entry.w, entry.h, raw, sizeof(raw)));

    // 坏输入
    uint8_t broken[8192];
    memcpy(broken, buf.blob, size);
    broken[0] = 'X';
    CHECK(!lime_assets_open(&assets, broken, size));
    memcpy(broken, buf.blob, size);
    wr32(broken + 12, 0);
    CHECK(!lime_assets_open(&assets, broken, size));
    memcpy(broken, buf.blob, size);
    wr32(broken + 32 + 4, 0x7FFFFFFF);      // 条目长度越界
    CHECK(!lime_assets_open(&assets, broken, size));
}

// ---------------------------------------------------------------------------
// 真实包(本地/真机验收用;没给路径就跳过)
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
    static uint8_t *buffer;
    buffer = (uint8_t *)malloc((size_t)length);
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

static const char *name_of(const lime_script_t *script, uint16_t index)
{
    static char scratch[256];
    const char *text = NULL;
    uint16_t len = 0;
    if (!lime_script_name(script, index, &text, &len) || len >= sizeof(scratch)) return NULL;
    memcpy(scratch, text, len);
    scratch[len] = '\0';
    return scratch;
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
    CHECK(script.entries == 68229);
    CHECK(script.chunks == 273);
    CHECK(script.chunk_entries == 250);
    CHECK(script.max_chunk_raw < sizeof(cache));
    CHECK(lime_script_chapters(&script) == 230);
    CHECK(lime_script_choices(&script) == 8);
    lime_script_set_cache(&script, cache, sizeof(cache));

    lime_dialogue_t dlg;
    CHECK(lime_script_dialogue(&script, 1, &dlg));
    CHECK((dlg.flags & LIME_DLG_HAS_TEXT) == 0);
    CHECK(strcmp(name_of(&script, dlg.bg), "ecall") == 0);
    CHECK(strcmp(name_of(&script, dlg.speaker), "[CHAPTER0-1]") == 0);

    CHECK(lime_script_dialogue(&script, 2, &dlg));
    CHECK(strcmp(name_of(&script, dlg.bg), "画面_黒") == 0);
    CHECK(strcmp(name_of(&script, dlg.cg), "演出_ライト2.jpg") == 0);
    CHECK(dlg.text_len == strlen("那里充满了热烈的气氛。"));
    CHECK(memcmp(dlg.text, "那里充满了热烈的气氛。", dlg.text_len) == 0);

    // 末条与块边界
    CHECK(lime_script_dialogue(&script, script.entries, &dlg));
    CHECK(!lime_script_dialogue(&script, script.entries + 1, &dlg));
    CHECK(lime_script_dialogue(&script, 250, &dlg));
    CHECK(lime_script_dialogue(&script, 251, &dlg));   // 跨块仍能取到

    // 章节表首项与选项表
    uint32_t first_id = 0;
    uint16_t name_index = 0;
    CHECK(lime_script_chapter(&script, 0, &first_id, &name_index) && first_id == 1);
    CHECK(lime_script_chapter_of(&script, 68229) == lime_script_chapters(&script) - 1);

    lime_choice_option_t options[4];
    uint8_t count = 0;
    uint32_t id = 0;
    CHECK(lime_script_choice(&script, 0, &id, &count, options, 4));
    CHECK(count >= 2 && count <= 4);
    for (uint8_t i = 0; i < count; i++) {
        CHECK(options[i].target >= 1 && options[i].target <= script.entries);
    }
    free(blob);
}

static void test_real_assets(const char *path)
{
    uint32_t size = 0;
    uint8_t *blob = read_file(path, &size);
    if (!blob) {
        printf("skip real material pack (%s 读不到)\n", path);
        return;
    }
    static lime_assets_t assets;
    if (!lime_assets_open(&assets, blob, size)) {
        CHECK(!"lime_assets_open");
        free(blob);
        return;
    }
    CHECK(lime_assets_count(&assets) == 991);
    CHECK(assets.bg_rows == 214);

    int bg = lime_assets_find(&assets, "学園_教室a_夏");
    CHECK(bg >= 0);
    lime_asset_t entry;
    CHECK(lime_assets_get(&assets, (uint16_t)bg, &entry));
    CHECK(entry.w == 240 && entry.h == 214);

    int cg = lime_assets_find(&assets, "ev001a.jpg");     // 带扩展名
    CHECK(cg >= 0);
    CHECK(lime_assets_get(&assets, (uint16_t)cg, &entry));
    // CG 与背景一样只存 214 行:画布就是 240x214(RAM 预算决定的,见 limelight_image_math.h)
    CHECK(entry.w == 240 && entry.h == 214);

    // 立绘尺寸上限必须与固件的静态缓冲一致(SPRITE_MAX_PIXELS x 2 = 40KB)。
    // 打包器把立绘夹在这个上限内,固件按它申请缓冲;这里守住这条契约。
    for (uint16_t i = 0; i < lime_assets_count(&assets); i++) {
        lime_asset_t item;
        if (!lime_assets_get(&assets, i, &item) || item.kind != LIME_ASSET_KIND_SPRITE) continue;
        CHECK((uint32_t)item.w * item.h <= 20000);
        CHECK(item.w <= 168 && item.h <= 252);
    }

    int sprite = lime_assets_find(&assets, "hz01_1");
    CHECK(sprite >= 0);
    CHECK(lime_assets_get(&assets, (uint16_t)sprite, &entry));
    CHECK(entry.kind == LIME_ASSET_KIND_SPRITE);
    CHECK(entry.w <= 168 && entry.h <= 252 && entry.jpeg_len > 0);

    uint32_t mask_len = 0;
    const uint8_t *mask = lime_assets_mask(&assets, &entry, &mask_len);
    CHECK(mask != NULL && mask_len > 0);
    static uint8_t raw[168 * 252 / 8 + 8];
    uint32_t raw_len = lime_mask_raw_len(entry.w, entry.h);
    CHECK(raw_len <= sizeof(raw));
    CHECK(lime_mask_decode(mask, mask_len, entry.w, entry.h, raw, raw_len));
    int set_bits = 0;
    for (uint32_t i = 0; i < raw_len; i++) {
        for (int bit = 0; bit < 8; bit++) set_bits += (raw[i] >> bit) & 1;
    }
    CHECK(set_bits > 0);        // 遮罩不是全透明

    CHECK(lime_assets_find(&assets, "不存在的素材") == -1);

    // 家族前缀:鉴赏页靠它挑 cg 条目;四个家族都要有,且数量与打包器一致
    int counts[4] = { 0, 0, 0, 0 };
    const char *families[4] = { "bg", "cg", "sprite", "misc" };
    for (uint16_t i = 0; i < lime_assets_count(&assets); i++) {
        uint16_t family_len = 0;
        const char *family = lime_assets_family(&assets, i, &family_len);
        CHECK(family != NULL);
        for (int k = 0; k < 4; k++) {
            if (family_len == strlen(families[k]) &&
                memcmp(family, families[k], family_len) == 0) {
                counts[k]++;
                break;
            }
        }
    }
    CHECK(counts[0] > 100);      // 背景
    CHECK(counts[1] > 300);      // CG(鉴赏页)
    CHECK(counts[2] > 400);      // 立绘
    CHECK(counts[3] > 0);        // 标题图
    free(blob);
}

int main(int argc, char **argv)
{
    test_synthetic_script();
    test_synthetic_assets();
    if (argc > 2) {
        test_real_script(argv[1]);
        test_real_assets(argv[2]);
    } else {
        printf("note: 未给真实包路径,只跑了合成用例\n");
    }
    printf("limelight data layer: checks=%d failures=%d\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
