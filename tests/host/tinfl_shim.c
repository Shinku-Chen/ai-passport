// tests/host/tinfl_shim.c —— 宿主测试用的最小 INFLATE（RFC1951 + zlib 头）。
// 生产固件用 ESP32-C3 ROM miniz；这里只在 Windows/Linux 宿主上复现同一接口，
// 以便用真正的 C 代码读取真实的 zlib 压缩数据（正文块 + 补丁）。
#include "miniz.h"

#include <setjmp.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    const uint8_t *in;
    size_t inlen, inpos;
    uint32_t bitbuf;
    int bitcnt;
    uint8_t *out;
    size_t outlen, outpos;
    jmp_buf jb;
} state_t;

static void fail(state_t *s) { longjmp(s->jb, 1); }

static int bits(state_t *s, int need)
{
    while (s->bitcnt < need) {
        if (s->inpos >= s->inlen) {
            fail(s);
        }
        s->bitbuf |= (uint32_t)s->in[s->inpos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    int v = (int)(s->bitbuf & ((1u << need) - 1));
    s->bitbuf >>= need;
    s->bitcnt -= need;
    return v;
}

// ---- 规范 Huffman
typedef struct {
    uint16_t count[16];
    uint16_t symbol[288];
} huff_t;

static void build(huff_t *h, const uint8_t *lengths, int n)
{
    memset(h, 0, sizeof(*h));
    for (int i = 0; i < n; i++) {
        h->count[lengths[i]]++;
    }
    h->count[0] = 0;
    uint16_t offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; len++) {
        offs[len + 1] = offs[len] + h->count[len];
    }
    for (int i = 0; i < n; i++) {
        if (lengths[i]) {
            h->symbol[offs[lengths[i]]++] = (uint16_t)i;
        }
    }
}

static int decode(state_t *s, const huff_t *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= bits(s, 1);
        int count = h->count[len];
        if (code - count < first) {
            return h->symbol[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    fail(s);
    return -1;
}

static const uint16_t len_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                      2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dist_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                       193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                                       6145, 8193, 12289, 16385, 24577};
static const uint8_t dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                       6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static void put(state_t *s, uint8_t b)
{
    if (s->outpos >= s->outlen) {
        fail(s);
    }
    s->out[s->outpos++] = b;
}

static void codes(state_t *s, const huff_t *lit, const huff_t *dist)
{
    for (;;) {
        int sym = decode(s, lit);
        if (sym < 256) {
            put(s, (uint8_t)sym);
        } else if (sym == 256) {
            return;
        } else {
            sym -= 257;
            if (sym >= 29) {
                fail(s);
            }
            size_t len = len_base[sym] + (size_t)bits(s, len_extra[sym]);
            int dsym = decode(s, dist);
            if (dsym >= 30) {
                fail(s);
            }
            size_t d = dist_base[dsym] + (size_t)bits(s, dist_extra[dsym]);
            if (d > s->outpos) {
                fail(s);
            }
            while (len--) {
                put(s, s->out[s->outpos - d]);
            }
        }
    }
}

size_t tinfl_decompress_mem_to_mem(void *pOut_buf, size_t out_buf_len, const void *pSrc_buf,
                                   size_t src_buf_len, int flags)
{
    state_t st;
    memset(&st, 0, sizeof(st));
    st.in = (const uint8_t *)pSrc_buf;
    st.inlen = src_buf_len;
    st.out = (uint8_t *)pOut_buf;
    st.outlen = out_buf_len;
    if (setjmp(st.jb)) {
        return TINFL_DECOMPRESS_MEM_TO_MEM_FAILED;
    }
    if (flags & TINFL_FLAG_PARSE_ZLIB_HEADER) {
        if (st.inlen < 2) {
            return TINFL_DECOMPRESS_MEM_TO_MEM_FAILED;
        }
        uint8_t cmf = st.in[0], flg = st.in[1];
        if ((cmf & 0x0F) != 8 || ((cmf << 8) + flg) % 31 != 0) {
            return TINFL_DECOMPRESS_MEM_TO_MEM_FAILED;
        }
        st.in += 2;
        st.inlen -= 2;
    }
    int last;
    do {
        last = bits(&st, 1);
        int type = bits(&st, 2);
        if (type == 0) {
            st.bitbuf = 0;
            st.bitcnt = 0;
            if (st.inpos + 4 > st.inlen) {
                return TINFL_DECOMPRESS_MEM_TO_MEM_FAILED;
            }
            unsigned len = st.in[st.inpos] | (st.in[st.inpos + 1] << 8);
            st.inpos += 4;
            if (st.inpos + len > st.inlen) {
                return TINFL_DECOMPRESS_MEM_TO_MEM_FAILED;
            }
            for (unsigned i = 0; i < len; i++) {
                put(&st, st.in[st.inpos + i]);
            }
            st.inpos += len;
        } else if (type == 1) {
            uint8_t lens[288];
            for (int i = 0; i < 144; i++) lens[i] = 8;
            for (int i = 144; i < 256; i++) lens[i] = 9;
            for (int i = 256; i < 280; i++) lens[i] = 7;
            for (int i = 280; i < 288; i++) lens[i] = 8;
            huff_t lit, dist;
            build(&lit, lens, 288);
            uint8_t dl[30];
            for (int i = 0; i < 30; i++) dl[i] = 5;
            build(&dist, dl, 30);
            codes(&st, &lit, &dist);
        } else if (type == 2) {
            int hlit = bits(&st, 5) + 257;
            int hdist = bits(&st, 5) + 1;
            int hclen = bits(&st, 4) + 4;
            static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
                                              11, 4, 12, 3, 13, 2, 14, 1, 15};
            uint8_t cl[19];
            memset(cl, 0, sizeof(cl));
            for (int i = 0; i < hclen; i++) {
                cl[order[i]] = (uint8_t)bits(&st, 3);
            }
            huff_t clh;
            build(&clh, cl, 19);
            uint8_t lens[288 + 30];
            memset(lens, 0, sizeof(lens));
            int n = 0;
            while (n < hlit + hdist) {
                int sym = decode(&st, &clh);
                if (sym < 16) {
                    lens[n++] = (uint8_t)sym;
                } else if (sym == 16) {
                    if (n == 0) {
                        fail(&st);
                    }
                    uint8_t prev = lens[n - 1];
                    int r = 3 + bits(&st, 2);
                    while (r-- && n < hlit + hdist) {
                        lens[n++] = prev;
                    }
                } else if (sym == 17) {
                    int r = 3 + bits(&st, 3);
                    while (r-- && n < hlit + hdist) {
                        lens[n++] = 0;
                    }
                } else {
                    int r = 11 + bits(&st, 7);
                    while (r-- && n < hlit + hdist) {
                        lens[n++] = 0;
                    }
                }
            }
            huff_t lit, dist;
            build(&lit, lens, hlit);
            build(&dist, lens + hlit, hdist);
            codes(&st, &lit, &dist);
        } else {
            return TINFL_DECOMPRESS_MEM_TO_MEM_FAILED;
        }
    } while (!last);
    return st.outpos;
}

// 与生产代码同签名的一发式封装（生产用 ROM miniz，状态在静态区）
tinfl_status tinfl_decompress(tinfl_decompressor *r, const mz_uint8 *pIn_buf_next,
                              size_t *pIn_buf_size, mz_uint8 *pOut_buf_start,
                              mz_uint8 *pOut_buf_next, size_t *pOut_buf_size,
                              const mz_uint32 decomp_flags)
{
    (void)r;
    (void)pOut_buf_start;
    size_t produced = tinfl_decompress_mem_to_mem(pOut_buf_next, *pOut_buf_size, pIn_buf_next,
                                                  *pIn_buf_size, (int)decomp_flags);
    if (produced == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) {
        return TINFL_STATUS_FAILED;
    }
    *pOut_buf_size = produced;
    return TINFL_STATUS_DONE;
}
