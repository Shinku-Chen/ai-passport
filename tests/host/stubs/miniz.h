#pragma once
#include <stddef.h>
#include <stdint.h>
enum { TINFL_FLAG_PARSE_ZLIB_HEADER = 1, TINFL_FLAG_HAS_MORE_INPUT = 2,
       TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF = 4, TINFL_FLAG_COMPUTE_ADLER32 = 8 };
#define TINFL_DECOMPRESS_MEM_TO_MEM_FAILED ((size_t)(-1))
typedef uint8_t mz_uint8;
typedef uint32_t mz_uint32;
typedef struct { uint32_t m_state; } tinfl_decompressor;
typedef enum {
    TINFL_STATUS_BAD_PARAM = -3,
    TINFL_STATUS_ADLER32_MISMATCH = -2,
    TINFL_STATUS_FAILED = -1,
    TINFL_STATUS_DONE = 0,
    TINFL_STATUS_NEEDS_MORE_INPUT = 1,
    TINFL_STATUS_HAS_MORE_OUTPUT = 2
} tinfl_status;
#define tinfl_init(r) do { (r)->m_state = 0; } while (0)
size_t tinfl_decompress_mem_to_mem(void *pOut_buf, size_t out_buf_len, const void *pSrc_buf,
                                   size_t src_buf_len, int flags);
tinfl_status tinfl_decompress(tinfl_decompressor *r, const mz_uint8 *pIn_buf_next,
                              size_t *pIn_buf_size, mz_uint8 *pOut_buf_start,
                              mz_uint8 *pOut_buf_next, size_t *pOut_buf_size,
                              const mz_uint32 decomp_flags);
