#include "nine_op_log.h"

#include <stdio.h>

void nine_op_stats_format(char *out, size_t capacity, const nine_media_op_stats_t *st)
{
    if (!out || !capacity) return;
    if (!st) { out[0] = '\0'; return; }
    snprintf(out, capacity,
             "OP stats wall_ms=%u source_ms=%u dur_ms=%u frames=%u late_skips=%u "
             "crc=%u jpeg=%u decode_max_us=%u decode_total_us=%u "
             "frame_render_max_us=%u gap_max_ms=%u audio_writes=%u audio_max_us=%u "
             "audio_total_us=%u audio_gap_us=%u",
             (unsigned)st->wall_ms, (unsigned)st->position_ms,
             (unsigned)st->duration_ms, (unsigned)st->frames,
             (unsigned)st->late_skips, (unsigned)st->crc_checks,
             (unsigned)st->jpeg_decodes, (unsigned)st->decode_max_us,
             (unsigned)st->decode_total_us, (unsigned)st->frame_render_max_us,
             (unsigned)st->frame_gap_max_ms, (unsigned)st->audio_writes,
             (unsigned)st->audio_write_max_us, (unsigned)st->audio_write_total_us,
             (unsigned)st->audio_gap_max_us);
}
