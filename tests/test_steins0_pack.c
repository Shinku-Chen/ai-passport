// tests/test_steins0_pack.c —— 真实包的结构/解压/边界回归(无 LVGL)。
#include "steins0_pack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef STEINS0_PACK_PATH
#define STEINS0_PACK_PATH "assets/steins0/steins0_pack.bin"
#endif

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); ++failures; } } while (0)

static uint8_t s_chunk[16384];

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static bool open_copy(const uint8_t *src, uint32_t size, uint32_t section, uint32_t field,
                      uint32_t value)
{
    uint8_t *copy = malloc(size);
    if (!copy) return false;
    memcpy(copy, src, size);
    wr32(copy + 20 + section * 16 + field, value);
    sg0_pack_t p;
    const bool ok = sg0_pack_open(&p, copy, size);
    free(copy);
    return ok;
}

int main(void)
{
    FILE *fh = fopen(STEINS0_PACK_PATH, "rb");
    if (!fh) {
        fprintf(stderr, "cannot open %s\n", STEINS0_PACK_PATH);
        return 1;
    }
    fseek(fh, 0, SEEK_END);
    long size = ftell(fh);
    fseek(fh, 0, SEEK_SET);
    uint8_t *blob = malloc((size_t)size);
    CHECK(blob != NULL, "malloc pack");
    CHECK(fread(blob, 1, (size_t)size, fh) == (size_t)size, "read pack");
    fclose(fh);

    sg0_pack_t pack;
    CHECK(sg0_pack_open(&pack, blob, (uint32_t)size), "open pack");
    CHECK(pack.sec[SG0_SEC_NODE].count == 41, "nodes %u", pack.sec[SG0_SEC_NODE].count);
    CHECK(pack.sec[SG0_SEC_CHUNK].count == 1567, "chunks %u", pack.sec[SG0_SEC_CHUNK].count);
    CHECK(pack.sec[SG0_SEC_CHOICE].count == 28, "choices %u", pack.sec[SG0_SEC_CHOICE].count);
    CHECK(pack.sec[SG0_SEC_ENDING].count == 12, "ending nodes %u",
          pack.sec[SG0_SEC_ENDING].count);
    CHECK(pack.sec[SG0_SEC_START].count == 104, "starts %u", pack.sec[SG0_SEC_START].count);
    CHECK(pack.sec[SG0_SEC_GG].count == 106, "gallery groups %u", pack.sec[SG0_SEC_GG].count);
    CHECK(pack.sec[SG0_SEC_GE].count == 198, "gallery entries %u", pack.sec[SG0_SEC_GE].count);
    CHECK(pack.sec[SG0_SEC_TITLE].count == 14, "titles %u", pack.sec[SG0_SEC_TITLE].count);
    CHECK(pack.sec[SG0_SEC_RMSG].count == 1385, "rine msgs %u", pack.sec[SG0_SEC_RMSG].count);
    CHECK(pack.sec[SG0_SEC_RREPLY].count == 16, "rine replies %u", pack.sec[SG0_SEC_RREPLY].count);
    CHECK(pack.sec[SG0_SEC_RCHOICE].count == 48, "rine choices %u", pack.sec[SG0_SEC_RCHOICE].count);
    CHECK(pack.sec[SG0_SEC_RSEQ].count == 504, "rine seq %u", pack.sec[SG0_SEC_RSEQ].count);
    CHECK(pack.sec[SG0_SEC_CARD].count == 14, "cards %u", pack.sec[SG0_SEC_CARD].count);
    CHECK(sg0_pack_meta_u16(&pack, "count_nodes", 0) == 41, "meta count_nodes");
    CHECK(sg0_pack_meta_u16(&pack, "count_chunks_unique", 0) == 853,
          "meta count_chunks_unique");
    CHECK(sg0_pack_meta_u16(&pack, "count_frames_unique", 0) == 33549,
          "meta count_frames_unique");

    // 结构负例:段重叠、计数宽度、段起点、RINE 引用越界都必须拒绝。
    uint32_t off1 = rd32(blob + 20 + SG0_SEC_STRT * 16 + 4);
    CHECK(!open_copy(blob, (uint32_t)size, SG0_SEC_STRT, 4, off1 - 8), "reject overlap");
    uint32_t count1 = rd32(blob + 20 + SG0_SEC_STRT * 16 + 8);
    CHECK(!open_copy(blob, (uint32_t)size, SG0_SEC_STRT, 8, count1 + 1), "reject count width");
    CHECK(!open_copy(blob, (uint32_t)size, SG0_SEC_TEXT, 4, 0), "reject section in header");
    uint32_t strt = rd32(blob + 20 + SG0_SEC_STRT * 16 + 8);
    uint8_t *copy = malloc((size_t)size);
    CHECK(copy != NULL, "neg copy");
    if (copy) {
        memcpy(copy, blob, (size_t)size);
        const uint32_t rmsg = rd32(copy + 20 + SG0_SEC_RMSG * 16 + 4);
        copy[rmsg + 10] = (uint8_t)strt;
        copy[rmsg + 11] = (uint8_t)(strt >> 8);
        sg0_pack_t bad;
        CHECK(!sg0_pack_open(&bad, copy, (uint32_t)size), "reject rmsg text ref");
        memcpy(copy, blob, (size_t)size);
        const uint32_t choice = rd32(copy + 20 + SG0_SEC_RCHOICE * 16 + 4);
        const uint32_t msgs = rd32(copy + 20 + SG0_SEC_RMSG * 16 + 8);
        copy[choice] = (uint8_t)msgs;
        copy[choice + 1] = (uint8_t)(msgs >> 8);
        CHECK(!sg0_pack_open(&bad, copy, (uint32_t)size), "reject rchoice selected ref");
        memcpy(copy, blob, (size_t)size);
        const uint32_t event = rd32(copy + 20 + SG0_SEC_EVENT * 16 + 4);
        uint32_t first_msg_event = 0xFFFFFFFFu;
        for (uint32_t i = 0; i < rd32(copy + 20 + SG0_SEC_EVENT * 16 + 8); ++i) {
            const uint32_t at = event + i * 16;
            if (copy[at + 2] == 1) { first_msg_event = at; break; }
        }
        CHECK(first_msg_event != 0xFFFFFFFFu, "find message event");
        if (first_msg_event != 0xFFFFFFFFu) {
            copy[first_msg_event + 12] = (uint8_t)msgs;
            copy[first_msg_event + 13] = (uint8_t)(msgs >> 8);
            CHECK(!sg0_pack_open(&bad, copy, (uint32_t)size), "reject event msg ref");
        }
        free(copy);
    }

    // 索引基本契约:count / count+1 必须安全拒绝,不能因 count 内字段坏而越段。
    const uint32_t nc = pack.sec[SG0_SEC_NODE].count;
    const uint32_t vc = pack.sec[SG0_SEC_VIEW].count;
    const uint32_t pc = pack.sec[SG0_SEC_PART].count;
    const uint32_t cc = pack.sec[SG0_SEC_CHOICE].count;
    const uint32_t dc = pack.sec[SG0_SEC_COND].count;
    const uint32_t sc = pack.sec[SG0_SEC_START].count;
    sg0_node_t n; sg0_view_t v; sg0_part_t pt; sg0_choice_t ch; sg0_cond_t cd; sg0_start_t st;
    CHECK(!sg0_pack_node(&pack, (uint8_t)nc, &n), "node count rejected");
    CHECK(!sg0_pack_node(&pack, (uint8_t)(nc + 1), &n), "node count+1 rejected");
    CHECK(!sg0_pack_view(&pack, (uint16_t)vc, &v), "view count rejected");
    CHECK(!sg0_pack_part(&pack, (uint16_t)pc, &pt), "part count rejected");
    CHECK(!sg0_pack_choice(&pack, (uint16_t)cc, &ch), "choice count rejected");
    CHECK(!sg0_pack_cond(&pack, (uint16_t)dc, &cd), "cond count rejected");
    CHECK(!sg0_pack_start(&pack, (uint16_t)sc, &st), "start count rejected");

    // 记录 0 原访问成功 -> 只改该记录 -> 同一 getter 拒绝。
    copy = malloc((size_t)size);
    CHECK(copy != NULL, "mut copy");
    if (copy) {
        memcpy(copy, blob, (size_t)size);
        sg0_pack_t mp;
        CHECK(sg0_pack_open(&mp, copy, (uint32_t)size), "mut base open");
        CHECK(sg0_pack_node(&mp, 0, &n), "node0 before");
        wr32(copy + mp.sec[SG0_SEC_NODE].offset, mp.sec[SG0_SEC_CHUNK].count + 1);
        CHECK(!sg0_pack_node(&mp, 0, &n), "node0 chunk_first rejected");

        memcpy(copy, blob, (size_t)size);
        CHECK(sg0_pack_view(&mp, 0, &v), "view0 before");
        const uint32_t vref = mp.sec[SG0_SEC_LAYERREF].count;
        copy[mp.sec[SG0_SEC_VIEW].offset] = (uint8_t)vref;
        copy[mp.sec[SG0_SEC_VIEW].offset + 1] = (uint8_t)(vref >> 8);
        copy[mp.sec[SG0_SEC_VIEW].offset + 2] = 1;
        CHECK(!sg0_pack_view(&mp, 0, &v), "view0 layer range rejected");

        memcpy(copy, blob, (size_t)size);
        CHECK(sg0_pack_part(&mp, 0, &pt), "part0 before");
        wr32(copy + mp.sec[SG0_SEC_PART].offset, mp.sec[SG0_SEC_BLOB].size);
        CHECK(!sg0_pack_part(&mp, 0, &pt), "part0 blob off rejected");

        memcpy(copy, blob, (size_t)size);
        CHECK(sg0_pack_gallery_entry(&mp, 0, &(uint16_t){0}), "ge0 before");
        wr32(copy + mp.sec[SG0_SEC_GE].offset, mp.sec[SG0_SEC_ASSETID].count);
        CHECK(!sg0_pack_gallery_entry(&mp, 0, &(uint16_t){0}), "ge0 asset rejected");

        memcpy(copy, blob, (size_t)size);
        CHECK(sg0_pack_choice(&mp, 0, &ch), "choice0 before");
        wr32(copy + mp.sec[SG0_SEC_CHOICE].offset + 2, mp.sec[SG0_SEC_STRT].count);
        CHECK(!sg0_pack_choice(&mp, 0, &ch), "choice0 heading rejected");

        memcpy(copy, blob, (size_t)size);
        CHECK(sg0_pack_cond(&mp, 0, &cd), "cond0 before");
        copy[mp.sec[SG0_SEC_COND].offset + 1] = (uint8_t)mp.sec[SG0_SEC_NODE].count;
        CHECK(!sg0_pack_cond(&mp, 0, &cd), "cond0 yes rejected");

        memcpy(copy, blob, (size_t)size);
        CHECK(sg0_pack_start(&mp, 0, &st), "start0 before");
        copy[mp.sec[SG0_SEC_START].offset] = (uint8_t)mp.sec[SG0_SEC_NODE].count;
        CHECK(!sg0_pack_start(&mp, 0, &st), "start0 node rejected");

        memcpy(copy, blob, (size_t)size);
        CHECK(sg0_pack_choice(&mp, 0, &ch), "choice0 count before");
        copy[mp.sec[SG0_SEC_CHOICE].offset + 6] = 3;
        CHECK(!sg0_pack_choice(&mp, 0, &ch), "choice0 count3 rejected");

        memcpy(copy, blob, (size_t)size);
        sg0_event_t ev;
        CHECK(sg0_pack_event(&mp, 0, &ev), "event0 before");
        copy[mp.sec[SG0_SEC_EVENT].offset + 2] = 99;
        CHECK(!sg0_pack_event(&mp, 0, &ev), "event type99 rejected");

        memcpy(copy, blob, (size_t)size);
        sg0_ending_t ending;
        CHECK(sg0_pack_ending(&mp, 0, &ending), "ending0 before");
        copy[mp.sec[SG0_SEC_ENDING].offset + 2] = 99;
        CHECK(!sg0_pack_ending(&mp, 0, &ending), "ending id99 rejected");

        memcpy(copy, blob, (size_t)size);
        const char *text = NULL; uint16_t text_len = 0;
        CHECK(sg0_pack_string(&mp, 0, &text, &text_len), "str0 before");
        wr32(copy + mp.sec[SG0_SEC_STRT].offset, mp.sec[SG0_SEC_TEXT].size);
        CHECK(!sg0_pack_string(&mp, 0, &text, &text_len), "str0 offset rejected");
        free(copy);
    }

    // 全量逐块解压:每个流都校验 adler32,并核对帧/文本边界。
    uint32_t frames = 0;
    uint32_t text_bytes = 0;
    uint32_t max_raw = 0;
    uint32_t view_checks = 0;
    for (uint32_t i = 0; i < pack.sec[SG0_SEC_CHUNK].count; ++i) {
        sg0_chunk_t chunk;
        CHECK(sg0_pack_chunk_at(&pack, (uint16_t)i, &chunk), "chunk %u", i);
        if (chunk.raw_len > sizeof(s_chunk)) {
            CHECK(0, "chunk %u raw %u > buf", i, chunk.raw_len);
            continue;
        }
        uint32_t raw_len = 0;
        CHECK(sg0_pack_chunk_load(&pack, &chunk, s_chunk, sizeof(s_chunk), &raw_len),
              "inflate chunk %u", i);
        CHECK(raw_len == chunk.raw_len, "chunk %u raw len %u != %u", i, raw_len,
              chunk.raw_len);
        if (raw_len > max_raw) max_raw = raw_len;
        CHECK((uint32_t)chunk.frames * 12u <= raw_len, "chunk %u frames %u raw %u", i,
              chunk.frames, raw_len);
        for (uint32_t f = 0; f < chunk.frames; ++f) {
            sg0_frame_t frame;
            CHECK(sg0_pack_frame(s_chunk, raw_len, (uint8_t)f, &frame), "frame %u:%u", i, f);
            uint16_t len = 0;
            const char *text = sg0_pack_frame_text(s_chunk, raw_len, chunk.frames, &frame, &len);
            CHECK(text != NULL, "frame text %u:%u", i, f);
            CHECK(len > 0, "empty text %u:%u", i, f);
            CHECK(len < 512, "text too long %u:%u (%u)", i, f, len);
            text_bytes += len;
            const char *speaker = NULL;
            uint16_t slen = 0;
            CHECK(sg0_pack_speaker(&pack, frame.speaker, &speaker, &slen), "speaker %u:%u",
                  i, f);
            sg0_view_t view;
            CHECK(sg0_pack_view(&pack, frame.view, &view), "view %u:%u", i, f);
            for (uint8_t l = 0; l < view.layer_count; ++l) {
                sg0_layer_t layer;
                CHECK(sg0_pack_layer(&pack, (uint16_t)(view.layer_first + l), &layer),
                      "layer %u:%u:%u", i, f, l);
                sg0_assetid_t asset;
                CHECK(sg0_pack_asset(&pack, layer.asset, &asset), "asset %u", layer.asset);
                CHECK(asset.part_count >= 1 && asset.part_count <= 2, "parts %u",
                      asset.part_count);
                CHECK(layer.w > 0 && layer.h > 0, "layer box %ux%u", layer.w, layer.h);
                CHECK(layer.opacity <= 1000, "opacity %u", layer.opacity);
                for (uint8_t p = 0; p < asset.part_count; ++p) {
                    sg0_part_t part;
                    CHECK(sg0_pack_part(&pack, (uint16_t)(asset.part_first + p), &part),
                          "part");
                    CHECK(part.len > 0 && part.w > 0 && part.h > 0, "part size");
                    CHECK(part.off <= pack.sec[SG0_SEC_BLOB].size &&
                          part.len <= pack.sec[SG0_SEC_BLOB].size - part.off, "blob bounds");
                }
                ++view_checks;
            }
            ++frames;
        }
    }
    CHECK(frames == 61810, "frames read %u", frames);
    CHECK(text_bytes > 1800000, "text bytes %u", text_bytes);
    printf("pack: %ld bytes, %u chunk refs, %u frames, %u text bytes, max raw %u, "
           "%u layer checks\n", size, pack.sec[SG0_SEC_CHUNK].count, frames, text_bytes,
           max_raw, view_checks);

    // 选项/条件/结局/章节入口
    for (uint32_t i = 0; i < pack.sec[SG0_SEC_CHOICE].count; ++i) {
        sg0_choice_t choice;
        CHECK(sg0_pack_choice(&pack, (uint16_t)i, &choice), "choice %u", i);
        CHECK(choice.count == 2, "choice %u count %u", i, choice.count);
        CHECK(choice.to[0] < 41 && choice.to[1] < 41, "choice target");
        CHECK(choice.action[0] <= 1 && choice.action[1] <= 1, "choice action");
        const char *text = NULL;
        uint16_t len = 0;
        CHECK(sg0_pack_string(&pack, choice.text[0], &text, &len), "choice text");
        CHECK(len > 0, "choice text empty");
    }
    for (uint32_t i = 0; i < pack.sec[SG0_SEC_START].count; ++i) {
        sg0_start_t start;
        CHECK(sg0_pack_start(&pack, (uint16_t)i, &start), "start %u", i);
        CHECK(start.node < 41, "start node");
        sg0_node_t node;
        CHECK(sg0_pack_node(&pack, start.node, &node), "start node load");
        CHECK(start.index < 65535, "start index");
        CHECK(start.group < 14, "start group");
    }
    for (uint32_t i = 0; i < pack.sec[SG0_SEC_GG].count; ++i) {
        sg0_gallery_group_t group;
        CHECK(sg0_pack_gallery_group(&pack, (uint8_t)i, &group), "gallery %u", i);
        CHECK(group.count >= 1, "gallery count");
        for (uint8_t k = 0; k < group.count; ++k) {
            uint16_t asset = 0;
            CHECK(sg0_pack_gallery_entry(&pack, (uint16_t)(group.first + k), &asset),
                  "gallery entry");
            CHECK(asset < pack.sec[SG0_SEC_ASSETID].count, "gallery asset");
        }
    }
    for (uint8_t g = 0; g < 14; ++g) {
        const char *title = NULL;
        uint16_t len = 0;
        CHECK(sg0_pack_title(&pack, g, &title, &len), "title %u", g);
        CHECK(len > 0, "title empty %u", g);
    }

    free(blob);
    if (failures) {
        fprintf(stderr, "FAIL %d checks\n", failures);
        return 1;
    }
    printf("PASS pack structure/inflate/coverage\n");
    return 0;
}
