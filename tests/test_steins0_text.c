// tests/test_steins0_text.c —— 排版/清理/UTF-8/存档/解锁的纯逻辑回归。
#include "steins0_pack.h"
#include "steins0_text.h"
#include "steins0_widths.h"

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

static void put_utf8(char *out, size_t *used, uint32_t cp)
{
    char tmp[4];
    const size_t n = sg0_utf8_encode(cp, tmp);
    memcpy(out + *used, tmp, n);
    *used += n;
    out[*used] = '\0';
}

static uint32_t measured(const char *text, size_t from, size_t to)
{
    uint32_t units = 0;
    size_t pos = from;
    while (pos < to) units += sg0_advance(sg0_utf8_next(text, to, &pos));
    return units;
}

int main(void)
{
    // UTF-8 roundtrip, including the PUA range.
    const uint32_t cps[] = {'A', 0x3001, 0xE000, 0xE2BE, 0xF1FF, 0x1F600};
    for (size_t i = 0; i < sizeof(cps) / sizeof(cps[0]); ++i) {
        char buf[8] = {0};
        const size_t n = sg0_utf8_encode(cps[i], buf);
        size_t pos = 0;
        CHECK(sg0_utf8_next(buf, n, &pos) == cps[i], "utf8 roundtrip U+%04X", cps[i]);
        CHECK(pos == n, "utf8 consume U+%04X", cps[i]);
    }
    CHECK(sg0_advance(0xE000 + 0xE8) == SG0_ADV_FULL, "PUA CJK advance");
    CHECK(sg0_advance('A') == 128, "ASCII A advance %u", sg0_advance('A'));
    CHECK(sg0_advance(0xE000 + 1) == 123, "PUA latin advance %u", sg0_advance(0xE000 + 1));
    CHECK(sg0_no_line_start(0xE000 + 191), "PUA 。 no-start");
    CHECK(sg0_no_line_start(0xE000 + 216), "PUA 「 no-start");
    CHECK(!sg0_no_line_start(0xE000 + 232), "PUA CJK allowed at line start");

    // clean: 外层「」、PUA 空格、连续空白。
    char cleaned[128];
    const char *raw = "\xEE\x80\x98\xEE\x80\x98\xEE\x82\xBE\xEE\x80\x80\xEE\x83\xBF"
                      "\xEE\x80\x99\xEE\x80\x99";
    const size_t clen = sg0_text_clean(raw, strlen(raw), cleaned, sizeof(cleaned));
    CHECK(clen > 0, "clean length");
    CHECK((unsigned char)cleaned[0] == 0xEE, "clean keeps PUA");

    // 禁则:构造一行 19 个全角字后跟句号,句号不得落在行首。
    char text[512];
    size_t used = 0;
    for (int i = 0; i < 19; ++i) put_utf8(text, &used, 0xE000 + 300 + i);
    put_utf8(text, &used, 0xE000 + 191);   // 。
    for (int i = 0; i < 5; ++i) put_utf8(text, &used, 0xE000 + 400 + i);
    sg0_pages_t pages;
    CHECK(sg0_text_layout(text, 304u * 16u, 3, &pages), "layout");
    CHECK(pages.pages >= 1, "pages");
    CHECK(pages.line_count[0] >= 1, "line count");
    // 第一行结尾应当是句号(挂在行尾)或少于 19 字;行首不能是句号。
    const size_t line0_end = pages.line_start[0][1];
    size_t pos = pages.line_start[0][0];
    uint32_t last = 0;
    while (pos < line0_end) last = sg0_utf8_next(text, line0_end, &pos);
    CHECK(last != 0xE000 + 191, "closing punctuation pushed off line start");
    size_t line1 = pages.line_start[0][1];
    if (line1 < strlen(text)) {
        size_t p = line1;
        CHECK(sg0_utf8_next(text, strlen(text), &p) != 0xE000 + 191,
              "punctuation must not start a line");
    }

    // 连续闭标点:第二个句号溢出时不能把第一个句号推到下一行行首。
    // 同时覆盖实际 U+3002 与源 PUA 句号(U+E0BF,SC3 id 191)的等价路径。
    const uint32_t punctuations[] = {0x3002u, 0xE000u + 191u};
    for (size_t pi = 0; pi < sizeof(punctuations) / sizeof(punctuations[0]); ++pi) {
        char sample[64];
        size_t at = 0;
        put_utf8(sample, &at, 'A'); put_utf8(sample, &at, 'B'); put_utf8(sample, &at, 'X');
        put_utf8(sample, &at, punctuations[pi]);
        put_utf8(sample, &at, punctuations[pi]);
        const uint16_t width = (uint16_t)(sg0_advance('A') + sg0_advance('B') +
                                          sg0_advance('X') + sg0_advance(punctuations[pi]));
        CHECK(sg0_text_layout(sample, width, 3, &pages), "continuous punctuation layout U+%04X",
              punctuations[pi]);
        CHECK(pages.pages >= 1 && pages.line_count[0] >= 2,
              "continuous punctuation split U+%04X", punctuations[pi]);
        for (int p = 0; p < pages.pages; ++p) {
            for (int l = 0; l < pages.line_count[p]; ++l) {
                const uint16_t start = pages.line_start[p][l];
                const uint16_t end = pages.line_start[p][l + 1];
                size_t cp_pos = start;
                const uint32_t first = sg0_utf8_next(sample, strlen(sample), &cp_pos);
                CHECK(!sg0_no_line_start(first), "U+%04X line p%d l%d starts U+%04X",
                      punctuations[pi], p, l, first);
                CHECK(end > start, "line advances");
            }
        }
        // 逐行拼接必须完整覆盖原文,不静默截断。
        char joined[64];
        size_t jn = 0;
        for (int p = 0; p < pages.pages; ++p) {
            for (int l = 0; l < pages.line_count[p]; ++l) {
                const uint16_t start = pages.line_start[p][l];
                const uint16_t end = pages.line_start[p][l + 1];
                memcpy(joined + jn, sample + start, end - start);
                jn += end - start;
            }
        }
        CHECK(jn == strlen(sample) && memcmp(joined, sample, jn) == 0,
              "continuous punctuation no loss U+%04X", punctuations[pi]);
    }

    // 全量语料:清理后文本必须被分页完整覆盖,行宽不超限。
    FILE *fh = fopen(STEINS0_PACK_PATH, "rb");
    CHECK(fh != NULL, "open pack");
    fseek(fh, 0, SEEK_END);
    long size = ftell(fh);
    fseek(fh, 0, SEEK_SET);
    uint8_t *blob = malloc((size_t)size);
    CHECK(fread(blob, 1, (size_t)size, fh) == (size_t)size, "read pack");
    fclose(fh);
    sg0_pack_t pack;
    CHECK(sg0_pack_open(&pack, blob, (uint32_t)size), "pack open");
    static uint8_t chunk[16384];
    uint32_t texts = 0;
    uint32_t pages_total = 0;
    uint32_t overlong = 0;
    uint32_t bad_line_starts = 0;
    uint32_t actual_3002_starts = 0;
    for (uint32_t ci = 0; ci < pack.sec[SG0_SEC_CHUNK].count; ++ci) {
        sg0_chunk_t info;
        CHECK(sg0_pack_chunk_at(&pack, (uint16_t)ci, &info), "chunk");
        uint32_t raw_len = 0;
        CHECK(sg0_pack_chunk_load(&pack, &info, chunk, sizeof(chunk), &raw_len), "inflate");
        for (uint8_t f = 0; f < info.frames; ++f) {
            sg0_frame_t frame;
            CHECK(sg0_pack_frame(chunk, raw_len, f, &frame), "frame");
            uint16_t len = 0;
            const char *src = sg0_pack_frame_text(chunk, raw_len, info.frames, &frame, &len);
            char clean[SG0_TEXT_BUFFER];
            const size_t n = sg0_text_clean(src, len, clean, sizeof(clean));
            CHECK(n < sizeof(clean), "clean overflow");
            sg0_text_layout(clean, 304u * 16u, 3, &pages);
            CHECK(pages.pages >= 1 && pages.pages <= SG0_TEXT_MAX_PAGES, "pages range");
            pages_total += pages.pages;
            // 每页行数内、每行推进 ≤ 行宽 + 一个全角(禁则挂行尾)。
            for (int p = 0; p < pages.pages; ++p) {
                for (int l = 0; l < pages.line_count[p]; ++l) {
                    const uint32_t width = measured(clean, pages.line_start[p][l],
                                                    pages.line_start[p][l + 1]);
                    if (width > 304u * 16u + SG0_ADV_FULL) ++overlong;
                    const uint16_t ls = pages.line_start[p][l];
                    if (ls > 0) {
                        size_t fp = ls;
                        const uint32_t first = sg0_utf8_next(clean, n, &fp);
                        if (first == 0x3002u) ++actual_3002_starts;
                        if (first == 0xE000u + 190u || first == 0xE000u + 191u) {
                            ++bad_line_starts;
                        }
                    }
                    CHECK(pages.line_start[p][l] <= pages.line_start[p][l + 1], "line order");
                }
                CHECK(pages.page_start[p] <= pages.page_start[p + 1], "page order");
            }
            ++texts;
        }
    }
    CHECK(overlong == 0, "overlong lines %u", overlong);
    CHECK(bad_line_starts == 0, "period PUA line starts %u (U+3002 %u)",
          bad_line_starts, actual_3002_starts);
    CHECK(actual_3002_starts == 0, "U+3002 line starts %u", actual_3002_starts);
    printf("text corpus: %u texts, %u pages, 0 overlong lines\n", texts, pages_total);
    printf("text line starts: bad_period=%u u3002=%u\n", bad_line_starts, actual_3002_starts);
    free(blob);

    // 存档:往返、CRC、版本、长度。
    sg0_save_t save;
    sg0_save_init(&save, 12, 3456, 1);
    save.meta.endings = 0x2A;
    save.meta.flags = 1;
    save.meta.group = 7;
    save.meta.date = 101123;
    save.meta.preview_len = 5;
    memcpy(save.meta.preview, "abcde", 5);
    save.meta.chapters[1] = 0x81;
    save.meta.gallery[2] = 0x42;
    sg0_save_rine_set(&save, 7, false);
    sg0_save_rine_set(&save, 9, true);
    save.rine_replied = 0x0004;
    save.rine_windows = 0x0008;
    save.pending_valid = 1;
    save.pending_node = 13;
    save.last_card = 4;
    save.event_node = 12;
    save.event_cursor = 3;
    save.event_valid = 1;
    uint8_t raw_save[SG0_SAVE_BYTES];
    CHECK(sg0_save_encode(&save, raw_save, sizeof(raw_save)) == SG0_SAVE_BYTES, "encode");
    sg0_save_t back;
    CHECK(sg0_save_decode(&back, raw_save, sizeof(raw_save)), "decode");
    CHECK(back.meta.node == 12 && back.meta.cursor == 3456 && back.meta.mode == 1,
          "save fields");
    CHECK(back.meta.date == 101123 && back.meta.group == 7, "save date/group");
    CHECK(back.meta.preview_len == 5 && memcmp(back.meta.preview, "abcde", 5) == 0,
          "preview");
    CHECK(sg0_save_rine_active(&back, 7) && !sg0_save_rine_read(&back, 7), "rine ref 7");
    CHECK(sg0_save_rine_active(&back, 9) && sg0_save_rine_read(&back, 9), "rine ref 9");
    CHECK(back.rine_replied == 4 && back.rine_windows == 8, "reply bits");
    CHECK(back.pending_valid == 1 && back.pending_node == 13 && back.last_card == 4,
          "pending/card");
    CHECK(back.event_valid == 1 && back.event_node == 12 && back.event_cursor == 3,
          "event position");
    raw_save[20] ^= 0x01;
    CHECK(!sg0_save_decode(&back, raw_save, sizeof(raw_save)), "CRC rejects corruption");
    raw_save[20] ^= 0x01;
    CHECK(!sg0_save_decode(&back, raw_save, sizeof(raw_save) - 1), "length rejects");
    uint8_t v2_save[SG0_SAVE_V2_BYTES];
    memset(v2_save, 0, sizeof(v2_save));
    memcpy(v2_save, raw_save, 64 + SG0_SAVE_RINE_BYTES * 2 + 7);
    v2_save[2] = 2;
    const uint16_t v2_core_crc = sg0_crc16(v2_save, 62);
    v2_save[62] = (uint8_t)(v2_core_crc & 0xFFu);
    v2_save[63] = (uint8_t)(v2_core_crc >> 8);
    const uint16_t v2_crc = sg0_crc16(v2_save + 64,
                                       (64 + SG0_SAVE_RINE_BYTES * 2 + 7) - 64);
    v2_save[SG0_SAVE_V2_BYTES - 2] = (uint8_t)(v2_crc & 0xFFu);
    v2_save[SG0_SAVE_V2_BYTES - 1] = (uint8_t)(v2_crc >> 8);
    CHECK(sg0_save_decode(&back, v2_save, sizeof(v2_save)), "version 2 accepted for migration");
    CHECK(back.event_valid == 0, "v2 has no event position");
    uint8_t v1_save[SG0_SAVE_CORE_BYTES];
    memcpy(v1_save, raw_save, sizeof(v1_save));
    v1_save[2] = 1;
    const uint16_t v1_crc = sg0_crc16(v1_save, 62);
    v1_save[62] = (uint8_t)(v1_crc & 0xFFu);
    v1_save[63] = (uint8_t)(v1_crc >> 8);
    CHECK(sg0_save_decode(&back, v1_save, sizeof(v1_save)), "version 1 accepted");
    CHECK(back.event_valid == 0 && back.last_card == 0xFFu, "v1 no RINE/event history");
    raw_save[2] = 9;
    CHECK(!sg0_save_decode(&back, raw_save, sizeof(raw_save)), "version rejects");

    // 解锁:位、并集、编码。
    sg0_unlock_t unlock;
    sg0_unlock_init(&unlock);
    CHECK(!sg0_unlock_has_chapter(&unlock, 3), "chapter initially locked");
    sg0_unlock_set_chapter(&unlock, 3);
    sg0_unlock_set_gallery(&unlock, 100);
    sg0_unlock_set_ending(&unlock, 5);
    sg0_unlock_set_flag(&unlock, 0);
    CHECK(sg0_unlock_has_chapter(&unlock, 3), "chapter set");
    CHECK(sg0_unlock_has_gallery(&unlock, 100), "gallery set");
    CHECK(sg0_unlock_has_ending(&unlock, 5), "ending set");
    sg0_unlock_t other;
    sg0_unlock_init(&other);
    sg0_unlock_set_chapter(&other, 7);
    CHECK(sg0_unlock_merge(&unlock, &other), "merge changed");
    CHECK(!sg0_unlock_merge(&unlock, &other), "merge idempotent");
    CHECK(sg0_unlock_has_chapter(&unlock, 7), "merge chapter");
    uint8_t raw_unlock[SG0_UNLOCK_BYTES];
    CHECK(sg0_unlock_encode(&unlock, raw_unlock, sizeof(raw_unlock)) == SG0_UNLOCK_BYTES,
          "unlock encode");
    sg0_unlock_t decoded;
    CHECK(sg0_unlock_decode(&decoded, raw_unlock, sizeof(raw_unlock)), "unlock decode");
    CHECK(decoded.endings == unlock.endings && decoded.flags == unlock.flags, "unlock fields");
    raw_unlock[10] ^= 0x08;
    CHECK(!sg0_unlock_decode(&decoded, raw_unlock, sizeof(raw_unlock)), "unlock CRC");

    if (failures) {
        fprintf(stderr, "FAIL %d checks\n", failures);
        return 1;
    }
    printf("PASS text/layout/save/unlock\n");
    return 0;
}
