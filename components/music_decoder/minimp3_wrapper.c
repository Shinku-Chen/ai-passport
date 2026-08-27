// components/music_decoder/minimp3_wrapper.c —— 用 minimp3 实现统一解码接口。
// minimp3 是 CC0 公共领域解码器,单头文件,解码器状态全在结构体内(约 2-3KB),
// 无堆分配 —— 正适合无 PSRAM 的 ESP32-C3。这里只做适配层。
#include "music_decoder.h"

// 用静态方式包含 minimp3,定义 MINIMP3_IMPLEMENTATION 展开其实现在本编译单元
// (minimp3.h 第 43 行用该宏)。放在 .c 而非头文件,避免多处包含造成重复符号。
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"   // 在组件 include 路径下

#include <stdlib.h>    // calloc / free

// minimp3 实际输出为 int16 (mp3d_sample_t)。
struct audio_decoder {
    mp3dec_t mp3;
};

audio_decoder_t *audio_decoder_create(void)
{
    audio_decoder_t *dec = (audio_decoder_t *)calloc(1, sizeof(audio_decoder_t));
    if (!dec) return NULL;
    mp3dec_init(&dec->mp3);
    return dec;
}

void audio_decoder_destroy(audio_decoder_t *dec)
{
    if (dec) free(dec);
}

void audio_decoder_reset(audio_decoder_t *dec)
{
    if (dec) mp3dec_init(&dec->mp3);
}

int audio_decoder_decode_frame(audio_decoder_t *dec,
                               const uint8_t *mp3, int mp3_bytes,
                               int16_t *pcm,
                               audio_decoder_frame_info_t *info)
{
    if (!dec || !mp3 || !pcm || mp3_bytes <= 0) return -1;

    mp3dec_frame_info_t fi = { 0 };
    int samples = mp3dec_decode_frame(&dec->mp3, mp3, mp3_bytes, (int16_t *)pcm, &fi);

    // minimp3 的 mp3dec_decode_frame:
    //   返回 samples: 输出 PCM 采样数 / 帧(成功,通常 1152);0 = 本帧头无效(坏帧)。
    //   fi.frame_bytes: 本帧实际消费的输入字节数(即使坏帧也可能 >0)。若为 0 表示
    //                   输入不足以拼出整帧,需要更多数据。
    // 统一接口的返回语义:
    //   >0 : 成功解出一帧,返回值 = 消费的压缩字节数
    //    0 : 数据不足一帧(需要更多输入),未消费任何字节
    //   <0 : 坏帧头(应跳过重同步),返回 0 在这里也归为坏帧处理
    if (info) {
        info->sample_rate    = fi.hz;
        info->channels       = fi.channels;
        info->bitrate_kbps   = fi.bitrate_kbps;
        info->frames_decoded = samples;   // 成功时 = 采样数(1152)
    }

    if (fi.frame_bytes == 0) return 0;     // 需要更多数据
    if (samples <= 0)      return -1;      // 坏帧:输入足够但帧头无效
    return fi.frame_bytes;                 // 成功:消费的压缩字节数
}
