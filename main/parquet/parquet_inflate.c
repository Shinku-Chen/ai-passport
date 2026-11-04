#include "parquet_inflate.h"

#include "miniz.h"

#include <stdbool.h>

static tinfl_decompressor s_inflate;
static bool s_busy;

size_t pq_inflate(void *dst, size_t dst_len, const uint8_t *src, size_t src_len)
{
    if (s_busy) {
        return PQ_INFLATE_FAILED;
    }
    s_busy = true;
    tinfl_init(&s_inflate);
    size_t in_bytes = src_len;
    size_t out_bytes = dst_len;
    tinfl_status st = tinfl_decompress(&s_inflate, (const mz_uint8 *)src, &in_bytes,
                                       (mz_uint8 *)dst, (mz_uint8 *)dst, &out_bytes,
                                       TINFL_FLAG_PARSE_ZLIB_HEADER |
                                           TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    size_t produced = (st == TINFL_STATUS_DONE) ? out_bytes : PQ_INFLATE_FAILED;
    s_busy = false;
    return produced;
}

size_t pq_inflate_state_bytes(void) { return sizeof(s_inflate); }
