#include "nine_op_log.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    nine_media_op_stats_t st;
    memset(&st, 0, sizeof(st));
    st.wall_ms = 100;
    st.position_ms = 90;
    st.duration_ms = 200;
    st.frames = 7;
    st.late_skips = 2;
    st.crc_checks = 9;
    st.jpeg_decodes = 5;
    st.decode_max_us = 11;
    st.decode_total_us = 22;
    char line[256];
    nine_op_stats_format(line, sizeof(line), &st);
    assert(strstr(line, "frames=7 late_skips=2 crc=9 jpeg=5") != NULL);
    assert(strstr(line, "decode_max_us=11 decode_total_us=22") != NULL);
    puts("nine OP stats target-format order: PASS");
    return 0;
}
