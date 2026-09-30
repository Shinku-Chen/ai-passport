// main/oc_audio.h —— 对讲上行音频:16kHz 麦克风采集 → Opus 60ms 帧 → 交给上层发送;
// 以及 TTS 下行音频:手机推来的 Opus 包 → 解码 → I2S 播放。
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
//   * 下行播放是另一个任务(oc_play),优先级低于采集任务:丢 60ms 上行比晚几毫秒出声严重。
//     两个方向半双工:进播放态时暂停采集(oc_audio_capture_paused),播完/打断立即恢复。
//     播放规划与内存预算见 docs/development/engineering/intercom-tts-playback.md。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "oc_tts.h"   // 下行播放的纯逻辑:载荷解析、24 包解码队列、统计

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

// 上行采集是否因 TTS 播放(半双工)而暂停。
bool oc_audio_capture_paused(void);

// ---- TTS 下行播放(客户端 → 设备,见 docs/development/engineering/intercom-tts-playback.md)----
//
// 数据流:oc_audio_play_push*() 把 Opus 包拷进 24 包解码队列(手机领先设备最多约 2s),
// 播放任务取出、解码成 PCM 放进 2 块播放队列,再按 60ms 一块写给 I2S。
// 所有缓冲与解码器状态都是静态的(运行期堆只剩十几 KB,不许走 malloc)。

// 一块 PCM = 60ms @24kHz 单声道 16bit(协议允许的最高速率;16kHz 只用前半块)。
#define OC_PLAY_PCM_BLOCK_SAMPLES (24000U / 1000U * OC_AUDIO_FRAME_MS)   // 1440
#define OC_PLAY_PCM_QUEUE_DEPTH 2U

// 统计字段与线协议 tts_playback_done 事件一一对应(frames/decoded/dropped/underruns/decode_us_max)。
typedef oc_tts_play_stats_t oc_audio_play_stats_t;

// 播放任务上报给应用任务的状态变化(应用任务每拍取一次,自己组 EVENT 帧发出)。
typedef enum {
    OC_AUDIO_PLAY_EV_NONE = 0,
    OC_AUDIO_PLAY_EV_DONE,      // tts_stop 之后队列播空
    OC_AUDIO_PLAY_EV_ABORTED,   // tts_abort / 用户打断 / 解码连续失败 / 长时间无声
} oc_audio_play_ev_t;

// 入队一个 Opus 包(调用方已把 [SEQ][rate_khz][frame_ms] 头剥好)。
// 队列满不报错:丢最旧的包并计入 dropped,新包一定收下(用户等的是最新那句)。
// 返回 ESP_ERR_INVALID_ARG(参数非法)、ESP_ERR_INVALID_STATE(不在播放态)、ESP_OK。
esp_err_t oc_audio_play_push(const uint8_t *opus, size_t len, uint8_t rate_khz);

// 线协议封装:TTS_OPUS 帧的整个载荷([SEQ][rate_khz][frame_ms] + Opus 包)。
// 解析、校验(长度 4..515 / rate 16|24 / frame_ms 60)与 SEQ 缺口统计都在这里,
// 应用层用它。与 oc_audio_play_push 同一条通路,只是多带一个 SEQ。
esp_err_t oc_audio_play_push_frame(const uint8_t *payload, size_t len);

// 立即丢弃本轮剩余音频并退出播放态,等 I2S 停下、codec 挂起、采集恢复后才返回。
// tts_abort、按键打断(下一轮开始前)与断连都调它;没在播放时是空操作(幂等)。
void oc_audio_play_flush(void);

// 队列空且没有在播(应用任务用来判断是否回到常规状态)。
bool oc_audio_play_idle(void);

// 本轮统计快照(tts_playback_done 用);新一轮 oc_audio_play_start 会清零。
void oc_audio_play_get_stats(oc_audio_play_stats_t *st);

// 下行播放是否可用(解码器就绪)。hello 的 caps 用它决定要不要报 tts_opus。
bool oc_audio_play_available(void);

// tts_start:进播放态并暂停上行采集。
void oc_audio_play_start(void);

// tts_stop:不再收包,把队列里剩下的播完再退出播放态。
void oc_audio_play_stop(void);

// 取走一次播放事件(只有应用任务调用;取走后自动清空)。
oc_audio_play_ev_t oc_audio_play_take_event(void);
