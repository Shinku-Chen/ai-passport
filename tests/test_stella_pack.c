// tests/test_stella_pack.c —— 真实资源包的全量引用/分页/存档审计（宿主机）。
#include "stella_model.h"
#include "stella_pack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef STELLA_PACK_PATH
#define STELLA_PACK_PATH "assets/stella/stella_pack.bin"
#endif

static int failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        failures++; \
    } \
} while (0)

static uint32_t section_offset(const uint8_t *pack, uint32_t type)
{
    const uint32_t n = stella_rd32(pack + 16);
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t *s = pack + 20 + i * 16;
        if (stella_rd32(s) == type) return stella_rd32(s + 4);
    }
    return 0;
}

static uint32_t section_entry(const uint8_t *pack, uint32_t type)
{
    const uint32_t n = stella_rd32(pack + 16);
    for (uint32_t i = 0; i < n; ++i) {
        if (stella_rd32(pack + 20 + i * 16) == type) return 20 + i * 16;
    }
    return 0;
}

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    uint8_t *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return buf;
}

int main(void)
{
    size_t size = 0;
    uint8_t *blob = read_file(STELLA_PACK_PATH, &size);
    CHECK(blob != NULL, "cannot read %s", STELLA_PACK_PATH);
    if (!blob) return 1;

    stella_pack_t pack;
    CHECK(stella_pack_open(&pack, blob, (uint32_t)size), "pack open failed");
    CHECK(pack.scene_count == 30, "scene_count=%u", (unsigned)pack.scene_count);
    CHECK(pack.group_count == 256, "group_count=%u", (unsigned)pack.group_count);
    CHECK(pack.gentry_count == 394, "gentry_count=%u", (unsigned)pack.gentry_count);
    CHECK(pack.asset_count > 900, "asset_count=%u", (unsigned)pack.asset_count);
    CHECK(pack.text_size > 500000, "text_size=%u", (unsigned)pack.text_size);

    const uint8_t *ops = NULL;
    unsigned text_ops = 0, choice_ops = 0, end_ops = 0;
    for (uint16_t s = 0; s < pack.scene_count; ++s) {
        stella_scene_t scene;
        CHECK(stella_pack_scene(&pack, s, &scene), "scene %u", (unsigned)s);
        uint16_t n = 0;
        ops = stella_pack_ops(&pack, &scene, &n);
        CHECK(ops != NULL && n == scene.op_count, "scene %u ops %u != %u", (unsigned)scene.id,
              (unsigned)n, (unsigned)scene.op_count);
        for (uint16_t i = 0; i < n; ++i) {
            stella_op_t op;
            CHECK(stella_pack_op(ops, n, i, &op), "scene %u op %u", (unsigned)scene.id, (unsigned)i);
            if (op.type == STELLA_OP_TEXT) {
                text_ops++;
                CHECK(op.text_len > 0 && op.text_off + op.text_len <= pack.text_size,
                      "text bounds scene %u op %u", (unsigned)scene.id, (unsigned)i);
                CHECK(op.bg == STELLA_NONE || op.bg < pack.asset_count, "bg %u", (unsigned)op.bg);
                CHECK(op.body_count <= 3, "bodies %u", (unsigned)op.body_count);
                for (uint8_t b = 0; b < op.body_count; ++b) {
                    CHECK(op.body[b] == STELLA_NONE || op.body[b] < pack.asset_count,
                          "body %u", (unsigned)op.body[b]);
                }
                char text[STELLA_TEXT_BUFFER];
                CHECK(stella_pack_text_copy(&pack, op.text_off, op.text_len, text, sizeof(text)),
                      "text copy scene %u op %u", (unsigned)scene.id, (unsigned)i);
                const int len = (int)strlen(text);
                CHECK(len > 0, "empty text scene %u op %u", (unsigned)scene.id, (unsigned)i);
                uint32_t off16[STELLA_MAX_PAGES + 1];
                const int pages16 = stella_text_pages(text, &STELLA_LAYOUT_16, off16,
                                                      STELLA_MAX_PAGES + 1);
                CHECK(pages16 >= 1 && pages16 <= STELLA_MAX_PAGES, "pages16=%d", pages16);
                CHECK(off16[pages16] == (uint32_t)len, "pages16 end %u != %d",
                      (unsigned)off16[pages16], len);
                for (int p = 0; p < pages16; ++p) {
                    CHECK(off16[p] < off16[p + 1], "page16 order scene %u op %u page %d",
                          (unsigned)scene.id, (unsigned)i, p);
                }
                uint32_t off20[STELLA_MAX_PAGES + 1];
                const int pages20 = stella_text_pages(text, &STELLA_LAYOUT_20, off20,
                                                      STELLA_MAX_PAGES + 1);
                CHECK(pages20 >= pages16, "20px pages %d < 16px %d", pages20, pages16);
                CHECK(off20[pages20] == (uint32_t)len, "pages20 end");
            } else if (op.type == STELLA_OP_CHOICE) {
                choice_ops++;
                CHECK(op.arg < n, "choice target %u >= %u", (unsigned)op.arg, (unsigned)n);
                CHECK(op.text_len > 0, "choice text");
            } else if (op.type == STELLA_OP_END) {
                end_ops++;
                CHECK(op.text_len > 0, "end label");
            } else {
                CHECK(0, "op type %u", (unsigned)op.type);
            }
        }
        if (scene.next != STELLA_NONE) {
            uint16_t next_idx;
            CHECK(stella_pack_find_scene(&pack, scene.next, &next_idx),
                  "scene %u next %u missing", (unsigned)scene.id, (unsigned)scene.next);
        }
    }
    CHECK(text_ops == 10975, "text_ops=%u", text_ops);
    CHECK(choice_ops == 1, "choice_ops=%u", choice_ops);
    CHECK(end_ops == 4, "end_ops=%u", end_ops);

    for (uint16_t g = 0; g < pack.group_count; ++g) {
        stella_group_t group;
        CHECK(stella_pack_group(&pack, g, &group), "group %u", (unsigned)g);
        for (uint16_t i = 0; i < group.count; ++i) {
            stella_gentry_t entry;
            CHECK(stella_pack_gentry(&pack, (uint16_t)(group.first + i), &entry),
                  "gentry %u/%u", (unsigned)g, (unsigned)i);
            CHECK(entry.asset < pack.asset_count, "group %u asset %u", (unsigned)g,
                  (unsigned)entry.asset);
        }
    }
    for (uint16_t a = 0; a < pack.asset_count; ++a) {
        stella_asset_t asset;
        CHECK(stella_pack_asset(&pack, a, &asset), "asset %u", (unsigned)a);
        CHECK(asset.jpeg_len > 0 && asset.w > 0 && asset.h > 0, "asset %u dims", (unsigned)a);
        if (asset.kind == STELLA_ASSET_SPRITE) {
            CHECK(asset.mask != NULL && asset.mask_len == (uint32_t)((asset.w + 7u) / 8u) * asset.h,
                  "sprite mask %u", (unsigned)a);
        }
    }

    // 坏包边界：段重叠、NAME 计数、mask 长度、文本越界、choice 目标。
    uint8_t *bad = malloc(size);
    CHECK(bad != NULL, "bad pack alloc");
    if (bad) {
        memcpy(bad, blob, size);
        const uint32_t entry = section_entry(bad, 0);
        bad[entry + 4] = 0; bad[entry + 5] = 0; bad[entry + 6] = 0; bad[entry + 7] = 0;
        stella_pack_t tmp;
        CHECK(!stella_pack_open(&tmp, bad, (uint32_t)size), "overlapping section accepted");

        memcpy(bad, blob, size);
        const uint32_t name_entry = section_entry(bad, 1);
        const uint32_t name_count = stella_rd32(bad + name_entry + 8);
        bad[name_entry + 8] = (uint8_t)((name_count - 1u) & 0xFFu);
        bad[name_entry + 9] = (uint8_t)(((name_count - 1u) >> 8) & 0xFFu);
        CHECK(!stella_pack_open(&tmp, bad, (uint32_t)size), "name count mismatch accepted");

        memcpy(bad, blob, size);
        const uint32_t asset_off = section_offset(bad, 5);
        stella_asset_t sprite;
        uint16_t sprite_idx = STELLA_NONE;
        for (uint16_t i = 0; i < pack.asset_count; ++i) {
            if (stella_pack_asset(&pack, i, &sprite) && sprite.kind == STELLA_ASSET_SPRITE) {
                sprite_idx = i;
                break;
            }
        }
        CHECK(sprite_idx != STELLA_NONE, "sprite for bad mask");
        if (sprite_idx != STELLA_NONE) {
            const uint32_t stride = ((uint32_t)sprite.w + 7u) / 8u;
            const uint32_t short_len = stride * sprite.h - 1u;
            uint8_t *mp = bad + asset_off + (size_t)sprite_idx * STELLA_ASSET_BYTES;
            mp[16] = (uint8_t)(short_len & 0xFFu); mp[17] = (uint8_t)((short_len >> 8) & 0xFFu);
            mp[18] = (uint8_t)((short_len >> 16) & 0xFFu); mp[19] = (uint8_t)((short_len >> 24) & 0xFFu);
            CHECK(!stella_pack_open(&tmp, bad, (uint32_t)size), "short mask accepted");
        }

        memcpy(bad, blob, size);
        const uint32_t ops_off = section_offset(bad, 3);
        uint16_t sid = 1, idx = 0;
        CHECK(stella_pack_find_scene(&pack, sid, &idx), "scene 1 for bad text");
        stella_scene_t sc;
        CHECK(stella_pack_scene(&pack, idx, &sc), "scene 1 record");
        uint8_t *op0 = bad + ops_off + sc.data_off;
        op0[12] = 0xFF; op0[13] = 0xFF; op0[14] = 0xFF; op0[15] = 0xFF;
        CHECK(!stella_pack_open(&tmp, bad, (uint32_t)size), "out-of-range text accepted");

        memcpy(bad, blob, size);
        CHECK(stella_pack_find_scene(&pack, 107, &idx), "scene 107");
        CHECK(stella_pack_scene(&pack, idx, &sc), "scene 107 record");
        uint8_t *ops107 = bad + section_offset(bad, 3) + sc.data_off;
        for (uint16_t i = 0; i < sc.op_count; ++i) {
            uint8_t *op = ops107 + (size_t)i * STELLA_OP_BYTES;
            if (op[0] == STELLA_OP_CHOICE) {
                op[18] = 0xFF; op[19] = 0xFF;
                break;
            }
        }
        CHECK(!stella_pack_open(&tmp, bad, (uint32_t)size), "out-of-range choice accepted");
        free(bad);
    }

    // 存档往返 + 损坏拒绝
    stella_save_t save = { .scene = 107, .op = 423, .page = 0, .flags = 3,
                           .mode = STELLA_MODE_STORY };
    uint8_t raw[STELLA_SAVE_BYTES];
    CHECK(stella_save_encode(&save, raw, sizeof(raw)) == STELLA_SAVE_BYTES, "save encode");
    stella_save_t back;
    CHECK(stella_save_decode(&back, raw, sizeof(raw)), "save decode");
    CHECK(back.scene == save.scene && back.op == save.op && back.page == save.page &&
          back.mode == STELLA_MODE_STORY, "save roundtrip");
    save.mode = STELLA_MODE_CHAPTER;
    CHECK(stella_save_encode(&save, raw, sizeof(raw)) == STELLA_SAVE_BYTES, "chapter encode");
    CHECK(stella_save_decode(&back, raw, sizeof(raw)) && back.mode == STELLA_MODE_CHAPTER,
          "chapter mode roundtrip");
    raw[8] ^= 0xFF;
    CHECK(!stella_save_decode(&back, raw, sizeof(raw)), "corrupt save accepted");
    // 旧开发版 v1 没有 mode：明确拒绝，不猜测 story/chapter。
    uint8_t v1[16] = {'S', 'T', 'L', 'S', 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(!stella_save_decode(&back, v1, sizeof(v1)), "v1 save accepted without mode");

    stella_unlock_t unlock;
    stella_unlock_init(&unlock);
    CHECK(!stella_unlock_has_scene(&unlock, 21), "unlock initial");
    stella_unlock_set_scene(&unlock, 21);
    stella_unlock_set_scene(&unlock, 107);
    CHECK(stella_unlock_has_scene(&unlock, 21) && stella_unlock_has_scene(&unlock, 107), "unlock scenes");
    stella_unlock_set_flag(&unlock, STELLA_FLAG_MAIN_CLEAR);
    CHECK(stella_unlock_has_flag(&unlock, STELLA_FLAG_MAIN_CLEAR), "unlock flag");
    stella_unlock_set_gallery(&unlock, 255);
    CHECK(stella_unlock_has_gallery(&unlock, 255) && !stella_unlock_has_gallery(&unlock, 254), "unlock gallery");

    free(blob);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("PASS: scenes=%u text=%u choice=%u end=%u groups=%u assets=%u\n",
           (unsigned)pack.scene_count, text_ops, choice_ops, end_ops,
           (unsigned)pack.group_count, (unsigned)pack.asset_count);
    return 0;
}
