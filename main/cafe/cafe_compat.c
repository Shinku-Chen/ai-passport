// main/cafe/cafe_compat.c
#include "cafe_compat.h"

#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "miniz.h"
#else
#include "miniz.h"
#endif

static uint32_t s_crc_table[256];
static bool s_crc_ready;

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        s_crc_table[i] = c;
    }
    s_crc_ready = true;
}

uint32_t cafe_crc32(uint32_t crc, const void *data, size_t len)
{
    if (!s_crc_ready) crc_init();
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = crc ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) c = s_crc_table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

bool cafe_inflate(void *dst, size_t dst_cap, size_t *out_len, const void *src, size_t src_len)
{
    if (!dst || !src || !out_len || src_len == 0) return false;
    *out_len = 0;
    // The ROM mem_to_mem helper reserves the complete tinfl_decompressor
    // (0x2b00 bytes on ESP32-C3) on the caller's stack, which overflows the
    // 4 KiB input task.  The low-level API keeps that state in an owned heap
    // block; its own frame is only 256 bytes (verified by ROM disassembly).
    // Each call owns its state, so independent callers need separate output
    // buffers but do not share decompressor state.
    tinfl_decompressor *state = (tinfl_decompressor *)malloc(sizeof(*state));
    if (!state) return false;
    tinfl_init(state);
    const mz_uint8 *in = (const mz_uint8 *)src;
    mz_uint8 *out = (mz_uint8 *)dst;
    size_t in_pos = 0;
    size_t out_pos = 0;
    bool ok = false;
    for (;;) {
        size_t in_left = src_len - in_pos;
        size_t out_left = dst_cap - out_pos;
        const size_t before_in = in_pos;
        const size_t before_out = out_pos;
        const tinfl_status st = tinfl_decompress(
            state, in + in_pos, &in_left, out, out + out_pos, &out_left,
            TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
        in_pos += in_left;
        out_pos += out_left;
        if (st == TINFL_STATUS_DONE) {
            // PARSE_ZLIB_HEADER makes tinfl verify the zlib Adler-32 trailer.
            ok = true;
            break;
        }
        if (st < TINFL_STATUS_DONE) break;  // BAD_PARAM / FAILED / ADLER32_MISMATCH
        // A truncated stream and a too-small output buffer must fail rather
        // than spin.  Any status must consume or produce at least one byte.
        if (st == TINFL_STATUS_NEEDS_MORE_INPUT && in_pos >= src_len) break;
        if (st == TINFL_STATUS_HAS_MORE_OUTPUT && out_pos >= dst_cap) break;
        if (in_pos == before_in && out_pos == before_out) break;
    }
    free(state);
    if (!ok) return false;
    *out_len = out_pos;
    return true;
}
