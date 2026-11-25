#pragma once
#include "nine_media.h"
#include <stddef.h>
// Pure formatter shared by the target OP stats log and its host test. The
// field order is: frames, late_skips, crc, jpeg.
void nine_op_stats_format(char *out, size_t capacity, const nine_media_op_stats_t *st);
