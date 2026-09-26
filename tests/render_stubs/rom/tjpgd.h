// Host-test stub: 组合测试不走 JPEG 解码,给最小可编译的 TJpgDec 声明。
#pragma once
#include <stdint.h>
typedef unsigned char BYTE;
typedef unsigned int UINT;
typedef uint16_t WORD;
typedef enum { JDR_OK = 0, JDR_INTR, JDR_INP, JDR_MEM1, JDR_MEM2, JDR_PAR, JDR_FMT1, JDR_FMT2, JDR_FMT3 } JRESULT;
typedef struct { WORD left, right, top, bottom; } JRECT;
typedef struct JDEC JDEC;
struct JDEC { uint16_t width, height; void *device; };
static inline JRESULT jd_prepare(JDEC *jd, UINT (*infunc)(JDEC *, BYTE *, UINT), void *inbuf,
                                 UINT inbuf_size, void *work) {
    (void)jd; (void)infunc; (void)inbuf; (void)inbuf_size; (void)work; return JDR_FMT3;
}
static inline JRESULT jd_decomp(JDEC *jd, UINT (*outfunc)(JDEC *, void *, JRECT *), BYTE scale) {
    (void)jd; (void)outfunc; (void)scale; return JDR_FMT3;
}
