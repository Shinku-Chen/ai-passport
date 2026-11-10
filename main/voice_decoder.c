#include "voice_decoder.h"
#include <stddef.h>

static OpusDecoder *s_decoder;

int voice_decoder_init(void)
{
    if (s_decoder) return OPUS_OK;
    int error = OPUS_OK;
    s_decoder = opus_decoder_create(VOICE_DECODER_SAMPLE_RATE, 1, &error);
    return s_decoder ? OPUS_OK : (error == OPUS_OK ? OPUS_ALLOC_FAIL : error);
}

OpusDecoder *voice_decoder_begin(void)
{
    if (!s_decoder || opus_decoder_ctl(s_decoder, OPUS_RESET_STATE) != OPUS_OK)
        return NULL;
    return s_decoder;
}
