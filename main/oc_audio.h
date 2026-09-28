// main/oc_audio.h —— 对讲上行音频:16kHz 麦克风采集 → Opus 60ms 帧 → 交给上层发送。
//
// 为什么在设备端编码(而不是把裸 PCM 送出去):
//   * 16k/16bit 单声道裸 PCM = 32 KB/s,占满 BLE 空口;Opus 约 3 KB/s,省 10 倍。
//   * 60ms 一帧在 MTU 247 下正好装进一个 notify,热路径上不需要分片。
//   * 与同芯片族参考固件的做法一致(complexity 0 + DTX),C3 上已被量产验证。
//
// 线程与职责:
//   * 采集/编码都在一个音频任务里(60ms 一轮,编码在 C3 上约几毫秒),不占用按键回调、
//     也不占用 BLE 任务;编好的帧通过回调交出去,回调只允许入队,禁止阻塞。
//   * turn 起停由 begin_turn/end_turn 触发,任务在两次读之间观察标志,所以松手后最多
//     多收一块(30ms)就停;空闲时挂起 ES8311,省掉 codec 的静态功耗。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define OC_AUDIO_HZ 16000U
#define OC_AUDIO_FRAME_MS 60U
#define OC_AUDIO_FRAME_SAMPLES (OC_AUDIO_HZ / 1000U * OC_AUDIO_FRAME_MS)   // 960
#define OC_AUDIO_FRAME_PCM_BYTES (OC_AUDIO_FRAME_SAMPLES * 2U)             // 1920
#define OC_AUDIO_READ_SAMPLES 480U                                         // 一次读 30ms

// 单帧 Opus 载荷上限(与线协议的 OPUS 帧上限一致)。
#define OC_AUDIO_OPUS_MAX 512U

// 一帧编好的音频:payload 结构由上层组装为 [SEQ:1B][Opus 包]。
// 运行在音频任务上下文,必须短小(只入队/拷贝),禁止阻塞与访问 LVGL。
// 返回 false 表示这一帧没送出去(链路不可用或背压),模块只计数,不重传。
typedef bool (*oc_audio_frame_cb_t)(uint8_t seq, const uint8_t *opus, size_t len, void *ctx);

typedef struct {
    uint32_t frames_encoded;    // 编出的帧数
    uint32_t frames_delivered;  // 回调返回 true 的帧数
    uint32_t frames_dropped;    // 回调返回 false 的帧数(链路不可用/背压)
    uint32_t pcm_overruns;      // 采集侧丢弃的 PCM 块数(编码跟不上或掉电)
    uint32_t encode_us_max;     // 单帧最长编码耗时(微秒)
    uint32_t encode_us_avg;     // 平均编码耗时(微秒,按帧数取整)
    uint32_t opus_bytes;        // 编码输出总字节数(算平均包长用)
} oc_audio_stats_t;

// 初始化:准备 BSP 音频(BSP 初始化 + 16k/mono/16bit 格式)与 Opus 编码器。
// 不启动采集任务;失败返回 ESP_FAIL / ESP_ERR_NO_MEM。
esp_err_t oc_audio_init(oc_audio_frame_cb_t cb, void *ctx);

// 启动采集任务(常驻)。任务在没开始 turn 时只睡眠,不读麦克风。
esp_err_t oc_audio_start(void);

// 停止采集任务并挂起 codec(关机或切页时调用)。
void oc_audio_stop(void);

// 开始一轮对讲:清零本轮统计,唤醒 codec,开始采集成帧。
esp_err_t oc_audio_begin_turn(void);

// 结束一轮:把不满一帧的尾巴补零编出去,然后挂起 codec。
// 会等待音频任务完成(最多约 300ms),返回时本轮音频已全部交给回调。
void oc_audio_end_turn(void);

// 音频任务是否正在采集(松手后最多 30ms 内变 false)。
bool oc_audio_turn_active(void);

// 麦克风增益(dB,0..32;转发给 BSP,越界钳位)。设置立即生效并保留到下次打开 codec。
esp_err_t oc_audio_set_mic_gain_db(uint8_t db);

// 本轮统计快照(用于 turn_end 事件与串口日志)。
void oc_audio_get_stats(oc_audio_stats_t *out);
