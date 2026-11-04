// tests/test_gal_pack.c —— 资源包结构验证(用真实 pack 跑,宿主机测试)。
//
// 覆盖:魔数/版本/长度、段表边界、全部交叉引用(事件->台词/图片/章节/结局/跳转、
// 选项->目标/条件/动作、鉴赏->图片/解锁事件)、章节范围、字符串 NUL 结尾。
// 运行方式:test_gal_pack <gal_pack.bin>(tools/validate.sh 会传路径)。
#include "gal_pack.h"

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

static uint8_t *load_pack(const char *path, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    const long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return NULL; }
    uint8_t *buf = malloc((size_t)len);
    if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size = (uint32_t)len;
    return buf;
}

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "main/gal_data/gal_pack.bin";
    uint32_t size = 0;
    uint8_t *blob = load_pack(path, &size);
    if (!blob) return 2;

    gal_pack_t pack;
    const char *err = NULL;
    CHECK(gal_pack_open(&pack, blob, size, &err), "open failed: %s", err ? err : "?");

    // 内容规模(与源仓库逐项对齐的硬指标)
    CHECK(pack.line_count == 5457, "lines=%u", (unsigned)pack.line_count);
    CHECK(pack.choice_count == 15, "choices=%u", (unsigned)pack.choice_count);
    CHECK(pack.option_count == 53, "options=%u", (unsigned)pack.option_count);
    CHECK(pack.chapter_count == 18, "chapters=%u", (unsigned)pack.chapter_count);
    CHECK(pack.gallery_count == 33, "gallery=%u", (unsigned)pack.gallery_count);
    CHECK(pack.ending_count == 4, "endings=%u", (unsigned)pack.ending_count);
    CHECK(pack.image_count == 124, "images=%u", (unsigned)pack.image_count);
    CHECK(pack.title_image < pack.image_count, "title image %u", (unsigned)pack.title_image);

    // 每一句台词:字符串以 NUL 结尾,说话人下标合法。
    for (uint32_t i = 0; i < pack.line_count; ++i) {
        const char *text = gal_line_text(&pack, (uint16_t)i);
        CHECK(text != NULL && strlen(text) > 0, "line %u empty", (unsigned)i);
        const uint16_t speaker = pack.lines[i].speaker;
        CHECK(speaker == GAL_NONE || speaker < pack.name_count, "line %u speaker %u",
              (unsigned)i, (unsigned)speaker);
    }

    // 事件:类型与下标全部在范围内。
    int back_jumps = 0, cond_jumps = 0, sprite_events = 0, cg_events = 0;
    for (uint32_t i = 0; i < pack.event_count; ++i) {
        const gal_event_t *ev = &pack.events[i];
        switch (ev->kind) {
        case GAL_EV_LINE:
            CHECK(ev->b < pack.line_count, "event %u line %u", (unsigned)i, (unsigned)ev->b);
            break;
        case GAL_EV_BG:
        case GAL_EV_CG:
            CHECK(ev->b < pack.image_count, "event %u image %u", (unsigned)i, (unsigned)ev->b);
            if (ev->kind == GAL_EV_CG) cg_events++;
            break;
        case GAL_EV_SPRITE:
            CHECK(ev->a < 3, "event %u slot %u", (unsigned)i, (unsigned)ev->a);
            CHECK(ev->b == GAL_NONE || ev->b < pack.image_count, "event %u sprite %u",
                  (unsigned)i, (unsigned)ev->b);
            CHECK(ev->c <= GAL_POS_RIGHT, "event %u pos %u", (unsigned)i, (unsigned)ev->c);
            if (ev->b != GAL_NONE) sprite_events++;
            break;
        case GAL_EV_CHOICE:
            CHECK(ev->b < pack.choice_count, "event %u choice %u", (unsigned)i, (unsigned)ev->b);
            break;
        case GAL_EV_GOTO:
            CHECK(ev->b < pack.event_count, "event %u goto %u", (unsigned)i, (unsigned)ev->b);
            CHECK(ev->c == GAL_NONE || ev->c < pack.cond_count, "event %u cond %u",
                  (unsigned)i, (unsigned)ev->c);
            if (ev->b < i) back_jumps++;
            if (ev->c != GAL_NONE) cond_jumps++;
            break;
        case GAL_EV_VAR:
            CHECK(ev->a < GAL_MAX_VARS, "event %u var %u", (unsigned)i, (unsigned)ev->a);
            CHECK(ev->b <= GAL_OP_SUB, "event %u op %u", (unsigned)i, (unsigned)ev->b);
            break;
        case GAL_EV_CHAPTER:
            CHECK(ev->b < pack.chapter_count, "event %u chapter %u", (unsigned)i, (unsigned)ev->b);
            break;
        case GAL_EV_ENDING:
            CHECK(ev->b < pack.ending_count, "event %u ending %u", (unsigned)i, (unsigned)ev->b);
            break;
        case GAL_EV_UNLOCK_END:
            CHECK(ev->b < pack.ending_count, "event %u ending %u", (unsigned)i, (unsigned)ev->b);
            break;
        case GAL_EV_END:
            break;
        default:
            CHECK(0, "event %u bad kind %u", (unsigned)i, (unsigned)ev->kind);
            break;
        }
    }
    CHECK(back_jumps == 3, "back jumps %d", back_jumps);
    CHECK(cond_jumps == 4, "cond jumps %d", cond_jumps);
    CHECK(sprite_events > 600, "sprite ops %d", sprite_events);
    CHECK(cg_events >= 33, "cg events %d", cg_events);

    // 选项与选项组。
    for (uint32_t i = 0; i < pack.choice_count; ++i) {
        const gal_choice_t *ch = &pack.choices[i];
        CHECK(ch->first_opt + ch->opt_count <= pack.option_count, "choice %u range", (unsigned)i);
        CHECK(ch->opt_count >= 2, "choice %u count %u", (unsigned)i, ch->opt_count);
    }
    for (uint32_t i = 0; i < pack.option_count; ++i) {
        const gal_option_t *opt = &pack.options[i];
        CHECK(opt->text < pack.name_count, "option %u text", (unsigned)i);
        CHECK(opt->target < pack.event_count, "option %u target", (unsigned)i);
        CHECK(opt->cond == GAL_NONE || opt->cond < pack.cond_count, "option %u cond", (unsigned)i);
        CHECK(opt->act_count == 0 || opt->act_first + opt->act_count <= pack.action_count,
              "option %u actions", (unsigned)i);
    }

    // 章节范围与解锁事件。
    for (uint32_t i = 0; i < pack.chapter_count; ++i) {
        const gal_chapter_t *c = &pack.chapters[i];
        CHECK(c->first_event < pack.event_count, "chapter %u start", (unsigned)i);
        CHECK(c->first_event + c->event_count <= pack.event_count, "chapter %u range", (unsigned)i);
        CHECK(c->name < pack.name_count, "chapter %u name", (unsigned)i);
    }
    for (uint32_t i = 0; i < pack.ending_count; ++i) {
        const gal_ending_t *e = &pack.endings[i];
        CHECK(e->name < pack.name_count, "ending %u name", (unsigned)i);
        CHECK(e->text == GAL_NONE || e->text < pack.name_count, "ending %u text", (unsigned)i);
        CHECK(e->unlock_event < pack.event_count, "ending %u event", (unsigned)i);
    }
    for (uint32_t i = 0; i < pack.gallery_count; ++i) {
        const gal_gallery_t *g = &pack.gallery[i];
        CHECK(g->image < pack.image_count, "gallery %u image", (unsigned)i);
        CHECK(g->unlock_event < pack.event_count, "gallery %u event", (unsigned)i);
        CHECK(g->name < pack.name_count, "gallery %u name", (unsigned)i);
    }

    // 图片:top 必须是 320x150,条带 160x45,尺寸与掩码长度自洽。
    for (uint32_t i = 0; i < pack.image_count; ++i) {
        const uint8_t *top = NULL, *bot = NULL, *mt = NULL, *mb = NULL;
        uint32_t top_len = 0, bot_len = 0, mt_len = 0, mb_len = 0;
        uint16_t w = 0, h = 0;
        CHECK(gal_image_parts(&pack, (uint16_t)i, &top, &top_len, &bot, &bot_len,
                              &mt, &mt_len, &mb, &mb_len, &w, &h),
              "image %u parts", (unsigned)i);
        CHECK(top_len > 0 && bot_len > 0, "image %u jpeg", (unsigned)i);
        CHECK(w > 0 && w <= 320 && h > 0 && h <= 240, "image %u size %ux%u", (unsigned)i, w, h);
        if (mt_len) {
            const uint32_t rows_top = (h < 150 ? h : 150);
            CHECK(mt_len == ((w + 7u) / 8u) * rows_top, "image %u mask top %u", (unsigned)i,
                  (unsigned)mt_len);
            const uint32_t rows_bot = (h > 150 ? h - 150 : 0);
            CHECK(mb_len == ((w + 7u) / 8u) * rows_bot, "image %u mask bot %u", (unsigned)i,
                  (unsigned)mb_len);
        }
    }

    // 名字表的文本偏移全部落在 TEXT 段内,且字符串非空。
    for (uint32_t i = 0; i < pack.name_count; ++i) {
        uint32_t off = 0;
        memcpy(&off, pack.name_tbl + (size_t)i * 8, 4);
        CHECK(off < pack.text_size, "name %u offset %u", (unsigned)i, (unsigned)off);
        CHECK(strlen(gal_pack_text(&pack, off)) > 0, "name %u empty", (unsigned)i);
    }

    printf("pack ok: events=%u lines=%u choices=%u options=%u chapters=%u gallery=%u endings=%u "
           "images=%u back_jumps=%d cond_jumps=%d sprite_ops=%d\n",
           (unsigned)pack.event_count, (unsigned)pack.line_count, (unsigned)pack.choice_count,
           (unsigned)pack.option_count, (unsigned)pack.chapter_count,
           (unsigned)pack.gallery_count, (unsigned)pack.ending_count,
           (unsigned)pack.image_count, back_jumps, cond_jumps, sprite_events);
    free(blob);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    return 0;
}
