// components/music_decoder/include/music_decoder.h —— 音频解码器统一接口。
// 设计意图:播放管道只依赖这一层,不直接触碰 minimp3。将来若上 AAC(替换为
// helix 编解码器),只需换 wrapper 实现,播放管道与 main 不动。
//
// 本接口是"流式解码一个字节流里的一帧 MP3"的抽象:输入压缩字节,
// 输出一帧 16bit PCM + 帧元信息(采样率/声道/码率)。
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// minimp3 解码器状态。这里的字段大小由最小 head 决定的依赖留到实现;
// 对外只暴露一个不透明句柄所需的最小完整对象。
typedef struct audio_decoder audio_decoder_t;

// 由实现决定的最大 PCM 采样/帧 = MP3 一帧 1152 * 2 声道。
#define AUDIO_DECODER_MAX_SAMPLES_PER_FRAME 1152
#define AUDIO_DECODER_MAX_CHANNELS          2

// 一帧 PCM 字节上限 = 1152 * 2 * 2(16bit)。供调用方分配固定输出缓冲。
#define AUDIO_DECODER_MAX_PCM_BYTES (AUDIO_DECODER_MAX_SAMPLES_PER_FRAME * AUDIO_DECODER_MAX_CHANNELS * 2)

// 一帧解码后的输出信息。
typedef struct {
    int sample_rate;      // Hz,如 44100 / 48000 / 32000
    int channels;         // 1 / 2
    int bitrate_kbps;     // 毫 kbps,如 128
    int frames_decoded;   // 实际解出的采样帧数(通常 1152)
} audio_decoder_frame_info_t;

// 创建一个解码器。失败返回 NULL(内部用静态 / 堆分配)。
audio_decoder_t *audio_decoder_create(void);

// 销毁解码器(释放内部资源)。
void audio_decoder_destroy(audio_decoder_t *dec);

// 重置到干净状态(换源时用,清掉内部 reservoir/overlap 状态)。
void audio_decoder_reset(audio_decoder_t *dec);

// 解码输入压缩流里从 mp3 起始的【一帧】。
//   mp3      : 指向压缩缓冲的起始处
//   mp3_bytes: 可读的压缩字节数(应 >= 一帧的完整字节,通常 417~1040)
//   pcm      : 输出 16bit PCM,容量至少 AUDIO_DECODER_MAX_PCM_BYTES
//   info     : 输出帧元信息(可为 NULL)
// 返回本次实际消费的压缩字节数(>=0)。
//   >0  : 解出了一帧,info->frames_decoded 为该帧采样数;
//   0   : 数据不足一帧(需要更多压缩字节);
//  <0  : 非法帧头 / 格式错误(调用方应跳过 / 报错)。
int audio_decoder_decode_frame(audio_decoder_t *dec,
                               const uint8_t *mp3, int mp3_bytes,
                               int16_t *pcm,
                               audio_decoder_frame_info_t *info);

#ifdef __cplusplus
}
#endif
